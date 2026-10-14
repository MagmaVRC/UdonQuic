#pragma once

#include "Flow.hpp"

#include <unordered_map>
#include <unordered_set>

namespace UdonQuic
{
    struct Node
    {
        OpCode op = OpCode::Nop;
        uint32_t operand = 0;
        int target = -1;
        bool removed = false;
    };

    /// <summary>A relocatable program: nodes are referenced by id, removed nodes resolve to the next live node in layout order.</summary>
    struct IR
    {
        Program program;
        const Model* model = nullptr;
        std::vector<Node> nodes;
        std::vector<uint32_t> origin;
        std::vector<Node> built;
        std::vector<int> order;
        std::vector<int> entries;
        std::unordered_map<uint32_t, int> addressCarriers;
        std::unordered_map<uint32_t, std::vector<int>> switchCarriers;
        std::unordered_map<int, std::vector<int>> insertAfter;
        std::unordered_map<int, std::vector<int>> insertBefore;
        std::unordered_set<std::string> names;
        int nameCounter = 0;

        int Add(OpCode op, uint32_t operand = 0, int target = -1);
        void Remove(int id) { nodes[size_t(id)].removed = true; }
        void InsertAfter(int anchor, std::vector<int> ids);
        void InsertBefore(int anchor, std::vector<int> ids);

        uint32_t NewSlot(const std::string& type, const Value& value);
        uint32_t Constant(const std::string& type, const Value& value);
        uint32_t ExternSlot(const std::string& signature, const ExternInfo& info);

        int Live() const;
        Program Emit();
    };

    /// <summary>Builds an IR from a decoded program, splitting code-address constants that are also used as data.</summary>
    IR Build(const Program& program, const Model& model);

    int CountInstructions(const Program& program);
    int CountExterns(const Program& program);
}
