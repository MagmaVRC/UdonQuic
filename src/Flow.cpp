#include "Flow.hpp"

#include <algorithm>
#include <deque>
#include <format>

namespace UdonQuic
{
    namespace
    {
        constexpr std::string_view SwitchGet = "SystemUInt32Array.__Get__SystemInt32__SystemUInt32";
        constexpr size_t MaxStack = 1024;

        struct Decoder
        {
            const Program& p;
            Model& m;
            std::string error;
            std::deque<int> work;
            std::vector<int> pendingIndirect;
            std::vector<int> heapCallReturns;

            bool Fail(std::string message)
            {
                if (error.empty()) error = std::move(message);
                return false;
            }

            bool IsAddressConstant(uint32_t slot, uint32_t& address) const
            {
                if (slot >= p.heap.size()) return false;
                const Slot& s = p.heap[slot];
                if (!s.Is(SlotInitialized) || s.Is(SlotExported) || s.Is(SlotSynced)) return false;
                if (s.value.kind != ValueKind::Unsigned || s.type != "SystemUInt32") return false;
                address = static_cast<uint32_t>(s.value.unsignedInteger);
                return true;
            }

            bool IsSentinel(uint32_t slot) const
            {
                uint32_t v = 0;
                return IsAddressConstant(slot, v) && v >= 0xFFFFFFFCu;
            }

            bool Instructions()
            {
                m.byWord.assign(p.code.size() + 1, -1);
                for (size_t pc = 0; pc < p.code.size();)
                {
                    Ins i;
                    i.address = uint32_t(pc * 4);
                    uint32_t raw = p.code[pc];
                    if (raw > 9 || raw == 3) return Fail(std::format("unknown opcode {:#x} at {:#06x}", raw, i.address));
                    i.op = static_cast<OpCode>(raw);
                    if (HasOperand(i.op))
                    {
                        if (pc + 1 >= p.code.size()) return Fail("truncated instruction at end of code");
                        i.operand = p.code[pc + 1];
                    }
                    if (i.op != OpCode::Jump && i.op != OpCode::JumpIfFalse && HasOperand(i.op) && i.operand >= p.heap.size())
                        return Fail(std::format("heap address {:#x} out of range at {:#06x}", i.operand, i.address));
                    m.byWord[pc] = int(m.ins.size());
                    pc += i.Size();
                    m.ins.push_back(std::move(i));
                }
                m.byWord[p.code.size()] = int(m.ins.size());
                for (Ins& i : m.ins)
                {
                    if (i.op != OpCode::Jump && i.op != OpCode::JumpIfFalse) continue;
                    if (i.operand >= 0xFFFFFFFCu)
                    {
                        i.end = true;
                        continue;
                    }
                    i.target = m.At(i.operand);
                    if (i.target < 0) return Fail(std::format("jump at {:#06x} to {:#x} is not an instruction", i.address, i.operand));
                }
                return true;
            }

            bool Flow(int index, const std::vector<Elem>& stack)
            {
                if (index == int(m.ins.size())) return true;
                if (stack.size() > MaxStack) return Fail("stack too deep");
                Ins& i = m.ins[size_t(index)];
                if (!i.reachable)
                {
                    i.reachable = true;
                    i.stackIn = stack;
                    work.push_back(index);
                    return true;
                }
                if (i.stackIn.size() != stack.size()) return Fail(std::format("stack depth differs at join {:#06x}", i.address));
                bool changed = false;
                for (size_t k = 0; k < stack.size(); ++k)
                {
                    Elem& a = i.stackIn[k];
                    if (!(a == stack[k])) return Fail(std::format("stack contents differ at join {:#06x}", i.address));
                    if (a.producer != stack[k].producer && a.producer != -1)
                    {
                        if (a.producer >= 0) m.ins[size_t(a.producer)].poisoned = true;
                        if (stack[k].producer >= 0) m.ins[size_t(stack[k].producer)].poisoned = true;
                        a.producer = -1;
                        changed = true;
                    }
                    else if (a.producer == -1 && stack[k].producer >= 0)
                    {
                        m.ins[size_t(stack[k].producer)].poisoned = true;
                    }
                }
                if (changed) work.push_back(index);
                return true;
            }

            bool Pop(std::vector<Elem>& stack, const Ins& i, Elem& out)
            {
                if (stack.empty()) return Fail(std::format("stack underflow at {:#06x}", i.address));
                out = stack.back();
                stack.pop_back();
                return true;
            }

