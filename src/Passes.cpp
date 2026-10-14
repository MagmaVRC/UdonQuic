#include "Passes.hpp"

#include "Externs.hpp"
#include "Fold.hpp"

#include <algorithm>
#include <bit>
#include <map>
#include <unordered_map>
#include <unordered_set>

namespace UdonQuic
{
    namespace
    {
        using Bits = std::vector<uint64_t>;

        void Set(Bits& b, uint32_t i) { b[i >> 6] |= 1ull << (i & 63); }
        void Clear(Bits& b, uint32_t i) { b[i >> 6] &= ~(1ull << (i & 63)); }
        bool Test(const Bits& b, uint32_t i) { return (b[i >> 6] >> (i & 63)) & 1; }

        bool Or(Bits& into, const Bits& from)
        {
            bool changed = false;
            for (size_t k = 0; k < into.size(); ++k)
            {
                uint64_t v = into[k] | from[k];
                changed |= v != into[k];
                into[k] = v;
            }
            return changed;
        }

        Effect EffectOf(const Model& m, const Ins& i) { return Classify(m.Signature(i)); }

        bool ResultOnly(const ExternInfo& info)
        {
            return info.hasResult && info.arity >= 1 && info.writes == (1ull << (info.arity - 1));
        }

        const std::string& TypeOf(const IR& ir, uint32_t slot) { return ir.program.heap[slot].type; }

        std::vector<uint8_t> Leaders(const Model& m);

        Value B(bool v) { Value x; x.kind = ValueKind::Boolean; x.boolean = v; return x; }
        Value I(int64_t v) { Value x; x.kind = ValueKind::Integer; x.integer = v; return x; }
        Value U(uint64_t v) { Value x; x.kind = ValueKind::Unsigned; x.unsignedInteger = v; return x; }

        /// <summary>Private-slot liveness over the whole program. Returns may reach any call continuation; an event's end and any
        /// barrier extern (which can run another event of this behaviour) see every slot that is live when an event starts.</summary>
        struct Liveness
        {
            const Model& m;
            std::vector<int32_t> dense;
            size_t words = 0;
            std::vector<Bits> out;

            explicit Liveness(const Model& model) : m(model), dense(model.program->heap.size(), -1)
            {
                uint32_t count = 0;
                for (size_t s = 0; s < dense.size(); ++s)
                    if (m.IsPrivate(uint32_t(s))) dense[s] = int32_t(count++);
                words = (count + 63) / 64;
                size_t n = m.ins.size();
                out.assign(n, Bits());
                std::vector<uint8_t> lead = Leaders(m);
                std::vector<uint8_t> barrier(n, 0);
                for (size_t k = 0; k < n; ++k)
                    barrier[k] = m.ins[k].reachable && m.ins[k].op == OpCode::Extern && EffectOf(m, m.ins[k]) == Effect::Barrier;

                struct Block
                {
                    int first, last;
                    bool exits = false, returns = false, barrier = false, entry = false, continuation = false;
                    std::vector<int> succ, pred;
                    Bits in, out;
                };
                std::vector<Block> blocks;
                std::vector<int> blockOf(n, -1);
                for (size_t k = 0; k < n; ++k)
                {
                    if (!m.ins[k].reachable) continue;
                    if (lead[k] || blocks.empty() || blockOf[k - 1] < 0) blocks.push_back({ int(k), int(k) });
                    blocks.back().last = int(k);
                    blockOf[k] = int(blocks.size()) - 1;
                    blocks.back().barrier |= barrier[k] != 0;
                }
                for (int e : m.entries) blocks[size_t(blockOf[size_t(e)])].entry = true;
                for (int c : m.continuations) blocks[size_t(blockOf[size_t(c)])].continuation = true;
                for (size_t b = 0; b < blocks.size(); ++b)
                {
                    Block& bl = blocks[b];
                    const Ins& last = m.ins[size_t(bl.last)];
                    bl.returns = last.isReturn;
                    bl.exits = last.isReturn || last.end || last.successors.empty();
                    if (!last.isReturn)
                        for (int s : last.successors)
                            if (s >= 0 && size_t(s) < n && blockOf[size_t(s)] >= 0) bl.succ.push_back(blockOf[size_t(s)]);
                    for (int s : bl.succ) blocks[size_t(s)].pred.push_back(int(b));
                    bl.in.assign(words, 0);
                    bl.out.assign(words, 0);
                }

                Bits exitLive(words, 0), returnLive(words, 0), x(words, 0);
                auto transfer = [&](const Block& bl, Bits& v, bool record) {
                    for (int k = bl.last; k >= bl.first; --k)
                    {
                        if (record) out[size_t(k)] = v;
                        const Ins& i = m.ins[size_t(k)];
                        Kill(i, v);
                        for (uint32_t r : i.reads)
                            if (dense[r] >= 0) Set(v, uint32_t(dense[r]));
                        if (barrier[size_t(k)]) Or(v, exitLive);
                    }
                };

                std::vector<uint8_t> queued(blocks.size(), 1);
                std::vector<int> work;
                for (size_t b = 0; b < blocks.size(); ++b) work.push_back(int(b));
                auto enqueue = [&](int b) {
                    if (!queued[size_t(b)]) queued[size_t(b)] = 1, work.push_back(b);
                };
                for (;;)
                {
                    while (!work.empty())
                    {
                        int b = work.back();
                        work.pop_back();
                        queued[size_t(b)] = 0;
                        Block& bl = blocks[size_t(b)];
                        std::fill(bl.out.begin(), bl.out.end(), 0);
                        for (int s : bl.succ) Or(bl.out, blocks[size_t(s)].in);
                        if (bl.returns) Or(bl.out, returnLive);
                        if (bl.exits) Or(bl.out, exitLive);
                        x = bl.out;
                        transfer(bl, x, false);
                        if (x == bl.in) continue;
                        bl.in = x;
                        for (int p : bl.pred) enqueue(p);
                    }

                    bool exitGrew = false, returnGrew = false;
                    for (const Block& bl : blocks)
                        if (bl.entry) exitGrew |= Or(exitLive, bl.in);
                    for (const Block& bl : blocks)
                        if (bl.continuation) returnGrew |= Or(returnLive, bl.in);
                    returnGrew |= Or(returnLive, exitLive);
                    if (!exitGrew && !returnGrew) break;
                    for (size_t r = blocks.size(); r-- > 0;)
                        if ((returnGrew && blocks[r].returns) || (exitGrew && (blocks[r].exits || blocks[r].barrier))) enqueue(int(r));
                }
                for (const Block& bl : blocks)
                {
                    x = bl.out;
                    transfer(bl, x, true);
                }
            }

            void Kill(const Ins& i, Bits& x) const
            {
                uint32_t slot = 0;
                if (i.op == OpCode::Copy && !i.writes.empty()) slot = i.writes[0];
                else if (i.op == OpCode::Extern && m.Extern(i)->hasResult && !i.stackIn.empty()) slot = i.stackIn.back().slot;
                else return;
                if (dense[slot] >= 0) Clear(x, uint32_t(dense[slot]));
            }

            bool LiveAfter(int k, uint32_t slot) const
            {
                return dense[slot] < 0 || Test(out[size_t(k)], uint32_t(dense[slot]));
            }
        };

        std::vector<uint8_t> Leaders(const Model& m)
        {
            std::vector<uint8_t> lead(m.ins.size() + 1, 0);
            if (!m.ins.empty()) lead[0] = 1;
            for (int e : m.entries) lead[size_t(e)] = 1;
            for (int c : m.continuations) lead[size_t(c)] = 1;
            for (int t : m.heapReturnTargets) lead[size_t(t)] = 1;
            for (size_t k = 0; k < m.ins.size(); ++k)
            {
                const Ins& i = m.ins[k];
                if (i.target >= 0) lead[size_t(i.target)] = 1;
                for (int s : i.successors)
                    if (s >= 0 && i.op == OpCode::JumpIndirect) lead[size_t(s)] = 1;
                if (i.op == OpCode::Jump || i.op == OpCode::JumpIfFalse || i.op == OpCode::JumpIndirect) lead[k + 1] = 1;
            }
            return lead;
        }

        /// <summary>The operand slots of a consuming instruction, bottom of stack first.</summary>
        std::vector<uint32_t> Operands(const Ins& i, size_t count)
        {
            std::vector<uint32_t> out;
            for (size_t k = 0; k < count; ++k) out.push_back(i.stackIn[i.stackIn.size() - count + k].slot);
            return out;
        }

        bool AllEditable(const Model& m, const Ins& i, int self)
        {
            for (int p : i.producers)
                if (!m.Editable(p) || m.ins[size_t(p)].consumers[0] != self) return false;
            return true;
        }

        void RemoveWithProducers(IR& ir, const Ins& i, int self)
        {
            for (int p : i.producers) ir.Remove(p);
            ir.Remove(self);
        }

        /// <summary>Turns a consuming instruction with editable producers into COPY src -&gt; (its last operand).</summary>
        void ToCopy(IR& ir, const Ins& i, int self, uint32_t src)
        {
            if (i.producers.size() == 1)
            {
                ir.InsertBefore(i.producers.front(), { ir.Add(OpCode::Push, src) });
                ir.nodes[size_t(self)] = { OpCode::Copy, 0, -1, false };
                return;
            }
            ir.nodes[size_t(i.producers.front())].operand = src;
            for (size_t k = 1; k + 1 < i.producers.size(); ++k) ir.Remove(i.producers[k]);
            ir.nodes[size_t(self)] = { OpCode::Copy, 0, -1, false };
        }

        bool IsTrampoline(const Model& m, int c)
        {
            if (c < 0 || size_t(c) + 2 >= m.ins.size()) return false;
            const Ins& push = m.ins[size_t(c)];
            const Ins& copy = m.ins[size_t(c) + 1];
            const Ins& jump = m.ins[size_t(c) + 2];
            return push.op == OpCode::Push && copy.returnCopy && jump.isReturn && copy.producers.size() == 2 && copy.producers[1] == c &&
                   jump.operand == push.operand;
        }

        int CallPush(const Model& m, const Ins& call)
        {
            int p = call.stackIn.back().producer;
            if (p < 0 || m.ins[size_t(p)].poisoned || !m.ins[size_t(p)].consumers.empty()) return -1;
            return p;
        }
    }

    bool DeadCode(IR& ir, const Model& m, PassContext&)
    {
        bool changed = false;
        for (size_t k = 0; k < m.ins.size(); ++k)
        {
            const Ins& i = m.ins[k];
            if (!i.reachable || i.op == OpCode::Nop || i.op == OpCode::Annotation)
            {
                ir.Remove(int(k));
                changed = true;
            }
        }
        return changed;
    }

