#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace UdonQuic
{
    enum class OpCode : uint32_t
    {
        Nop = 0,
        Push = 1,
        Pop = 2,
        JumpIfFalse = 4,
        Jump = 5,
        Extern = 6,
        Annotation = 7,
        JumpIndirect = 8,
        Copy = 9,
    };

    constexpr bool HasOperand(OpCode op)
    {
        return op == OpCode::Push || op == OpCode::JumpIfFalse || op == OpCode::Jump || op == OpCode::Extern ||
               op == OpCode::Annotation || op == OpCode::JumpIndirect;
    }

    constexpr uint32_t EndAddress = 0xFFFFFFFFu;

    enum SlotFlags : uint32_t
    {
        SlotInitialized = 1u << 0,
        SlotExported = 1u << 1,
        SlotSynced = 1u << 2,
        SlotPrivate = 1u << 3,
    };

    enum class ValueKind : uint8_t
    {
        None = 0,
        Boolean = 1,
        Integer = 2,
        Unsigned = 3,
        Real = 4,
        String = 5,
        UIntArray = 6,
    };

    /// <summary>A heap slot's initial value, when it is one the optimizer can reason about.</summary>
    struct Value
    {
        ValueKind kind = ValueKind::None;
        bool boolean = false;
        int64_t integer = 0;
        uint64_t unsignedInteger = 0;
        double real = 0;
        std::string text;
        std::vector<uint32_t> array;

        bool operator==(const Value&) const = default;
    };

    struct Slot
    {
        std::string symbol;
        bool hasSymbol = false;
        std::string type;
        uint32_t flags = 0;
        Value value;
        bool rewritten = false;
        uint32_t source = 0xFFFFFFFFu;

        bool Is(SlotFlags flag) const { return (flags & flag) != 0; }
    };

    struct EntryPoint
    {
        std::string name;
        uint32_t address = 0;
    };

    struct ExternInfo
    {
        int arity = 0;
        bool hasResult = false;
        uint64_t writes = ~0ull;
    };

    /// <summary>An Udon program: code words (opcode and operand words, addresses are byte offsets), heap and entry points.</summary>
    struct Program
    {
        std::vector<uint32_t> code;
        std::vector<Slot> heap;
        std::vector<EntryPoint> entries;
        std::unordered_map<std::string, ExternInfo> externs;

        /// <summary>Per instruction, the input address it came from (EndAddress when created by the optimizer). Empty means every instruction is its own origin.</summary>
        std::vector<uint32_t> origins;

        std::vector<uint8_t> ByteCode() const;
        static std::vector<uint32_t> Words(const uint8_t* bytes, size_t length);
    };

    std::string Disassemble(const Program& program);
}