            bool Step(int index)
            {
                Ins& i = m.ins[size_t(index)];
                std::vector<Elem> stack = i.stackIn;
                int next = index + 1;

                switch (i.op)
                {
                case OpCode::Nop:
                case OpCode::Annotation:
                    return Flow(next, stack);

                case OpCode::Push:
                {
                    Elem e{ i.operand, index, false };
                    if (stack.empty() && IsSentinel(i.operand))
                    {
                        e.base = true;
                        i.sentinel = true;
                    }
                    stack.push_back(e);
                    return Flow(next, stack);
                }

                case OpCode::Pop:
                {
                    Elem e;
                    if (!Pop(stack, i, e)) return false;
                    if (e.base) return Fail(std::format("POP of the return address at {:#06x}", i.address));
                    return Flow(next, stack);
                }

                case OpCode::JumpIfFalse:
                {
                    Elem c;
                    if (!Pop(stack, i, c)) return false;
                    if (c.base) return Fail(std::format("branch on the return address at {:#06x}", i.address));
                    return (i.end || Flow(i.target, stack)) && Flow(next, stack);
                }

                case OpCode::Jump:
                {
                    if (i.end) return true;
                    uint32_t address = 0;
                    if (!stack.empty() && !stack.back().base && IsAddressConstant(stack.back().slot, address) &&
                        index + 1 < int(m.ins.size()) && address == m.ins[size_t(index) + 1].address)
                    {
                        i.call = true;
                        i.continuation = index + 1;
                        m.continuations.push_back(index + 1);
                        std::vector<Elem> after = stack;
                        after.pop_back();
                        return Flow(i.target, { Elem{ 0, -1, true } }) && Flow(index + 1, after);
                    }
                    if (index > 0)
                    {
                        const Ins& copy = m.ins[size_t(index) - 1];
                        uint32_t back = 0;
                        if (copy.op == OpCode::Copy && !copy.returnCopy && copy.reachable && copy.stackIn.size() >= 2 && stack.size() + 2 == copy.stackIn.size())
                        {
                            const Elem& src = copy.stackIn[copy.stackIn.size() - 2];
                            int resume = src.base || !IsAddressConstant(src.slot, back) ? -1 : m.At(back);
                            if (resume >= 0 && resume < int(m.ins.size()))
                            {
                                i.heapCall = true;
                                i.continuation = resume;
                                if (std::find(heapCallReturns.begin(), heapCallReturns.end(), resume) == heapCallReturns.end()) heapCallReturns.push_back(resume);
                                return Flow(i.target, {}) && Flow(resume, stack);
                            }
                        }
                    }
                    return Flow(i.target, stack);
                }

                case OpCode::Extern:
                {
                    const ExternInfo* info = m.Extern(i);
                    if (!info) return Fail(std::format("unknown extern '{}' at {:#06x}", m.Signature(i), i.address));
                    if (info->arity < 0 || size_t(info->arity) > stack.size())
                        return Fail(std::format("extern '{}' at {:#06x} pops more than the stack holds", m.Signature(i), i.address));
                    for (int k = 0; k < info->arity; ++k)
                        if (stack[stack.size() - size_t(info->arity) + size_t(k)].base)
                            return Fail(std::format("extern consumes the return address at {:#06x}", i.address));
                    stack.resize(stack.size() - size_t(info->arity));
                    return Flow(next, stack);
                }

                case OpCode::Copy:
                {
                    Elem dst, src;
                    if (!Pop(stack, i, dst) || !Pop(stack, i, src)) return false;
                    if (dst.base) return Fail(std::format("COPY into the return address at {:#06x}", i.address));
                    if (src.base)
                    {
                        if (!stack.empty() || next >= int(m.ins.size())) return Fail(std::format("unrecognised return at {:#06x}", i.address));
                        const Ins& j = m.ins[size_t(next)];
                        if (j.op != OpCode::JumpIndirect || j.operand != dst.slot) return Fail(std::format("unrecognised return at {:#06x}", i.address));
                        i.returnCopy = true;
                    }
                    return Flow(next, stack);
                }

                case OpCode::JumpIndirect:
                {
                    if (index > 0 && m.ins[size_t(index) - 1].returnCopy && m.ins[size_t(index) - 1].reachable)
                    {
                        i.isReturn = true;
                        return true;
                    }
                    if (index > 0)
                    {
                        const Ins& g = m.ins[size_t(index) - 1];
                        if (g.op == OpCode::Extern && m.Signature(g) == SwitchGet && g.stackIn.size() >= 3 &&
                            g.stackIn.back().slot == i.operand)
                        {
                            uint32_t table = g.stackIn[g.stackIn.size() - 3].slot;
                            const Slot& t = p.heap[table];
                            if (t.value.kind != ValueKind::UIntArray || t.Is(SlotExported) || t.Is(SlotSynced))
                                return Fail(std::format("switch table at {:#06x} is not a constant", i.address));
                            if (m.roles[table] != Role::None && m.roles[table] != Role::Switch)
                                return Fail("switch table slot has another role");
                            m.roles[table] = Role::Switch;
                            for (uint32_t a : t.value.array)
                            {
                                int target = m.At(a);
                                if (target < 0) return Fail(std::format("switch entry {:#x} is not an instruction", a));
                                i.successors.push_back(target);
                                if (!Flow(target, stack)) return false;
                            }
                            return true;
                        }
                    }
                    pendingIndirect.push_back(index);
                    return true;
                }
                }
                return Fail("unreachable opcode");
            }