    bool Jumps(IR& ir, const Model& m, PassContext& ctx)
    {
        bool changed = false;
        auto plain = [&](int k) {
            const Ins& i = m.ins[size_t(k)];
            return i.reachable && i.op == OpCode::Jump && !i.call && !i.heapCall && !i.end;
        };
        auto isEnd = [&](int k) { return m.ins[size_t(k)].reachable && m.ins[size_t(k)].end && m.ins[size_t(k)].op == OpCode::Jump; };
        int live = ir.Live();

        for (size_t k = 0; k < m.ins.size(); ++k)
        {
            const Ins& i = m.ins[k];
            if (!i.reachable) continue;

            if ((i.op == OpCode::Jump && !i.call && !i.end) || (i.op == OpCode::JumpIfFalse && !i.end))
            {
                int t = i.target;
                for (int hops = 0; hops < 32 && plain(t) && t != int(k); ++hops) t = m.ins[size_t(t)].target;

                if (i.op == OpCode::Jump && isEnd(t))
                {
                    ir.nodes[k] = { OpCode::Jump, EndAddress, -1, false };
                    changed = true;
                    continue;
                }
                if (t == int(k) + 1)
                {
                    if (i.op == OpCode::Jump) ir.Remove(int(k));
                    else ir.nodes[k] = { OpCode::Pop, 0, -1, false };
                    changed = true;
                    continue;
                }
                if (i.op == OpCode::Jump && IsTrampoline(m, t) && live + 2 <= ctx.growthLimit)
                {
                    live += 2;
                    const Ins& push = m.ins[size_t(t)];
                    ir.nodes[k] = { OpCode::Push, push.operand, -1, false };
                    int copy = ir.Add(OpCode::Copy), jump = ir.Add(OpCode::JumpIndirect, push.operand);
                    ir.origin[size_t(copy)] = ir.origin[size_t(t) + 1];
                    ir.origin[size_t(jump)] = ir.origin[size_t(t) + 2];
                    ir.InsertAfter(int(k), { copy, jump });
                    changed = true;
                    continue;
                }
                if (t != i.target)
                {
                    ir.nodes[k].target = t;
                    changed = true;
                }
                continue;
            }

            if (i.op == OpCode::Pop && m.Editable(i.producers[0]) && i.producers[0] == int(k) - 1)
            {
                ir.Remove(int(k) - 1);
                ir.Remove(int(k));
                changed = true;
            }
        }
        return changed;
    }

    bool TailCalls(IR& ir, const Model& m, PassContext& ctx)
    {
        bool changed = false;
        for (size_t k = 0; k < m.ins.size(); ++k)
        {
            const Ins& i = m.ins[k];
            if (!i.reachable || !i.call || i.stackIn.size() != 2 || !i.stackIn[0].base) continue;
            if (!IsTrampoline(m, i.continuation)) continue;
            int push = CallPush(m, i);
            if (push < 0) continue;
            ir.addressCarriers.erase(ir.nodes[size_t(push)].operand);
            ir.Remove(push);
            ++ctx.stats.tailCalls;
            changed = true;
        }
        return changed;
    }

    bool Inline(IR& ir, const Model& m, PassContext& ctx)
    {
        struct Body
        {
            bool ok = false;
            std::vector<uint8_t> seen;
            std::vector<int> nodes;
            int size = 0;
        };
        std::map<int, Body> bodies;

        auto bodyOf = [&](int target) -> const Body& {
            auto it = bodies.find(target);
            if (it != bodies.end()) return it->second;
            Body b;
            std::vector<uint8_t> seen(m.ins.size(), 0);
            std::vector<int> work{ target };
            seen[size_t(target)] = 1;
            b.ok = true;
            while (!work.empty() && b.ok)
            {
                int k = work.back();
                work.pop_back();
                const Ins& i = m.ins[size_t(k)];
                b.nodes.push_back(k);
                std::vector<int> next;
                if (!i.reachable || i.sentinel) b.ok = false;
                else if (i.call)
                {
                    if (i.target == target || CallPush(m, i) < 0) b.ok = false;
                    next = { i.continuation };
                }
                else if (i.isReturn || (i.end && i.op == OpCode::Jump)) {}
                else if (i.op == OpCode::JumpIndirect) b.ok = false;
                else if (i.op == OpCode::Copy && i.returnCopy)
                {
                    if (!IsTrampoline(m, k - 1)) b.ok = false;
                    next = i.successors;
                }
                else next = i.successors;
                if (i.successors.empty() && !i.isReturn && !i.end && !i.call) b.ok = false;
                for (int s : next)
                {
                    if (s < 0 || size_t(s) >= m.ins.size()) { b.ok = false; break; }
                    if (!seen[size_t(s)]) seen[size_t(s)] = 1, work.push_back(s);
                }
            }
            std::sort(b.nodes.begin(), b.nodes.end());
            for (int k : b.nodes)
                if (m.ins[size_t(k)].call && !seen[size_t(CallPush(m, m.ins[size_t(k)]))]) b.ok = false;

            b.seen = std::move(seen);
            for (int k : b.nodes)
            {
                const Ins& i = m.ins[size_t(k)];
                if (i.isReturn) b.size += 1;
                else if (!i.returnCopy && !(i.op == OpCode::Push && size_t(k) + 2 < m.ins.size() && m.ins[size_t(k) + 2].isReturn &&
                                              m.ins[size_t(k) + 1].returnCopy))
                    b.size += 1;
            }
            return bodies.emplace(target, std::move(b)).first->second;
        };

        auto closed = [&](const Body& b, int target) {
            for (size_t j = 0; j < m.ins.size(); ++j)
            {
                const Ins& i = m.ins[j];
                if (!i.reachable || b.seen[j]) continue;
                for (int s : i.successors)
                    if (s >= 0 && s != target && b.seen[size_t(s)]) return false;
                if (i.call && b.seen[size_t(i.continuation)]) return false;
            }
            for (int e : m.entries)
                if (b.seen[size_t(e)] && e != target && !(m.ins[size_t(e)].sentinel && e + 1 == target)) return false;
            return true;
        };

        std::unordered_set<int> entered;
        for (int e : m.entries)
        {
            entered.insert(e);
            if (m.ins[size_t(e)].sentinel) entered.insert(e + 1);
        }

        std::unordered_map<int, int> callers;
        std::vector<int> sites;
        for (size_t k = 0; k < m.ins.size(); ++k)
        {
            const Ins& i = m.ins[k];
            if (!i.reachable || !i.call) continue;
            ++callers[i.target];
            sites.push_back(int(k));
        }
        std::stable_sort(sites.begin(), sites.end(), [&](int a, int b) { return bodyOf(m.ins[size_t(a)].target).size < bodyOf(m.ins[size_t(b)].target).size; });

        bool changed = false;
        int live = ir.Live();
        for (int k : sites)
        {
            const Ins& call = m.ins[size_t(k)];
            const Body& body = bodyOf(call.target);
            if (!body.ok) continue;
            int push = CallPush(m, call);
            if (push < 0) continue;
            int constantArguments = 0;
            for (int x = push + 1; x < k; ++x)
                if (m.ins[size_t(x)].op == OpCode::Copy && !m.ins[size_t(x)].returnCopy && m.IsConstant(Operands(m.ins[size_t(x)], 2)[0])) ++constantArguments;
            int budget = std::min(ctx.options.inlineBudget * 3, ctx.options.inlineBudget + 16 * constantArguments);
            if (body.size > budget && (callers[call.target] != 1 || entered.contains(call.target) || !closed(body, call.target))) continue;
            if (live + body.size > ctx.growthLimit) continue;
            if (ctx.options.maxInlines >= 0 && ctx.stats.inlined >= ctx.options.maxInlines) break;

            std::unordered_map<int, int> clone;
            for (int b : body.nodes)
            {
                const Node& source = ir.built[size_t(b)];
                clone[b] = ir.Add(source.op, source.operand, source.target);
                ir.origin[size_t(clone[b])] = ir.origin[size_t(b)];
            }

            std::vector<int> layout;
            for (int b : body.nodes)
            {
                const Ins& src = m.ins[size_t(b)];
                Node& n = ir.nodes[size_t(clone[b])];
                if ((src.op == OpCode::Jump && !src.call) || src.op == OpCode::JumpIfFalse)
                    if (n.target >= 0 && clone.contains(n.target)) n.target = clone[n.target];
                if (src.isReturn)
                    n = { OpCode::Jump, 0, call.continuation, false };
                else if (src.returnCopy || (src.op == OpCode::Push && size_t(b) + 2 < m.ins.size() && m.ins[size_t(b) + 2].isReturn && m.ins[size_t(b) + 1].returnCopy &&
                                            clone.contains(b + 2)))
                    n.removed = true;
                if (src.call)
                {
                    int innerPush = CallPush(m, src);
                    uint32_t oldSlot = ir.built[size_t(innerPush)].operand;
                    const Slot& s = ir.program.heap[oldSlot];
                    uint32_t fresh = ir.NewSlot(s.type, s.value);
                    ir.nodes[size_t(clone[innerPush])].operand = fresh;
                    ir.addressCarriers[fresh] = clone[src.continuation];
                }
                layout.push_back(clone[b]);
            }

            ir.addressCarriers.erase(ir.nodes[size_t(push)].operand);
            ir.Remove(push);
            ir.nodes[size_t(k)] = { OpCode::Jump, 0, clone[call.target], false };
            ir.InsertAfter(k, layout);
            live += body.size - 2;
            ++ctx.stats.inlined;
            changed = true;
        }
        return changed;
    }

