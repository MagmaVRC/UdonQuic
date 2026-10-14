#include "UdonQuic/Optimizer.hpp"

#include "IR.hpp"
#include "Passes.hpp"

#include <format>
#include <optional>

namespace UdonQuic
{
    namespace
    {
        using PassFn = bool (*)(IR&, const Model&, PassContext&);

        struct PassEntry
        {
            uint32_t flags;
            const char* name;
            PassFn run;
        };

        constexpr PassEntry Pipeline[] = {
            { PassDeadCode, "dead code", DeadCode },
            { PassJumps, "jumps", Jumps },
            { PassTailCall, "tail calls", TailCalls },
            { PassInline, "inline", Inline },
            { PassUnroll, "unroll", Unroll },
            { PassSwitches, "switches", Switches },
            { PassConcat, "concat", Concat },
            { PassFusion, "fusion", Fusion },
            { PassFold | PassCopies, "local", Local },
            { PassGlobal, "global", Global },
            { PassMemo, "memo", Memo },
            { PassHoist, "hoist", Hoist },
            { PassLoops, "loops", Loops },
            { PassBranches, "branches", Branches },
            { PassDeadStores, "dead stores", DeadStores },
            { PassEvents, "events", Events },
        };

        constexpr PassInfo Infos[] = {
            { PassDeadCode, "Dead code", "Removes unreachable instructions, NOP and ANNOTATION." },
            { PassJumps, "Jumps", "Threads jump chains, drops jumps to the next instruction and copies return trampolines to the jumps that reach them." },
            { PassTailCall, "Tail calls", "Turns a call followed directly by a return into a jump." },
            { PassInline, "Inline", "Copies small methods, and methods with a single caller, into their call sites." },
            { PassSwitches, "Switches", "Turns if/else chains that compare one integer against constants into a jump table." },
            { PassConcat, "String concat", "Merges chains of String.Concat into the three- and four-argument overloads." },
            { PassFold, "Constant folding", "Evaluates operators on constants and simplifies identities such as x + 0 and b == true." },
            { PassCopies, "Copies", "Propagates copies and reuses results of pure operations computed earlier in the same block." },
            { PassGlobal, "Redundant checks", "Reuses results of pure operations and checks already computed on every path, across branches." },
            { PassHoist, "Loop hoisting", "Moves pure work that does not change inside a loop, such as an array's Length, out of the loop." },
            { PassLoops, "Loop rotation", "Tests a loop's condition at the bottom, saving a jump per iteration." },
            { PassBranches, "Branches", "Inverts negated integer compares and branches on the operand of a negation." },
            { PassDeadStores, "Dead stores", "Removes writes nothing reads and writes extern results straight into the variable they were copied to." },
            { PassFusion, "Extern fusion", "Replaces several externs with one Unity method that computes the same thing, such as component-wise vector math, Dot, magnitude, Distance and Lerp." },
            { PassMemo, "Remember stable reads", "Computes values that never change for the session, such as Networking.LocalPlayer and its playerId, once and reuses them across events." },
            { PassUnroll, "Loop unrolling", "Fully unrolls counted loops with a small constant trip count." },
            { PassEvents, "Empty per-frame events", "Stops exporting Update, LateUpdate, FixedUpdate and PostLateUpdate when they do nothing, so the behaviour leaves the per-frame loop." },
            { PassHeap, "Heap compaction", "Removes heap slots nothing uses, merges duplicate constants, and lets temporaries that are never live at the same time share one slot." },
        };

        bool SameEntries(const Program& a, const Program& b)
        {
            size_t j = 0;
            for (const EntryPoint& e : a.entries)
            {
                if (j < b.entries.size() && b.entries[j].name == e.name)
                {
                    ++j;
                    continue;
                }
                if (!IsFrameEvent(e.name)) return false;
            }
            return j == b.entries.size();
        }
    }

    std::span<const PassInfo> Passes() { return Infos; }

    const char* Version() { return "0.2.0"; }

    Result Optimize(const Program& input, const Options& options)
    {
        Result result;
        result.program = input;
        result.stats.instructionsBefore = result.stats.instructionsAfter = CountInstructions(input);
        result.stats.externsBefore = result.stats.externsAfter = CountExterns(input);

        std::string error;
        Program current = input;
        std::optional<Model> model = Decode(current, error);
        if (!model)
        {
            result.message = "not optimized: " + error;
            return result;
        }

        Stats stats = result.stats;
        std::string warnings;
        int growthLimit = result.stats.instructionsBefore * 3 / 2 + 64;

        for (int round = 0; round < options.maxRounds; ++round)
        {
            bool any = false;
            for (const PassEntry& pass : Pipeline)
            {
                if ((options.passes & pass.flags) == 0) continue;

                Stats trial = stats;
                PassContext ctx{ options, trial, growthLimit };
                Program next;
                try
                {
                    IR ir = Build(current, *model);
                    if (!pass.run(ir, *model, ctx)) continue;
                    next = ir.Emit();
                }
                catch (const std::exception& e)
                {
                    warnings += std::format("{} skipped: {}; ", pass.name, e.what());
                    continue;
                }

                std::optional<Model> verified = Decode(next, error);
                if (!verified || !SameEntries(current, next))
                {
                    warnings += std::format("{} skipped: output did not verify ({}); ", pass.name, error);
                    continue;
                }
                current = std::move(next);
                model = std::move(verified);
                model->program = &current;
                stats = trial;
                any = true;
            }
            stats.rounds = round + 1;
            if (!any) break;
        }

        stats.heapBefore = stats.heapAfter = int(input.heap.size());
        if ((options.passes & PassHeap) != 0)
        {
            Stats trial = stats;
            std::optional<Program> compact;
            try
            {
                compact = CompactHeap(current, *model, trial);
            }
            catch (const std::exception& e)
            {
                warnings += std::format("heap skipped: {}; ", e.what());
            }
            if (compact)
            {
                if (Decode(*compact, error) && SameEntries(current, *compact))
                {
                    current = std::move(*compact);
                    stats = trial;
                    stats.heapBefore = int(input.heap.size());
                }
                else
                    warnings += std::format("heap skipped: output did not verify ({}); ", error);
            }
        }
        stats.heapAfter = int(current.heap.size());

        stats.instructionsAfter = CountInstructions(current);
        stats.externsAfter = CountExterns(current);
        result.optimized = current.code != input.code;
        result.program = std::move(current);
        result.stats = stats;
        result.message = warnings.empty() ? (result.optimized ? "optimized" : "nothing to optimize") : warnings;
        return result;
    }
}
