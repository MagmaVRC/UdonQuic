#pragma once

#include "UdonQuic/Program.hpp"

#include <span>
#include <string>

namespace UdonQuic
{
    enum Pass : uint32_t
    {
        PassDeadCode = 1u << 0,
        PassInline = 1u << 1,
        PassTailCall = 1u << 2,
        PassFold = 1u << 3,
        PassCopies = 1u << 4,
        PassBranches = 1u << 5,
        PassDeadStores = 1u << 6,
        PassJumps = 1u << 7,
        PassHoist = 1u << 8,
        PassLoops = 1u << 9,
        PassGlobal = 1u << 10,
        PassSwitches = 1u << 11,
        PassConcat = 1u << 12,
        PassFusion = 1u << 13,
        PassMemo = 1u << 14,
        PassUnroll = 1u << 15,
        PassEvents = 1u << 16,
        PassHeap = 1u << 17,
        PassAll = (1u << 18) - 1,
    };

    struct PassInfo
    {
        Pass bit;
        const char* name;
        const char* description;
    };

    /// <summary>Every pass, in the order the optimizer runs them.</summary>
    std::span<const PassInfo> Passes();

    const char* Version();

    struct Options
    {
        uint32_t passes = PassAll;
        int inlineBudget = 24;
        int maxRounds = 8;
        int maxInlines = -1;
    };

    struct Stats
    {
        int instructionsBefore = 0;
        int instructionsAfter = 0;
        int externsBefore = 0;
        int externsAfter = 0;
        int inlined = 0;
        int tailCalls = 0;
        int folded = 0;
        int copiesRemoved = 0;
        int branchesSimplified = 0;
        int deadStores = 0;
        int rounds = 0;
        int hoisted = 0;
        int loopsRotated = 0;
        int checksRemoved = 0;
        int switchesBuilt = 0;
        int concatsMerged = 0;
        int fused = 0;
        int memoized = 0;
        int unrolled = 0;
        int eventsRemoved = 0;
        int propagated = 0;
        int heapBefore = 0;
        int heapAfter = 0;
    };

    struct Result
    {
        bool optimized = false;
        std::string message;
        Program program;
        Stats stats;
    };

    /// <summary>Optimizes a program. On any failure the result carries the input unchanged and a message.</summary>
    Result Optimize(const Program& program, const Options& options = {});
}
