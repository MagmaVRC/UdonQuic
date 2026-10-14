using System;
using System.Collections;
using System.Collections.Generic;
using System.Diagnostics;
using System.Linq;
using System.Reflection;
using UnityEditor;
using UnityEngine;
using UnityEngine.SceneManagement;
using VRC.SDKBase.Editor.BuildPipeline;
using VRC.Udon;
using VRC.Udon.Common.Interfaces;
using Debug = UnityEngine.Debug;

namespace Magma.VRC.UdonQuic
{
    /// <summary>Optimizes Udon programs right after any compiler stores them, so the optimized program is the compiled output.</summary>
    [InitializeOnLoad]
    internal static class QuicPostCompiler
    {
        private const double SweepInterval = 0.5;
        private static readonly Dictionary<int, int> Handled = new Dictionary<int, int>();
        private static double _nextSweep;

        static QuicPostCompiler()
        {
            EditorApplication.update += Tick;
        }

        private static void Tick()
        {
            if (EditorApplication.timeSinceStartup < _nextSweep) return;
            _nextSweep = EditorApplication.timeSinceStartup + SweepInterval;
            if (EditorApplication.isCompiling || EditorApplication.isUpdating || EditorApplication.isPlayingOrWillChangePlaymode || BuildPipeline.isBuildingPlayer) return;
            Sweep();
        }

        /// <summary>Optimizes every loaded program asset that changed since UdonQuic last looked at it.</summary>
        public static void Sweep()
        {
            if (!QuicSettings.Enabled) return;
            var changed = new List<AbstractSerializedUdonProgramAsset>();
            foreach (AbstractSerializedUdonProgramAsset asset in Resources.FindObjectsOfTypeAll<AbstractSerializedUdonProgramAsset>())
            {
                if (asset == null || !EditorUtility.IsPersistent(asset) || !EditorUtility.IsDirty(asset)) continue;
                if (Handled.TryGetValue(asset.GetInstanceID(), out int count) && count == EditorUtility.GetDirtyCount(asset)) continue;
                changed.Add(asset);
            }
            if (changed.Count > 0) Optimize(changed, "compile");
        }

        /// <summary>The serialized programs the scene's UdonBehaviours run, each once.</summary>
        public static List<AbstractSerializedUdonProgramAsset> CollectPrograms(Scene scene)
        {
            var programs = new List<AbstractSerializedUdonProgramAsset>();
            var seen = new HashSet<AbstractSerializedUdonProgramAsset>();
            if (!scene.IsValid() || !scene.isLoaded) return programs;
            foreach (GameObject root in scene.GetRootGameObjects())
            {
                foreach (UdonBehaviour behaviour in root.GetComponentsInChildren<UdonBehaviour>(true))
                {
                    AbstractSerializedUdonProgramAsset asset = behaviour.programSource == null ? null : behaviour.programSource.SerializedProgramAsset;
                    if (asset != null && seen.Add(asset)) programs.Add(asset);
                }
            }
            return programs;
        }

        /// <summary>The serialized programs used by every loaded scene, each once.</summary>
        public static List<AbstractSerializedUdonProgramAsset> CollectLoadedPrograms()
        {
            var programs = new List<AbstractSerializedUdonProgramAsset>();
            var seen = new HashSet<AbstractSerializedUdonProgramAsset>();
            for (int i = 0; i < SceneManager.sceneCount; i++)
                foreach (AbstractSerializedUdonProgramAsset asset in CollectPrograms(SceneManager.GetSceneAt(i)))
                    if (seen.Add(asset)) programs.Add(asset);
            return programs;
        }

