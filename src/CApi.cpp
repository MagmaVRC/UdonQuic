#include "UdonQuic/UdonQuic.h"

#include "UdonQuic/Optimizer.hpp"

#include <optional>

using namespace UdonQuic;

struct uq_program
{
    Program program;
    std::optional<std::string> disassembly;
};

struct uq_result
{
    Result result;
    std::vector<uint8_t> bytes;
    std::vector<uint32_t> addresses;
    std::optional<std::string> disassembly;
};

namespace
{
    template <typename F, typename R = std::invoke_result_t<F>>
    R Guarded(F&& f) noexcept
    {
        try
        {
            return f();
        }
        catch (...)
        {
            if constexpr (!std::is_void_v<R>) return R{};
        }
    }

    std::string Text(const char* s) { return s ? s : ""; }

    Value ToValue(const uq_value* v)
    {
        Value out;
        if (!v) return out;
        out.kind = static_cast<ValueKind>(v->kind);
        out.boolean = v->boolean != 0;
        out.integer = v->integer;
        out.unsignedInteger = v->unsigned_integer;
        out.real = v->real;
        out.text = Text(v->text);
        if (out.kind == ValueKind::UIntArray) out.kind = ValueKind::None;
        return out;
    }

    void FromValue(const Value& v, uq_value* out)
    {
        out->kind = static_cast<int32_t>(v.kind);
        out->boolean = v.boolean ? 1 : 0;
        out->integer = v.integer;
        out->unsigned_integer = v.unsignedInteger;
        out->real = v.real;
        out->text = v.kind == ValueKind::String ? v.text.c_str() : nullptr;
    }
}

