using System;
using System.Collections.Generic;
using System.IO;
using System.Security.Cryptography;
using UnityEditor;
using UnityEngine;
using VRC.Udon;

namespace Magma.VRC.UdonQuic
{
    /// <summary>What happened to a program the last time UdonQuic looked at it.</summary>
    internal enum QuicStatus
    {
        NotOptimized = 0,
        Optimized = 1,
        Unchanged = 2,
        Refused = 3,
        Excluded = 4,
        Error = 5,
    }

    /// <summary>The recorded outcome for one serialized program asset.</summary>
    [Serializable]
    internal sealed class QuicResult
    {
        /// <summary>The serialized program asset's GUID.</summary>
        public string Guid;
        public string AssetPath;
        public string SourceName;
        public string SourcePath;
        public string Compiler;
        public QuicStatus Status;
        public string Message;
        public NativeStats Stats;
        public string Version;
        public double DurationMs;

        /// <summary>UTC ticks.</summary>
        public long Timestamp;

        /// <summary>A hash of the input bytecode.</summary>
        public string Hash;
        public bool HasListings;
        public bool AnalysisOnly;
        public bool DebugInfoRemapped;

        /// <summary>The time of the result in local time.</summary>
        public DateTime Time => new DateTime(Timestamp, DateTimeKind.Utc).ToLocalTime();
    }

    /// <summary>The results of every program UdonQuic optimized or refused, persisted in Library/UdonQuic/results.json.</summary>
    internal static class QuicResults
    {
        [Serializable]
        private sealed class Index
        {
            public List<QuicResult> Results = new List<QuicResult>();
        }

        private static readonly string Folder = Path.Combine("Library", "UdonQuic");
        private static readonly string IndexPath = Path.Combine(Folder, "results.json");
        private static readonly string ListingFolder = Path.Combine(Folder, "listings");
        private static Dictionary<string, QuicResult> _results;
        private static bool _unsaved;

        /// <summary>Raised after recorded results were saved.</summary>
        public static event Action Changed;

        /// <summary>Every recorded result.</summary>
        public static IEnumerable<QuicResult> All => Results.Values;

        private static Dictionary<string, QuicResult> Results
        {
            get
            {
                if (_results != null) return _results;
                _results = new Dictionary<string, QuicResult>();
                try
                {
                    if (File.Exists(IndexPath))
                        foreach (QuicResult result in JsonUtility.FromJson<Index>(File.ReadAllText(IndexPath))?.Results ?? new List<QuicResult>())
                            if (!string.IsNullOrEmpty(result?.Guid)) _results[result.Guid] = result;
                }
                catch (Exception e)
                {
                    Debug.LogWarning($"[UdonQuic] Could not read {IndexPath}; starting a new result history: {e.Message}");
                }
                return _results;
            }
        }

        /// <summary>The result recorded for a serialized program asset GUID, or null.</summary>
        public static QuicResult Get(string guid) => guid != null && Results.TryGetValue(guid, out QuicResult result) ? result : null;

        /// <summary>The listing file of a program before or after optimization.</summary>
        public static string ListingPath(string guid, bool after) => Path.Combine(ListingFolder, $"{guid}.{(after ? "after" : "before")}.txt");

        /// <summary>Records a result, replacing the program's previous one, and writes its listings when given; <see cref="Save"/> persists the index.</summary>
        public static void Record(QuicResult result, string before, string after)
        {
            if (string.IsNullOrEmpty(result?.Guid)) return;
            result.Timestamp = DateTime.UtcNow.Ticks;
            if (before != null)
            {
                try
                {
                    Directory.CreateDirectory(ListingFolder);
                    File.WriteAllText(ListingPath(result.Guid, false), before);
                    string afterPath = ListingPath(result.Guid, true);
                    if (after != null) File.WriteAllText(afterPath, after);
                    else if (File.Exists(afterPath)) File.Delete(afterPath);
                    result.HasListings = true;
                }
                catch (Exception e) when (e is IOException || e is UnauthorizedAccessException)
                {
                    Debug.LogWarning($"[UdonQuic] Could not write the listings of '{result.SourceName}': {e.Message}");
                }
            }
            else
            {
                result.HasListings = Get(result.Guid)?.HasListings == true && File.Exists(ListingPath(result.Guid, false));
            }

            Results[result.Guid] = result;
            _unsaved = true;
        }

        /// <summary>Forgets every result and deletes the listings.</summary>
        public static void Clear()
        {
            Results.Clear();
            try
            {
                if (Directory.Exists(ListingFolder)) Directory.Delete(ListingFolder, true);
            }
            catch (Exception e) when (e is IOException || e is UnauthorizedAccessException)
            {
            }
            Flush();
        }

        /// <summary>A short hash of a program's bytecode.</summary>
        public static string Hash(byte[] code)
        {
            if (code == null) return "";
            using var md5 = MD5.Create();
            return BitConverter.ToString(md5.ComputeHash(code)).Replace("-", "");
        }

