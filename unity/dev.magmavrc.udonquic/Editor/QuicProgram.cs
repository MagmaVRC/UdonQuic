using System;
using System.Collections.Concurrent;
using System.Collections.Generic;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;
using VRC.Udon.Common;
using VRC.Udon.Common.Interfaces;
using VRC.Udon.Editor;

namespace Magma.VRC.UdonQuic
{
    /// <summary>What an optimization produced besides the program.</summary>
    internal sealed class QuicDetails
    {
        /// <summary>The listing of the program as given.</summary>
        public string Before;

        /// <summary>The listing of the rewritten program, or null when it was not rewritten.</summary>
        public string After;

        /// <summary>Where each rewritten instruction came from, or null when the library does not report it.</summary>
        public InstructionOrigin[] Origins;
    }

    /// <summary>One program on its way through <see cref="QuicProgram.Prepare"/>, <see cref="QuicProgram.Run"/> and <see cref="QuicProgram.Finish"/>.</summary>
    internal sealed class QuicJob
    {
        public IUdonProgram Source;
        public QuicOutput Output;
        public bool Cached;
        public double Milliseconds;
        internal ProgramHandle Program;
        internal NativeOptions Options;
        internal Dictionary<string, Type> Types;
        internal string Key;
    }

    /// <summary>Hands Udon programs to the native optimizer and rebuilds them from its result.</summary>
    internal static class QuicProgram
    {
        private const uint MaximumHeapSize = 1024 * 1024;
        private const uint Extern = 6;
        private const int WorkerStackSize = 64 * 1024 * 1024;

        /// <summary>The symbol of the slot that marks a program as optimized; it holds <see cref="Version"/>.</summary>
        public const string MarkerSymbol = "__const_quic_optimized";

        /// <summary>The message of a program the optimizer had nothing to change in.</summary>
        public const string Unchanged = "nothing to optimize";

        /// <summary>The optimizer version written into the marker slot.</summary>
        public static string Version => Native.Version ?? "0.1.0";

        private static readonly string[] PrivatePrefixes = { "__intnl_", "__gintnl_", "__lcl_", "__const_" };
        private static readonly Dictionary<Type, string> TypeNames = new Dictionary<Type, string>();

        /// <summary>Whether the program already went through the optimizer.</summary>
        public static bool IsOptimized(IUdonProgram program) => program?.SymbolTable != null && program.SymbolTable.TryGetAddressFromSymbol(MarkerSymbol, out _);

        /// <summary>Describes a program to the optimizer, or takes its result from the cache; main thread only.</summary>
        public static QuicJob Prepare(IUdonProgram source)
        {
            var job = new QuicJob { Source = source };
            var watch = Stopwatch.StartNew();
            if (IsOptimized(source))
            {
                job.Output = QuicOutput.Failed("the program is already optimized");
                return job;
            }
            try
            {
                Native.Load();
            }
            catch (Exception e) when (e is DllNotFoundException || e is EntryPointNotFoundException || e is BadImageFormatException || e is IOException)
            {
                job.Output = QuicOutput.Failed($"the native optimizer could not be loaded: {e.Message}");
                return job;
            }
            QuicCache.Prune();

            var allocations = new List<IntPtr>();
            job.Types = SeedTypes();
            job.Options = QuicSettings.Options();
            try
            {
                using var keyStream = new MemoryStream();
                using var key = new BinaryWriter(keyStream, Encoding.UTF8);
                ProgramHandle program = Describe(source, allocations, job.Types, key, out string message);
                if (program == null)
                {
                    job.Output = QuicOutput.Failed(message);
                    return job;
                }

                key.Write(job.Options.Passes);
                key.Write(job.Options.InlineBudget);
                key.Write(job.Options.MaxRounds);
                key.Write(Path.GetFileName(Native.LoadedPath) ?? "");
                key.Flush();
                job.Key = QuicCache.Key(keyStream.ToArray());
                if (QuicCache.TryLoad(job.Key, out QuicOutput cached))
                {
                    program.Dispose();
                    job.Output = cached;
                    job.Cached = true;
                }
                else
                    job.Program = program;
            }
            catch (Exception e)
            {
                job.Program?.Dispose();
                job.Program = null;
                job.Output = QuicOutput.Failed($"{e.GetType().Name}: {e.Message}");
            }
            finally
            {
                foreach (IntPtr allocation in allocations) Marshal.FreeHGlobal(allocation);
                job.Milliseconds += watch.Elapsed.TotalMilliseconds;
            }
            return job;
        }