            bool Run()
            {
                while (!work.empty())
                {
                    int index = work.front();
                    work.pop_front();
                    if (!Step(index)) return false;
                }
                return true;
            }

            bool ResolveIndirect(bool& progressed)
            {
                progressed = false;
                std::vector<int> pending = std::move(pendingIndirect);
                pendingIndirect.clear();
                for (int index : pending)
                {
                    Ins& i = m.ins[size_t(index)];
                    uint32_t slot = i.operand;
                    if (!p.heap[slot].Is(SlotPrivate)) return Fail(std::format("JUMP_INDIRECT at {:#06x} reads a slot other behaviours can set", i.address));
                    std::vector<int> targets;
                    for (const Ins& w : m.ins)
                    {
                        if (!w.reachable) continue;
                        if (w.op == OpCode::Copy && !w.returnCopy && w.stackIn.size() >= 2 && w.stackIn.back().slot == slot)
                        {
                            uint32_t address = 0;
                            const Elem& src = w.stackIn[w.stackIn.size() - 2];
                            if (src.base || !IsAddressConstant(src.slot, address)) return Fail(std::format("JUMP_INDIRECT at {:#06x} reads an unproven address", i.address));
                            int t = m.At(address);
                            if (t < 0) return Fail(std::format("indirect target {:#x} is not an instruction", address));
                            if (std::find(targets.begin(), targets.end(), t) == targets.end()) targets.push_back(t);
                        }
                    }
                    for (int t : targets)
                    {
                        if (std::find(i.successors.begin(), i.successors.end(), t) != i.successors.end()) continue;
                        i.successors.push_back(t);
                        if (std::find(m.heapReturnTargets.begin(), m.heapReturnTargets.end(), t) == m.heapReturnTargets.end())
                            m.heapReturnTargets.push_back(t);
                        progressed = true;
                        if (std::find(heapCallReturns.begin(), heapCallReturns.end(), t) != heapCallReturns.end()) continue;
                        if (!Flow(t, i.stackIn)) return false;
                    }
                    pendingIndirect.push_back(index);
                }
                return true;
            }

            void Effects()
            {
                for (size_t index = 0; index < m.ins.size(); ++index)
                {
                    Ins& i = m.ins[index];
                    if (!i.reachable) continue;
                    const auto& s = i.stackIn;
                    auto operand = [&](size_t k, size_t count) -> const Elem& { return s[s.size() - count + k]; };
                    auto consume = [&](size_t count) {
                        for (size_t k = 0; k < count; ++k)
                        {
                            const Elem& e = operand(k, count);
                            i.producers.push_back(e.producer);
                            if (e.producer >= 0) m.ins[size_t(e.producer)].consumers.push_back(int(index));
                        }
                    };

                    switch (i.op)
                    {
                    case OpCode::Pop: consume(1); break;
                    case OpCode::JumpIfFalse:
                        consume(1);
                        i.reads.push_back(operand(0, 1).slot);
                        i.successors = i.end ? std::vector<int>{ int(index) + 1 } : std::vector<int>{ i.target, int(index) + 1 };
                        break;
                    case OpCode::Copy:
                        consume(2);
                        if (!i.returnCopy) i.reads.push_back(operand(0, 2).slot);
                        i.writes.push_back(operand(1, 2).slot);
                        break;
                    case OpCode::Extern:
                    {
                        const ExternInfo* info = m.Extern(i);
                        size_t n = size_t(info->arity);
                        consume(n);
                        for (size_t k = 0; k < n; ++k)
                        {
                            uint32_t slot = operand(k, n).slot;
                            bool isResult = info->hasResult && k + 1 == n;
                            bool written = k < 64 && (info->writes >> k) & 1;
                            if (isResult || written) i.writes.push_back(slot);
                            if (!isResult) i.reads.push_back(slot);
                        }
                        i.reads.push_back(i.operand);
                        break;
                    }
                    case OpCode::JumpIndirect: i.reads.push_back(i.operand); break;
                    case OpCode::Jump:
                        if (i.call) i.successors = { i.target };
                        else if (!i.end) i.successors = { i.target };
                        break;
                    default: break;
                    }
                    if (i.op != OpCode::Jump && i.op != OpCode::JumpIfFalse && i.op != OpCode::JumpIndirect && index + 1 < m.ins.size())
                        i.successors = { int(index) + 1 };
                    if (i.op == OpCode::Copy && i.returnCopy) i.successors = { int(index) + 1 };

                    for (uint32_t w : i.writes) m.written[w] = 1;
                    for (uint32_t r : i.reads) m.used[r] = 1;
                    if (i.op == OpCode::Push) m.used[i.operand] = 1;
                }
            }

