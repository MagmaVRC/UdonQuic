#pragma once

#include <stddef.h>
#include <stdint.h>

#if defined(UDONQUIC_SHARED)
#if defined(UDONQUIC_EXPORTS)
#define UDONQUIC_API __declspec(dllexport)
#else
#define UDONQUIC_API __declspec(dllimport)
#endif
#else
#define UDONQUIC_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct uq_program uq_program;
typedef struct uq_result uq_result;

#define UQ_SLOT_INITIALIZED 1u
#define UQ_SLOT_EXPORTED 2u
#define UQ_SLOT_SYNCED 4u
#define UQ_SLOT_PRIVATE 8u

typedef enum uq_value_kind {
    UQ_VALUE_NONE = 0,
    UQ_VALUE_BOOLEAN = 1,
    UQ_VALUE_INTEGER = 2,
    UQ_VALUE_UNSIGNED = 3,
    UQ_VALUE_REAL = 4,
    UQ_VALUE_STRING = 5,
    UQ_VALUE_UINT_ARRAY = 6,
} uq_value_kind;

typedef struct uq_value {
    int32_t     kind;
    int32_t     boolean;
    int64_t     integer;
    uint64_t    unsigned_integer;
    double      real;
    const char* text;
} uq_value;

typedef struct uq_slot {
    const char* symbol;
    const char* type;
    uint32_t    flags;
    int32_t     rewritten;
    uq_value    value;
} uq_slot;

typedef struct uq_entry_point {
    const char* name;
    uint32_t    address;
} uq_entry_point;

typedef struct uq_options {
    uint32_t passes;
    int32_t  inline_budget;
    int32_t  max_rounds;
} uq_options;

typedef struct uq_stats {
    int32_t instructions_before;
    int32_t instructions_after;
    int32_t externs_before;
    int32_t externs_after;
    int32_t inlined;
    int32_t tail_calls;
    int32_t folded;
    int32_t copies_removed;
    int32_t branches_simplified;
    int32_t dead_stores;
    int32_t rounds;
    int32_t hoisted;
    int32_t loops_rotated;
    int32_t checks_removed;
    int32_t switches_built;
    int32_t concats_merged;
    int32_t fused;
    int32_t memoized;
    int32_t unrolled;
    int32_t events_removed;
    int32_t propagated;
    int32_t heap_before;
    int32_t heap_after;
} uq_stats;

/// <summary>The optimizer's version, for example "0.2.0".</summary>
UDONQUIC_API const char* uq_version(void);

/// <summary>The number of optimizer passes.</summary>
UDONQUIC_API int32_t uq_pass_count(void);

/// <summary>A pass: its bit in uq_options.passes, a short name and a one-sentence description.</summary>
/// <returns>0 when the index is out of range.</returns>
UDONQUIC_API int32_t uq_pass_info(int32_t index, uint32_t* bit, const char** name, const char** description);

/// <summary>Creates a program from code in Udon's big-endian byte order.</summary>
UDONQUIC_API uq_program* uq_program_create(const uint8_t* bytecode, size_t length);

/// <summary>Destroys a program.</summary>
UDONQUIC_API void uq_program_destroy(uq_program* program);

/// <summary>Appends a heap slot; its address is the number of slots added before it. symbol may be null for a slot no symbol names.
/// type is the Udon type name, for example "SystemInt32". value may be null.</summary>
UDONQUIC_API void uq_program_add_slot(uq_program* program, const char* symbol, const char* type, uint32_t flags, const uq_value* value);

/// <summary>Sets a slot added earlier to a uint array, for jump tables.</summary>
/// <returns>0 when the address is out of range.</returns>
UDONQUIC_API int32_t uq_program_set_slot_array(uq_program* program, uint32_t address, const uint32_t* values, int32_t count);

/// <summary>Appends an exported entry point. Order is preserved.</summary>
UDONQUIC_API void uq_program_add_entry(uq_program* program, const char* name, uint32_t address);

/// <summary>Declares an extern: the operand count the VM pops, whether the last operand receives a result,
/// and a mask of the operands it may write (bit i is operand i).</summary>
UDONQUIC_API void uq_program_add_extern(uq_program* program, const char* signature, int32_t arity, int32_t has_result, uint64_t written);

/// <summary>Fills options with the defaults.</summary>
UDONQUIC_API void uq_options_default(uq_options* options);

/// <summary>Optimizes a program. Always returns a result; options may be null.</summary>
UDONQUIC_API uq_result* uq_optimize(const uq_program* program, const uq_options* options);

/// <summary>Destroys a result and every string it returned.</summary>
UDONQUIC_API void uq_result_destroy(uq_result* result);

/// <summary>Whether the program was rewritten. When 0, the message says why.</summary>
UDONQUIC_API int32_t uq_result_succeeded(const uq_result* result);

UDONQUIC_API const char* uq_result_message(const uq_result* result);

/// <summary>The rewritten code in Udon's big-endian byte order.</summary>
UDONQUIC_API const uint8_t* uq_result_bytecode(const uq_result* result, size_t* length);

/// <summary>The rewritten heap size. Slots past the input's count are new.</summary>
UDONQUIC_API int32_t uq_result_slot_count(const uq_result* result);
UDONQUIC_API int32_t uq_result_slot(const uq_result* result, int32_t address, uq_slot* out);

/// <summary>The input address a result slot came from, or 0xFFFFFFFF for a slot the optimizer created. When the heap was compacted,
/// result addresses no longer match input addresses; a slot that is not rewritten keeps its source slot's value and box.</summary>
/// <returns>0 when the address is out of range.</returns>
UDONQUIC_API int32_t uq_result_slot_source(const uq_result* result, int32_t address, uint32_t* source);

/// <summary>The elements of a UQ_VALUE_UINT_ARRAY slot.</summary>
/// <returns>The element count, or -1 when the slot holds no array.</returns>
UDONQUIC_API int32_t uq_result_slot_array(const uq_result* result, int32_t address, const uint32_t** values);

UDONQUIC_API int32_t uq_result_entry_count(const uq_result* result);
UDONQUIC_API int32_t uq_result_entry(const uq_result* result, int32_t index, uq_entry_point* out);

UDONQUIC_API void uq_result_stats(const uq_result* result, uq_stats* out);

/// <summary>The number of instructions in the rewritten code.</summary>
UDONQUIC_API int32_t uq_result_origin_count(const uq_result* result);

/// <summary>Instruction index of the rewritten code: its byte address and the byte address of the input instruction it came from,
/// or 0xFFFFFFFF when the optimizer created it. Inlined and duplicated instructions report the instruction they were copied from.</summary>
/// <returns>0 when the index is out of range.</returns>
UDONQUIC_API int32_t uq_result_origin(const uq_result* result, int32_t index, uint32_t* address, uint32_t* origin);

/// <summary>A readable listing of the rewritten program.</summary>
UDONQUIC_API const char* uq_result_disassembly(uq_result* result);

/// <summary>A readable listing of a program as given.</summary>
UDONQUIC_API const char* uq_program_disassembly(uq_program* program);

#ifdef __cplusplus
}
#endif
