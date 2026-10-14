#pragma once

#include "UdonQuic/Program.hpp"

#include <optional>
#include <string>
#include <vector>

namespace UdonQuic
{
    /// <summary>A stack entry: the heap address a PUSH put there, or the frame's base (a return address or the exit sentinel).</summary>
    struct Elem
    {
        uint32_t slot = 0;
        int producer = -1;
        bool base = false;

        bool operator==(const Elem& o) const { return base == o.base && (base || slot == o.slot); }
    };

    enum class Role : uint8_t
    {
        None,
        CallReturn,
        HeapReturn,
        Switch,
    };

    struct Ins
    {
        uint32_t address = 0;
        OpCode op = OpCode::Nop;
        uint32_t operand = 0;
        int target = -1;
        bool reachable = false;
        std::vector<Elem> stackIn;

        std::vector<uint32_t> reads;
        std::vector<uint32_t> writes;
        std::vector<int> producers;
        std::vector<int> consumers;
        bool poisoned = false;

        bool call = false;
        bool heapCall = false;
        int continuation = -1;
        bool returnCopy = false;
        bool isReturn = false;
        bool end = false;
        bool sentinel = false;
        std::vector<int> successors;

        int Size() const { return HasOperand(op) ? 2 : 1; }
    };

    struct Model
    {
        const Program* program = nullptr;
        std::vector<Ins> ins;
        std::vector<int> byWord;
        std::vector<int> entries;
        std::vector<uint8_t> written;
        std::vector<uint8_t> used;
        std::vector<Role> roles;
        std::vector<int> continuations;
        std::vector<int> heapReturnTargets;

        int At(uint32_t address) const;
        bool Editable(int push) const;
        bool IsConstant(uint32_t slot) const;
        bool IsPrivate(uint32_t slot) const;
        const ExternInfo* Extern(const Ins& i) const;
        const std::string& Signature(const Ins& i) const;
    };

    /// <summary>Decodes a program, proving every stack shape and code address. Returns nullopt and sets error when anything is not proven.</summary>
    std::optional<Model> Decode(const Program& program, std::string& error);
}