    bool Local(IR& ir, const Model& m, PassContext& ctx)
    {
        struct Available
        {
            uint32_t signature;
            std::vector<uint32_t> inputs;
            uint32_t result;
        };

        std::unordered_map<uint32_t, uint32_t> copyOf;
        std::unordered_map<uint32_t, Value> known;
        std::vector<Available> avail;
        auto reset = [&] {
            copyOf.clear();
            known.clear();
            avail.clear();
        };
        auto kill = [&](uint32_t w) {
            copyOf.erase(w);
            std::erase_if(copyOf, [&](const auto& kv) { return kv.second == w; });
            known.erase(w);
            std::erase_if(avail, [&](const Available& a) {
                return a.result == w || std::find(a.inputs.begin(), a.inputs.end(), w) != a.inputs.end();
            });
        };
        auto valueOf = [&](uint32_t slot) -> std::optional<Value> {
            if (auto it = known.find(slot); it != known.end()) return it->second;
            if (m.IsConstant(slot)) return ir.program.heap[slot].value;
            return std::nullopt;
        };
        const bool folding = (ctx.options.passes & PassFold) != 0;
        const bool copies = (ctx.options.passes & PassCopies) != 0;
        auto propagate = [&](const Ins& i, int self, size_t index, uint32_t& slot) {
            if (!copies) return false;
            auto it = copyOf.find(slot);
            if (it == copyOf.end()) return false;
            int p = i.producers[index];
            if (!m.Editable(p) || m.ins[size_t(p)].consumers[0] != self) return false;
            ir.nodes[size_t(p)].operand = it->second;
            slot = it->second;
            ++ctx.stats.copiesRemoved;
            return true;
        };

        std::vector<uint8_t> mixed(ir.program.heap.size(), 0);
        for (bool grew = true; grew;)
        {
            grew = false;
            for (const Ins& i : m.ins)
            {
                if (!i.reachable) continue;
                auto mark = [&](uint32_t slot) {
                    if (!mixed[slot]) mixed[slot] = 1, grew = true;
                };
                if (i.op == OpCode::Copy && !i.returnCopy)
                {
                    std::vector<uint32_t> ops = Operands(i, 2);
                    if (mixed[ops[0]] || TypeOf(ir, ops[0]) != TypeOf(ir, ops[1])) mark(ops[1]);
                }
                else if (i.op == OpCode::Extern)
                {
                    const ExternInfo* info = m.Extern(i);
                    Signature sig;
                    bool parsed = ParseSignature(m.Signature(i), sig);
                    for (uint32_t w : i.writes)
                        if (!(ResultOnly(*info) && parsed && w == i.writes.back() && TypeOf(ir, w) == sig.result)) mark(w);
                }
            }
        }

        std::vector<uint8_t> lead = Leaders(m);
        bool changed = false;
        for (size_t k = 0; k < m.ins.size(); ++k)
        {
            const Ins& i = m.ins[k];
            if (lead[k]) reset();
            if (!i.reachable) continue;
            int self = int(k);

            if (i.op == OpCode::Extern)
            {
                const ExternInfo* info = m.Extern(i);
                size_t n = size_t(info->arity);
                std::vector<uint32_t> ops = Operands(i, n);
                for (size_t a = 0; a < n; ++a)
                {
                    bool written = (a + 1 == n && info->hasResult) || (a < 64 && (info->writes >> a) & 1);
                    if (!written) changed |= propagate(i, self, a, ops[a]);
                }

                Effect effect = EffectOf(m, i);
                if (effect == Effect::Barrier)
                {
                    reset();
                    continue;
                }
                if (effect == Effect::Write)
                    std::erase_if(avail, [&](const Available& a) { return Classify(ir.program.heap[a.signature].value.text) == Effect::Read; });
                if (!ResultOnly(*info) || n < 2 || !AllEditable(m, i, self))
                {
                    for (uint32_t w : i.writes) kill(w);
                    continue;
                }

                uint32_t result = ops.back();
                std::vector<uint32_t> inputs(ops.begin(), ops.end() - 1);
                Signature sig;
                bool parsed = ParseSignature(m.Signature(i), sig) && sig.parameters.size() == inputs.size();

                std::vector<std::optional<Value>> values;
                bool allKnown = parsed;
                for (size_t a = 0; a < inputs.size(); ++a)
                {
                    auto v = valueOf(inputs[a]);
                    if (v && parsed && TypeOf(ir, inputs[a]) != sig.parameters[a]) v.reset();
                    values.push_back(v);
                    allKnown &= v.has_value();
                }

                if (allKnown && folding)
                {
                    std::vector<Value> args;
                    for (auto& v : values) args.push_back(*v);
                    if (auto folded = Fold(sig, args))
                    {
                        uint32_t c = ir.Constant(sig.result, *folded);
                        ToCopy(ir, i, self, c);
                        kill(result);
                        if (TypeOf(ir, result) == sig.result) known[result] = *folded;
                        ++ctx.stats.folded;
                        changed = true;
                        continue;
                    }
                }

                if (parsed && folding)
                    if (auto keep = Identity(sig, values); keep && TypeOf(ir, inputs[*keep]) == sig.result && !mixed[inputs[*keep]])
                    {
                        uint32_t src = inputs[*keep];
                        if (src == result) RemoveWithProducers(ir, i, self);
                        else ToCopy(ir, i, self, src);
                        kill(result);
                        if (src != result && TypeOf(ir, src) == TypeOf(ir, result)) copyOf[result] = src;
                        ++ctx.stats.folded;
                        changed = true;
                        continue;
                    }

                auto hit = std::find_if(avail.begin(), avail.end(), [&](const Available& a) { return a.signature == i.operand && a.inputs == inputs; });
                if (hit == avail.end())
                    hit = std::find_if(avail.begin(), avail.end(), [&](const Available& a) {
                        return ir.program.heap[a.signature].value.text == m.Signature(i) && a.inputs == inputs;
                    });
                if (hit != avail.end() && copies)
                {
                    uint32_t previous = hit->result;
                    if (previous == result) RemoveWithProducers(ir, i, self);
                    else ToCopy(ir, i, self, previous);
                    kill(result);
                    if (previous != result && TypeOf(ir, previous) == TypeOf(ir, result)) copyOf[result] = previous;
                    ++ctx.stats.folded;
                    changed = true;
                    continue;
                }

                kill(result);
                if (std::find(inputs.begin(), inputs.end(), result) == inputs.end()) avail.push_back({ i.operand, inputs, result });
                continue;
            }

            if (i.op == OpCode::Copy)
            {
                if (i.returnCopy)
                {
                    for (uint32_t w : i.writes) kill(w);
                    continue;
                }
                std::vector<uint32_t> ops = Operands(i, 2);
                changed |= propagate(i, self, 0, ops[0]);
                uint32_t src = ops[0], dst = ops[1];
                if (src == dst && copies && AllEditable(m, i, self))
                {
                    RemoveWithProducers(ir, i, self);
                    ++ctx.stats.copiesRemoved;
                    changed = true;
                    continue;
                }
                std::optional<Value> v = valueOf(src);
                kill(dst);
                if (TypeOf(ir, src) == TypeOf(ir, dst))
                {
                    if (src != dst) copyOf[dst] = src;
                    if (v) known[dst] = *v;
                }
                continue;
            }

            if (i.op == OpCode::JumpIfFalse)
            {
                std::vector<uint32_t> ops = Operands(i, 1);
                changed |= propagate(i, self, 0, ops[0]);
                std::optional<Value> v = valueOf(ops[0]);
                if (folding && v && v->kind == ValueKind::Boolean && TypeOf(ir, ops[0]) == "SystemBoolean" && AllEditable(m, i, self))
                {
                    ir.Remove(i.producers[0]);
                    if (v->boolean) ir.Remove(self);
                    else ir.nodes[k] = { OpCode::Jump, i.end ? EndAddress : 0, i.target, false };
                    ++ctx.stats.branchesSimplified;
                    changed = true;
                }
                continue;
            }
        }
        return changed;
    }

    namespace
    {
        /// <summary>Per-block record of the last instruction that touched each slot.</summary>
        struct Access
        {
            std::unordered_map<uint32_t, int> last;
            int barrier = -1;
            int mayThrow = -1;

            void Reset()
            {
                last.clear();
                barrier = -1;
                mayThrow = -1;
            }

            void Touch(const Ins& i, int k, const Model& m)
            {
                for (uint32_t r : i.reads) last[r] = k;
                for (uint32_t w : i.writes) last[w] = k;
                if (i.op != OpCode::Extern) return;
                Effect e = EffectOf(m, i);
                if (e == Effect::Barrier) barrier = k;
                if (e != Effect::PureNoThrow) mayThrow = k;
            }

            bool Quiet(uint32_t slot, int after) const
            {
                auto it = last.find(slot);
                return it == last.end() || it->second <= after;
            }
        };
    }

    bool Branches(IR& ir, const Model& m, PassContext& ctx)
    {
        Liveness live(m);
        std::vector<uint8_t> lead = Leaders(m);
        std::unordered_set<uint32_t> touched;
        std::unordered_set<int> consumed;
        Access access;
        std::unordered_map<uint32_t, int> def;
        bool changed = false;

        auto fresh = [&](std::initializer_list<uint32_t> slots) {
            for (uint32_t s : slots)
                if (touched.contains(s)) return false;
            return true;
        };

        for (size_t k = 0; k < m.ins.size(); ++k)
        {
            const Ins& i = m.ins[k];
            if (lead[k])
            {
                access.Reset();
                def.clear();
            }
            if (!i.reachable) continue;
            int self = int(k);

            if (i.op == OpCode::Extern && IsBooleanNot(m.Signature(i)) && AllEditable(m, i, self) && !consumed.contains(self))
            {
                std::vector<uint32_t> ops = Operands(i, 2);
                uint32_t c = ops[0], t = ops[1];
                auto f = def.find(c);
                if (f != def.end() && c != t && m.IsPrivate(c) && !live.LiveAfter(self, c) && fresh({ c, t }))
                {
                    const Ins& cmp = m.ins[size_t(f->second)];
                    Signature sig;
                    std::optional<std::string> inverse;
                    if (ParseSignature(m.Signature(cmp), sig)) inverse = InvertedCompare(sig);
                    bool quiet = access.Quiet(c, f->second) && access.Quiet(t, f->second) && access.mayThrow <= f->second;
                    for (uint32_t r : Operands(cmp, 3)) quiet &= r == c || access.Quiet(r, f->second);
                    if (inverse && quiet && m.Editable(cmp.producers[2]) && TypeOf(ir, t) == "SystemBoolean")
                    {
                        ExternInfo info = *m.Extern(cmp);
                        ir.nodes[size_t(f->second)].operand = ir.ExternSlot(*inverse, info);
                        ir.nodes[size_t(cmp.producers[2])].operand = t;
                        RemoveWithProducers(ir, i, self);
                        touched.insert({ c, t });
                        ++ctx.stats.branchesSimplified;
                        changed = true;
                        access.Touch(i, self, m);
                        def.erase(c);
                        def[t] = f->second;
                        continue;
                    }
                }
            }

            if (i.op == OpCode::JumpIfFalse && m.Editable(i.producers[0]) && m.ins[size_t(i.producers[0])].consumers[0] == self)
            {
                uint32_t t = Operands(i, 1)[0];
                auto e = def.find(t);
                if (e != def.end() && !consumed.contains(e->second) && m.IsPrivate(t) && !live.LiveAfter(self, t))
                {
                    const Ins& neg = m.ins[size_t(e->second)];
                    if (neg.op == OpCode::Extern && IsBooleanNot(m.Signature(neg)) && AllEditable(m, neg, e->second))
                    {
                        uint32_t c = Operands(neg, 2)[0];
                        if (c != t && access.Quiet(c, e->second) && access.Quiet(t, e->second) && fresh({ c, t }))
                        {
                            ir.nodes[size_t(i.producers[0])].operand = c;
                            ir.nodes[k].target = int(k) + 1;
                            ir.InsertAfter(self, { ir.Add(OpCode::Jump, i.end ? EndAddress : 0, i.target) });
                            RemoveWithProducers(ir, neg, e->second);
                            consumed.insert(e->second);
                            touched.insert({ c, t });
                            ++ctx.stats.branchesSimplified;
                            changed = true;
                        }
                    }
                }
            }

            access.Touch(i, self, m);
            if (i.op == OpCode::Extern && EffectOf(m, i) == Effect::Barrier) def.clear();
            for (uint32_t w : i.writes) def.erase(w);
            if (i.op == OpCode::Extern && EffectOf(m, i) != Effect::Barrier)
            {
                const ExternInfo* info = m.Extern(i);
                if (ResultOnly(*info) && AllEditable(m, i, self)) def[i.writes.back()] = self;
            }
        }
        return changed;
    }

