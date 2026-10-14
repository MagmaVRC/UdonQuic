using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.IO;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text;
using UnityEditor;

namespace Magma.VRC.UdonQuic
{
    internal enum ValueKind
    {
        None = 0,
        Boolean = 1,
        Integer = 2,
        Unsigned = 3,
        Real = 4,
        String = 5,
        UintArray = 6,
    }

    [Flags]
    internal enum SlotFlags : uint
    {
        None = 0,
        Initialized = 1,
        Exported = 2,
        Synced = 4,
        Private = 8,
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct NativeValue
    {
        public int Kind;
        public int Boolean;
        public long Integer;
        public ulong Unsigned;
        public double Real;
        public IntPtr Text;
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct NativeSlot
    {
        public IntPtr Symbol;
        public IntPtr Type;
        public uint Flags;
        public int Rewritten;
        public NativeValue Value;
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct NativeEntryPoint
    {
        public IntPtr Name;
        public uint Address;
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct NativeOptions
    {
        public uint Passes;
        public int InlineBudget;
        public int MaxRounds;
    }

    /// <summary>What one optimization did.</summary>
    [Serializable]
    [StructLayout(LayoutKind.Sequential)]
    internal struct NativeStats
    {
        public int InstructionsBefore;
        public int InstructionsAfter;
        public int ExternsBefore;
        public int ExternsAfter;
        public int Inlined;
        public int TailCalls;
        public int Folded;
        public int CopiesRemoved;
        public int BranchesSimplified;
        public int DeadStores;
        public int Rounds;
        public int Hoisted;
        public int LoopsRotated;
        public int ChecksRemoved;
        public int SwitchesBuilt;
        public int ConcatsMerged;
        public int Fused;
        public int Memoized;
        public int Unrolled;
        public int EventsRemoved;
        public int Propagated;
        public int HeapBefore;
        public int HeapAfter;
    }

    /// <summary>An optimizer pass as the native library describes it.</summary>
    internal readonly struct PassInfo
    {
        public PassInfo(uint bit, string name, string description)
        {
            Bit = bit;
            Name = name;
            Description = description;
        }

        public uint Bit { get; }
        public string Name { get; }
        public string Description { get; }
    }

    /// <summary>A rewritten instruction's byte address and the byte address of the input instruction it came from.</summary>
    internal readonly struct InstructionOrigin
    {
        /// <summary>The origin of an instruction the optimizer created.</summary>
        public const uint None = 0xFFFFFFFF;

        public InstructionOrigin(uint address, uint origin)
        {
            Address = address;
            Origin = origin;
        }

        public uint Address { get; }
        public uint Origin { get; }
    }

    internal abstract class NativeHandle : IDisposable
    {
        private readonly int _generation;

        protected NativeHandle(IntPtr ptr)
        {
            Ptr = ptr;
            _generation = Native.Generation;
        }

        public IntPtr Ptr { get; private set; }

        public bool IsInvalid => Ptr == IntPtr.Zero;

        public static implicit operator IntPtr(NativeHandle handle) => handle?.Ptr ?? IntPtr.Zero;

        public void Dispose()
        {
            if (Ptr != IntPtr.Zero && Native.IsLoaded && Native.Generation == _generation) Release(Ptr);
            Ptr = IntPtr.Zero;
        }

        protected abstract void Release(IntPtr ptr);
    }

    internal sealed class ProgramHandle : NativeHandle
    {
        public ProgramHandle(IntPtr ptr) : base(ptr) { }

        protected override void Release(IntPtr ptr) => Native.uq_program_destroy(ptr);
    }

    internal sealed class ResultHandle : NativeHandle
    {
        public ResultHandle(IntPtr ptr) : base(ptr) { }

        protected override void Release(IntPtr ptr) => Native.uq_result_destroy(ptr);
    }

    /// <summary>Loads a private copy of UdonQuic.dll so a new build can replace the package's file while the editor runs.</summary>
    [InitializeOnLoad]
    internal static class Native
    {
        private const string PluginGuid = "ade05cd71a174b278e3126ca6885ae72";

        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate IntPtr CreateProgram(byte[] bytecode, UIntPtr length);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void Destroy(IntPtr handle);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void AddSlot(IntPtr program, byte[] symbol, byte[] type, uint flags, NativeValue[] value);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate int SetSlotArray(IntPtr program, uint address, uint[] values, int count);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void AddEntry(IntPtr program, byte[] name, uint address);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void AddExtern(IntPtr program, byte[] signature, int arity, int hasResult, ulong written);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void DefaultOptions(out NativeOptions options);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate IntPtr Optimize(IntPtr program, ref NativeOptions options);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate int ResultInt(IntPtr result);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate IntPtr ResultText(IntPtr result);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate IntPtr GetBytecode(IntPtr result, out UIntPtr length);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate int GetSlot(IntPtr result, int address, out NativeSlot slot);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate int GetSlotArray(IntPtr result, int address, out IntPtr values);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate int GetEntry(IntPtr result, int index, out NativeEntryPoint entry);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void GetStats(IntPtr result, ref NativeStats stats);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate IntPtr GetText();
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate int GetCount();
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate int GetPass(int index, out uint bit, out IntPtr name, out IntPtr description);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate int GetOrigin(IntPtr result, int index, out uint address, out uint origin);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate int GetSlotSource(IntPtr result, int address, out uint source);

        private static readonly HashSet<string> Optional = new HashSet<string>
        {
            nameof(uq_version), nameof(uq_pass_count), nameof(uq_pass_info), nameof(uq_result_origin_count), nameof(uq_result_origin), nameof(uq_result_slot_source),
        };

        public static CreateProgram uq_program_create;
        public static Destroy uq_program_destroy;
        public static AddSlot uq_program_add_slot;
        public static SetSlotArray uq_program_set_slot_array;
        public static AddEntry uq_program_add_entry;
        public static AddExtern uq_program_add_extern;
        public static DefaultOptions uq_options_default;
        public static Optimize uq_optimize;
        public static Destroy uq_result_destroy;
        public static ResultInt uq_result_succeeded;
        public static ResultText uq_result_message;
        public static GetBytecode uq_result_bytecode;
        public static ResultInt uq_result_slot_count;
        public static GetSlot uq_result_slot;
        public static GetSlotArray uq_result_slot_array;
        public static ResultInt uq_result_entry_count;
        public static GetEntry uq_result_entry;
        public static GetStats uq_result_stats;
        public static ResultText uq_result_disassembly;
        public static ResultText uq_program_disassembly;
        public static GetText uq_version;
        public static GetCount uq_pass_count;
        public static GetPass uq_pass_info;
        public static ResultInt uq_result_origin_count;
        public static GetOrigin uq_result_origin;
        public static GetSlotSource uq_result_slot_source;

        private static IntPtr _module;

        static Native()
        {
            AssemblyReloadEvents.beforeAssemblyReload += Unload;
            EditorApplication.quitting += Unload;
        }

        public static int Generation { get; private set; }

        public static bool IsLoaded => _module != IntPtr.Zero;

        /// <summary>The private copy that is loaded, or null.</summary>
        public static string LoadedPath { get; private set; }

        public static byte[] Utf8(string text) => text == null ? null : Encoding.UTF8.GetBytes(text + "\0");

        public static string Read(IntPtr text) => text == IntPtr.Zero ? null : Marshal.PtrToStringUTF8(text);

        /// <summary>Copies the package's UdonQuic.dll under Library/UdonQuic and binds its exports.</summary>
        /// <exception cref="DllNotFoundException">The library is missing, cannot be loaded or lacks an export.</exception>
        public static void Load()
        {
            if (IsLoaded) return;

            string source = LocateLibrary();
            if (source == null || !File.Exists(source)) throw new DllNotFoundException($"UdonQuic.dll was not found at {source ?? "the package's Editor/Plugins/x86_64 folder"}");

            byte[] bytes = File.ReadAllBytes(source);
            string hash;
            using (var sha = SHA256.Create()) hash = BitConverter.ToString(sha.ComputeHash(bytes), 0, 8).Replace("-", "");

            string folder = Path.GetFullPath(Path.Combine("Library", "UdonQuic"));
            Directory.CreateDirectory(folder);
            string copy = Path.Combine(folder, $"UdonQuic-{hash}.dll");
            if (!File.Exists(copy))
            {
                string partial = copy + ".tmp";
                File.WriteAllBytes(partial, bytes);
                File.Move(partial, copy);
            }
            DeleteStaleCopies(folder, copy);

            IntPtr module = LoadLibraryW(copy);
            if (module == IntPtr.Zero) throw new DllNotFoundException($"{copy} could not be loaded: {new Win32Exception(Marshal.GetLastWin32Error()).Message}");

            string missing = null;
            foreach (FieldInfo field in typeof(Native).GetFields(BindingFlags.Public | BindingFlags.Static))
            {
                if (!field.Name.StartsWith("uq_", StringComparison.Ordinal)) continue;
                IntPtr export = GetProcAddress(module, field.Name);
                if (export == IntPtr.Zero && !Optional.Contains(field.Name)) missing ??= field.Name;
                field.SetValue(null, export == IntPtr.Zero ? null : Marshal.GetDelegateForFunctionPointer(export, field.FieldType));
            }

            if (missing != null)
            {
                foreach (FieldInfo field in typeof(Native).GetFields(BindingFlags.Public | BindingFlags.Static))
                    if (field.Name.StartsWith("uq_", StringComparison.Ordinal)) field.SetValue(null, null);
                FreeLibrary(module);
                throw new DllNotFoundException($"{source} does not export {missing}");
            }

            _module = module;
            LoadedPath = copy;
            Generation++;
        }

        /// <summary>The loaded library's version, or null when it does not report one.</summary>
        public static string Version => uq_version == null ? null : Read(uq_version());

        /// <summary>The loaded library's passes, empty when it does not describe them.</summary>
        public static List<PassInfo> Passes()
        {
            var passes = new List<PassInfo>();
            if (uq_pass_count == null || uq_pass_info == null) return passes;
            int count = uq_pass_count();
            for (int i = 0; i < count; i++)
                if (uq_pass_info(i, out uint bit, out IntPtr name, out IntPtr description) != 0) passes.Add(new PassInfo(bit, Read(name), Read(description)));
            return passes;
        }

        /// <summary>Where each rewritten instruction came from, in address order, or null when the library does not report it.</summary>
        public static InstructionOrigin[] Origins(IntPtr result)
        {
            if (uq_result_origin_count == null || uq_result_origin == null) return null;
            var origins = new InstructionOrigin[Math.Max(0, uq_result_origin_count(result))];
            for (int i = 0; i < origins.Length; i++)
            {
                if (uq_result_origin(result, i, out uint address, out uint origin) == 0) return null;
                origins[i] = new InstructionOrigin(address, origin);
            }
            return origins;
        }

        /// <summary>Frees the loaded copy.</summary>
        public static void Unload()
        {
            if (!IsLoaded) return;
            foreach (FieldInfo field in typeof(Native).GetFields(BindingFlags.Public | BindingFlags.Static))
                if (field.Name.StartsWith("uq_", StringComparison.Ordinal)) field.SetValue(null, null);
            FreeLibrary(_module);
            _module = IntPtr.Zero;
            LoadedPath = null;
        }

        private static string LocateLibrary()
        {
            var package = UnityEditor.PackageManager.PackageInfo.FindForAssembly(typeof(Native).Assembly);
            if (package != null) return Path.Combine(package.resolvedPath, "Editor", "Plugins", "x86_64", "UdonQuic.dll");

            string asset = AssetDatabase.GUIDToAssetPath(PluginGuid);
            return string.IsNullOrEmpty(asset) ? null : Path.GetFullPath(asset);
        }

        private static void DeleteStaleCopies(string folder, string keep)
        {
            foreach (string file in Directory.GetFiles(folder, "UdonQuic-*.dll"))
            {
                if (string.Equals(file, keep, StringComparison.OrdinalIgnoreCase)) continue;
                try
                {
                    File.Delete(file);
                }
                catch (IOException)
                {
                }
                catch (UnauthorizedAccessException)
                {
                }
            }
        }

        [DllImport("kernel32", CharSet = CharSet.Unicode, SetLastError = true)]
        private static extern IntPtr LoadLibraryW(string path);

        [DllImport("kernel32", CharSet = CharSet.Ansi, ExactSpelling = true)]
        private static extern IntPtr GetProcAddress(IntPtr module, string name);

        [DllImport("kernel32")]
        private static extern bool FreeLibrary(IntPtr module);
    }
}
