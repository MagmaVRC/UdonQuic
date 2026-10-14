using System;
using System.Collections.Generic;
using System.IO;
using UnityEditor;
using UnityEngine;

namespace Magma.VRC.UdonQuic
{
    /// <summary>UdonQuic's settings and its Tools menu.</summary>
    internal static class QuicSettings
    {
        private const string EnabledMenu = "Tools/UdonQuic/Enabled";

        /// <summary>The path of UdonQuic's page in Project Settings.</summary>
        public const string SettingsPath = "Project/UdonQuic";

        /// <summary>Whether compiled programs are optimized.</summary>
        public static bool Enabled
        {
            get => QuicProjectSettings.instance.Enabled;
            set => QuicProjectSettings.instance.Enabled = value;
        }

        /// <summary>The largest body, in instructions, the optimizer inlines.</summary>
        public static int InlineBudget
        {
            get => QuicProjectSettings.instance.InlineBudget;
            set => QuicProjectSettings.instance.InlineBudget = value;
        }

        /// <summary>A mask of optimizer passes to skip.</summary>
        public static uint DisabledPasses
        {
            get => QuicProjectSettings.instance.DisabledPasses;
            set => QuicProjectSettings.instance.DisabledPasses = value;
        }

        /// <summary>Whether the serialized program asset with this GUID is never optimized.</summary>
        public static bool IsExcluded(string guid) => QuicProjectSettings.instance.IsExcluded(guid);

        /// <summary>The native defaults with these settings applied.</summary>
        public static NativeOptions Options()
        {
            Native.uq_options_default(out NativeOptions options);
            options.Passes &= ~DisabledPasses;
            options.InlineBudget = InlineBudget;
            return options;
        }

        /// <summary>A one-line account of an optimization.</summary>
        public static string Describe(NativeStats stats) =>
            $"instructions {stats.InstructionsBefore} -> {stats.InstructionsAfter}, externs {stats.ExternsBefore} -> {stats.ExternsAfter}, " +
            $"{stats.Inlined} inlined, {stats.TailCalls} tail calls, {stats.Folded} folded, {stats.CopiesRemoved} copies removed, " +
            $"{stats.BranchesSimplified} branches simplified, {stats.DeadStores} dead stores, {stats.Rounds} rounds";

        /// <summary>Turns optimization on or off and recompiles every program so the plain or optimized output comes back.</summary>
        public static void SetEnabled(bool enabled)
        {
            if (Enabled == enabled) return;
            Enabled = enabled;
            QuicPostCompiler.RecompileAll();
        }

        /// <summary>Loads the native optimizer if it is not loaded, logging why it could not be.</summary>
        public static bool TryLoadNative(bool log = true)
        {
            try
            {
                Native.Load();
                return true;
            }
            catch (Exception e) when (e is DllNotFoundException || e is EntryPointNotFoundException || e is BadImageFormatException || e is IOException)
            {
                if (log) Debug.LogError($"[UdonQuic] The native optimizer could not be loaded: {e.Message}");
                return false;
            }
        }

        [MenuItem(EnabledMenu, false, 0)]
        private static void ToggleEnabled() => SetEnabled(!Enabled);

        [MenuItem(EnabledMenu, true)]
        private static bool ToggleEnabledCheck()
        {
            Menu.SetChecked(EnabledMenu, Enabled);
            return true;
        }

        [MenuItem("Tools/UdonQuic/Settings", false, 31)]
        private static void OpenSettings() => SettingsService.OpenProjectSettings(SettingsPath);

        [MenuItem("Tools/UdonQuic/Reload Native Optimizer", false, 40)]
        private static void ReloadNative()
        {
            Native.Unload();
            QuicExterns.ClearCache();
            if (TryLoadNative()) Debug.Log($"[UdonQuic] Loaded {Native.LoadedPath}");
        }

        [SettingsProvider]
        private static SettingsProvider CreateProvider() => new QuicSettingsProvider();
    }

    internal sealed class QuicSettingsProvider : SettingsProvider
    {
        private List<PassInfo> _passes;
        private Vector2 _scroll;

        public QuicSettingsProvider() : base(QuicSettings.SettingsPath, SettingsScope.Project, new[] { "Udon", "optimizer", "inline", "pass", "exclude" })
        {
        }