    bool DeadStores(IR& ir, const Model& m, PassContext& ctx)
    {
        Liveness live(m);
        std::vector<uint8_t> lead = Leaders(m);
        std::unordered_set<uint32_t> touched;
        std::unordered_set<int> removed;
        Access access;
        std::unordered_map<uint32_t, int> def;
        bool changed = false;

        for (size_t k = 0; k < m.ins.size(); ++k)
        {
            const Ins& i = m.ins[k];
            if (lead[k])
            {
                access.Reset();
                def.clear();
            }
            if (!i.reachable) continue;
            int self = int(k);

            if (i.op == OpCode::Extern)
            {
                const ExternInfo* info = m.Extern(i);
                uint32_t r = i.writes.empty() ? 0 : i.writes.back();
                if (ResultOnly(*info) && EffectOf(m, i) == Effect::PureNoThrow && m.IsPrivate(r) && !live.LiveAfter(self, r) &&
                    !touched.contains(r) && AllEditable(m, i, self))
                {
                    RemoveWithProducers(ir, i, self);
                    removed.insert(self);
                    touched.insert(r);
                    ++ctx.stats.deadStores;
                    changed = true;
                    continue;
                }
            }

            if (i.op == OpCode::Copy && !i.returnCopy)
            {
                std::vector<uint32_t> ops = Operands(i, 2);
                uint32_t src = ops[0], dst = ops[1];
                if (m.IsPrivate(dst) && !live.LiveAfter(self, dst) && !touched.contains(dst) && AllEditable(m, i, self))
                {
                    RemoveWithProducers(ir, i, self);
                    touched.insert(dst);
                    ++ctx.stats.deadStores;
                    changed = true;
                    continue;
                }

                auto d = def.find(src);
                if (d != def.end() && !removed.contains(d->second) && src != dst && m.IsPrivate(src) && !live.LiveAfter(self, src) &&
                    TypeOf(ir, src) == TypeOf(ir, dst) && !touched.contains(src) && !touched.contains(dst) && AllEditable(m, i, self) &&
                    access.Quiet(dst, d->second) && access.mayThrow <= d->second)
                {
                    auto lastSrc = access.last.find(src);
                    if (lastSrc != access.last.end() && lastSrc->second == d->second)
                    {
                        const Ins& e = m.ins[size_t(d->second)];
                        ir.nodes[size_t(e.producers.back())].operand = dst;
                        RemoveWithProducers(ir, i, self);
                        touched.insert({ src, dst });
                        ++ctx.stats.copiesRemoved;
                        changed = true;
                        access.last[dst] = self;
                        continue;
                    }
                }
            }

            access.Touch(i, self, m);
            if (i.op == OpCode::Extern && EffectOf(m, i) == Effect::Barrier) def.clear();
            for (uint32_t w : i.writes) def.erase(w);
            if (i.op == OpCode::Extern)
            {
                const ExternInfo* info = m.Extern(i);
                if (ResultOnly(*info) && AllEditable(m, i, self)) def[i.writes.back()] = self;
            }
        }
        return changed;
    }

    namespace
    {
        std::vector<uint32_t> Inputs(const Model& m, const Ins& i)
        {
            std::vector<uint32_t> ops = Operands(i, size_t(m.Extern(i)->arity));
            if (m.Extern(i)->hasResult) ops.pop_back();
            return ops;
        }

        bool Reusable(const Model& m, const Ins& i, int self)
        {
            if (i.op != OpCode::Extern) return false;
            Effect e = EffectOf(m, i);
            if (e == Effect::Barrier || !ResultOnly(*m.Extern(i)) || !AllEditable(m, i, self)) return false;
            return size_t(self) + 1 >= m.ins.size() || m.ins[size_t(self) + 1].op != OpCode::JumpIndirect;
        }

        std::vector<std::vector<int>> Predecessors(const Model& m)
        {
            std::vector<std::vector<int>> preds(m.ins.size());
            for (size_t k = 0; k < m.ins.size(); ++k)
            {
                const Ins& i = m.ins[k];
                if (!i.reachable) continue;
                for (int s : i.successors)
                    if (s >= 0 && size_t(s) < m.ins.size()) preds[size_t(s)].push_back(int(k));
                if (i.call && i.continuation >= 0) preds[size_t(i.continuation)].push_back(int(k));
            }
            return preds;
        }

        int Clone(IR& ir, int source)
        {
            const Node& n = ir.built[size_t(source)];
            int id = ir.Add(n.op, n.operand, n.target);
            ir.origin[size_t(id)] = ir.origin[size_t(source)];
            return id;
        }
    }

    bool Global(IR& ir, const Model& m, PassContext& ctx)
    {
        struct Expr
        {
            std::string signature;
            std::vector<uint32_t> inputs;
            uint32_t result;
            bool operator==(const Expr&) const = default;
        };
        struct Known
        {
            std::string type;
            Value value;
            bool operator==(const Known&) const = default;
        };
        struct Facts
        {
            std::vector<Expr> exprs;
            std::map<uint32_t, Known> consts;
            std::map<uint32_t, uint32_t> copies;
            bool operator==(const Facts&) const = default;
        };

        std::vector<uint8_t> lead = Leaders(m);
        std::vector<int> blockOf(m.ins.size(), -1);
        std::vector<std::pair<int, int>> blocks;
        for (size_t k = 0; k < m.ins.size(); ++k)
        {
            if (!m.ins[k].reachable) continue;
            if (lead[k] || blocks.empty() || blockOf[k - 1] < 0) blocks.push_back({ int(k), int(k) });
            blocks.back().second = int(k);
            blockOf[k] = int(blocks.size()) - 1;
        }

        std::vector<uint8_t> fresh(blocks.size(), 0);
        for (int e : m.entries) fresh[size_t(blockOf[size_t(e)])] = 1;
        for (int c : m.continuations) fresh[size_t(blockOf[size_t(c)])] = 1;
        for (int h : m.heapReturnTargets) fresh[size_t(blockOf[size_t(h)])] = 1;
        for (const Ins& i : m.ins)
            if (i.reachable && i.call) fresh[size_t(blockOf[size_t(i.target)])] = 1;

        std::vector<std::vector<int>> preds(blocks.size());
        for (size_t b = 0; b < blocks.size(); ++b)
        {
            const Ins& last = m.ins[size_t(blocks[b].second)];
            if (last.call || last.isReturn) continue;
            for (int s : last.successors)
                if (s >= 0 && size_t(s) < m.ins.size() && blockOf[size_t(s)] >= 0) preds[size_t(blockOf[size_t(s)])].push_back(int(b));
        }

        auto kill = [](Facts& f, uint32_t slot) {
            std::erase_if(f.exprs, [&](const Expr& e) { return e.result == slot || std::find(e.inputs.begin(), e.inputs.end(), slot) != e.inputs.end(); });
            f.consts.erase(slot);
            f.copies.erase(slot);
            std::erase_if(f.copies, [&](const auto& kv) { return kv.second == slot; });
        };
        auto known = [&](const Facts& f, uint32_t slot) -> std::optional<Known> {
            if (auto it = f.consts.find(slot); it != f.consts.end()) return it->second;
            if (m.IsConstant(slot)) return Known{ ir.program.heap[slot].type, ir.program.heap[slot].value };
            return std::nullopt;
        };
        auto step = [&](Facts& f, int k) {
            const Ins& i = m.ins[size_t(k)];
            if (i.op == OpCode::Extern && EffectOf(m, i) == Effect::Barrier)
            {
                f = {};
                return;
            }
            if (i.op == OpCode::Extern && EffectOf(m, i) == Effect::Write)
                std::erase_if(f.exprs, [](const Expr& e) { return Classify(e.signature) == Effect::Read; });

            std::optional<Known> copied;
            uint32_t copySource = 0;
            if (i.op == OpCode::Copy && !i.returnCopy)
            {
                copySource = Operands(i, 2)[0];
                copied = known(f, copySource);
                if (auto root = f.copies.find(copySource); root != f.copies.end()) copySource = root->second;
            }
            std::optional<Known> folded;
            if (i.op == OpCode::Extern && EffectOf(m, i) != Effect::Barrier && ResultOnly(*m.Extern(i)))
            {
                Signature sig;
                std::vector<uint32_t> ops = Operands(i, size_t(m.Extern(i)->arity));
                if (ParseSignature(m.Signature(i), sig) && sig.parameters.size() + 1 == ops.size())
                {
                    std::vector<Value> args;
                    for (size_t a = 0; a + 1 < ops.size(); ++a)
                        if (auto v = known(f, ops[a]); v && v->type == sig.parameters[a]) args.push_back(v->value);
                    if (args.size() + 1 == ops.size())
                        if (auto v = Fold(sig, args)) folded = Known{ sig.result, *v };
                }
            }

            for (uint32_t w : i.writes) kill(f, w);

            if (i.op == OpCode::Copy && !i.returnCopy)
            {
                uint32_t dst = Operands(i, 2)[1];
                if (copied && copied->type == TypeOf(ir, dst)) f.consts[dst] = *copied;
                else if (copySource != dst && TypeOf(ir, copySource) == TypeOf(ir, dst)) f.copies[dst] = copySource;
            }
            if (folded && !i.writes.empty()) f.consts[i.writes.back()] = *folded;
            if (Reusable(m, i, k))
            {
                std::vector<uint32_t> ops = Operands(i, size_t(m.Extern(i)->arity));
                uint32_t r = ops.back();
                ops.pop_back();
                if (std::find(ops.begin(), ops.end(), r) == ops.end()) f.exprs.push_back({ m.Signature(i), ops, r });
            }
        };
        auto transfer = [&](size_t b, Facts f) {
            for (int k = blocks[b].first; k <= blocks[b].second; ++k) step(f, k);
            return f;
        };

        std::vector<std::optional<Facts>> out(blocks.size());
        std::vector<Facts> in(blocks.size());
        for (bool moved = true; moved;)
        {
            moved = false;
            for (size_t b = 0; b < blocks.size(); ++b)
            {
                Facts entry;
                if (!fresh[b] && !preds[b].empty())
                {
                    bool first = true;
                    for (int p : preds[b])
                    {
                        if (!out[size_t(p)]) continue;
                        const Facts& o = *out[size_t(p)];
                        if (first)
                        {
                            entry = o;
                            first = false;
                            continue;
                        }
                        std::erase_if(entry.exprs, [&](const Expr& e) { return std::find(o.exprs.begin(), o.exprs.end(), e) == o.exprs.end(); });
                        std::erase_if(entry.consts, [&](const auto& kv) { auto it = o.consts.find(kv.first); return it == o.consts.end() || !(it->second == kv.second); });
                        std::erase_if(entry.copies, [&](const auto& kv) { auto it = o.copies.find(kv.first); return it == o.copies.end() || it->second != kv.second; });
                    }
                }
                in[b] = entry;
                Facts o = transfer(b, entry);
                if (!out[b] || *out[b] != o)
                {
                    out[b] = std::move(o);
                    moved = true;
                }
            }
        }

        bool changed = false;
        for (size_t b = 0; b < blocks.size(); ++b)
        {
            Facts f = in[b];
            for (int k = blocks[b].first; k <= blocks[b].second; ++k)
            {
                const Ins& i = m.ins[size_t(k)];

                size_t readCount = 0;
                if (i.op == OpCode::Copy && !i.returnCopy) readCount = 1;
                else if (i.op == OpCode::JumpIfFalse) readCount = 1;
                else if (i.op == OpCode::Extern)
                {
                    const ExternInfo* info = m.Extern(i);
                    readCount = info->hasResult ? size_t(info->arity) - 1 : size_t(info->arity);
                }
                for (size_t a = 0; a < readCount && a < i.producers.size(); ++a)
                {
                    if (i.op == OpCode::Extern && a < 64 && (m.Extern(i)->writes >> a) & 1) continue;
                    int p = i.producers[a];
                    if (!m.Editable(p) || m.ins[size_t(p)].consumers[0] != k) continue;
                    uint32_t slot = i.stackIn[i.stackIn.size() - i.producers.size() + a].slot;
                    if (auto c = f.consts.find(slot); c != f.consts.end() && c->second.type == TypeOf(ir, slot) && !m.IsConstant(slot))
                    {
                        ir.nodes[size_t(p)].operand = ir.Constant(c->second.type, c->second.value);
                        ++ctx.stats.propagated;
                        changed = true;
                    }
                    else if (auto cp = f.copies.find(slot); cp != f.copies.end())
                    {
                        ir.nodes[size_t(p)].operand = cp->second;
                        ++ctx.stats.propagated;
                        changed = true;
                    }
                }

                if (Reusable(m, i, k))
                {
                    std::vector<uint32_t> ops = Operands(i, size_t(m.Extern(i)->arity));
                    uint32_t r = ops.back();
                    ops.pop_back();
                    auto hit = std::find_if(f.exprs.begin(), f.exprs.end(), [&](const Expr& e) { return e.signature == m.Signature(i) && e.inputs == ops; });
                    if (hit != f.exprs.end())
                    {
                        uint32_t previous = hit->result;
                        Expr reuse = *hit;
                        if (previous == r) RemoveWithProducers(ir, i, k);
                        else ToCopy(ir, i, k, previous);
                        ++ctx.stats.checksRemoved;
                        changed = true;
                        kill(f, r);
                        if (previous != r)
                        {
                            reuse.result = r;
                            f.exprs.push_back(reuse);
                            if (TypeOf(ir, previous) == TypeOf(ir, r)) f.copies[r] = previous;
                        }
                        continue;
                    }
                }
                step(f, k);
            }
        }
        return changed;
    }