extern "C" {

UDONQUIC_API uq_program* uq_program_create(const uint8_t* bytecode, size_t length)
{
    return Guarded([&]() -> uq_program* {
        auto* p = new uq_program();
        if (bytecode) p->program.code = Program::Words(bytecode, length);
        return p;
    });
}

UDONQUIC_API void uq_program_destroy(uq_program* program) { delete program; }

UDONQUIC_API void uq_program_add_slot(uq_program* program, const char* symbol, const char* type, uint32_t flags, const uq_value* value)
{
    Guarded([&] {
        if (!program) return;
        Slot s;
        s.hasSymbol = symbol != nullptr;
        s.symbol = Text(symbol);
        s.type = Text(type);
        s.flags = flags;
        s.value = ToValue(value);
        s.source = uint32_t(program->program.heap.size());
        program->program.heap.push_back(std::move(s));
        program->disassembly.reset();
    });
}

UDONQUIC_API int32_t uq_program_set_slot_array(uq_program* program, uint32_t address, const uint32_t* values, int32_t count)
{
    return Guarded([&]() -> int32_t {
        if (!program || address >= program->program.heap.size() || count < 0 || (count && !values)) return 0;
        Value& v = program->program.heap[address].value;
        v = {};
        v.kind = ValueKind::UIntArray;
        v.array.assign(values, values + count);
        return 1;
    });
}

UDONQUIC_API void uq_program_add_entry(uq_program* program, const char* name, uint32_t address)
{
    Guarded([&] {
        if (program) program->program.entries.push_back({ Text(name), address });
    });
}

UDONQUIC_API void uq_program_add_extern(uq_program* program, const char* signature, int32_t arity, int32_t has_result, uint64_t written)
{
    Guarded([&] {
        if (program && signature) program->program.externs[signature] = { arity, has_result != 0, written };
    });
}

UDONQUIC_API void uq_options_default(uq_options* options)
{
    if (!options) return;
    Options o;
    options->passes = o.passes;
    options->inline_budget = o.inlineBudget;
    options->max_rounds = o.maxRounds;
}

UDONQUIC_API uq_result* uq_optimize(const uq_program* program, const uq_options* options)
{
    return Guarded([&]() -> uq_result* {
        auto* r = new uq_result();
        if (!program)
        {
            r->result.message = "no program";
            return r;
        }
        Options o;
        if (options)
        {
            o.passes = options->passes;
            o.inlineBudget = options->inline_budget;
            o.maxRounds = options->max_rounds;
        }
        try
        {
            r->result = Optimize(program->program, o);
        }
        catch (const std::exception& e)
        {
            r->result = {};
            r->result.program = program->program;
            r->result.message = std::string("internal error: ") + e.what();
        }
        r->bytes = r->result.program.ByteCode();
        const auto& code = r->result.program.code;
        for (size_t pc = 0; pc < code.size(); pc += HasOperand(static_cast<OpCode>(code[pc])) ? 2 : 1) r->addresses.push_back(uint32_t(pc * 4));
        return r;
    });
}

UDONQUIC_API void uq_result_destroy(uq_result* result) { delete result; }

UDONQUIC_API int32_t uq_result_succeeded(const uq_result* result) { return result && result->result.optimized ? 1 : 0; }

UDONQUIC_API const char* uq_result_message(const uq_result* result) { return result ? result->result.message.c_str() : ""; }

UDONQUIC_API const uint8_t* uq_result_bytecode(const uq_result* result, size_t* length)
{
    if (length) *length = result ? result->bytes.size() : 0;
    return result ? result->bytes.data() : nullptr;
}

UDONQUIC_API int32_t uq_result_slot_count(const uq_result* result)
{
    return result ? static_cast<int32_t>(result->result.program.heap.size()) : 0;
}

UDONQUIC_API int32_t uq_result_slot(const uq_result* result, int32_t address, uq_slot* out)
{
    if (!result || !out || address < 0 || size_t(address) >= result->result.program.heap.size()) return 0;
    const Slot& s = result->result.program.heap[size_t(address)];
    out->symbol = s.hasSymbol ? s.symbol.c_str() : nullptr;
    out->type = s.type.c_str();
    out->flags = s.flags;
    out->rewritten = s.rewritten ? 1 : 0;
    FromValue(s.value, &out->value);
    return 1;
}

UDONQUIC_API int32_t uq_result_slot_source(const uq_result* result, int32_t address, uint32_t* source)
{
    if (!result || address < 0 || size_t(address) >= result->result.program.heap.size()) return 0;
    if (source) *source = result->result.program.heap[size_t(address)].source;
    return 1;
}

UDONQUIC_API int32_t uq_result_slot_array(const uq_result* result, int32_t address, const uint32_t** values)
{
    if (!result || address < 0 || size_t(address) >= result->result.program.heap.size()) return -1;
    const Value& v = result->result.program.heap[size_t(address)].value;
    if (v.kind != ValueKind::UIntArray) return -1;
    if (values) *values = v.array.data();
    return static_cast<int32_t>(v.array.size());
}

UDONQUIC_API int32_t uq_result_entry_count(const uq_result* result)
{
    return result ? static_cast<int32_t>(result->result.program.entries.size()) : 0;
}

UDONQUIC_API int32_t uq_result_entry(const uq_result* result, int32_t index, uq_entry_point* out)
{
    if (!result || !out || index < 0 || size_t(index) >= result->result.program.entries.size()) return 0;
    const EntryPoint& e = result->result.program.entries[size_t(index)];
    out->name = e.name.c_str();
    out->address = e.address;
    return 1;
}

UDONQUIC_API void uq_result_stats(const uq_result* result, uq_stats* out)
{
    if (!out) return;
    *out = {};
    if (!result) return;
    const Stats& s = result->result.stats;
    out->instructions_before = s.instructionsBefore;
    out->instructions_after = s.instructionsAfter;
    out->externs_before = s.externsBefore;
    out->externs_after = s.externsAfter;
    out->inlined = s.inlined;
    out->tail_calls = s.tailCalls;
    out->folded = s.folded;
    out->copies_removed = s.copiesRemoved;
    out->branches_simplified = s.branchesSimplified;
    out->dead_stores = s.deadStores;
    out->rounds = s.rounds;
    out->hoisted = s.hoisted;
    out->loops_rotated = s.loopsRotated;
    out->checks_removed = s.checksRemoved;
    out->switches_built = s.switchesBuilt;
    out->concats_merged = s.concatsMerged;
    out->fused = s.fused;
    out->memoized = s.memoized;
    out->unrolled = s.unrolled;
    out->events_removed = s.eventsRemoved;
    out->propagated = s.propagated;
    out->heap_before = s.heapBefore;
    out->heap_after = s.heapAfter;
}

UDONQUIC_API int32_t uq_result_origin_count(const uq_result* result)
{
    return result ? static_cast<int32_t>(result->addresses.size()) : 0;
}

UDONQUIC_API int32_t uq_result_origin(const uq_result* result, int32_t index, uint32_t* address, uint32_t* origin)
{
    if (!result || index < 0 || size_t(index) >= result->addresses.size()) return 0;
    const auto& origins = result->result.program.origins;
    uint32_t at = result->addresses[size_t(index)];
    if (address) *address = at;
    if (origin) *origin = origins.empty() ? at : (size_t(index) < origins.size() ? origins[size_t(index)] : EndAddress);
    return 1;
}

UDONQUIC_API const char* uq_version(void) { return Version(); }

UDONQUIC_API int32_t uq_pass_count(void) { return static_cast<int32_t>(Passes().size()); }

UDONQUIC_API int32_t uq_pass_info(int32_t index, uint32_t* bit, const char** name, const char** description)
{
    auto passes = Passes();
    if (index < 0 || size_t(index) >= passes.size()) return 0;
    const PassInfo& p = passes[size_t(index)];
    if (bit) *bit = p.bit;
    if (name) *name = p.name;
    if (description) *description = p.description;
    return 1;
}

UDONQUIC_API const char* uq_result_disassembly(uq_result* result)
{
    if (!result) return "";
    return Guarded([&]() -> const char* {
        if (!result->disassembly) result->disassembly = Disassemble(result->result.program);
        return result->disassembly->c_str();
    });
}

UDONQUIC_API const char* uq_program_disassembly(uq_program* program)
{
    if (!program) return "";
    return Guarded([&]() -> const char* {
        if (!program->disassembly) program->disassembly = Disassemble(program->program);
        return program->disassembly->c_str();
    });
}

}