        public override void OnActivate(string searchContext, UnityEngine.UIElements.VisualElement rootElement)
        {
            _passes = QuicSettings.TryLoadNative(false) ? Native.Passes() : new List<PassInfo>();
        }

        public override void OnGUI(string searchContext)
        {
            QuicProjectSettings settings = QuicProjectSettings.instance;
            using var scroll = new EditorGUILayout.ScrollViewScope(_scroll);
            _scroll = scroll.scrollPosition;
            float labelWidth = EditorGUIUtility.labelWidth;
            EditorGUIUtility.labelWidth = 200;
            EditorGUILayout.Space(6);

            bool enabled = EditorGUILayout.Toggle(new GUIContent("Enabled", "Optimize every Udon program after it compiles. Changing this recompiles every program."), settings.Enabled);
            if (enabled != settings.Enabled) EditorApplication.delayCall += () => QuicSettings.SetEnabled(enabled);

            settings.InlineBudget = EditorGUILayout.DelayedIntField(new GUIContent("Inline Budget", "The largest method body, in instructions, that is inlined. Methods with a single caller are inlined whatever their size."), settings.InlineBudget);

            EditorGUILayout.Space(10);
            EditorGUILayout.LabelField("Passes", EditorStyles.boldLabel);
            if (_passes == null || _passes.Count == 0)
            {
                EditorGUILayout.HelpBox("The loaded optimizer does not describe its passes. Edit the mask of passes to skip directly, or install a newer UdonQuic.dll.", MessageType.None);
                settings.DisabledPasses = unchecked((uint)EditorGUILayout.IntField(new GUIContent("Disabled Pass Mask", "Bit i skips pass i."), unchecked((int)settings.DisabledPasses)));
            }
            else
            {
                uint mask = settings.DisabledPasses;
                foreach (PassInfo pass in _passes)
                {
                    bool on = (mask & pass.Bit) == 0;
                    bool now = EditorGUILayout.Toggle(new GUIContent(pass.Name, pass.Description), on);
                    if (now != on) mask = now ? mask & ~pass.Bit : mask | pass.Bit;
                }
                settings.DisabledPasses = mask;
                using (new EditorGUILayout.HorizontalScope())
                {
                    GUILayout.Space(EditorGUIUtility.labelWidth + 2);
                    if (GUILayout.Button("Enable All", EditorStyles.miniButton, GUILayout.Width(90))) settings.DisabledPasses = 0;
                }
            }
            EditorGUILayout.HelpBox("Pass and budget changes apply to programs compiled from now on. Recompile All applies them everywhere.", MessageType.Info);

            EditorGUILayout.Space(10);
            EditorGUILayout.LabelField("Excluded Programs", EditorStyles.boldLabel);
            if (settings.ExcludedPrograms.Count == 0) EditorGUILayout.LabelField("None. Exclude programs from Tools > UdonQuic > Manager.", EditorStyles.miniLabel);
            foreach (string guid in new List<string>(settings.ExcludedPrograms))
            {
                using (new EditorGUILayout.HorizontalScope())
                {
                    string path = AssetDatabase.GUIDToAssetPath(guid);
                    string label = QuicPrograms.SourceLabel(guid);
                    EditorGUILayout.LabelField(new GUIContent(label, string.IsNullOrEmpty(path) ? guid : path));
                    if (GUILayout.Button("Include", EditorStyles.miniButton, GUILayout.Width(70)))
                    {
                        string included = guid;
                        EditorApplication.delayCall += () => QuicPrograms.SetExcluded(included, false);
                    }
                }
            }

            EditorGUILayout.Space(10);
            using (new EditorGUILayout.HorizontalScope())
            {
                if (GUILayout.Button("Open Manager", GUILayout.Width(120))) QuicManagerWindow.Open();
                if (GUILayout.Button("Recompile All", GUILayout.Width(120))) EditorApplication.delayCall += QuicPostCompiler.RecompileAll;
            }
            EditorGUILayout.LabelField($"Optimizer {(Native.IsLoaded ? Native.Version ?? "0.1.0 or earlier" : "not loaded")}", EditorStyles.miniLabel);
            EditorGUIUtility.labelWidth = labelWidth;
        }
    }
}