        /// <summary>Runs the optimizer on a prepared job and caches the result; safe on any thread.</summary>
        public static void Run(QuicJob job)
        {
            if (job.Output != null) return;
            var watch = Stopwatch.StartNew();
            try
            {
                string before = Native.Read(Native.uq_program_disassembly(job.Program));
                using var native = new ResultHandle(Native.uq_optimize(job.Program, ref job.Options));
                if (native.IsInvalid)
                {
                    job.Output = QuicOutput.Failed("the optimizer returned no result", before);
                    return;
                }
                job.Output = QuicOutput.Capture(native, before);
                QuicCache.Store(job.Key, job.Output);
            }
            catch (InvalidDataException e)
            {
                job.Output = QuicOutput.Failed(e.Message);
            }
            catch (Exception e)
            {
                job.Output = QuicOutput.Failed($"{e.GetType().Name}: {e.Message}");
            }
            finally
            {
                job.Program.Dispose();
                job.Program = null;
                job.Milliseconds += watch.Elapsed.TotalMilliseconds;
            }
        }

        /// <summary>Runs every job that still needs the optimizer, in parallel, and returns when all are done.</summary>
        public static void RunAll(IEnumerable<QuicJob> jobs)
        {
            var pending = new ConcurrentQueue<QuicJob>(jobs.Where(job => job.Output == null).OrderByDescending(job => job.Source.ByteCode.Length));
            int workers = Math.Min(pending.Count, Environment.ProcessorCount);
            if (workers <= 1)
            {
                while (pending.TryDequeue(out QuicJob job)) Run(job);
                return;
            }

            var threads = new Thread[workers];
            for (int i = 0; i < workers; i++)
            {
                threads[i] = new Thread(() =>
                {
                    while (pending.TryDequeue(out QuicJob job)) Run(job);
                }, WorkerStackSize) { IsBackground = true, Name = "UdonQuic" };
                threads[i].Start();
            }
            foreach (Thread thread in threads) thread.Join();
        }

        /// <summary>Rebuilds the optimized program from a finished job; main thread only.</summary>
        /// <param name="details">Receives the listings before and after and the instruction origins, when given.</param>
        /// <returns>Whether result holds a rewritten program; when not, message says why.</returns>
        public static bool Finish(QuicJob job, out IUdonProgram result, out string message, out NativeStats stats, QuicDetails details = null)
        {
            result = null;
            QuicOutput output = job.Output;
            stats = output.Stats;
            message = output.Message;
            if (details != null)
            {
                details.Before = output.Before;
                details.After = output.After;
                details.Origins = output.Origins;
            }
            if (!output.Succeeded)
            {
                message ??= "the optimizer refused the program";
                return false;
            }

            var watch = Stopwatch.StartNew();
            try
            {
                result = Rebuild(job.Source, output, job.Types, out string error);
                if (result != null) return true;
                message = error;
                return false;
            }
            catch (Exception e)
            {
                result = null;
                message = $"{e.GetType().Name}: {e.Message}";
                return false;
            }
            finally
            {
                job.Milliseconds += watch.Elapsed.TotalMilliseconds;
            }
        }

        /// <summary>The Udon name of a type, such as UnityEngineTransformArray for Transform[].</summary>
        public static string UdonTypeName(Type type)
        {
            if (TypeNames.TryGetValue(type, out string cached)) return cached;

            string suffix = "";
            Type element = type;
            while (element.IsArray)
            {
                suffix += "Array";
                element = element.GetElementType();
            }

            string name = StripArity(element.Name);
            for (Type outer = element.DeclaringType; outer != null; outer = outer.DeclaringType) name = StripArity(outer.Name) + name;
            string full = (element.Namespace ?? "").Replace(".", "") + name;
            if (element.IsGenericType)
                foreach (Type argument in element.GetGenericArguments()) full += UdonTypeName(argument);

            full += suffix;
            TypeNames[type] = full;
            return full;
        }

        private static string StripArity(string name)
        {
            int tick = name.IndexOf('`');
            return tick < 0 ? name : name.Substring(0, tick);
        }

        private static Dictionary<string, Type> SeedTypes()
        {
            var types = new Dictionary<string, Type>();
            foreach (Type type in new[]
                     {
                         typeof(object), typeof(bool), typeof(char), typeof(sbyte), typeof(byte), typeof(short), typeof(ushort), typeof(int), typeof(uint),
                         typeof(long), typeof(ulong), typeof(float), typeof(double), typeof(string), typeof(uint[]),
                     })
                types[UdonTypeName(type)] = type;
            return types;
        }

