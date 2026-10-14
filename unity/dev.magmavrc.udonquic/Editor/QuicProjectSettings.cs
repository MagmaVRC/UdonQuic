using System;
using System.Collections.Generic;
using UnityEditor;
using UnityEngine;

namespace Magma.VRC.UdonQuic
{
    /// <summary>UdonQuic's project settings, stored in ProjectSettings/UdonQuicSettings.asset.</summary>
    [FilePath("ProjectSettings/UdonQuicSettings.asset", FilePathAttribute.Location.ProjectFolder)]
    internal sealed class QuicProjectSettings : ScriptableSingleton<QuicProjectSettings>
    {
        private const string EnabledKey = "UdonQuic_Enabled";
        private const string InlineBudgetKey = "UdonQuic_InlineBudget";
        private const string DisabledPassesKey = "UdonQuic_DisabledPasses";

        [SerializeField] private bool enabled = true;
        [SerializeField] private int inlineBudget = 24;
        [SerializeField] private uint disabledPasses;
        [SerializeField] private List<string> excludedPrograms = new List<string>();
        [SerializeField] private bool migrated;

        /// <summary>Raised after any setting changes.</summary>
        public static event Action Changed;

        /// <summary>Whether compiled programs are optimized.</summary>
        public bool Enabled
        {
            get => enabled;
            set => Set(ref enabled, value);
        }

        /// <summary>The largest body, in instructions, the optimizer inlines.</summary>
        public int InlineBudget
        {
            get => Math.Max(0, inlineBudget);
            set => Set(ref inlineBudget, Math.Max(0, value));
        }

        /// <summary>A mask of optimizer passes to skip.</summary>
        public uint DisabledPasses
        {
            get => disabledPasses;
            set => Set(ref disabledPasses, value);
        }

        /// <summary>GUIDs of the serialized program assets that are never optimized.</summary>
        public IReadOnlyList<string> ExcludedPrograms => excludedPrograms;

        /// <summary>Whether the serialized program asset with this GUID is never optimized.</summary>
        public bool IsExcluded(string guid) => !string.IsNullOrEmpty(guid) && excludedPrograms.Contains(guid);

        /// <summary>Adds or removes a serialized program asset GUID from the exclusions.</summary>
        public void SetExcluded(string guid, bool excluded)
        {
            if (string.IsNullOrEmpty(guid) || IsExcluded(guid) == excluded) return;
            if (excluded) excludedPrograms.Add(guid);
            else excludedPrograms.Remove(guid);
            Commit();
        }

        private void OnEnable()
        {
            if (migrated) return;
            if (EditorPrefs.HasKey(EnabledKey)) enabled = EditorPrefs.GetBool(EnabledKey, true);
            if (EditorPrefs.HasKey(InlineBudgetKey)) inlineBudget = Math.Max(0, EditorPrefs.GetInt(InlineBudgetKey, 24));
            if (EditorPrefs.HasKey(DisabledPassesKey)) disabledPasses = unchecked((uint)EditorPrefs.GetInt(DisabledPassesKey, 0));
            migrated = true;
            Save(true);
        }

        private void Set<T>(ref T field, T value)
        {
            if (EqualityComparer<T>.Default.Equals(field, value)) return;
            field = value;
            Commit();
        }

        private void Commit()
        {
            Save(true);
            Changed?.Invoke();
        }
    }
}
