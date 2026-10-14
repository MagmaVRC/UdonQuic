#pragma once

#include "IR.hpp"
#include "UdonQuic/Optimizer.hpp"

namespace UdonQuic
{
    struct PassContext
    {
        const Options& options;
        Stats& stats;
        int growthLimit = 0;
    };

    bool DeadCode(IR& ir, const Model& m, PassContext& ctx);
    bool Jumps(IR& ir, const Model& m, PassContext& ctx);
    bool TailCalls(IR& ir, const Model& m, PassContext& ctx);
    bool Inline(IR& ir, const Model& m, PassContext& ctx);
    bool Local(IR& ir, const Model& m, PassContext& ctx);
    bool Branches(IR& ir, const Model& m, PassContext& ctx);
    bool DeadStores(IR& ir, const Model& m, PassContext& ctx);
    bool Switches(IR& ir, const Model& m, PassContext& ctx);
    bool Concat(IR& ir, const Model& m, PassContext& ctx);
    bool Global(IR& ir, const Model& m, PassContext& ctx);
    bool Hoist(IR& ir, const Model& m, PassContext& ctx);
    bool Loops(IR& ir, const Model& m, PassContext& ctx);
    bool Fusion(IR& ir, const Model& m, PassContext& ctx);
    bool Memo(IR& ir, const Model& m, PassContext& ctx);
    bool Unroll(IR& ir, const Model& m, PassContext& ctx);
    bool Events(IR& ir, const Model& m, PassContext& ctx);

    bool IsFrameEvent(std::string_view name);

    /// <summary>Removes unreferenced private heap slots, merges duplicate constants and private slots that are never live at the same time,
    /// and renumbers the heap. Returns nullopt when nothing changed.</summary>
    std::optional<Program> CompactHeap(const Program& program, const Model& m, Stats& stats);
}