    bool Hoist(IR& ir, const Model& m, PassContext& ctx)
    {
        auto preds = Predecessors(m);
        std::vector<uint8_t> lead = Leaders(m);
        std::unordered_set<int> done;
        bool changed = false;

        for (size_t k = 0; k < m.ins.size(); ++k)
        {
            const Ins& latch = m.ins[k];
            if (!latch.reachable || latch.op != OpCode::Jump || latch.call || latch.heapCall || latch.end || latch.target < 0 || latch.target > int(k)) continue;
            int h = latch.target;
            if (done.contains(h) || h == 0) continue;

            std::vector<uint8_t> inLoop(m.ins.size(), 0);
            std::vector<int> work{ int(k) };
            inLoop[size_t(h)] = 1;
            inLoop[k] = 1;
            while (!work.empty())
            {
                int x = work.back();
                work.pop_back();
                if (x == h) continue;
                for (int p : preds[size_t(x)])
                    if (!inLoop[size_t(p)]) inLoop[size_t(p)] = 1, work.push_back(p);
            }

            int outside = 0;
            for (int p : preds[size_t(h)]) outside += inLoop[size_t(p)] ? 0 : 1;
            const Ins& before = m.ins[size_t(h) - 1];
            bool fallsIn = before.reachable && !inLoop[size_t(h) - 1] && before.op != OpCode::Jump && before.op != OpCode::JumpIfFalse &&
                           before.op != OpCode::JumpIndirect && !before.returnCopy;
            if (outside != 1 || !fallsIn || std::find(m.entries.begin(), m.entries.end(), h) != m.entries.end()) continue;
            bool single = true;
            for (size_t x = 0; x < m.ins.size() && single; ++x)
            {
                if (!inLoop[x] || int(x) == h) continue;
                for (int p : preds[x]) single &= inLoop[size_t(p)] != 0;
                single &= std::find(m.entries.begin(), m.entries.end(), int(x)) == m.entries.end();
            }
            if (!single) continue;

            bool barrier = false, writer = false;
            std::unordered_set<uint32_t> written;
            for (size_t x = 0; x < m.ins.size(); ++x)
            {
                if (!inLoop[x]) continue;
                const Ins& i = m.ins[x];
                if (i.op == OpCode::Extern && EffectOf(m, i) == Effect::Barrier) barrier = true;
                if (i.op == OpCode::Extern && EffectOf(m, i) == Effect::Write) writer = true;
                if (i.call || i.op == OpCode::JumpIndirect) barrier = true;
                for (uint32_t w : i.writes) written.insert(w);
            }
            if (barrier) continue;

            int e = h;
            while (e < int(m.ins.size()) && m.ins[size_t(e)].op == OpCode::Push && (e == h || !lead[size_t(e)])) ++e;
            if (e >= int(m.ins.size()) || (lead[size_t(e)] && e != h)) continue;
            const Ins& ext = m.ins[size_t(e)];
            if (!Reusable(m, ext, e) || int(ext.producers.size()) != e - h || (writer && EffectOf(m, ext) == Effect::Read)) continue;
            bool contiguous = true;
            for (size_t a = 0; a < ext.producers.size(); ++a) contiguous &= ext.producers[a] == h + int(a);
            if (!contiguous) continue;

            std::vector<uint32_t> ops = Operands(ext, ext.producers.size());
            uint32_t result = ops.back();
            bool invariant = true;
            for (size_t a = 0; a + 1 < ops.size(); ++a) invariant &= !written.contains(ops[a]);
            for (size_t x = 0; x < m.ins.size() && invariant; ++x)
                if (inLoop[x] && int(x) != e)
                    for (uint32_t w : m.ins[x].writes) invariant &= w != result;
            if (!invariant) continue;

            std::vector<int> moved;
            for (int x = h; x <= e; ++x)
            {
                moved.push_back(Clone(ir, x));
                ir.Remove(x);
            }
            ir.InsertBefore(h, moved);
            done.insert(h);
            ++ctx.stats.hoisted;
            changed = true;
        }
        return changed;
    }

    bool Loops(IR& ir, const Model& m, PassContext& ctx)
    {
        Liveness live(m);
        std::vector<uint8_t> lead = Leaders(m);
        bool changed = false;
        int size = ir.Live();
        std::unordered_set<int> touched;

        for (size_t k = 0; k < m.ins.size(); ++k)
        {
            const Ins& latch = m.ins[k];
            if (!latch.reachable || latch.op != OpCode::Jump || latch.call || latch.heapCall || latch.end || latch.target < 0 || latch.target > int(k)) continue;
            int h = latch.target;

            int j = h;
            while (j < int(m.ins.size()) && m.ins[size_t(j)].op != OpCode::JumpIfFalse && j - h < 16)
            {
                const Ins& x = m.ins[size_t(j)];
                if (x.op == OpCode::Jump || x.op == OpCode::JumpIndirect || x.returnCopy || (j != h && lead[size_t(j)])) break;
                ++j;
            }
            if (j >= int(m.ins.size()) || m.ins[size_t(j)].op != OpCode::JumpIfFalse || (j != h && lead[size_t(j)]) || j - h < 5) continue;
            const Ins& branch = m.ins[size_t(j)];
            if (branch.end || (branch.target <= int(k) && branch.target >= h)) continue;
            const Ins& cmp = m.ins[size_t(j) - 2];
            const Ins& push = m.ins[size_t(j) - 1];
            if (cmp.op != OpCode::Extern || push.op != OpCode::Push || branch.producers[0] != j - 1 || !m.Editable(j - 1)) continue;
            if (!ResultOnly(*m.Extern(cmp)) || cmp.writes.empty() || cmp.writes.back() != push.operand) continue;
            uint32_t c = push.operand;
            if (!m.IsPrivate(c) || live.LiveAfter(j, c)) continue;
            Signature sig;
            if (!ParseSignature(m.Signature(cmp), sig)) continue;
            auto inverse = InvertedCompare(sig);
            if (!inverse) continue;
            int length = j - h + 1;
            if (size + length > ctx.growthLimit || touched.contains(int(k))) continue;

            uint32_t inverted = ir.ExternSlot(*inverse, *m.Extern(cmp));
            std::vector<int> copy;
            for (int x = h; x <= j; ++x)
            {
                int id = Clone(ir, x);
                if (x == j - 2) ir.nodes[size_t(id)].operand = inverted;
                if (x == j) ir.nodes[size_t(id)].target = j + 1;
                copy.push_back(id);
            }
            if (branch.target != int(k) + 1) copy.push_back(ir.Add(OpCode::Jump, 0, branch.target));
            ir.Remove(int(k));
            ir.InsertAfter(int(k), copy);
            touched.insert(int(k));
            size += length;
            ++ctx.stats.loopsRotated;
            changed = true;
        }
        return changed;
    }