        private static ProgramHandle Describe(IUdonProgram source, List<IntPtr> allocations, Dictionary<string, Type> types, BinaryWriter key, out string message)
        {
            message = null;
            byte[] code = source.ByteCode;
            IUdonHeap heap = source.Heap;
            IUdonSymbolTable table = source.SymbolTable;
            if (code == null || heap == null || table == null)
            {
                message = "the program has no code, heap or symbol table";
                return null;
            }

            uint capacity = heap.GetHeapCapacity();
            var signatures = new List<string>();
            var seen = new HashSet<string>();
            for (int offset = 0; offset + 4 <= code.Length;)
            {
                uint opcode = ReadWord(code, offset);
                if (opcode > 9 || opcode == 3)
                {
                    message = $"unknown opcode {opcode} at 0x{offset:X8}";
                    return null;
                }
                bool hasOperand = opcode != 0 && opcode != 2 && opcode != 9;
                if (hasOperand && offset + 8 > code.Length)
                {
                    message = $"the instruction at 0x{offset:X8} is cut off";
                    return null;
                }
                if (opcode == Extern)
                {
                    uint address = ReadWord(code, offset + 4);
                    if (address >= capacity || !heap.IsHeapVariableInitialized(address) || !(heap.GetHeapVariable(address) is string signature))
                    {
                        message = $"the EXTERN at 0x{offset:X8} names heap slot {address}, which holds no signature";
                        return null;
                    }
                    if (seen.Add(signature)) signatures.Add(signature);
                }
                offset += hasOperand ? 8 : 4;
            }

            var externs = new List<(string Signature, ExternInfo Info)>();
            foreach (string signature in signatures)
            {
                ExternInfo info = QuicExterns.Describe(signature);
                if (info.Arity < 0)
                {
                    message = $"the installed SDK does not know the extern '{signature}'";
                    return null;
                }
                externs.Add((signature, info));
            }

            var synced = new HashSet<string>();
            if (source.SyncMetadataTable != null)
                foreach (IUdonSyncMetadata metadata in source.SyncMetadataTable.GetAllSyncMetadata())
                    if (metadata?.Name != null) synced.Add(metadata.Name);

            var program = new ProgramHandle(Native.uq_program_create(code, (UIntPtr)(uint)code.Length));
            if (program.IsInvalid)
            {
                message = "the optimizer could not read the code";
                return null;
            }
            key.Write(code.Length);
            key.Write(code);
            key.Write(capacity);

            for (uint address = 0; address < capacity; address++)
            {
                string symbol = table.TryGetSymbolFromAddress(address, out string name) ? name : null;
                bool initialized = heap.IsHeapVariableInitialized(address);
                Type boxed = initialized ? heap.GetHeapVariableType(address) : null;
                Type type = (symbol != null ? table.GetSymbolType(symbol) : null) ?? boxed ?? typeof(object);
                string typeName = UdonTypeName(type);
                if (!types.ContainsKey(typeName)) types[typeName] = type;
                if (boxed != null && !types.ContainsKey(UdonTypeName(boxed))) types[UdonTypeName(boxed)] = boxed;

                bool exported = symbol != null && table.HasExportedSymbol(symbol);
                bool isSynced = symbol != null && synced.Contains(symbol);
                var flags = SlotFlags.None;
                if (initialized) flags |= SlotFlags.Initialized;
                if (exported) flags |= SlotFlags.Exported;
                if (isSynced) flags |= SlotFlags.Synced;
                if (!exported && !isSynced && (symbol == null || PrivatePrefixes.Any(p => symbol.StartsWith(p, StringComparison.Ordinal)))) flags |= SlotFlags.Private;

                object value = initialized ? heap.GetHeapVariable(address) : null;
                NativeValue[] constant = Value(value, allocations);
                Native.uq_program_add_slot(program, Native.Utf8(symbol), Native.Utf8(typeName), (uint)flags, constant);
                key.Write(symbol != null);
                key.Write(symbol ?? "");
                key.Write(typeName);
                key.Write((uint)flags);
                if (constant == null)
                    key.Write(-1);
                else
                {
                    key.Write(constant[0].Kind);
                    key.Write(constant[0].Boolean);
                    key.Write(constant[0].Integer);
                    key.Write(constant[0].Unsigned);
                    key.Write(constant[0].Real);
                    key.Write(value as string ?? "");
                }
                if (value is uint[] array)
                {
                    if (Native.uq_program_set_slot_array(program, address, array, array.Length) == 0)
                    {
                        program.Dispose();
                        message = $"the optimizer rejected the array in heap slot {address}";
                        return null;
                    }
                    key.Write(array.Length);
                    foreach (uint element in array) key.Write(element);
                }
            }

            key.Write(externs.Count);
            foreach (var (signature, info) in externs)
            {
                Native.uq_program_add_extern(program, Native.Utf8(signature), info.Arity, info.HasResult ? 1 : 0, info.Written);
                key.Write(signature);
                key.Write(info.Arity);
                key.Write(info.HasResult);
                key.Write(info.Written);
            }

            if (source.EntryPoints != null)
                foreach (string entry in source.EntryPoints.GetExportedSymbols())
                {
                    uint address = source.EntryPoints.GetAddressFromSymbol(entry);
                    Native.uq_program_add_entry(program, Native.Utf8(entry), address);
                    key.Write(entry);
                    key.Write(address);
                }

            return program;
        }

