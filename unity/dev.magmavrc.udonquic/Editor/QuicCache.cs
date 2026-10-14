using System;
using System.IO;
using System.IO.Compression;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text;

namespace Magma.VRC.UdonQuic
{
    internal struct QuicValue
    {
        public int Kind;
        public bool Boolean;
        public long Integer;
        public ulong Unsigned;
        public double Real;
        public string Text;
    }

    internal struct QuicSlot
    {
        public string Symbol;
        public string Type;
        public bool Rewritten;
        public QuicValue Value;
        public uint[] Array;
        public uint Source;
    }

    internal struct QuicEntry
    {
        public string Name;
        public uint Address;
    }

    /// <summary>A managed copy of an optimizer result, independent of the native library.</summary>
    internal sealed class QuicOutput
    {
        public bool Succeeded;
        public string Message;
        public NativeStats Stats;
        public string Before;
        public string After;
        public InstructionOrigin[] Origins;
        public byte[] Code;
        public bool Mapped;
        public QuicSlot[] Slots;
        public QuicEntry[] Entries;

        /// <summary>A result that did not reach or come from the optimizer.</summary>
        public static QuicOutput Failed(string message, string before = null) => new QuicOutput { Message = message, Before = before };

        /// <summary>Copies everything a native result holds.</summary>
        /// <exception cref="InvalidDataException">The result is missing a slot, array, source or entry point it reports.</exception>
        public static QuicOutput Capture(IntPtr result, string before)
        {
            var output = new QuicOutput { Before = before };
            Native.uq_result_stats(result, ref output.Stats);
            output.Message = Native.Read(Native.uq_result_message(result));
            output.Succeeded = Native.uq_result_succeeded(result) != 0;
            if (!output.Succeeded) return output;

            output.After = Native.Read(Native.uq_result_disassembly(result));
            output.Origins = Native.Origins(result);

            IntPtr code = Native.uq_result_bytecode(result, out UIntPtr length);
            output.Code = new byte[(int)length];
            if (code != IntPtr.Zero) Marshal.Copy(code, output.Code, 0, output.Code.Length);

            output.Mapped = Native.uq_result_slot_source != null;
            output.Slots = new QuicSlot[Math.Max(0, Native.uq_result_slot_count(result))];
            for (int i = 0; i < output.Slots.Length; i++)
            {
                if (Native.uq_result_slot(result, i, out NativeSlot slot) == 0) throw new InvalidDataException($"heap slot {i} is missing from the result");
                var copy = new QuicSlot
                {
                    Symbol = Native.Read(slot.Symbol),
                    Type = Native.Read(slot.Type),
                    Rewritten = slot.Rewritten != 0,
                    Value = new QuicValue
                    {
                        Kind = slot.Value.Kind,
                        Boolean = slot.Value.Boolean != 0,
                        Integer = slot.Value.Integer,
                        Unsigned = slot.Value.Unsigned,
                        Real = slot.Value.Real,
                        Text = (ValueKind)slot.Value.Kind == ValueKind.String ? Native.Read(slot.Value.Text) : null,
                    },
                    Source = (uint)i,
                };
                if ((ValueKind)slot.Value.Kind == ValueKind.UintArray)
                {
                    int elements = Native.uq_result_slot_array(result, i, out IntPtr values);
                    if (elements < 0) throw new InvalidDataException($"heap slot {i} holds no array");
                    var signed = new int[elements];
                    if (elements > 0) Marshal.Copy(values, signed, 0, elements);
                    copy.Array = new uint[elements];
                    Buffer.BlockCopy(signed, 0, copy.Array, 0, elements * sizeof(uint));
                }
                if (output.Mapped && Native.uq_result_slot_source(result, i, out copy.Source) == 0) throw new InvalidDataException($"heap slot {i} has no source in the result");
                output.Slots[i] = copy;
            }

            output.Entries = new QuicEntry[Math.Max(0, Native.uq_result_entry_count(result))];
            for (int i = 0; i < output.Entries.Length; i++)
            {
                if (Native.uq_result_entry(result, i, out NativeEntryPoint entry) == 0) throw new InvalidDataException($"entry point {i} is missing from the result");
                output.Entries[i] = new QuicEntry { Name = Native.Read(entry.Name), Address = entry.Address };
            }
            return output;
        }

        public void Write(BinaryWriter w)
        {
            w.Write(Succeeded);
            WriteText(w, Message);
            int size = Marshal.SizeOf<NativeStats>();
            var stats = new byte[size];
            GCHandle pin = GCHandle.Alloc(stats, GCHandleType.Pinned);
            try
            {
                Marshal.StructureToPtr(Stats, pin.AddrOfPinnedObject(), false);
            }
            finally
            {
                pin.Free();
            }
            w.Write(size);
            w.Write(stats);
            WriteText(w, Before);
            WriteText(w, After);

            w.Write(Origins != null);
            if (Origins != null)
            {
                w.Write(Origins.Length);
                foreach (InstructionOrigin origin in Origins)
                {
                    w.Write(origin.Address);
                    w.Write(origin.Origin);
                }
            }

            if (!Succeeded) return;
            w.Write(Code.Length);
            w.Write(Code);
            w.Write(Mapped);
            w.Write(Slots.Length);
            foreach (QuicSlot slot in Slots)
            {
                WriteText(w, slot.Symbol);
                WriteText(w, slot.Type);
                w.Write(slot.Rewritten);
                w.Write(slot.Value.Kind);
                w.Write(slot.Value.Boolean);
                w.Write(slot.Value.Integer);
                w.Write(slot.Value.Unsigned);
                w.Write(slot.Value.Real);
                WriteText(w, slot.Value.Text);
                w.Write(slot.Array != null);
                if (slot.Array != null)
                {
                    w.Write(slot.Array.Length);
                    foreach (uint value in slot.Array) w.Write(value);
                }
                w.Write(slot.Source);
            }
            w.Write(Entries.Length);
            foreach (QuicEntry entry in Entries)
            {
                WriteText(w, entry.Name);
                w.Write(entry.Address);
            }
        }