    bool Switches(IR& ir, const Model& m, PassContext& ctx)
    {
        constexpr std::string_view Equal = "SystemInt32.__op_Equality__SystemInt32_SystemInt32__SystemBoolean";
        Liveness live(m);

        struct Case
        {
            int32_t key;
            int body;
        };
        auto match = [&](int s, uint32_t& x, int32_t& key, int& next) {
            if (s < 0 || size_t(s) + 6 > m.ins.size()) return false;
            const Ins& cmp = m.ins[size_t(s) + 3];
            const Ins& branch = m.ins[size_t(s) + 5];
            if (!cmp.reachable || cmp.op != OpCode::Extern || m.Signature(cmp) != Equal || branch.op != OpCode::JumpIfFalse || branch.end) return false;
            if (m.ins[size_t(s) + 4].op != OpCode::Push || branch.producers[0] != s + 4) return false;
            if (cmp.producers.size() != 3 || cmp.producers[0] != s || cmp.producers[1] != s + 1 || cmp.producers[2] != s + 2) return false;
            for (int p = s; p <= s + 2; ++p)
                if (!m.Editable(p)) return false;
            if (!m.Editable(s + 4)) return false;
            std::vector<uint32_t> ops = Operands(cmp, 3);
            uint32_t t = ops[2];
            if (m.ins[size_t(s) + 4].operand != t || !m.IsPrivate(t) || live.LiveAfter(s + 5, t)) return false;
            uint32_t a = ops[0], b = ops[1];
            uint32_t k = m.IsConstant(b) ? b : (m.IsConstant(a) ? a : UINT32_MAX);
            if (k == UINT32_MAX || ir.program.heap[k].type != "SystemInt32") return false;
            x = k == b ? a : b;
            if (m.IsConstant(x)) return false;
            key = int32_t(ir.program.heap[k].value.integer);
            next = branch.target;
            return true;
        };

        std::unordered_set<int> used;
        bool changed = false;
        for (size_t s = 0; s < m.ins.size(); ++s)
        {
            uint32_t x;
            int32_t key;
            int next;
            if (used.contains(int(s)) || !match(int(s), x, key, next)) continue;

            std::vector<Case> cases{ { key, int(s) + 6 } };
            std::vector<int> chain{ int(s) };
            int fallback = next;
            for (;;)
            {
                uint32_t x2;
                int32_t k2;
                int n2;
                if (std::find(chain.begin(), chain.end(), fallback) != chain.end() || !match(fallback, x2, k2, n2) || x2 != x) break;
                if (std::none_of(cases.begin(), cases.end(), [&](const Case& c) { return c.key == k2; })) cases.push_back({ k2, fallback + 6 });
                chain.push_back(fallback);
                fallback = n2;
            }
            for (int c : chain) used.insert(c);
            if (cases.size() < 6) continue;

            int32_t low = cases[0].key, high = cases[0].key;
            for (const Case& c : cases) low = std::min(low, c.key), high = std::max(high, c.key);
            int64_t span = int64_t(high) - low + 1;
            if (span > int64_t(cases.size()) * 4 || span > 256) continue;

            Value tableValue;
            tableValue.kind = ValueKind::UIntArray;
            tableValue.array.assign(size_t(span), 0);
            uint32_t table = ir.NewSlot("SystemUInt32Array", tableValue);
            std::vector<int> targets(size_t(span), fallback);
            for (const Case& c : cases) targets[size_t(int64_t(c.key) - low)] = c.body;
            ir.switchCarriers[table] = targets;

            ExternInfo binary{ 3, true, 1ull << 2 };
            uint32_t lt = ir.ExternSlot("SystemInt32.__op_LessThan__SystemInt32_SystemInt32__SystemBoolean", binary);
            uint32_t gt = ir.ExternSlot("SystemInt32.__op_GreaterThan__SystemInt32_SystemInt32__SystemBoolean", binary);
            uint32_t sub = ir.ExternSlot("SystemInt32.__op_Subtraction__SystemInt32_SystemInt32__SystemInt32", binary);
            uint32_t get = ir.ExternSlot("SystemUInt32Array.__Get__SystemInt32__SystemUInt32", binary);
            uint32_t flag = ir.NewSlot("SystemBoolean", B(false));
            uint32_t index = ir.NewSlot("SystemInt32", I(0));
            uint32_t target = ir.NewSlot("SystemUInt32", U(0));
            uint32_t lowSlot = ir.Constant("SystemInt32", I(low));
            uint32_t highSlot = ir.Constant("SystemInt32", I(high));

            std::vector<int> code;
            auto add = [&](OpCode op, uint32_t operand = 0, int jumpTarget = -1) {
                code.push_back(ir.Add(op, operand, jumpTarget));
                return code.back();
            };
            for (auto [bound, ext] : { std::pair{ lowSlot, lt }, std::pair{ highSlot, gt } })
            {
                add(OpCode::Push, x);
                add(OpCode::Push, bound);
                add(OpCode::Push, flag);
                add(OpCode::Extern, ext);
                add(OpCode::Push, flag);
                int branch = add(OpCode::JumpIfFalse);
                add(OpCode::Jump, 0, fallback);
                ir.nodes[size_t(branch)].target = -2 - int(code.size());
            }
            uint32_t indexSlot = x;
            if (low != 0)
            {
                add(OpCode::Push, x);
                add(OpCode::Push, lowSlot);
                add(OpCode::Push, index);
                add(OpCode::Extern, sub);
                indexSlot = index;
            }
            add(OpCode::Push, table);
            add(OpCode::Push, indexSlot);
            add(OpCode::Push, target);
            add(OpCode::Extern, get);
            add(OpCode::JumpIndirect, target);
            for (size_t c = 0; c < code.size(); ++c)
                if (ir.nodes[size_t(code[c])].target <= -2) ir.nodes[size_t(code[c])].target = code[size_t(-2 - ir.nodes[size_t(code[c])].target)];

            for (int p = int(s); p <= int(s) + 5; ++p) ir.Remove(p);
            ir.InsertAfter(int(s) + 5, code);
            ++ctx.stats.switchesBuilt;
            changed = true;
        }
        return changed;
    }

    bool Concat(IR& ir, const Model& m, PassContext& ctx)
    {
        auto isConcat = [](const std::string& sig, size_t& count) {
            if (sig == "SystemString.__op_Addition__SystemString_SystemString__SystemString" || sig == "SystemString.__Concat__SystemString_SystemString__SystemString")
                return count = 2, true;
            if (sig == "SystemString.__Concat__SystemString_SystemString_SystemString__SystemString") return count = 3, true;
            return false;
        };
        auto signature = [](size_t count) {
            std::string s = "SystemString.__Concat__";
            for (size_t i = 0; i < count; ++i) s += i ? "_SystemString" : "SystemString";
            return s + "__SystemString";
        };

        Liveness live(m);
        std::vector<uint8_t> lead = Leaders(m);
        Access access;
        std::unordered_map<uint32_t, int> def;
        std::unordered_set<uint32_t> touched;
        std::unordered_set<int> consumed;
        bool changed = false;

        for (size_t k = 0; k < m.ins.size(); ++k)
        {
            const Ins& i = m.ins[k];
            if (lead[k])
            {
                access.Reset();
                def.clear();
            }
            if (!i.reachable) continue;
            int self = int(k);
            size_t outer = 0;
            if (i.op == OpCode::Extern && isConcat(m.Signature(i), outer) && outer == 2 && AllEditable(m, i, self) && !consumed.contains(self))
            {
                std::vector<uint32_t> ops = Operands(i, 3);
                for (size_t slotIndex = 0; slotIndex < 2; ++slotIndex)
                {
                    uint32_t t = ops[slotIndex];
                    auto d = def.find(t);
                    if (d == def.end() || consumed.contains(d->second) || touched.contains(t) || !m.IsPrivate(t) || live.LiveAfter(self, t)) continue;
                    if (ops[0] == ops[1] || ops[2] == t) continue;
                    const Ins& inner = m.ins[size_t(d->second)];
                    size_t count = 0;
                    if (!isConcat(m.Signature(inner), count) || count + 1 > 4) continue;
                    std::vector<uint32_t> innerOps = Operands(inner, count + 1);
                    bool quiet = access.Quiet(t, d->second) && access.barrier <= d->second;
                    for (size_t a = 0; a < count; ++a) quiet &= access.Quiet(innerOps[a], d->second);
                    if (!quiet) continue;

                    int slotPush = i.producers[slotIndex];
                    ir.nodes[size_t(slotPush)].operand = innerOps[0];
                    std::vector<int> extra;
                    for (size_t a = 1; a < count; ++a)
                    {
                        int id = Clone(ir, inner.producers[a]);
                        extra.push_back(id);
                    }
                    ir.InsertAfter(slotPush, extra);
                    ExternInfo info{ int(count) + 2, true, 1ull << (count + 1) };
                    ir.nodes[k].operand = ir.ExternSlot(signature(count + 1), info);
                    RemoveWithProducers(ir, inner, d->second);
                    consumed.insert(d->second);
                    consumed.insert(self);
                    touched.insert(t);
                    ++ctx.stats.concatsMerged;
                    changed = true;
                    break;
                }
            }

            access.Touch(i, self, m);
            if (i.op == OpCode::Extern && EffectOf(m, i) == Effect::Barrier) def.clear();
            for (uint32_t w : i.writes) def.erase(w);
            if (i.op == OpCode::Extern && ResultOnly(*m.Extern(i)) && AllEditable(m, i, self) && !i.writes.empty()) def[i.writes.back()] = self;
        }
        return changed;
    }

    std::optional<Program> CompactHeap(const Program& p, const Model& m, Stats& stats)
    {
        size_t n = p.heap.size();
        std::vector<uint8_t> referenced(n, 0), indirect(n, 0);
        for (const Ins& i : m.ins)
        {
            if (!HasOperand(i.op) || i.op == OpCode::Jump || i.op == OpCode::JumpIfFalse) continue;
            referenced[i.operand] = 1;
            if (i.op == OpCode::JumpIndirect) indirect[i.operand] = 1;
        }

        std::vector<uint32_t> rep(n);
        for (size_t s = 0; s < n; ++s) rep[s] = uint32_t(s);

        std::map<std::pair<std::string, std::vector<uint64_t>>, uint32_t> constants;
        for (size_t s = 0; s < n; ++s)
        {
            const Slot& slot = p.heap[s];
            if (!referenced[s] || !m.IsConstant(uint32_t(s))) continue;
            const Value& v = slot.value;
            if (v.kind != ValueKind::Boolean && v.kind != ValueKind::Integer && v.kind != ValueKind::Unsigned && v.kind != ValueKind::Real) continue;
            std::vector<uint64_t> key = { uint64_t(v.kind), v.boolean, uint64_t(v.integer), v.unsignedInteger, std::bit_cast<uint64_t>(v.real) };
            auto [it, fresh] = constants.emplace(std::pair{ slot.type, key }, uint32_t(s));
            if (!fresh) rep[s] = it->second;
        }

        Liveness live(m);
        std::vector<uint32_t> slotOf;
        for (size_t s = 0; s < n; ++s)
            if (live.dense[s] >= 0) slotOf.push_back(uint32_t(s));
        size_t count = slotOf.size();
        auto candidate = [&](uint32_t s) {
            return live.dense[s] >= 0 && referenced[s] && !indirect[s] && m.written[s] && m.roles[s] == Role::None;
        };

        std::vector<Bits> clash(count, Bits(live.words, 0));
        auto conflict = [&](uint32_t a, uint32_t b) {
            Set(clash[size_t(live.dense[a])], uint32_t(live.dense[b]));
            Set(clash[size_t(live.dense[b])], uint32_t(live.dense[a]));
        };
        std::vector<uint8_t> entryLive(n, 0);
        for (size_t k = 0; k < m.ins.size(); ++k)
        {
            const Ins& i = m.ins[k];
            if (!i.reachable) continue;
            for (uint32_t d : i.writes)
            {
                if (!candidate(d)) continue;
                const Bits& out = live.out[k];
                for (size_t w = 0; w < out.size(); ++w)
                    for (uint64_t bits = out[w]; bits; bits &= bits - 1)
                    {
                        size_t j = w * 64 + size_t(std::countr_zero(bits));
                        if (slotOf[j] != d) conflict(d, slotOf[j]);
                    }
            }
        }
        for (int e : m.entries)
        {
            std::vector<uint32_t> at;
            const Bits& out = live.out[size_t(e)];
            for (size_t w = 0; w < out.size(); ++w)
                for (uint64_t bits = out[w]; bits; bits &= bits - 1) at.push_back(slotOf[w * 64 + size_t(std::countr_zero(bits))]);
            for (uint32_t r : m.ins[size_t(e)].reads)
                if (live.dense[r] >= 0) at.push_back(r);
            for (uint32_t a : at)
            {
                entryLive[a] = 1;
                for (uint32_t b : at)
                    if (a != b) conflict(a, b);
            }
        }

        std::map<std::string, std::vector<std::vector<uint32_t>>> groups;
        for (uint32_t s : slotOf)
        {
            if (!candidate(s)) continue;
            auto& list = groups[p.heap[s].type];
            bool placed = false;
            for (auto& g : list)
            {
                bool fits = true;
                for (uint32_t t : g) fits &= !Test(clash[size_t(live.dense[s])], uint32_t(live.dense[t]));
                if (!fits) continue;
                g.push_back(s);
                placed = true;
                break;
            }
            if (!placed) list.push_back({ s });
        }
        for (auto& [type, list] : groups)
            for (auto& g : list)
            {
                uint32_t leader = g.front();
                for (uint32_t s : g)
                    if (entryLive[s]) leader = s;
                for (uint32_t s : g) rep[s] = leader;
            }

        std::vector<uint32_t> index(n, UINT32_MAX);
        Program out = p;
        out.heap.clear();
        for (size_t s = 0; s < n; ++s)
        {
            bool keep = rep[s] == s && (referenced[s] || !p.heap[s].Is(SlotPrivate));
            if (!keep) continue;
            index[s] = uint32_t(out.heap.size());
            out.heap.push_back(p.heap[s]);
        }
        if (out.heap.size() == n) return std::nullopt;

        for (size_t pc = 0; pc < out.code.size();)
        {
            auto op = static_cast<OpCode>(out.code[pc]);
            if (HasOperand(op) && op != OpCode::Jump && op != OpCode::JumpIfFalse)
            {
                uint32_t target = index[rep[out.code[pc + 1]]];
                if (target == UINT32_MAX) return std::nullopt;
                out.code[pc + 1] = target;
            }
            pc += HasOperand(op) ? 2 : 1;
        }
        stats.heapBefore = int(n);
        stats.heapAfter = int(out.heap.size());
        return out;
    }