        private static NativeValue[] Value(object value, List<IntPtr> allocations)
        {
            var native = new NativeValue();
            switch (value)
            {
                case bool b:
                    native.Kind = (int)ValueKind.Boolean;
                    native.Boolean = b ? 1 : 0;
                    break;
                case sbyte _:
                case byte _:
                case short _:
                case ushort _:
                case int _:
                case long _:
                    native.Kind = (int)ValueKind.Integer;
                    native.Integer = System.Convert.ToInt64(value, CultureInfo.InvariantCulture);
                    break;
                case uint u:
                    native.Kind = (int)ValueKind.Unsigned;
                    native.Unsigned = u;
                    break;
                case ulong u:
                    native.Kind = (int)ValueKind.Unsigned;
                    native.Unsigned = u;
                    break;
                case char c:
                    native.Kind = (int)ValueKind.Unsigned;
                    native.Unsigned = c;
                    break;
                case float f:
                    native.Kind = (int)ValueKind.Real;
                    native.Real = f;
                    break;
                case double d:
                    native.Kind = (int)ValueKind.Real;
                    native.Real = d;
                    break;
                case string s:
                    byte[] bytes = Native.Utf8(s);
                    IntPtr text = Marshal.AllocHGlobal(bytes.Length);
                    allocations.Add(text);
                    Marshal.Copy(bytes, 0, text, bytes.Length);
                    native.Kind = (int)ValueKind.String;
                    native.Text = text;
                    break;
                default:
                    return null;
            }
            return new[] { native };
        }