        public static QuicOutput Read(BinaryReader r)
        {
            var output = new QuicOutput { Succeeded = r.ReadBoolean(), Message = ReadText(r) };
            int size = r.ReadInt32();
            if (size != Marshal.SizeOf<NativeStats>()) throw new InvalidDataException("the cached stats have another layout");
            byte[] stats = r.ReadBytes(size);
            GCHandle pin = GCHandle.Alloc(stats, GCHandleType.Pinned);
            try
            {
                output.Stats = Marshal.PtrToStructure<NativeStats>(pin.AddrOfPinnedObject());
            }
            finally
            {
                pin.Free();
            }
            output.Before = ReadText(r);
            output.After = ReadText(r);

            if (r.ReadBoolean())
            {
                output.Origins = new InstructionOrigin[r.ReadInt32()];
                for (int i = 0; i < output.Origins.Length; i++) output.Origins[i] = new InstructionOrigin(r.ReadUInt32(), r.ReadUInt32());
            }

            if (!output.Succeeded) return output;
            output.Code = r.ReadBytes(r.ReadInt32());
            output.Mapped = r.ReadBoolean();
            output.Slots = new QuicSlot[r.ReadInt32()];
            for (int i = 0; i < output.Slots.Length; i++)
            {
                var slot = new QuicSlot
                {
                    Symbol = ReadText(r),
                    Type = ReadText(r),
                    Rewritten = r.ReadBoolean(),
                    Value = new QuicValue
                    {
                        Kind = r.ReadInt32(),
                        Boolean = r.ReadBoolean(),
                        Integer = r.ReadInt64(),
                        Unsigned = r.ReadUInt64(),
                        Real = r.ReadDouble(),
                        Text = ReadText(r),
                    },
                };
                if (r.ReadBoolean())
                {
                    slot.Array = new uint[r.ReadInt32()];
                    for (int j = 0; j < slot.Array.Length; j++) slot.Array[j] = r.ReadUInt32();
                }
                slot.Source = r.ReadUInt32();
                output.Slots[i] = slot;
            }
            output.Entries = new QuicEntry[r.ReadInt32()];
            for (int i = 0; i < output.Entries.Length; i++) output.Entries[i] = new QuicEntry { Name = ReadText(r), Address = r.ReadUInt32() };
            return output;
        }

        private static void WriteText(BinaryWriter w, string text)
        {
            w.Write(text != null);
            if (text != null) w.Write(text);
        }

        private static string ReadText(BinaryReader r) => r.ReadBoolean() ? r.ReadString() : null;
    }

    /// <summary>Optimizer results on disk under Library/UdonQuic/cache, keyed by a hash of everything the optimizer was given.</summary>
    internal static class QuicCache
    {
        private const int Format = 1;
        private static readonly TimeSpan MaximumAge = TimeSpan.FromDays(14);
        private static string Folder => Path.GetFullPath(Path.Combine("Library", "UdonQuic", "cache"));
        private static bool _pruned;

        /// <summary>The cache key of an optimizer input.</summary>
        public static string Key(byte[] input)
        {
            using var sha = SHA256.Create();
            sha.TransformBlock(BitConverter.GetBytes(Format), 0, sizeof(int), null, 0);
            sha.TransformFinalBlock(input, 0, input.Length);
            var key = new StringBuilder(64);
            foreach (byte b in sha.Hash) key.Append(b.ToString("x2"));
            return key.ToString();
        }

        public static bool TryLoad(string key, out QuicOutput output)
        {
            output = null;
            string path = Path.Combine(Folder, key + ".bin");
            try
            {
                if (!File.Exists(path)) return false;
                using (var file = File.OpenRead(path))
                using (var gzip = new GZipStream(file, CompressionMode.Decompress))
                using (var reader = new BinaryReader(gzip, Encoding.UTF8))
                    output = QuicOutput.Read(reader);
                File.SetLastWriteTimeUtc(path, DateTime.UtcNow);
                return true;
            }
            catch (Exception)
            {
                output = null;
                TryDelete(path);
                return false;
            }
        }

        /// <summary>Stores a result; safe to call from any thread.</summary>
        public static void Store(string key, QuicOutput output)
        {
            string folder = Folder;
            string path = Path.Combine(folder, key + ".bin");
            string partial = $"{path}.{Guid.NewGuid():N}.tmp";
            try
            {
                Directory.CreateDirectory(folder);
                using (var file = File.Create(partial))
                using (var gzip = new GZipStream(file, CompressionLevel.Fastest))
                using (var writer = new BinaryWriter(gzip, Encoding.UTF8))
                    output.Write(writer);
                if (File.Exists(path)) File.Delete(path);
                File.Move(partial, path);
            }
            catch (Exception)
            {
                TryDelete(partial);
            }
        }

        /// <summary>Deletes entries unused for two weeks, once per editor session.</summary>
        public static void Prune()
        {
            if (_pruned) return;
            _pruned = true;
            try
            {
                if (!Directory.Exists(Folder)) return;
                DateTime cutoff = DateTime.UtcNow - MaximumAge;
                foreach (string file in Directory.GetFiles(Folder))
                    if (file.EndsWith(".tmp", StringComparison.Ordinal) || File.GetLastWriteTimeUtc(file) < cutoff) TryDelete(file);
            }
            catch (Exception)
            {
            }
        }

        private static void TryDelete(string path)
        {
            try
            {
                File.Delete(path);
            }
            catch (Exception)
            {
            }
        }
    }
}
