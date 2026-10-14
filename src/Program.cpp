#include "UdonQuic/Program.hpp"

#include <format>

namespace UdonQuic
{
    std::vector<uint8_t> Program::ByteCode() const
    {
        std::vector<uint8_t> bytes;
        bytes.reserve(code.size() * 4);
        for (uint32_t word : code)
        {
            bytes.push_back(static_cast<uint8_t>(word >> 24));
            bytes.push_back(static_cast<uint8_t>(word >> 16));
            bytes.push_back(static_cast<uint8_t>(word >> 8));
            bytes.push_back(static_cast<uint8_t>(word));
        }
        return bytes;
    }

    std::vector<uint32_t> Program::Words(const uint8_t* bytes, size_t length)
    {
        std::vector<uint32_t> words(length / 4);
        for (size_t i = 0; i < words.size(); ++i)
        {
            const uint8_t* b = bytes + i * 4;
            words[i] = (uint32_t(b[0]) << 24) | (uint32_t(b[1]) << 16) | (uint32_t(b[2]) << 8) | uint32_t(b[3]);
        }
        return words;
    }

    static std::string Describe(const Value& v)
    {
        switch (v.kind)
        {
        case ValueKind::Boolean: return v.boolean ? "true" : "false";
        case ValueKind::Integer: return std::to_string(v.integer);
        case ValueKind::Unsigned: return std::format("{:#x}", v.unsignedInteger);
        case ValueKind::Real: return std::format("{}", v.real);
        case ValueKind::String: return std::format("\"{}\"", v.text);
        case ValueKind::UIntArray:
        {
            std::string s = "[";
            for (size_t i = 0; i < v.array.size(); ++i) s += std::format("{}{:#x}", i ? ", " : "", v.array[i]);
            return s + "]";
        }
        default: return "?";
        }
    }

    static std::string SlotName(const Program& p, uint32_t address)
    {
        if (address >= p.heap.size()) return std::format("<{:#x}>", address);
        const Slot& s = p.heap[address];
        return s.hasSymbol ? s.symbol : std::format("<{:#x}>", address);
    }

    std::string Disassemble(const Program& p)
    {
        std::string out = ".data\n";
        for (size_t i = 0; i < p.heap.size(); ++i)
        {
            const Slot& s = p.heap[i];
            out += std::format("  {:#06x} {}{}: {}", i, s.Is(SlotExported) ? "export " : "", SlotName(p, uint32_t(i)), s.type);
            if (s.value.kind != ValueKind::None) out += " = " + Describe(s.value);
            out += "\n";
        }

        out += ".code\n";
        for (size_t pc = 0; pc < p.code.size();)
        {
            uint32_t address = uint32_t(pc * 4);
            for (const EntryPoint& e : p.entries)
                if (e.address == address) out += e.name + ":\n";

            auto op = static_cast<OpCode>(p.code[pc]);
            uint32_t operand = HasOperand(op) && pc + 1 < p.code.size() ? p.code[pc + 1] : 0;
            switch (op)
            {
            case OpCode::Nop: out += std::format("  {:#06x} NOP\n", address); break;
            case OpCode::Pop: out += std::format("  {:#06x} POP\n", address); break;
            case OpCode::Copy: out += std::format("  {:#06x} COPY\n", address); break;
            case OpCode::Push: out += std::format("  {:#06x} PUSH {}\n", address, SlotName(p, operand)); break;
            case OpCode::JumpIfFalse: out += std::format("  {:#06x} JUMP_IF_FALSE {:#06x}\n", address, operand); break;
            case OpCode::Jump: out += std::format("  {:#06x} JUMP {:#06x}\n", address, operand); break;
            case OpCode::JumpIndirect: out += std::format("  {:#06x} JUMP_INDIRECT {}\n", address, SlotName(p, operand)); break;
            case OpCode::Annotation: out += std::format("  {:#06x} ANNOTATION {}\n", address, SlotName(p, operand)); break;
            case OpCode::Extern:
            {
                std::string sig = operand < p.heap.size() ? p.heap[operand].value.text : "";
                out += std::format("  {:#06x} EXTERN {}\n", address, sig.empty() ? SlotName(p, operand) : sig);
                break;
            }
            default: out += std::format("  {:#06x} ?? {:#x}\n", address, p.code[pc]); break;
            }
            pc += HasOperand(op) ? 2 : 1;
        }
        return out;
    }
}