        /// <summary>Optimizes and stores in place every program that is not optimized yet, logging one line for the batch.</summary>
        /// <returns>How many programs were stored.</returns>
        public static int Optimize(IEnumerable<AbstractSerializedUdonProgramAsset> assets, string reason)
        {
            int optimized = 0, refused = 0, cached = 0, instructionsBefore = 0, instructionsAfter = 0, externsBefore = 0, externsAfter = 0;
            var watch = Stopwatch.StartNew();
            var pending = new List<Pending>();
            foreach (AbstractSerializedUdonProgramAsset asset in assets)
            {
                QuicStatus status = Begin(asset, out Pending work);
                if (work != null) pending.Add(work);
                else if (status == QuicStatus.Error) refused++;
            }

            QuicProgram.RunAll(pending.Select(work => work.Job));

            foreach (Pending work in pending)
            {
                QuicStatus status = Complete(work, out NativeStats stats);
                if (status == QuicStatus.Refused || status == QuicStatus.Error) refused++;
                if (status != QuicStatus.Optimized) continue;
                optimized++;
                if (work.Job.Cached) cached++;
                instructionsBefore += stats.InstructionsBefore;
                instructionsAfter += stats.InstructionsAfter;
                externsBefore += stats.ExternsBefore;
                externsAfter += stats.ExternsAfter;
            }
            QuicResults.Save();

            if (optimized + refused > 0)
                Debug.Log($"[UdonQuic] Optimized {optimized} programs after {reason}" + (cached > 0 ? $" ({cached} from cache)" : "") + (refused > 0 ? $", {refused} left unoptimized" : "") +
                          $"; instructions {instructionsBefore} -> {instructionsAfter}, externs {externsBefore} -> {externsAfter}; {watch.Elapsed.TotalSeconds:0.00} s.");
            return optimized;
        }

        /// <summary>Runs the optimizer on every plain program without storing anything, recording what it would do.</summary>
        /// <returns>How many plain programs were analyzed.</returns>
        public static int Analyze(IEnumerable<AbstractSerializedUdonProgramAsset> assets)
        {
            var pending = new List<(AbstractSerializedUdonProgramAsset Asset, IUdonProgram Program, QuicJob Job)>();
            foreach (AbstractSerializedUdonProgramAsset asset in assets)
            {
                IUdonProgram program;
                try
                {
                    program = asset.RetrieveProgram();
                }
                catch (Exception)
                {
                    continue;
                }
                if (program == null || QuicProgram.IsOptimized(program)) continue;
                pending.Add((asset, program, QuicProgram.Prepare(program)));
            }

            QuicProgram.RunAll(pending.Select(work => work.Job));

            int analyzed = 0;
            foreach (var (asset, program, job) in pending)
            {
                QuicResult result = QuicPrograms.Describe(asset);
                var details = new QuicDetails();
                bool succeeded = QuicProgram.Finish(job, out _, out string message, out NativeStats stats, details);
                result.DurationMs = job.Milliseconds;
                result.Stats = stats;
                result.Hash = QuicResults.Hash(program.ByteCode);
                result.AnalysisOnly = true;
                result.Version = Native.IsLoaded ? QuicProgram.Version : null;
                result.Status = succeeded ? QuicStatus.Optimized : message == QuicProgram.Unchanged ? QuicStatus.Unchanged : QuicStatus.Refused;
                result.Message = succeeded ? "Analysis only: the stored program is plain." : message;
                QuicResults.Record(result, details.Before, succeeded ? details.After : result.Status == QuicStatus.Unchanged ? details.Before : null);
                analyzed++;
            }
            QuicResults.Save();
            return analyzed;
        }

        private sealed class Pending
        {
            public AbstractSerializedUdonProgramAsset Asset;
            public string Label;
            public QuicResult Result;
            public QuicResult Previous;
            public IUdonProgram Program;
            public QuicJob Job;
        }