    bool IsFrameEvent(std::string_view name)
    {
        return name == "_update" || name == "_lateUpdate" || name == "_fixedUpdate" || name == "_postLateUpdate";
    }

    bool Events(IR& ir, const Model& m, PassContext& ctx)
    {
        bool changed = false;
        for (size_t e = ir.program.entries.size(); e-- > 0;)
        {
            if (!IsFrameEvent(ir.program.entries[e].name)) continue;
            int k = m.entries[e];
            if (m.ins[size_t(k)].sentinel) ++k;
            bool empty = (m.ins[size_t(k)].op == OpCode::Jump && m.ins[size_t(k)].end) || IsTrampoline(m, k);
            if (!empty) continue;
            ir.program.entries.erase(ir.program.entries.begin() + std::ptrdiff_t(e));
            ir.entries.erase(ir.entries.begin() + std::ptrdiff_t(e));
            ++ctx.stats.eventsRemoved;
            changed = true;
        }
        return changed;
    }

    bool Memo(IR& ir, const Model& m, PassContext& ctx)
    {
        constexpr std::string_view LocalPlayer = "VRCSDKBaseNetworking.__get_LocalPlayer__VRCSDKBaseVRCPlayerApi";
        constexpr std::string_view PlayerFacts[] = {
            "VRCSDKBaseVRCPlayerApi.__get_playerId__SystemInt32",
            "VRCSDKBaseVRCPlayerApi.__get_displayName__SystemString",
            "VRCSDKBaseVRCPlayerApi.__get_isLocal__SystemBoolean",
            "VRCSDKBaseVRCPlayerApi.__IsUserInVR__SystemBoolean",
        };

        struct Cache
        {
            uint32_t value;
            uint32_t flag;
        };
        std::map<std::string, Cache> caches;
        std::vector<uint8_t> lead = Leaders(m);
        std::unordered_set<uint32_t> local;
        bool changed = false;

        for (size_t k = 0; k < m.ins.size(); ++k)
        {
            const Ins& i = m.ins[k];
            if (lead[k]) local.clear();
            if (!i.reachable) continue;

            if (i.op == OpCode::Extern && k + 1 < m.ins.size())
            {
                const std::string& sig = m.Signature(i);
                bool isLocalPlayer = sig == LocalPlayer;
                bool isFact = std::find(std::begin(PlayerFacts), std::end(PlayerFacts), sig) != std::end(PlayerFacts);
                std::vector<uint32_t> ops = Operands(i, i.producers.size());
                bool contiguous = AllEditable(m, i, int(k));
                for (size_t a = 0; a < i.producers.size(); ++a) contiguous &= i.producers[a] == int(k - i.producers.size() + a);
                bool stableInput = isLocalPlayer || (isFact && ops.size() == 2 && local.contains(ops[0]));
                int before = i.producers.empty() ? -1 : i.producers.front() - 1;
                bool wrapped = before >= 0 && m.ins[size_t(before)].op == OpCode::Jump && m.ins[size_t(before)].target > int(k) &&
                               m.ins[size_t(before)].target <= int(k) + 8;
                if (wrapped)
                {
                    for (uint32_t w : i.writes) local.erase(w);
                    if (isLocalPlayer) local.insert(ops.back());
                    continue;
                }
                if ((isLocalPlayer || isFact) && stableInput && contiguous && ResultOnly(*m.Extern(i)))
                {
                    uint32_t r = ops.back();
                    Signature parsed;
                    ParseSignature(sig, parsed);
                    std::string key = isLocalPlayer ? sig : sig + "@local";
                    auto it = caches.find(key);
                    if (it == caches.end())
                        it = caches.emplace(key, Cache{ ir.NewSlot(parsed.result, Value{}), ir.NewSlot("SystemBoolean", B(false)) }).first;
                    Cache c = it->second;

                    int first = i.producers.front();
                    int miss = Clone(ir, first);
                    std::vector<int> head = {
                        ir.Add(OpCode::JumpIfFalse, 0, miss), ir.Add(OpCode::Push, c.value), ir.Add(OpCode::Push, r),
                        ir.Add(OpCode::Copy),                 ir.Add(OpCode::Jump, 0, int(k) + 1), miss,
                    };
                    ir.nodes[size_t(first)] = { OpCode::Push, c.flag, -1, false };
                    std::vector<int> tail = { ir.Add(OpCode::Push, r), ir.Add(OpCode::Push, c.value), ir.Add(OpCode::Copy) };
                    if (isLocalPlayer)
                    {
                        uint32_t valid = ir.ExternSlot("VRCSDKBaseUtilities.__IsValid__SystemObject__SystemBoolean", ExternInfo{ 2, true, 1ull << 1 });
                        tail.push_back(ir.Add(OpCode::Push, r));
                        tail.push_back(ir.Add(OpCode::Push, c.flag));
                        tail.push_back(ir.Add(OpCode::Extern, valid));
                    }
                    else
                    {
                        tail.push_back(ir.Add(OpCode::Push, ir.Constant("SystemBoolean", B(true))));
                        tail.push_back(ir.Add(OpCode::Push, c.flag));
                        tail.push_back(ir.Add(OpCode::Copy));
                    }
                    for (int id : head) ir.origin[size_t(id)] = ir.origin[k];
                    for (int id : tail) ir.origin[size_t(id)] = ir.origin[k];
                    ir.InsertAfter(first, head);
                    ir.InsertAfter(int(k), tail);
                    ++ctx.stats.memoized;
                    changed = true;
                    for (uint32_t w : i.writes) local.erase(w);
                    if (isLocalPlayer) local.insert(r);
                    continue;
                }
            }
            for (uint32_t w : i.writes) local.erase(w);
            if (i.op == OpCode::Extern && EffectOf(m, i) == Effect::Barrier) local.clear();
        }
        return changed;
    }

    namespace
    {
        struct Pattern
        {
            std::string signature;
            std::vector<int> args;
            std::vector<Pattern> subs;
        };

        Pattern Leaf(std::string sig, std::vector<int> args) { return { std::move(sig), std::move(args), {} }; }

        struct Rule
        {
            Pattern pattern;
            std::string replacement;
            std::vector<int> order;
            bool sameAB = false;
        };

        std::vector<Rule> BuildRules()
        {
            std::vector<Rule> rules;
            const std::string F = "SystemSingle.__";
            auto fop = [&](const char* op) { return F + op + "__SystemSingle_SystemSingle__SystemSingle"; };
            enum { A = 1, B = 2, S = 3 };
            auto var = [](int v) { return -v; };

            for (int dims : { 2, 3 })
            {
                std::string V = dims == 2 ? "UnityEngineVector2" : "UnityEngineVector3";
                const char* comps[] = { "x", "y", "z" };
                auto get = [&](int c, int v) { return Leaf(V + ".__get_" + comps[c] + "__SystemSingle", { var(v) }); };
                std::string ctor = V + (dims == 2 ? ".__ctor__SystemSingle_SystemSingle__" : ".__ctor__SystemSingle_SystemSingle_SystemSingle__") + V;

                for (const char* op : { "op_Addition", "op_Subtraction" })
                {
                    Pattern p{ ctor, {}, {} };
                    for (int c = 0; c < dims; ++c)
                    {
                        Pattern comp{ fop(op), { 0, 0 }, { get(c, A), get(c, B) } };
                        p.args.push_back(0);
                        p.subs.push_back(comp);
                    }
                    rules.push_back({ p, V + ".__" + op + "__" + V + "_" + V + "__" + V, { A, B } });
                }
                for (const char* op : { "op_Multiply", "op_Division" })
                {
                    Pattern p{ ctor, {}, {} };
                    for (int c = 0; c < dims; ++c)
                    {
                        p.args.push_back(0);
                        p.subs.push_back({ fop(op), { 0, var(S) }, { get(c, A) } });
                    }
                    rules.push_back({ p, V + ".__" + op + "__" + V + "_SystemSingle__" + V, { A, S } });
                }
                {
                    Pattern p{ ctor, {}, {} };
                    for (int c = 0; c < dims; ++c)
                    {
                        p.args.push_back(0);
                        p.subs.push_back({ fop("op_Multiply"), { var(S), 0 }, { get(c, A) } });
                    }
                    rules.push_back({ p, V + ".__op_Multiply__" + V + "_SystemSingle__" + V, { A, S } });
                }

                auto product = [&](int c) { return Pattern{ fop("op_Multiply"), { 0, 0 }, { get(c, A), get(c, B) } }; };
                Pattern dot = dims == 2 ? Pattern{ fop("op_Addition"), { 0, 0 }, { product(0), product(1) } }
                                        : Pattern{ fop("op_Addition"), { 0, 0 }, { Pattern{ fop("op_Addition"), { 0, 0 }, { product(0), product(1) } }, product(2) } };
                rules.push_back({ dot, V + ".__get_sqrMagnitude__SystemSingle", { A }, true });
                rules.push_back({ dot, V + ".__Dot__" + V + "_" + V + "__SystemSingle", { A, B } });

                rules.push_back({ Pattern{ "UnityEngineMathf.__Sqrt__SystemSingle__SystemSingle", { 0 }, { Leaf(V + ".__get_sqrMagnitude__SystemSingle", { var(A) }) } },
                                  V + ".__get_magnitude__SystemSingle", { A } });
                rules.push_back({ Pattern{ V + ".__get_magnitude__SystemSingle", { 0 }, { Leaf(V + ".__op_Subtraction__" + V + "_" + V + "__" + V, { var(A), var(B) }) } },
                                  V + ".__Distance__" + V + "_" + V + "__SystemSingle", { A, B } });
                Pattern lerp{ V + ".__op_Addition__" + V + "_" + V + "__" + V, { var(A), 0 },
                              { Pattern{ V + ".__op_Multiply__" + V + "_SystemSingle__" + V, { 0, var(S) },
                                         { Leaf(V + ".__op_Subtraction__" + V + "_" + V + "__" + V, { var(B), var(A) }) } } } };
                rules.push_back({ lerp, V + ".__LerpUnclamped__" + V + "_" + V + "_SystemSingle__" + V, { A, B, S } });
            }
            Pattern lerp{ fop("op_Addition"), { var(A), 0 }, { Pattern{ fop("op_Multiply"), { 0, var(S) }, { Leaf(fop("op_Subtraction"), { var(B), var(A) }) } } } };
            rules.push_back({ lerp, "UnityEngineMathf.__LerpUnclamped__SystemSingle_SystemSingle_SystemSingle__SystemSingle", { A, B, S } });
            return rules;
        }
    }