        private static IUdonProgram Rebuild(IUdonProgram source, QuicOutput output, Dictionary<string, Type> types, out string error)
        {
            error = null;
            byte[] code = output.Code;

            IUdonHeap old = source.Heap;
            IUdonSymbolTable table = source.SymbolTable;
            uint oldCapacity = old.GetHeapCapacity();
            int count = output.Slots.Length;
            bool mapped = output.Mapped;
            uint marker = mapped ? (uint)count : Math.Max((uint)count, oldCapacity);
            uint capacity = marker + 1;
            if (capacity > MaximumHeapSize)
            {
                error = $"{capacity} heap slots is past Udon's limit of {MaximumHeapSize}";
                return null;
            }

            var heap = new UdonHeap(capacity);
            var symbols = new List<IUdonSymbol>();
            var taken = new HashSet<string>();
            if (!mapped)
            {
                for (uint address = 0; address < oldCapacity; address++)
                    if (old.IsHeapVariableInitialized(address)) heap.SetHeapVariable(address, old.GetHeapVariable(address), old.GetHeapVariableType(address));
                foreach (string name in table.GetSymbols()) symbols.Add(new UdonSymbol(name, table.GetSymbolType(name), table.GetAddressFromSymbol(name)));
                taken.UnionWith(table.GetSymbols());
            }

            for (int i = 0; i < count; i++)
            {
                uint address = (uint)i;
                QuicSlot slot = output.Slots[i];
                uint origin = slot.Source;
                bool added = mapped ? origin == uint.MaxValue || origin >= oldCapacity : address >= oldCapacity;

                if (mapped)
                {
                    string name = slot.Symbol;
                    if (name != null)
                    {
                        if (!taken.Add(name))
                        {
                            error = $"heap slot {address} reuses the symbol '{name}'";
                            return null;
                        }
                        Type symbolType = table.HasAddressForSymbol(name) ? table.GetSymbolType(name) : Resolve(slot.Type, types);
                        symbols.Add(new UdonSymbol(name, symbolType, address));
                    }
                    if (!added && old.IsHeapVariableInitialized(origin)) heap.SetHeapVariable(address, old.GetHeapVariable(origin), old.GetHeapVariableType(origin));
                }
                if (!added && !slot.Rewritten) continue;

                string typeName = slot.Type;
                Type type = Resolve(typeName, types) ?? (!added && old.IsHeapVariableInitialized(origin) ? old.GetHeapVariableType(origin) : null);
                if (type == null)
                {
                    error = $"heap slot {address} has the unknown type '{typeName}'";
                    return null;
                }

                switch ((ValueKind)slot.Value.Kind)
                {
                    case ValueKind.None:
                        if (added) heap.InitializeHeapVariable(address, type);
                        break;
                    case ValueKind.UintArray:
                        heap.SetHeapVariable(address, (uint[])slot.Array.Clone(), typeof(uint[]));
                        break;
                    default:
                        heap.SetHeapVariable(address, Convert(slot.Value, type), type);
                        break;
                }

                if (!added || mapped) continue;
                string symbol = slot.Symbol;
                if (symbol == null) continue;
                if (!taken.Add(symbol))
                {
                    error = $"the new heap slot {address} reuses the symbol '{symbol}'";
                    return null;
                }
                symbols.Add(new UdonSymbol(symbol, type, address));
            }

            if (!taken.Add(MarkerSymbol))
            {
                error = $"the program already has a symbol named '{MarkerSymbol}'";
                return null;
            }
            heap.SetHeapVariable(marker, Version, typeof(string));
            symbols.Add(new UdonSymbol(MarkerSymbol, typeof(string), marker));

            var entries = new List<IUdonSymbol>();
            var entryNames = new List<string>();
            foreach (QuicEntry entry in output.Entries)
            {
                entries.Add(new UdonSymbol(entry.Name, null, entry.Address));
                entryNames.Add(entry.Name);
            }

            return new UdonProgram(
                source.InstructionSetIdentifier,
                source.InstructionSetVersion,
                code,
                heap,
                new UdonSymbolTable(entries, entryNames),
                new UdonSymbolTable(symbols, table.GetExportedSymbols().ToList()),
                source.SyncMetadataTable,
                source.UpdateOrder);
        }

        private static Type Resolve(string typeName, Dictionary<string, Type> types)
        {
            if (string.IsNullOrEmpty(typeName)) return null;
            if (types.TryGetValue(typeName, out Type type)) return type;
            try
            {
                type = UdonEditorManager.Instance.GetTypeFromTypeString(typeName);
            }
            catch (Exception)
            {
                type = null;
            }
            if (type == null && typeName.EndsWith("Array", StringComparison.Ordinal) && typeName.Length > 5)
                type = Resolve(typeName.Substring(0, typeName.Length - 5), types)?.MakeArrayType();
            if (type != null) types[typeName] = type;
            return type;
        }

        private static object Convert(QuicValue value, Type type)
        {
            switch ((ValueKind)value.Kind)
            {
                case ValueKind.Boolean:
                    return ToType(value.Boolean, type);
                case ValueKind.Integer:
                    if (type.IsEnum) return Enum.ToObject(type, value.Integer);
                    return ToType(value.Integer >= int.MinValue && value.Integer <= int.MaxValue ? (object)(int)value.Integer : value.Integer, type);
                case ValueKind.Unsigned:
                    if (type.IsEnum) return Enum.ToObject(type, value.Unsigned);
                    return ToType(value.Unsigned <= uint.MaxValue ? (object)(uint)value.Unsigned : value.Unsigned, type);
                case ValueKind.Real:
                    return ToType(value.Real, type);
                case ValueKind.String:
                    string text = value.Text;
                    return type == typeof(char) && !string.IsNullOrEmpty(text) ? text[0] : ToType(text, type);
                default:
                    throw new InvalidDataException($"value kind {value.Kind} is not a constant");
            }
        }

        private static object ToType(object value, Type type)
        {
            if (value == null || type == typeof(object) || type.IsInstanceOfType(value)) return value;
            return System.Convert.ChangeType(value, type, CultureInfo.InvariantCulture);
        }

        private static uint ReadWord(byte[] code, int offset) =>
            (uint)(code[offset] << 24 | code[offset + 1] << 16 | code[offset + 2] << 8 | code[offset + 3]);
    }
}