        private static QuicStatus Begin(AbstractSerializedUdonProgramAsset asset, out Pending work)
        {
            work = null;
            Remember(asset);
            string label = AssetLabel(asset);
            QuicResult result = QuicPrograms.Describe(asset);
            QuicResult previous = QuicResults.Get(result.Guid);

            IUdonProgram program;
            try
            {
                program = asset.RetrieveProgram();
            }
            catch (Exception e)
            {
                result.Status = QuicStatus.Error;
                result.Message = $"it could not be read ({e.Message})";
                result.Hash = "unreadable";
                if (previous?.Status != QuicStatus.Error || previous.Hash != result.Hash) Debug.LogWarning($"[UdonQuic] Left {label} unoptimized: {result.Message}.");
                QuicResults.Record(result, null, null);
                return result.Status;
            }

            if (program == null || QuicProgram.IsOptimized(program)) return QuicStatus.NotOptimized;

            result.Hash = QuicResults.Hash(program.ByteCode);
            if (QuicSettings.IsExcluded(result.Guid))
            {
                result.Status = QuicStatus.Excluded;
                result.Message = "Excluded in Project Settings > UdonQuic; stored as compiled.";
                if (previous?.Status != QuicStatus.Excluded || previous.Hash != result.Hash) QuicResults.Record(result, null, null);
                return result.Status;
            }

            work = new Pending { Asset = asset, Label = label, Result = result, Previous = previous, Program = program, Job = QuicProgram.Prepare(program) };
            return QuicStatus.NotOptimized;
        }

        private static QuicStatus Complete(Pending work, out NativeStats stats)
        {
            AbstractSerializedUdonProgramAsset asset = work.Asset;
            string label = work.Label;
            QuicResult result = work.Result;
            QuicResult previous = work.Previous;
            IUdonProgram program = work.Program;

            var details = new QuicDetails();
            bool succeeded = QuicProgram.Finish(work.Job, out IUdonProgram rewritten, out string message, out stats, details);
            result.DurationMs = work.Job.Milliseconds;
            result.Stats = stats;
            result.Message = message;
            result.Version = Native.IsLoaded ? QuicProgram.Version : null;

            if (!succeeded)
            {
                result.Status = message == QuicProgram.Unchanged ? QuicStatus.Unchanged : QuicStatus.Refused;
                if (result.Status == QuicStatus.Refused && (previous?.Status != QuicStatus.Refused || previous.Hash != result.Hash))
                    Debug.LogWarning($"[UdonQuic] Left {label} unoptimized: {message}");
                QuicResults.Record(result, details.Before, result.Status == QuicStatus.Unchanged ? details.Before : null);
                return result.Status;
            }

            try
            {
                asset.StoreProgram(rewritten, asset.GetNetworkCallingMetadata());
                InvalidateEntrypointHashes(asset);
                EditorUtility.SetDirty(asset);
                Remember(asset);
            }
            catch (Exception e)
            {
                Debug.LogError($"[UdonQuic] Could not store {label}: {e.Message}");
                result.Status = QuicStatus.Error;
                result.Message = $"could not be stored: {e.Message}";
                QuicResults.Record(result, details.Before, details.After);
                return result.Status;
            }

            result.Status = QuicStatus.Optimized;
            if (QuicPrograms.Source(asset) is UdonSharp.UdonSharpProgramAsset udonSharp)
            {
                try
                {
                    result.DebugInfoRemapped = QuicDebugInfo.Remap(udonSharp, program.ByteCode.Length, rewritten.ByteCode.Length, details.Origins, out string problem);
                    if (!result.DebugInfoRemapped) result.Message = $"Runtime error lines are not remapped: {problem}.";
                }
                catch (Exception e)
                {
                    result.Message = $"Runtime error lines are not remapped: {e.GetType().Name}: {e.Message}";
                }
            }
            QuicResults.Record(result, details.Before, details.After);
            return result.Status;
        }

        /// <summary>Recompiles every UdonSharp and UdonLuau program, which UdonQuic then optimizes when it is enabled.</summary>
        [MenuItem("Tools/UdonQuic/Recompile All", false, 20)]
        public static void RecompileAll()
        {
            Handled.Clear();
            UdonSharp.UdonSharpProgramAsset.CompileAllCsPrograms(true);
            if (Type.GetType("Magma.VRC.UdonLuau.LuauEditorHooks, Magma.VRC.UdonLuau.Editor") != null) EditorApplication.ExecuteMenuItem("Tools/UdonLuau/Compile All Scripts");
        }