    bool Fusion(IR& ir, const Model& m, PassContext& ctx)
    {
        static const std::vector<Rule> rules = BuildRules();
        Liveness live(m);
        std::vector<uint8_t> lead = Leaders(m);
        std::unordered_map<uint32_t, int> def;
        std::unordered_set<int> consumed;
        int blockStart = 0;
        bool changed = false;

        for (size_t k = 0; k < m.ins.size(); ++k)
        {
            const Ins& root = m.ins[k];
            if (lead[k])
            {
                def.clear();
                blockStart = int(k);
            }
            if (!root.reachable) continue;

            if (root.op == OpCode::Extern && !consumed.contains(int(k)) && ResultOnly(*m.Extern(root)) && AllEditable(m, root, int(k)))
            {
                for (const Rule& rule : rules)
                {
                    std::map<int, uint32_t> bound;
                    std::vector<int> inner;
                    std::map<uint32_t, int> uses;
                    auto match = [&](auto& self, const Pattern& p, int at) -> bool {
                        const Ins& e = m.ins[size_t(at)];
                        if (e.op != OpCode::Extern || m.Signature(e) != p.signature || !ResultOnly(*m.Extern(e))) return false;
                        std::vector<uint32_t> ops = Operands(e, size_t(m.Extern(e)->arity));
                        if (ops.size() != p.args.size() + 1) return false;
                        size_t sub = 0;
                        for (size_t a = 0; a < p.args.size(); ++a)
                        {
                            uint32_t slot = ops[a];
                            if (p.args[a] < 0)
                            {
                                auto [it, fresh] = bound.emplace(-p.args[a], slot);
                                if (!fresh && it->second != slot) return false;
                                continue;
                            }
                            auto d = def.find(slot);
                            if (d == def.end() || consumed.contains(d->second) || !m.IsPrivate(slot)) return false;
                            ++uses[slot];
                            if (std::find(inner.begin(), inner.end(), d->second) == inner.end())
                            {
                                if (!self(self, p.subs[sub], d->second)) return false;
                                inner.push_back(d->second);
                            }
                            else if (m.ins[size_t(d->second)].op != OpCode::Extern || m.Signature(m.ins[size_t(d->second)]) != p.subs[sub].signature)
                                return false;
                            ++sub;
                        }
                        return true;
                    };
                    if (!match(match, rule.pattern, int(k))) continue;
                    if (rule.sameAB != (bound.contains(1) && bound.contains(2) && bound[1] == bound[2])) continue;

                    int earliest = int(k);
                    for (int e : inner) earliest = std::min(earliest, e);
                    if (earliest < blockStart) continue;
                    bool ok = true;
                    for (int x = earliest; x < int(k) && ok; ++x)
                    {
                        const Ins& mid = m.ins[size_t(x)];
                        if (mid.op == OpCode::Extern && EffectOf(m, mid) == Effect::Barrier) ok = false;
                        for (uint32_t w : mid.writes)
                            for (auto& [v, slot] : bound) ok &= slot != w;
                    }
                    for (int e : inner)
                    {
                        const Ins& mid = m.ins[size_t(e)];
                        uint32_t t = mid.writes.back();
                        ok &= AllEditable(m, mid, e) && !live.LiveAfter(int(k), t);
                        int expected = 0;
                        for (int f : inner)
                            for (uint32_t r : Inputs(m, m.ins[size_t(f)])) expected += r == t ? 1 : 0;
                        for (uint32_t r : Inputs(m, root)) expected += r == t ? 1 : 0;
                        int seen = 0;
                        for (int x = e + 1; x <= int(k); ++x)
                            for (uint32_t r : m.ins[size_t(x)].reads) seen += r == t ? 1 : 0;
                        ok &= seen == expected;
                    }
                    if (!ok) continue;

                    std::vector<uint32_t> inputs;
                    for (int v : rule.order) inputs.push_back(bound[v]);
                    size_t oldInputs = root.producers.size() - 1;
                    for (size_t a = 0; a < std::min(oldInputs, inputs.size()); ++a) ir.nodes[size_t(root.producers[a])].operand = inputs[a];
                    for (size_t a = inputs.size(); a < oldInputs; ++a) ir.Remove(root.producers[a]);
                    std::vector<int> extra;
                    for (size_t a = oldInputs; a < inputs.size(); ++a) extra.push_back(ir.Add(OpCode::Push, inputs[a]));
                    if (!extra.empty()) ir.InsertBefore(root.producers.back(), extra);
                    ir.nodes[k].operand = ir.ExternSlot(rule.replacement, ExternInfo{ int(inputs.size()) + 1, true, 1ull << inputs.size() });
                    for (int e : inner)
                    {
                        RemoveWithProducers(ir, m.ins[size_t(e)], e);
                        consumed.insert(e);
                    }
                    consumed.insert(int(k));
                    ++ctx.stats.fused;
                    changed = true;
                    break;
                }
            }

            for (uint32_t w : root.writes) def.erase(w);
            if (root.op == OpCode::Extern && EffectOf(m, root) == Effect::Barrier) def.clear();
            if (root.op == OpCode::Extern && EffectOf(m, root) != Effect::Barrier && ResultOnly(*m.Extern(root)) && !root.writes.empty())
                def[root.writes.back()] = int(k);
        }
        return changed;
    }

    bool Unroll(IR& ir, const Model& m, PassContext& ctx)
    {
        constexpr std::string_view Less = "SystemInt32.__op_LessThan__SystemInt32_SystemInt32__SystemBoolean";
        constexpr std::string_view LessEqual = "SystemInt32.__op_LessThanOrEqual__SystemInt32_SystemInt32__SystemBoolean";
        constexpr std::string_view Add = "SystemInt32.__op_Addition__SystemInt32_SystemInt32__SystemInt32";
        Liveness live(m);
        auto preds = Predecessors(m);
        bool changed = false;
        int size = ir.Live();
        std::unordered_set<int> used;

        auto constant = [&](uint32_t slot, int64_t& v) {
            if (!m.IsConstant(slot) || ir.program.heap[slot].type != "SystemInt32") return false;
            v = ir.program.heap[slot].value.integer;
            return true;
        };

        for (size_t k = 0; k < m.ins.size(); ++k)
        {
            const Ins& latch = m.ins[k];
            if (!latch.reachable || latch.op != OpCode::Jump || latch.call || latch.heapCall || latch.end || latch.target < 3 || latch.target >= int(k)) continue;
            int h = latch.target;
            if (used.contains(h) || int(k) - h < 11) continue;

            const Ins& cmp = m.ins[size_t(h) + 3];
            const Ins& branch = m.ins[size_t(h) + 5];
            if (cmp.op != OpCode::Extern || (m.Signature(cmp) != Less && m.Signature(cmp) != LessEqual) || branch.op != OpCode::JumpIfFalse || branch.end) continue;
            bool shaped = cmp.producers.size() == 3 && cmp.producers[0] == h && cmp.producers[1] == h + 1 && cmp.producers[2] == h + 2 &&
                          m.ins[size_t(h) + 4].op == OpCode::Push && branch.producers[0] == h + 4;
            if (!shaped) continue;
            std::vector<uint32_t> ops = Operands(cmp, 3);
            uint32_t i = ops[0], c = ops[2];
            int64_t limit = 0, start = 0, step = 0;
            if (!constant(ops[1], limit) || !m.IsPrivate(i) || !m.IsPrivate(c) || live.LiveAfter(h + 5, c)) continue;

            const Ins& init = m.ins[size_t(h) - 1];
            if (init.op != OpCode::Copy || init.returnCopy || Operands(init, 2)[1] != i || !constant(Operands(init, 2)[0], start)) continue;
            const Ins& inc = m.ins[k - 1];
            if (inc.op != OpCode::Extern || m.Signature(inc) != Add || inc.producers.size() != 3 || inc.producers[0] != int(k) - 4 ||
                inc.producers[2] != int(k) - 2)
                continue;
            std::vector<uint32_t> incOps = Operands(inc, 3);
            if (incOps[0] != i || incOps[2] != i || !constant(incOps[1], step) || step != 1) continue;

            int exit = branch.target;
            int64_t trips = limit - start + (m.Signature(cmp) == LessEqual ? 1 : 0);
            int bodyFirst = h + 6, bodyLast = int(k) - 1;
            int bodySize = bodyLast - bodyFirst + 1;
            if (trips < 1 || trips > 8 || bodySize * trips > 160 || size + bodySize * int(trips) > ctx.growthLimit) continue;

            int outside = 0;
            for (int p : preds[size_t(h)]) outside += (p >= h && p <= int(k)) ? 0 : 1;
            if (outside != 1 || std::find(preds[size_t(h)].begin(), preds[size_t(h)].end(), h - 1) == preds[size_t(h)].end()) continue;

            bool ok = true;
            for (int x = bodyFirst; x <= bodyLast && ok; ++x)
            {
                const Ins& b = m.ins[size_t(x)];
                if (!b.reachable || b.call || b.heapCall || b.op == OpCode::JumpIndirect || b.returnCopy || b.isReturn) ok = false;
                if (b.op == OpCode::Extern && EffectOf(m, b) == Effect::Barrier) ok = false;
                if (x != int(k) - 1)
                    for (uint32_t w : b.writes) ok &= w != i;
                if ((b.op == OpCode::Jump || b.op == OpCode::JumpIfFalse) && !b.end)
                    ok &= (b.target >= bodyFirst && b.target <= int(k)) || b.target == exit;
                for (int p : preds[size_t(x)]) ok &= (p >= h && p <= int(k));
            }
            if (!ok || exit <= int(k) && exit >= h) continue;

            std::vector<int> layout;
            std::vector<std::unordered_map<int, int>> copies(static_cast<size_t>(trips));
            std::vector<int> firsts;
            for (int64_t t = 0; t < trips; ++t)
            {
                auto& map = copies[size_t(t)];
                for (int x = bodyFirst; x <= bodyLast; ++x) map[x] = Clone(ir, x);
                firsts.push_back(map[bodyFirst]);
            }
            int after = exit == int(k) + 1 ? -1 : ir.Add(OpCode::Jump, 0, exit);
            for (int64_t t = 0; t < trips; ++t)
            {
                auto& map = copies[size_t(t)];
                int next = t + 1 < trips ? firsts[size_t(t) + 1] : (after >= 0 ? after : exit);
                for (int x = bodyFirst; x <= bodyLast; ++x)
                {
                    Node& n = ir.nodes[size_t(map[x])];
                    if ((n.op == OpCode::Jump || n.op == OpCode::JumpIfFalse) && n.target >= 0)
                    {
                        if (n.target == int(k)) n.target = next;
                        else if (map.contains(n.target)) n.target = map[n.target];
                    }
                    layout.push_back(map[x]);
                }
            }
            if (after >= 0) layout.push_back(after);

            for (int x = h; x <= int(k); ++x) ir.Remove(x);
            ir.InsertAfter(int(k), layout);
            used.insert(h);
            size += bodySize * int(trips);
            ++ctx.stats.unrolled;
            changed = true;
        }
        return changed;
    }
}
