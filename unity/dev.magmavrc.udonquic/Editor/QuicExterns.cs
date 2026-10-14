using System;
using System.Collections.Generic;
using VRC.Udon.Editor;
using VRC.Udon.Graph;

namespace Magma.VRC.UdonQuic
{
    /// <summary>What the optimizer needs to know about one extern.</summary>
    internal sealed class ExternInfo
    {
        /// <summary>The operand count the VM pops, or -1 when the wrapper does not know the signature.</summary>
        public int Arity;

        /// <summary>Whether the last operand receives a result.</summary>
        public bool HasResult;

        /// <summary>Bit i is set when operand i may be written.</summary>
        public ulong Written;
    }

    /// <summary>Extern facts per signature, read from the installed SDK's wrapper and node definitions.</summary>
    internal static class QuicExterns
    {
        private static readonly Dictionary<string, ExternInfo> Cache = new Dictionary<string, ExternInfo>();

        public static void ClearCache() => Cache.Clear();

        public static ExternInfo Describe(string signature)
        {
            if (Cache.TryGetValue(signature, out ExternInfo cached)) return cached;
            ExternInfo info = Build(signature);
            Cache[signature] = info;
            return info;
        }

        private static ExternInfo Build(string signature)
        {
            int arity;
            try
            {
                arity = UdonEditorManager.Instance.GetWrapper().GetExternFunctionParameterCount(signature);
            }
            catch (Exception)
            {
                arity = -1;
            }
            if (arity < 0) return new ExternInfo { Arity = -1 };

            var info = new ExternInfo { Arity = arity };
            UdonNodeDefinition definition = TryGetDefinition(signature);
            if (definition?.parameters == null || definition.parameters.Count != arity)
            {
                info.Written = Mask(arity);
                return info;
            }

            for (int i = 0; i < arity && i < 64; i++)
                if (definition.parameters[i].parameterType != UdonNodeParameter.ParameterType.IN) info.Written |= 1UL << i;

            info.HasResult = arity > 0 && definition.parameters[arity - 1].parameterType == UdonNodeParameter.ParameterType.OUT;

            if (arity > 0 && definition.type != null && definition.type.IsValueType && definition.parameters[0].name == "instance") info.Written |= 1UL;

            return info;
        }

        private static ulong Mask(int arity) => arity >= 64 ? ulong.MaxValue : (1UL << arity) - 1;

        private static UdonNodeDefinition TryGetDefinition(string signature)
        {
            try
            {
                return UdonEditorManager.Instance.GetNodeDefinition(signature);
            }
            catch (Exception)
            {
                return null;
            }
        }
    }
}