        /// <summary>Makes the next sweep look at a program again.</summary>
        public static void Forget(AbstractSerializedUdonProgramAsset asset)
        {
            if (asset != null) Handled.Remove(asset.GetInstanceID());
        }

        /// <summary>Optimizes every plain program in the project's program folder and the open scenes now.</summary>
        public static int OptimizeNow()
        {
            if (!QuicSettings.Enabled) return 0;
            Handled.Clear();
            int stored = Optimize(AllPrograms(), "Optimize Now");
            if (stored > 0) AssetDatabase.SaveAssets();
            return stored;
        }

        /// <summary>Every serialized program asset whose program source still exists, and every one the open scenes use, each once.</summary>
        public static List<AbstractSerializedUdonProgramAsset> AllPrograms()
        {
            var programs = new List<AbstractSerializedUdonProgramAsset>();
            var seen = new HashSet<AbstractSerializedUdonProgramAsset>();
            foreach (string guid in AssetDatabase.FindAssets("t:" + nameof(AbstractSerializedUdonProgramAsset)))
            {
                AbstractSerializedUdonProgramAsset asset = QuicPrograms.Load(guid);
                if (asset != null && QuicPrograms.Source(asset) != null && seen.Add(asset)) programs.Add(asset);
            }
            foreach (AbstractSerializedUdonProgramAsset asset in CollectLoadedPrograms())
                if (seen.Add(asset)) programs.Add(asset);
            return programs;
        }

        private static void Remember(AbstractSerializedUdonProgramAsset asset) => Handled[asset.GetInstanceID()] = EditorUtility.GetDirtyCount(asset);

        private static string AssetLabel(AbstractSerializedUdonProgramAsset asset)
        {
            string path = AssetDatabase.GetAssetPath(asset);
            return string.IsNullOrEmpty(path) ? $"'{asset.name}'" : $"'{path}'";
        }

        private static void InvalidateEntrypointHashes(AbstractSerializedUdonProgramAsset asset)
        {
            const BindingFlags flags = BindingFlags.Instance | BindingFlags.NonPublic | BindingFlags.Public;
            Type type = asset.GetType();
            try
            {
                (type.GetField("_entrypointHashToName", flags)?.GetValue(asset) as IDictionary)?.Clear();
                (type.GetField("_entrypointNameToHash", flags)?.GetValue(asset) as IDictionary)?.Clear();
                type.GetField("_entrypointHashesLoaded", flags)?.SetValue(asset, 0);
            }
            catch (Exception e)
            {
                Debug.LogWarning($"[UdonQuic] Could not reset the cached entry point hashes of '{asset.name}'; networked events may misreport in Play Mode: {e.Message}");
            }
        }
    }

    internal sealed class QuicSaveHook : AssetModificationProcessor
    {
        private static string[] OnWillSaveAssets(string[] paths)
        {
            if (!EditorApplication.isPlayingOrWillChangePlaymode) QuicPostCompiler.Sweep();
            return paths;
        }
    }

    internal sealed class QuicBuildCompile : IVRCSDKBuildRequestedCallback
    {
        public int callbackOrder => 101;

        public bool OnBuildRequested(VRCSDKRequestedBuildType requestedBuildType)
        {
            if (requestedBuildType == VRCSDKRequestedBuildType.Avatar || !QuicSettings.Enabled) return true;
            QuicPostCompiler.Sweep();
            if (QuicPostCompiler.Optimize(QuicPostCompiler.CollectLoadedPrograms(), "build request") > 0) AssetDatabase.SaveAssets();
            return true;
        }
    }

    [InitializeOnLoad]
    internal static class QuicPlayModeHook
    {
        static QuicPlayModeHook()
        {
            EditorApplication.playModeStateChanged += state =>
            {
                if (state != PlayModeStateChange.ExitingEditMode || !QuicSettings.Enabled) return;
                if (QuicPostCompiler.Optimize(QuicPostCompiler.CollectLoadedPrograms(), "entering Play Mode") > 0) AssetDatabase.SaveAssets();
            };
        }
    }
}
