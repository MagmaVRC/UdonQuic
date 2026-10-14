using System;
using System.Collections;
using System.Collections.Generic;
using System.Reflection;
using UdonSharp;
using UdonSharp.Compiler;

namespace Magma.VRC.UdonQuic
{
    /// <summary>Rewrites UdonSharp's debug information so runtime errors in optimized programs point at the right source lines.</summary>
    internal static class QuicDebugInfo
    {
        private const BindingFlags Members = BindingFlags.Instance | BindingFlags.NonPublic | BindingFlags.Public;
        private static readonly Type CacheType = typeof(UdonSharpProgramAsset).Assembly.GetType("UdonSharp.UdonSharpEditorCache");
        private static readonly FieldInfo MethodsField = typeof(AssemblyDebugInfo).GetField("_methodDebugInfos", Members);
        private static readonly FieldInfo CallSitesField = typeof(AssemblyDebugInfo).GetField("_callSiteLookup", Members);

        /// <summary>Maps every instruction of the optimized program to the source position of the instruction it came from.</summary>
        /// <param name="originalLength">The byte length of the program that was optimized, used to recognize debug information that belongs to it.</param>
        /// <param name="codeLength">The byte length of the optimized program.</param>
        /// <param name="problem">Why nothing was remapped, when it was not.</param>
        /// <returns>Whether any debug information was rewritten.</returns>
        public static bool Remap(UdonSharpProgramAsset source, int originalLength, int codeLength, InstructionOrigin[] origins, out string problem)
        {
            problem = null;
            if (origins == null)
            {
                problem = "the loaded optimizer does not report instruction origins";
                return false;
            }
            if (CacheType == null || MethodsField == null)
            {
                problem = "this UdonSharp version stores debug information differently";
                return false;
            }

            object cache = CacheType.GetProperty("Instance", BindingFlags.Static | BindingFlags.Public | BindingFlags.NonPublic)?.GetValue(null);
            MethodInfo get = CacheType.GetMethod("GetDebugInfo", Members);
            MethodInfo set = CacheType.GetMethod("SetDebugInfo", Members);
            Type kinds = CacheType.GetNestedType("DebugInfoType", BindingFlags.Public | BindingFlags.NonPublic);
            if (cache == null || get == null || set == null || kinds == null)
            {
                problem = "this UdonSharp version stores debug information differently";
                return false;
            }

            var done = new HashSet<AssemblyDebugInfo>();
            foreach (object kind in Enum.GetValues(kinds))
            {
                if (!(get.Invoke(cache, new[] { source, kind }) is AssemblyDebugInfo info) || done.Contains(info)) continue;
                if (!(MethodsField.GetValue(info) is List<MethodDebugInfo> methods) || End(methods) != originalLength) continue;

                MethodsField.SetValue(info, Rebuild(info, origins, codeLength));
                (CallSitesField?.GetValue(info) as IDictionary)?.Clear();
                set.Invoke(cache, new[] { source, kind, info });
                done.Add(info);
            }

            if (done.Count == 0) problem = "no UdonSharp debug information matches the compiled program";
            return done.Count > 0;
        }

        private static uint End(List<MethodDebugInfo> methods)
        {
            uint end = 0;
            foreach (MethodDebugInfo method in methods) end = Math.Max(end, method.methodEndAddress);
            return end;
        }

        private static List<MethodDebugInfo> Rebuild(AssemblyDebugInfo info, InstructionOrigin[] origins, int codeLength)
        {
            var methods = new List<MethodDebugInfo>();
            MethodDebugInfo current = null;
            string file = null, method = null;
            int line = 0, character = 0;
            bool known = false;

            foreach (InstructionOrigin origin in origins)
            {
                if (origin.Origin != InstructionOrigin.None)
                {
                    info.GetPositionFromProgramCounter((int)origin.Origin, out file, out method, out line, out character);
                    known = method != null;
                }
                if (!known)
                {
                    Close(current, origin.Address);
                    current = null;
                    continue;
                }

                if (current == null || current.methodName != method || current.containingFilePath != file)
                {
                    Close(current, origin.Address);
                    current = new MethodDebugInfo
                    {
                        methodName = method,
                        containingFilePath = file,
                        methodStartAddress = origin.Address,
                        debugMarkers = new List<MethodDebugMarker>(),
                    };
                    methods.Add(current);
                }

                List<MethodDebugMarker> markers = current.debugMarkers;
                if (markers.Count == 0 || markers[markers.Count - 1].startLine != line || markers[markers.Count - 1].startChar != character)
                    markers.Add(new MethodDebugMarker { startInstruction = (int)origin.Address, startLine = line, startChar = character });
            }
            Close(current, (uint)codeLength);
            return methods;
        }

        private static void Close(MethodDebugInfo method, uint end)
        {
            if (method != null) method.methodEndAddress = end;
        }
    }
}
