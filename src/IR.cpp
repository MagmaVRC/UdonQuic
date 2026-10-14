#include "IR.hpp"

#include <bit>
#include <format>
#include <stdexcept>

namespace UdonQuic
{
    int IR::Add(OpCode op, uint32_t operand, int target)
    {
        nodes.push_back({ op, operand, target, false });
        origin.push_back(EndAddress);
        return int(nodes.size()) - 1;
    }

    void IR::InsertAfter(int anchor, std::vector<int> ids)
    {
        auto& list = insertAfter[anchor];
        list.insert(list.end(), ids.begin(), ids.end());
    }

    void IR::InsertBefore(int anchor, std::vector<int> ids)
    {
        auto& list = insertBefore[anchor];
        list.insert(list.end(), ids.begin(), ids.end());
    }

    uint32_t IR::NewSlot(const std::string& type, const Value& value)
    {
        std::string name;
        do name = std::format("__const_quic_{}_{}", type, nameCounter++);
        while (names.contains(name));
        names.insert(name);

        Slot s;
        s.symbol = name;
        s.hasSymbol = true;
        s.type = type;
        s.flags = SlotInitialized | SlotPrivate;
        s.value = value;
        s.rewritten = true;
        program.heap.push_back(std::move(s));
        return uint32_t(program.heap.size() - 1);
    }

    uint32_t IR::Constant(const std::string& type, const Value& value)
    {
        for (size_t i = 0; i < program.heap.size(); ++i)
        {
            const Slot& s = program.heap[i];
            if (s.type != type || !(s.value == value)) continue;
            if (value.kind == ValueKind::Real && std::bit_cast<uint64_t>(s.value.real) != std::bit_cast<uint64_t>(value.real)) continue;
            bool isNew = i >= model->written.size();
            if (isNew ? !addressCarriers.contains(uint32_t(i)) : model->IsConstant(uint32_t(i))) return uint32_t(i);
        }
        return NewSlot(type, value);
    }

    uint32_t IR::ExternSlot(const std::string& signature, const ExternInfo& info)
    {
        program.externs.try_emplace(signature, info);
        for (size_t i = 0; i < program.heap.size(); ++i)
        {
            const Slot& s = program.heap[i];
            if (s.type == "SystemString" && s.value.kind == ValueKind::String && s.value.text == signature) return uint32_t(i);
        }
        Value v;
        v.kind = ValueKind::String;
        v.text = signature;
        return NewSlot("SystemString", v);
    }

    int IR::Live() const
    {
        int n = 0;
        for (const Node& node : nodes) n += node.removed ? 0 : 1;
        return n;
    }

    Program IR::Emit()
    {
        std::vector<int> layout;
        std::vector<uint8_t> placed(nodes.size(), 0);
        auto place = [&](auto& self, int id) -> void {
            if (placed[size_t(id)]) throw std::logic_error("node placed twice");
            if (auto it = insertBefore.find(id); it != insertBefore.end())
                for (int b : it->second) self(self, b);
            placed[size_t(id)] = 1;
            layout.push_back(id);
            if (auto it = insertAfter.find(id); it != insertAfter.end())
                for (int a : it->second) self(self, a);
        };
        for (int id : order) place(place, id);

        std::vector<int> pos(nodes.size(), -1);
        for (size_t k = 0; k < layout.size(); ++k) pos[size_t(layout[k])] = int(k);

        std::vector<uint32_t> address(layout.size() + 1, 0);
        uint32_t at = 0;
        for (size_t k = 0; k < layout.size(); ++k)
        {
            address[k] = at;
            const Node& n = nodes[size_t(layout[k])];
            if (!n.removed) at += HasOperand(n.op) ? 8 : 4;
        }
        address[layout.size()] = at;

        std::vector<uint32_t> nextLive(layout.size() + 1, at);
        for (size_t k = layout.size(); k-- > 0;)
            nextLive[k] = nodes[size_t(layout[k])].removed ? nextLive[k + 1] : address[k];

        auto resolve = [&](int id) -> uint32_t {
            if (id < 0 || size_t(id) >= nodes.size() || pos[size_t(id)] < 0) throw std::logic_error("reference to a node outside the layout");
            return nextLive[size_t(pos[size_t(id)])];
        };

        Program out = program;
        out.code.clear();
        out.origins.clear();
        for (int id : layout)
        {
            const Node& n = nodes[size_t(id)];
            if (n.removed) continue;
            out.origins.push_back(origin[size_t(id)]);
            out.code.push_back(uint32_t(n.op));
            if (!HasOperand(n.op)) continue;
            bool jump = n.op == OpCode::Jump || n.op == OpCode::JumpIfFalse;
            out.code.push_back(jump && n.target >= 0 ? resolve(n.target) : n.operand);
        }

        for (size_t e = 0; e < out.entries.size(); ++e) out.entries[e].address = resolve(entries[e]);

        for (const auto& [slot, id] : addressCarriers)
        {
            Slot& s = out.heap[slot];
            uint64_t value = resolve(id);
            if (s.value.unsignedInteger != value) s.rewritten = true;
            s.value.unsignedInteger = value;
        }
        for (const auto& [slot, ids] : switchCarriers)
        {
            Slot& s = out.heap[slot];
            std::vector<uint32_t> table;
            for (int id : ids) table.push_back(resolve(id));
            if (s.value.array != table) s.rewritten = true;
            s.value.array = std::move(table);
        }
        return out;
    }