            bool Carriers()
            {
                for (const Ins& i : m.ins)
                {
                    if (!i.reachable || !i.call) continue;
                    uint32_t slot = i.stackIn.back().slot;
                    if (m.roles[slot] != Role::None && m.roles[slot] != Role::CallReturn) return Fail("return constant has another role");
                    m.roles[slot] = Role::CallReturn;
                }
                for (int index : pendingIndirect)
                {
                    uint32_t slot = m.ins[size_t(index)].operand;
                    for (const Ins& w : m.ins)
                    {
                        if (!w.reachable) continue;
                        if (std::find(w.writes.begin(), w.writes.end(), slot) == w.writes.end()) continue;
                        if (w.op != OpCode::Copy || w.returnCopy) return Fail("return address slot written by something other than a COPY");
                        uint32_t src = w.stackIn[w.stackIn.size() - 2].slot;
                        if (m.roles[src] != Role::None && m.roles[src] != Role::HeapReturn) return Fail("return address constant has another role");
                        m.roles[src] = Role::HeapReturn;
                    }
                }
                for (size_t s = 0; s < m.roles.size(); ++s)
                    if (m.roles[s] != Role::None && m.written[s]) return Fail(std::format("code address slot {:#x} is written at run time", s));
                for (const Ins& i : m.ins)
                    if (i.reachable && i.sentinel && m.written[i.operand]) return Fail("exit sentinel slot is written at run time");
                return true;
            }
        };
    }

    int Model::At(uint32_t address) const
    {
        if (address % 4 != 0) return -1;
        size_t word = address / 4;
        if (word >= byWord.size()) return -1;
        return byWord[word];
    }

    bool Model::Editable(int push) const
    {
        if (push < 0) return false;
        const Ins& i = ins[size_t(push)];
        return i.op == OpCode::Push && i.reachable && !i.poisoned && !i.sentinel && i.consumers.size() == 1;
    }

    bool Model::IsPrivate(uint32_t slot) const
    {
        return slot < program->heap.size() && program->heap[slot].Is(SlotPrivate);
    }

    bool Model::IsConstant(uint32_t slot) const
    {
        if (slot >= program->heap.size()) return false;
        const Slot& s = program->heap[slot];
        return s.Is(SlotPrivate) && s.Is(SlotInitialized) && !s.Is(SlotExported) && !s.Is(SlotSynced) && s.value.kind != ValueKind::None &&
               s.value.kind != ValueKind::UIntArray && !written[slot] && roles[slot] == Role::None;
    }

    const std::string& Model::Signature(const Ins& i) const
    {
        static const std::string empty;
        if (i.operand >= program->heap.size()) return empty;
        const Value& v = program->heap[i.operand].value;
        return v.kind == ValueKind::String ? v.text : empty;
    }

    const ExternInfo* Model::Extern(const Ins& i) const
    {
        auto it = program->externs.find(Signature(i));
        return it == program->externs.end() ? nullptr : &it->second;
    }

    std::optional<Model> Decode(const Program& program, std::string& error)
    {
        Model m;
        m.program = &program;
        m.written.assign(program.heap.size(), 0);
        m.used.assign(program.heap.size(), 0);
        m.roles.assign(program.heap.size(), Role::None);
        Decoder d{ program, m, {}, {}, {} };

        if (!d.Instructions())
        {
            error = d.error;
            return std::nullopt;
        }

        for (const EntryPoint& e : program.entries)
        {
            int at = m.At(e.address);
            if (at < 0 || at == int(m.ins.size()))
            {
                error = std::format("entry '{}' at {:#x} is not an instruction", e.name, e.address);
                return std::nullopt;
            }
            m.entries.push_back(at);
            if (!d.Flow(at, {}))
            {
                error = d.error;
                return std::nullopt;
            }
        }

        bool progressed = true;
        while (progressed)
        {
            if (!d.Run() || !d.ResolveIndirect(progressed))
            {
                error = d.error;
                return std::nullopt;
            }
        }

        for (int index : d.pendingIndirect)
            if (m.ins[size_t(index)].successors.empty())
            {
                error = std::format("unproven JUMP_INDIRECT at {:#06x}", m.ins[size_t(index)].address);
                return std::nullopt;
            }

        for (Ins& i : m.ins)
            if (i.reachable && i.op == OpCode::Extern && !m.Extern(i))
            {
                error = std::format("unknown extern at {:#06x}", i.address);
                return std::nullopt;
            }

        d.Effects();
        if (!d.Carriers())
        {
            error = d.error;
            return std::nullopt;
        }
        return m;
    }
}