        /// <summary>Writes the index if results were recorded since the last save and raises <see cref="Changed"/>.</summary>
        public static void Save()
        {
            if (_unsaved) Flush();
        }

        private static void Flush()
        {
            _unsaved = false;
            try
            {
                Directory.CreateDirectory(Folder);
                var index = new Index { Results = new List<QuicResult>(Results.Values) };
                string partial = IndexPath + ".tmp";
                File.WriteAllText(partial, JsonUtility.ToJson(index));
                File.Copy(partial, IndexPath, true);
                File.Delete(partial);
            }
            catch (Exception e) when (e is IOException || e is UnauthorizedAccessException)
            {
                Debug.LogWarning($"[UdonQuic] Could not write {IndexPath}: {e.Message}");
            }
            Changed?.Invoke();
        }
    }

    /// <summary>Finds what compiled a serialized program and recompiles it.</summary>
    internal static class QuicPrograms
    {
        /// <summary>The GUID of a serialized program asset, or null when it is not an asset.</summary>
        public static string Guid(AbstractSerializedUdonProgramAsset asset) =>
            asset != null && AssetDatabase.TryGetGUIDAndLocalFileIdentifier(asset, out string guid, out long _) ? guid : null;

        /// <summary>The serialized program asset with a GUID, or null.</summary>
        public static AbstractSerializedUdonProgramAsset Load(string guid)
        {
            string path = AssetDatabase.GUIDToAssetPath(guid);
            return string.IsNullOrEmpty(path) ? null : AssetDatabase.LoadAssetAtPath<AbstractSerializedUdonProgramAsset>(path);
        }

        /// <summary>The program source a serialized program asset was compiled from, or null.</summary>
        public static AbstractUdonProgramSource Source(AbstractSerializedUdonProgramAsset asset)
        {
            if (asset == null) return null;
            string path = AssetDatabase.GUIDToAssetPath(asset.name);
            return string.IsNullOrEmpty(path) ? null : AssetDatabase.LoadAssetAtPath<AbstractUdonProgramSource>(path);
        }

        /// <summary>The compiler that produced a program source.</summary>
        public static string Compiler(AbstractUdonProgramSource source)
        {
            if (source == null) return "Other";
            if (source is UdonSharp.UdonSharpProgramAsset) return "UdonSharp";
            string type = source.GetType().FullName ?? "";
            if (type.IndexOf("Luau", StringComparison.OrdinalIgnoreCase) >= 0) return "UdonLuau";
            if (type.IndexOf("UdonGraph", StringComparison.Ordinal) >= 0) return "Udon Graph";
            if (type.IndexOf("UdonAssembly", StringComparison.Ordinal) >= 0) return "Udon Assembly";
            return "Other";
        }

        /// <summary>A result describing a program, with its identity filled in.</summary>
        public static QuicResult Describe(AbstractSerializedUdonProgramAsset asset)
        {
            AbstractUdonProgramSource source = Source(asset);
            return new QuicResult
            {
                Guid = Guid(asset),
                AssetPath = AssetDatabase.GetAssetPath(asset),
                SourceName = source != null ? source.name : asset.name,
                SourcePath = source != null ? AssetDatabase.GetAssetPath(source) : null,
                Compiler = Compiler(source),
                Version = Native.IsLoaded ? QuicProgram.Version : null,
            };
        }

        /// <summary>The display name of a serialized program asset GUID.</summary>
        public static string SourceLabel(string guid)
        {
            QuicResult result = QuicResults.Get(guid);
            if (result != null) return result.SourceName;
            AbstractUdonProgramSource source = Source(Load(guid));
            return source != null ? source.name : AssetDatabase.GUIDToAssetPath(guid);
        }

        /// <summary>Excludes or includes a program and recompiles it so the plain or optimized output comes back.</summary>
        public static void SetExcluded(string guid, bool excluded)
        {
            if (QuicSettings.IsExcluded(guid) == excluded) return;
            QuicProjectSettings.instance.SetExcluded(guid, excluded);
            AbstractSerializedUdonProgramAsset asset = Load(guid);
            if (asset != null) Recompile(asset);
        }

        /// <summary>Recompiles one program; UdonSharp programs only compile all together, so that asks first.</summary>
        public static void Recompile(AbstractSerializedUdonProgramAsset asset)
        {
            QuicPostCompiler.Forget(asset);
            AbstractUdonProgramSource source = Source(asset);
            if (source != null && !(source is UdonSharp.UdonSharpProgramAsset))
            {
                source.RefreshProgram();
                return;
            }

            string what = source == null ? "UdonQuic cannot tell which compiler produced this program." : "UdonSharp compiles every program together.";
            if (EditorUtility.DisplayDialog("UdonQuic", $"{what} Recompile every UdonSharp and UdonLuau program now?", "Recompile All", "Later"))
                QuicPostCompiler.RecompileAll();
        }
    }
}