    IR Build(const Program& program, const Model& m)
    {
        IR ir;
        ir.program = program;
        ir.model = &m;
        for (const Slot& s : program.heap)
            if (s.hasSymbol) ir.names.insert(s.symbol);
        for (size_t k = 0; k < m.ins.size(); ++k)
        {
            const Ins& i = m.ins[k];
            ir.nodes.push_back({ i.op, i.operand, i.end ? -1 : i.target, false });
            ir.origin.push_back(program.origins.empty() ? i.address : program.origins[k]);
        }
        for (size_t k = 0; k < ir.nodes.size(); ++k) ir.order.push_back(int(k));
        ir.entries = m.entries;

        std::vector<int> refs(program.heap.size(), 0);
        for (const Ins& i : m.ins)
            if (HasOperand(i.op) && i.op != OpCode::Jump && i.op != OpCode::JumpIfFalse) ++refs[i.operand];

        auto relocate = [&](uint32_t slot, const std::vector<int>& users) -> uint32_t {
            for (int u : users)
                if (u < 0) throw std::runtime_error("a code-address constant is pushed on more than one path");
            if (refs[slot] == int(users.size())) return slot;
            const Slot& s = program.heap[slot];
            uint32_t fresh = ir.NewSlot(s.type, s.value);
            for (int u : users) ir.nodes[size_t(u)].operand = fresh;
            return fresh;
        };

        std::unordered_map<uint32_t, std::vector<int>> callUsers;
        std::unordered_map<uint32_t, int> callTargets;
        for (size_t k = 0; k < m.ins.size(); ++k)
        {
            const Ins& i = m.ins[k];
            if (!i.reachable || !i.call) continue;
            uint32_t slot = i.stackIn.back().slot;
            callUsers[slot].push_back(i.stackIn.back().producer);
            callTargets[slot] = i.continuation;
        }
        for (auto& [slot, users] : callUsers) ir.addressCarriers[relocate(slot, users)] = callTargets[slot];

        std::unordered_map<uint32_t, std::vector<int>> heapUsers;
        for (const Ins& i : m.ins)
        {
            if (!i.reachable || i.op != OpCode::Copy || i.returnCopy) continue;
            uint32_t src = i.stackIn[i.stackIn.size() - 2].slot;
            if (m.roles[src] == Role::HeapReturn) heapUsers[src].push_back(i.producers[0]);
        }
        for (auto& [slot, users] : heapUsers)
        {
            int target = m.At(uint32_t(program.heap[slot].value.unsignedInteger));
            ir.addressCarriers[relocate(slot, users)] = target;
        }

        for (size_t s = 0; s < m.roles.size(); ++s)
        {
            if (m.roles[s] != Role::Switch) continue;
            std::vector<int> ids;
            for (uint32_t a : program.heap[s].value.array) ids.push_back(m.At(a));
            ir.switchCarriers[uint32_t(s)] = std::move(ids);
        }
        ir.built = ir.nodes;
        return ir;
    }

    int CountInstructions(const Program& program)
    {
        int n = 0;
        for (size_t pc = 0; pc < program.code.size(); ++n) pc += HasOperand(static_cast<OpCode>(program.code[pc])) ? 2 : 1;
        return n;
    }

    int CountExterns(const Program& program)
    {
        int n = 0;
        for (size_t pc = 0; pc < program.code.size();)
        {
            auto op = static_cast<OpCode>(program.code[pc]);
            n += op == OpCode::Extern ? 1 : 0;
            pc += HasOperand(op) ? 2 : 1;
        }
        return n;
    }
}
