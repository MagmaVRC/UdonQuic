using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text.RegularExpressions;
using System.Threading;
using System.Threading.Tasks;
using UnityEditor;
using UnityEditor.IMGUI.Controls;
using UnityEditor.SceneManagement;
using UnityEngine;
using VRC.Udon;

namespace Magma.VRC.UdonQuic
{
    /// <summary>Lists every program UdonQuic handled with its results, listings and a diff, and manages exclusions.</summary>
    internal sealed class QuicManagerWindow : EditorWindow
    {
        private enum Column
        {
            Name,
            Compiler,
            Status,
            Instructions,
            Externs,
            Inlined,
            Folded,
            Copies,
            Time,
        }

        private enum ListingTab
        {
            Before,
            After,
            Diff,
        }

        private sealed class Row
        {
            public string Guid;
            public QuicResult Result;
        }

        private readonly struct StatRow
        {
            public StatRow(string label, uint pass, Func<NativeStats, int> value)
            {
                Label = label;
                Pass = pass;
                Value = value;
            }

            public string Label { get; }
            public uint Pass { get; }
            public Func<NativeStats, int> Value { get; }
        }

        private static readonly StatRow[] Stats =
        {
            new StatRow("Inlined", 1u << 1, s => s.Inlined),
            new StatRow("Tail calls", 1u << 2, s => s.TailCalls),
            new StatRow("Folded", 1u << 3, s => s.Folded),
            new StatRow("Copies removed", 1u << 4, s => s.CopiesRemoved),
            new StatRow("Branches simplified", 1u << 5, s => s.BranchesSimplified),
            new StatRow("Dead stores", 1u << 6, s => s.DeadStores),
            new StatRow("Hoisted", 1u << 8, s => s.Hoisted),
            new StatRow("Loops rotated", 1u << 9, s => s.LoopsRotated),
            new StatRow("Checks removed", 1u << 10, s => s.ChecksRemoved),
            new StatRow("Switches built", 1u << 11, s => s.SwitchesBuilt),
            new StatRow("Concats merged", 1u << 12, s => s.ConcatsMerged),
            new StatRow("Externs fused", 1u << 13, s => s.Fused),
            new StatRow("Reads remembered", 1u << 14, s => s.Memoized),
            new StatRow("Loops unrolled", 1u << 15, s => s.Unrolled),
            new StatRow("Events removed", 1u << 16, s => s.EventsRemoved),
            new StatRow("Values propagated", 1u << 10, s => s.Propagated),
            new StatRow("Heap slots removed", 1u << 17, s => s.HeapBefore - s.HeapAfter),
            new StatRow("Rounds", 0, s => s.Rounds),
        };

        private static readonly string[] StatusNames = { "Not optimized yet", "Optimized", "Unchanged", "Refused", "Excluded", "Error" };
        private static readonly Regex Hex = new Regex("0x[0-9a-fA-F]+", RegexOptions.Compiled);
        private const float RowHeight = 20;
        private const float LineHeight = 16;
        private const float SplitterSize = 5;
        private const float StripHeight = 24;

        [SerializeField] private MultiColumnHeaderState _headerState;
        [SerializeField] private string _selected;
        [SerializeField] private string _search = "";
        [SerializeField] private int _statusMask = ~0;
        [SerializeField] private int _compiler;
        [SerializeField] private float _split = 0.45f;
        [SerializeField] private float _infoWidth = 360;
        [SerializeField] private ListingTab _tab = ListingTab.Diff;

        private MultiColumnHeader _header;
        private SearchField _searchField;
        private List<Row> _rows = new List<Row>();
        private List<Row> _visible = new List<Row>();
        private string[] _compilers = { "All compilers" };
        private Vector2 _listScroll, _infoScroll, _listingScroll;
        private Dictionary<uint, PassInfo> _passes = new Dictionary<uint, PassInfo>();
        private bool _dirty = true;
        private int _dragging;

        private string _listingKey;
        private string[] _before, _after;
        private DiffLine[] _diff;
        private int _added, _removed;
        private float _listingWidth;
        private CancellationTokenSource _diffCancel;
        private volatile bool _diffReady;
        private DiffLine[] _pendingDiff;
        private string _pendingKey;

        private GUIStyle _mono, _cell, _rightCell, _title, _chip, _wrap;
        private Font _monoFont;

        /// <summary>Opens the manager.</summary>
        [MenuItem("Tools/UdonQuic/Manager", false, 30)]
        public static void Open()
        {
            var window = GetWindow<QuicManagerWindow>();
            window.titleContent = new GUIContent("UdonQuic", EditorGUIUtility.IconContent("d_Profiler.CPU").image);
            window.minSize = new Vector2(720, 420);
            window.Show();
        }

        private void OnEnable()
        {
            QuicResults.Changed += MarkDirty;
            QuicProjectSettings.Changed += MarkDirty;
            EditorSceneManager.sceneOpened += OnSceneOpened;
            _searchField = new SearchField();
            CreateHeader();
            if (QuicSettings.TryLoadNative(false)) _passes = Native.Passes().ToDictionary(p => p.Bit);
        }

        private void OnDisable()
        {
            QuicResults.Changed -= MarkDirty;
            QuicProjectSettings.Changed -= MarkDirty;
            EditorSceneManager.sceneOpened -= OnSceneOpened;
            _diffCancel?.Cancel();
        }

        private void OnFocus() => MarkDirty();

        private void OnSceneOpened(UnityEngine.SceneManagement.Scene scene, OpenSceneMode mode) => MarkDirty();

        private void MarkDirty()
        {
            _dirty = true;
            _listingKey = null;
            Repaint();
        }

        private void OnInspectorUpdate()
        {
            if (!_diffReady) return;
            _diffReady = false;
            if (_pendingKey != _listingKey || _pendingDiff == null) return;
            _diff = _pendingDiff;
            _added = _diff.Count(d => d.Kind > 0);
            _removed = _diff.Count(d => d.Kind < 0);
            _listingWidth = MeasureWidth(_diff.Select(d => d.Text));
            Repaint();
        }

        private void CreateHeader()
        {
            MultiColumnHeaderState.Column Make(string name, float width, bool right = false, string tip = null) => new MultiColumnHeaderState.Column
            {
                headerContent = new GUIContent(name, tip),
                width = width,
                minWidth = 40,
                headerTextAlignment = right ? TextAlignment.Right : TextAlignment.Left,
                sortingArrowAlignment = right ? TextAlignment.Left : TextAlignment.Right,
                canSort = true,
                autoResize = false,
                allowToggleVisibility = true,
            };

            var columns = new[]
            {
                Make("Program", 220),
                Make("Compiler", 90),
                Make("Status", 160),
                Make("Instructions", 170, true, "Instructions before and after, with the change"),
                Make("Externs", 150, true, "Extern calls before and after, with the change"),
                Make("Inlined", 60, true),
                Make("Folded", 60, true),
                Make("Copies", 60, true, "Copies removed"),
                Make("Time", 70, true, "Optimizer time in milliseconds"),
            };
            columns[0].autoResize = true;
            columns[0].allowToggleVisibility = false;

            var state = new MultiColumnHeaderState(columns);
            if (MultiColumnHeaderState.CanOverwriteSerializedFields(_headerState, state)) MultiColumnHeaderState.OverwriteSerializedFields(_headerState, state);
            _headerState = state;
            _header = new MultiColumnHeader(state) { height = 22 };
            _header.sortingChanged += _ => MarkDirty();
            if (_header.sortedColumnIndex < 0) _header.SetSorting((int)Column.Name, true);
        }

        private void Styles()
        {
            if (_mono != null && _monoFont != null) return;
            _monoFont = Font.CreateDynamicFontFromOSFont(new[] { "Consolas", "Cascadia Mono", "Courier New" }, 12);
            _mono = new GUIStyle(EditorStyles.label) { font = _monoFont, fontSize = 12, padding = new RectOffset(6, 6, 0, 0), alignment = TextAnchor.MiddleLeft, richText = false };
            _cell = new GUIStyle(EditorStyles.label) { alignment = TextAnchor.MiddleLeft, padding = new RectOffset(6, 6, 0, 0), clipping = TextClipping.Clip };
            _rightCell = new GUIStyle(_cell) { alignment = TextAnchor.MiddleRight };
            _title = new GUIStyle(EditorStyles.boldLabel) { fontSize = 14, clipping = TextClipping.Clip };
            _chip = new GUIStyle(EditorStyles.miniLabel) { alignment = TextAnchor.MiddleLeft, padding = new RectOffset(4, 10, 0, 0), richText = true };
            _wrap = new GUIStyle(EditorStyles.wordWrappedLabel) { richText = false };
        }

        private static bool Dark => EditorGUIUtility.isProSkin;

        private static Color StatusColor(QuicStatus status)
        {
            switch (status)
            {
                case QuicStatus.Optimized: return Dark ? new Color(0.36f, 0.78f, 0.45f) : new Color(0.1f, 0.55f, 0.2f);
                case QuicStatus.Refused: return Dark ? new Color(0.95f, 0.65f, 0.25f) : new Color(0.78f, 0.45f, 0f);
                case QuicStatus.Excluded: return Dark ? new Color(0.45f, 0.65f, 0.95f) : new Color(0.15f, 0.4f, 0.8f);
                case QuicStatus.Error: return Dark ? new Color(0.95f, 0.38f, 0.38f) : new Color(0.8f, 0.1f, 0.1f);
                default: return Dark ? new Color(0.6f, 0.6f, 0.6f) : new Color(0.42f, 0.42f, 0.42f);
            }
        }

        private static Color Good => Dark ? new Color(0.45f, 0.82f, 0.5f) : new Color(0.1f, 0.5f, 0.2f);
        private static Color Bad => Dark ? new Color(0.95f, 0.5f, 0.45f) : new Color(0.75f, 0.15f, 0.1f);
        private static Color Stripe => Dark ? new Color(1, 1, 1, 0.025f) : new Color(0, 0, 0, 0.03f);
        private static Color SelectedRow =>Dark ? new Color(0.17f, 0.36f, 0.53f) : new Color(0.23f, 0.45f, 0.69f, 0.55f);
        private static Color Line => Dark ? new Color(0, 0, 0, 0.35f) : new Color(0, 0, 0, 0.18f);
        private static Color AddedTint => Dark ? new Color(0.2f, 0.65f, 0.3f, 0.2f) : new Color(0.2f, 0.7f, 0.3f, 0.18f);
        private static Color RemovedTint => Dark ? new Color(0.85f, 0.25f, 0.25f, 0.2f) : new Color(0.9f, 0.25f, 0.25f, 0.16f);

        private void OnGUI()
        {
            Styles();
            if (_header == null) CreateHeader();
            if (_dirty) Rebuild();

            Toolbar();
            Totals();

            float top = EditorStyles.toolbar.fixedHeight + StripHeight;
            var area = new Rect(0, top, position.width, position.height - top);
            float listHeight = Mathf.Clamp(area.height * _split, 80, Mathf.Max(80, area.height - 140));
            var list = new Rect(area.x, area.y, area.width, listHeight);
            var splitter = new Rect(area.x, list.yMax, area.width, SplitterSize);
            var detail = new Rect(area.x, splitter.yMax, area.width, area.height - listHeight - SplitterSize);

            List(list);
            if (Splitter(splitter, 1, MouseCursor.ResizeVertical)) _split = Mathf.Clamp01((Event.current.mousePosition.y - area.y) / Mathf.Max(1, area.height));
            EditorGUI.DrawRect(new Rect(splitter.x, splitter.y + 2, splitter.width, 1), Line);
            Detail(detail);
            Keyboard();
        }

        private bool Splitter(Rect rect, int id, MouseCursor cursor)
        {
            EditorGUIUtility.AddCursorRect(rect, cursor);
            Event e = Event.current;
            if (e.type == EventType.MouseDown && rect.Contains(e.mousePosition))
            {
                _dragging = id;
                e.Use();
            }
            else if (_dragging == id && e.type == EventType.MouseDrag)
            {
                e.Use();
                Repaint();
                return true;
            }
            else if (_dragging == id && e.rawType == EventType.MouseUp)
            {
                _dragging = 0;
            }
            return false;
        }

        private void Toolbar()
        {
            using (new EditorGUILayout.HorizontalScope(EditorStyles.toolbar))
            {
                bool enabled = GUILayout.Toggle(QuicSettings.Enabled, new GUIContent(" Enabled", "Optimize programs after they compile. Changing this recompiles every program."), EditorStyles.toolbarButton, GUILayout.Width(70));
                if (enabled != QuicSettings.Enabled) EditorApplication.delayCall += () => QuicSettings.SetEnabled(enabled);
                if (GUILayout.Button(new GUIContent("Recompile All", "Recompile every UdonSharp and UdonLuau program."), EditorStyles.toolbarButton)) EditorApplication.delayCall += QuicPostCompiler.RecompileAll;
                using (new EditorGUI.DisabledScope(!QuicSettings.Enabled))
                    if (GUILayout.Button(new GUIContent("Optimize Now", "Optimize and store every plain program in the project and the open scenes."), EditorStyles.toolbarButton))
                        EditorApplication.delayCall += () => QuicPostCompiler.OptimizeNow();
                if (GUILayout.Button(new GUIContent("Analyze", "Run the optimizer on every plain program without storing anything, and record what it would do."), EditorStyles.toolbarButton))
                    EditorApplication.delayCall += () => QuicPostCompiler.Analyze(QuicPostCompiler.AllPrograms());

                GUILayout.FlexibleSpace();

                EditorGUI.BeginChangeCheck();
                _statusMask = EditorGUILayout.MaskField(GUIContent.none, _statusMask, StatusNames, EditorStyles.toolbarDropDown, GUILayout.Width(130));
                _compiler = EditorGUILayout.Popup(Mathf.Clamp(_compiler, 0, _compilers.Length - 1), _compilers, EditorStyles.toolbarPopup, GUILayout.Width(120));
                _search = _searchField.OnToolbarGUI(_search, GUILayout.MinWidth(120), GUILayout.MaxWidth(260));
                if (EditorGUI.EndChangeCheck()) _dirty = true;

                if (GUILayout.Button(EditorGUIUtility.IconContent("_Popup"), EditorStyles.toolbarButton, GUILayout.Width(26)))
                {
                    var menu = new GenericMenu();
                    menu.AddItem(new GUIContent("Project Settings"), false, () => SettingsService.OpenProjectSettings(QuicSettings.SettingsPath));
                    menu.AddItem(new GUIContent("Reveal Results Folder"), false, () => EditorUtility.RevealInFinder(Path.GetFullPath(Path.Combine("Library", "UdonQuic", "results.json"))));
                    menu.AddSeparator("");
                    menu.AddItem(new GUIContent("Clear History"), false, () =>
                    {
                        if (EditorUtility.DisplayDialog("UdonQuic", "Forget every recorded result and listing?", "Clear", "Cancel")) QuicResults.Clear();
                    });
                    menu.ShowAsContext();
                }
            }
        }

        private void Totals()
        {
            int Count(QuicStatus status) => _rows.Count(r => r.Result.Status == status);
            long instrBefore = 0, instrAfter = 0, externBefore = 0, externAfter = 0;
            double time = 0;
            foreach (Row row in _rows)
            {
                time += row.Result.DurationMs;
                if (row.Result.Status != QuicStatus.Optimized) continue;
                instrBefore += row.Result.Stats.InstructionsBefore;
                instrAfter += row.Result.Stats.InstructionsAfter;
                externBefore += row.Result.Stats.ExternsBefore;
                externAfter += row.Result.Stats.ExternsAfter;
            }

            Rect strip = GUILayoutUtility.GetRect(0, StripHeight, GUILayout.ExpandWidth(true));
            EditorGUI.DrawRect(strip, Dark ? new Color(0.16f, 0.16f, 0.16f) : new Color(0.84f, 0.84f, 0.84f));
            EditorGUI.DrawRect(new Rect(strip.x, strip.yMax - 1, strip.width, 1), Line);
            float x = strip.x + 6;

            void Chip(string label, string value, Color color)
            {
                var content = new GUIContent($"{label} <b>{value}</b>");
                float width = _chip.CalcSize(content).x;
                var dot = new Rect(x, strip.y + strip.height / 2 - 3, 6, 6);
                EditorGUI.DrawRect(dot, color);
                GUI.Label(new Rect(x + 8, strip.y, width, strip.height), content, _chip);
                x += width + 12;
            }

            Chip("Optimized", Count(QuicStatus.Optimized).ToString(), StatusColor(QuicStatus.Optimized));
            Chip("Refused", Count(QuicStatus.Refused).ToString(), StatusColor(QuicStatus.Refused));
            Chip("Unchanged", Count(QuicStatus.Unchanged).ToString(), StatusColor(QuicStatus.Unchanged));
            Chip("Excluded", Count(QuicStatus.Excluded).ToString(), StatusColor(QuicStatus.Excluded));
            int errors = Count(QuicStatus.Error);
            if (errors > 0) Chip("Errors", errors.ToString(), StatusColor(QuicStatus.Error));
            Chip("Instructions", $"{instrBefore:N0} → {instrAfter:N0} ({Percent(instrBefore, instrAfter)})", Line);
            Chip("Externs", $"{externBefore:N0} → {externAfter:N0} ({Percent(externBefore, externAfter)})", Line);
            Chip("Time", $"{time:N0} ms", Line);
        }

        private static string Percent(long before, long after) => before == 0 ? "0%" : $"{(after - before) * 100.0 / before:+0.0;-0.0;0}%";

        private void Rebuild()
        {
            _dirty = false;
            var rows = new Dictionary<string, Row>();
            foreach (QuicResult result in QuicResults.All)
                if (!string.IsNullOrEmpty(AssetDatabase.GUIDToAssetPath(result.Guid))) rows[result.Guid] = new Row { Guid = result.Guid, Result = result };
            foreach (AbstractSerializedUdonProgramAsset asset in QuicPostCompiler.CollectLoadedPrograms())
            {
                string guid = QuicPrograms.Guid(asset);
                if (guid == null || rows.ContainsKey(guid)) continue;
                QuicResult result = QuicPrograms.Describe(asset);
                result.Status = QuicSettings.IsExcluded(guid) ? QuicStatus.Excluded : QuicStatus.NotOptimized;
                result.Message = "No result is recorded for this program yet. Re-optimize it, or press Analyze if it is stored plain.";
                rows[guid] = new Row { Guid = guid, Result = result };
            }
            _rows = rows.Values.ToList();

            string current = _compiler > 0 && _compiler < _compilers.Length ? _compilers[_compiler] : null;
            _compilers = new[] { "All compilers" }.Concat(_rows.Select(r => r.Result.Compiler ?? "Other").Distinct().OrderBy(c => c)).ToArray();
            _compiler = current == null ? 0 : Math.Max(0, Array.IndexOf(_compilers, current));
            current = _compiler > 0 ? _compilers[_compiler] : null;

            IEnumerable<Row> visible = _rows.Where(r => (_statusMask & (1 << (int)r.Result.Status)) != 0 && (current == null || r.Result.Compiler == current));
            if (!string.IsNullOrWhiteSpace(_search))
            {
                string term = _search.Trim();
                visible = visible.Where(r => Contains(r.Result.SourceName, term) || Contains(r.Result.SourcePath, term) || Contains(r.Result.AssetPath, term) || Contains(r.Result.Message, term));
            }

            var column = (Column)Math.Max(0, _header.sortedColumnIndex);
            bool ascending = _header.sortedColumnIndex < 0 || _header.IsSortedAscending(_header.sortedColumnIndex);
            Func<Row, IComparable> key = column switch
            {
                Column.Compiler => r => r.Result.Compiler ?? "",
                Column.Status => r => (int)r.Result.Status,
                Column.Instructions => r => Change(r.Result.Stats.InstructionsBefore, r.Result.Stats.InstructionsAfter),
                Column.Externs => r => Change(r.Result.Stats.ExternsBefore, r.Result.Stats.ExternsAfter),
                Column.Inlined => r => r.Result.Stats.Inlined,
                Column.Folded => r => r.Result.Stats.Folded,
                Column.Copies => r => r.Result.Stats.CopiesRemoved,
                Column.Time => r => r.Result.DurationMs,
                _ => r => r.Result.SourceName ?? "",
            };
            IOrderedEnumerable<Row> sorted = ascending ? visible.OrderBy(key) : visible.OrderByDescending(key);
            _visible = sorted.ThenBy(r => r.Result.SourceName ?? "", StringComparer.OrdinalIgnoreCase).ToList();
        }

        private static double Change(int before, int after) => before == 0 ? 0 : (after - before) / (double)before;

        private static bool Contains(string text, string term) => text != null && text.IndexOf(term, StringComparison.OrdinalIgnoreCase) >= 0;

        private void List(Rect rect)
        {
            var headerRect = new Rect(rect.x, rect.y, rect.width, _header.height);
            var body = new Rect(rect.x, headerRect.yMax, rect.width, rect.height - _header.height);
            float contentWidth = Mathf.Max(_header.state.widthOfAllVisibleColumns, body.width - 14);
            _header.OnGUI(headerRect, _listScroll.x);

            var view = new Rect(0, 0, contentWidth, Mathf.Max(_visible.Count * RowHeight, 0));
            _listScroll = GUI.BeginScrollView(body, _listScroll, view);
            int first = Mathf.Max(0, Mathf.FloorToInt(_listScroll.y / RowHeight));
            int last = Mathf.Min(_visible.Count, first + Mathf.CeilToInt(body.height / RowHeight) + 1);
            int[] columns = _header.state.visibleColumns;
            for (int i = first; i < last; i++)
            {
                Row row = _visible[i];
                var rowRect = new Rect(0, i * RowHeight, contentWidth, RowHeight);
                Event e = Event.current;
                if (e.type == EventType.MouseDown && e.button == 0 && rowRect.Contains(e.mousePosition))
                {
                    Select(row.Guid);
                    if (e.clickCount == 2) PingSource(row.Result);
                    e.Use();
                }
                else if (e.type == EventType.ContextClick && rowRect.Contains(e.mousePosition))
                {
                    Select(row.Guid);
                    RowMenu(row);
                    e.Use();
                }

                if (e.type == EventType.Repaint)
                {
                    if (row.Guid == _selected) EditorGUI.DrawRect(rowRect, SelectedRow);
                    else if (i % 2 == 1) EditorGUI.DrawRect(rowRect, Stripe);
                }

                for (int c = 0; c < columns.Length; c++)
                {
                    Rect columnRect = _header.GetColumnRect(c);
                    Cell(new Rect(columnRect.x, rowRect.y, columnRect.width, RowHeight), (Column)columns[c], row.Result);
                }
            }
            GUI.EndScrollView();

            if (_visible.Count == 0)
            {
                string text = _rows.Count == 0 ? "No results yet. Compile your scripts, or press Analyze to see what UdonQuic would do." : "No program matches the filters.";
                GUI.Label(body, text, new GUIStyle(EditorStyles.centeredGreyMiniLabel) { wordWrap = true });
            }
        }

        private void Cell(Rect rect, Column column, QuicResult result)
        {
            if (Event.current.type != EventType.Repaint) return;
            bool hasStats = result.Status == QuicStatus.Optimized || result.Status == QuicStatus.Unchanged || result.Status == QuicStatus.Refused && result.Stats.InstructionsBefore > 0;
            switch (column)
            {
                case Column.Name:
                    GUI.Label(rect, new GUIContent(result.SourceName, result.SourcePath ?? result.AssetPath), _cell);
                    break;
                case Column.Compiler:
                    GUI.Label(rect, result.Compiler, _cell);
                    break;
                case Column.Status:
                    EditorGUI.DrawRect(new Rect(rect.x + 6, rect.y + rect.height / 2 - 3.5f, 7, 7), StatusColor(result.Status));
                    GUI.Label(new Rect(rect.x + 10, rect.y, rect.width - 10, rect.height), StatusText(result), _cell);
                    break;
                case Column.Instructions:
                    if (hasStats) BeforeAfter(rect, result.Stats.InstructionsBefore, result.Stats.InstructionsAfter);
                    break;
                case Column.Externs:
                    if (hasStats) BeforeAfter(rect, result.Stats.ExternsBefore, result.Stats.ExternsAfter);
                    break;
                case Column.Inlined:
                    if (hasStats) GUI.Label(rect, result.Stats.Inlined.ToString("N0"), _rightCell);
                    break;
                case Column.Folded:
                    if (hasStats) GUI.Label(rect, result.Stats.Folded.ToString("N0"), _rightCell);
                    break;
                case Column.Copies:
                    if (hasStats) GUI.Label(rect, result.Stats.CopiesRemoved.ToString("N0"), _rightCell);
                    break;
                case Column.Time:
                    if (result.DurationMs > 0) GUI.Label(rect, $"{result.DurationMs:0.0} ms", _rightCell);
                    break;
            }
        }

        private void BeforeAfter(Rect rect, int before, int after)
        {
            string change = Percent(before, after);
            Color previous = GUI.contentColor;
            float changeWidth = 58;
            GUI.contentColor = after < before ? Good : after > before ? Bad : previous;
            GUI.Label(new Rect(rect.xMax - changeWidth, rect.y, changeWidth, rect.height), change, _rightCell);
            GUI.contentColor = previous;
            GUI.Label(new Rect(rect.x, rect.y, rect.width - changeWidth, rect.height), $"{before:N0} → {after:N0}", _rightCell);
        }

        private static string StatusText(QuicResult result) =>
            result.AnalysisOnly ? $"{StatusNames[(int)result.Status]} (analysis)" : StatusNames[(int)result.Status];

        private void Select(string guid)
        {
            if (_selected == guid) return;
            _selected = guid;
            _listingKey = null;
            _listingScroll = Vector2.zero;
            GUI.FocusControl(null);
            Repaint();
        }

        private void Keyboard()
        {
            Event e = Event.current;
            if (e.type != EventType.KeyDown || _visible.Count == 0 || EditorGUIUtility.editingTextField) return;
            int index = _visible.FindIndex(r => r.Guid == _selected);
            int next = e.keyCode == KeyCode.DownArrow ? index + 1 : e.keyCode == KeyCode.UpArrow ? index - 1 : int.MinValue;
            if (next == int.MinValue) return;
            next = Mathf.Clamp(next, 0, _visible.Count - 1);
            Select(_visible[next].Guid);
            float y = next * RowHeight;
            if (y < _listScroll.y) _listScroll.y = y;
            e.Use();
        }

        private void RowMenu(Row row)
        {
            var menu = new GenericMenu();
            bool excluded = QuicSettings.IsExcluded(row.Guid);
            menu.AddItem(new GUIContent("Ping Source"), false, () => PingSource(row.Result));
            menu.AddItem(new GUIContent("Open Source"), false, () => OpenSource(row.Result));
            menu.AddItem(new GUIContent("Ping Serialized Program"), false, () => Ping(row.Result.AssetPath));
            menu.AddSeparator("");
            menu.AddItem(new GUIContent("Re-optimize"), false, () => Reoptimize(row.Guid));
            menu.AddItem(new GUIContent(excluded ? "Include" : "Exclude"), false, () => QuicPrograms.SetExcluded(row.Guid, !excluded));
            menu.ShowAsContext();
        }

        private static void Ping(string path)
        {
            UnityEngine.Object asset = string.IsNullOrEmpty(path) ? null : AssetDatabase.LoadMainAssetAtPath(path);
            if (asset == null) return;
            EditorGUIUtility.PingObject(asset);
            Selection.activeObject = asset;
        }

        private static UnityEngine.Object SourceScript(QuicResult result)
        {
            UnityEngine.Object source = string.IsNullOrEmpty(result.SourcePath) ? null : AssetDatabase.LoadMainAssetAtPath(result.SourcePath);
            if (source is UdonSharp.UdonSharpProgramAsset udonSharp && udonSharp.sourceCsScript != null) return udonSharp.sourceCsScript;
            if (source != null && new SerializedObject(source).FindProperty("sourceScript")?.objectReferenceValue is UnityEngine.Object script && script != null) return script;
            return source;
        }

        private static void PingSource(QuicResult result)
        {
            UnityEngine.Object script = SourceScript(result);
            if (script == null) return;
            EditorGUIUtility.PingObject(script);
        }

        private static void OpenSource(QuicResult result)
        {
            UnityEngine.Object script = SourceScript(result);
            if (script != null) AssetDatabase.OpenAsset(script);
        }

        private static void Reoptimize(string guid)
        {
            AbstractSerializedUdonProgramAsset asset = QuicPrograms.Load(guid);
            if (asset != null) EditorApplication.delayCall += () => QuicPrograms.Recompile(asset);
        }

        private void Detail(Rect rect)
        {
            Row row = _rows.FirstOrDefault(r => r.Guid == _selected);
            if (row == null)
            {
                GUI.Label(rect, "Select a program to see what UdonQuic did to it.", EditorStyles.centeredGreyMiniLabel);
                return;
            }

            float infoWidth = Mathf.Clamp(_infoWidth, 260, Mathf.Max(260, rect.width - 260));
            var info = new Rect(rect.x, rect.y, infoWidth, rect.height);
            var splitter = new Rect(info.xMax, rect.y, SplitterSize, rect.height);
            var listing = new Rect(splitter.xMax, rect.y, rect.width - infoWidth - SplitterSize, rect.height);

            Info(info, row.Result);
            if (Splitter(splitter, 2, MouseCursor.ResizeHorizontal)) _infoWidth = Event.current.mousePosition.x - rect.x;
            EditorGUI.DrawRect(new Rect(splitter.x + 2, splitter.y, 1, splitter.height), Line);
            Listing(listing, row.Result);
        }

        private void Info(Rect rect, QuicResult result)
        {
            GUILayout.BeginArea(new RectOffset(8, 6, 6, 6).Remove(rect));
            _infoScroll = EditorGUILayout.BeginScrollView(_infoScroll);

            EditorGUILayout.LabelField(new GUIContent(result.SourceName, result.SourcePath), _title);
            EditorGUILayout.LabelField(result.SourcePath ?? result.AssetPath ?? "", EditorStyles.miniLabel);
            using (new EditorGUILayout.HorizontalScope())
            {
                using (new EditorGUI.DisabledScope(string.IsNullOrEmpty(result.SourcePath)))
                {
                    if (GUILayout.Button("Ping", EditorStyles.miniButtonLeft)) PingSource(result);
                    if (GUILayout.Button("Open", EditorStyles.miniButtonMid)) OpenSource(result);
                }
                if (GUILayout.Button(new GUIContent("Program", "Ping the serialized program asset"), EditorStyles.miniButtonRight)) Ping(result.AssetPath);
            }
            using (new EditorGUILayout.HorizontalScope())
            {
                bool excluded = QuicSettings.IsExcluded(result.Guid);
                if (GUILayout.Button(new GUIContent("Re-optimize", "Recompile this program so UdonQuic optimizes it again."))) Reoptimize(result.Guid);
                string guid = result.Guid;
                if (GUILayout.Button(new GUIContent(excluded ? "Include" : "Exclude", excluded ? "Optimize this program again." : "Store this program as compiled from now on.")))
                    EditorApplication.delayCall += () => QuicPrograms.SetExcluded(guid, !excluded);
            }

            EditorGUILayout.Space(8);
            Rect statusRect = EditorGUILayout.GetControlRect();
            EditorGUI.DrawRect(new Rect(statusRect.x, statusRect.y + statusRect.height / 2 - 4, 8, 8), StatusColor(result.Status));
            GUI.Label(new Rect(statusRect.x + 14, statusRect.y, statusRect.width - 14, statusRect.height), $"{StatusText(result)} · {result.Compiler}", EditorStyles.boldLabel);
            if (!string.IsNullOrEmpty(result.Message)) EditorGUILayout.LabelField(Capitalize(result.Message), _wrap);

            if (result.Stats.InstructionsBefore > 0)
            {
                EditorGUILayout.Space(8);
                Stat("Instructions", $"{result.Stats.InstructionsBefore:N0} → {result.Stats.InstructionsAfter:N0}  ({Percent(result.Stats.InstructionsBefore, result.Stats.InstructionsAfter)})");
                Stat("Externs", $"{result.Stats.ExternsBefore:N0} → {result.Stats.ExternsAfter:N0}  ({Percent(result.Stats.ExternsBefore, result.Stats.ExternsAfter)})");
                EditorGUILayout.Space(4);
                foreach (StatRow stat in Stats)
                {
                    string label = stat.Label, tip = null;
                    if (stat.Pass != 0 && _passes.TryGetValue(stat.Pass, out PassInfo pass))
                    {
                        label = $"{stat.Label} ({pass.Name})";
                        tip = pass.Description;
                    }
                    int value = stat.Value(result.Stats);
                    using (new EditorGUI.DisabledScope(value == 0))
                        Stat(label, value.ToString("N0"), tip);
                }
            }

            EditorGUILayout.Space(8);
            if (result.Compiler == "UdonSharp" && result.Status == QuicStatus.Optimized && !result.AnalysisOnly)
                Stat("Error lines", result.DebugInfoRemapped ? "Remapped" : "Not remapped");
            if (result.DurationMs > 0) Stat("Time", $"{result.DurationMs:0.00} ms");
            if (!string.IsNullOrEmpty(result.Version)) Stat("Optimizer", result.Version);
            if (result.Timestamp > 0) Stat("Recorded", result.Time.ToString("yyyy-MM-dd HH:mm:ss"));

            EditorGUILayout.EndScrollView();
            GUILayout.EndArea();
        }

        private static string Capitalize(string text) => string.IsNullOrEmpty(text) ? text : char.ToUpperInvariant(text[0]) + text.Substring(1);

        private void Stat(string label, string value, string tip = null)
        {
            Rect r = EditorGUILayout.GetControlRect(false, 18);
            GUI.Label(new Rect(r.x, r.y, r.width * 0.58f, r.height), new GUIContent(label, tip), EditorStyles.label);
            GUI.Label(new Rect(r.x + r.width * 0.58f, r.y, r.width * 0.42f, r.height), new GUIContent(value, tip), _rightCell);
        }

        private void Listing(Rect rect, QuicResult result)
        {
            LoadListing(result);
            var bar = new Rect(rect.x, rect.y, rect.width, 21);
            GUI.Box(bar, GUIContent.none, EditorStyles.toolbar);
            var tabs = new Rect(bar.x + 4, bar.y, 210, bar.height);
            _tab = (ListingTab)GUI.Toolbar(tabs, (int)_tab, new[] { "Before", "After", "Diff" }, EditorStyles.toolbarButton);
            string summary = _tab == ListingTab.Diff ? (_diff == null ? (_before != null && _after != null ? "Comparing…" : "") : $"+{_added:N0}  −{_removed:N0}  (addresses ignored)")
                : $"{(_tab == ListingTab.Before ? _before : _after)?.Length ?? 0:N0} lines";
            GUI.Label(new Rect(tabs.xMax + 8, bar.y, bar.width - tabs.width - 16, bar.height), summary, EditorStyles.miniLabel);

            var body = new Rect(rect.x, bar.yMax, rect.width, rect.height - bar.height);
            EditorGUI.DrawRect(body, Dark ? new Color(0.13f, 0.13f, 0.13f) : new Color(0.97f, 0.97f, 0.97f));

            string empty = null;
            if (!result.HasListings) empty = "No listing was recorded for this program.";
            else if (_tab == ListingTab.Before && _before == null) empty = "The listing before optimization is missing.";
            else if (_tab != ListingTab.Before && _after == null) empty = result.Status == QuicStatus.Refused ? "The optimizer refused this program, so there is no listing after." : "The listing after optimization is missing.";
            else if (_tab == ListingTab.Diff && _diff == null) empty = "Comparing listings…";
            if (empty != null)
            {
                GUI.Label(body, empty, EditorStyles.centeredGreyMiniLabel);
                return;
            }

            if (_tab == ListingTab.Diff) Lines(body, _diff.Length, i => _diff[i].Text, i => _diff[i].Kind);
            else
            {
                string[] lines = _tab == ListingTab.Before ? _before : _after;
                Lines(body, lines.Length, i => lines[i], null);
            }
        }

        private void Lines(Rect body, int count, Func<int, string> text, Func<int, int> kind)
        {
            float width = Mathf.Max(body.width - 14, _tab == ListingTab.Diff ? _listingWidth + 20 : MeasureCurrent());
            var view = new Rect(0, 0, width, count * LineHeight + 4);
            _listingScroll = GUI.BeginScrollView(body, _listingScroll, view);
            if (Event.current.type == EventType.Repaint)
            {
                int first = Mathf.Max(0, Mathf.FloorToInt(_listingScroll.y / LineHeight));
                int last = Mathf.Min(count, first + Mathf.CeilToInt(body.height / LineHeight) + 1);
                for (int i = first; i < last; i++)
                {
                    var line = new Rect(0, i * LineHeight + 2, width, LineHeight);
                    int k = kind?.Invoke(i) ?? 0;
                    if (k > 0) EditorGUI.DrawRect(line, AddedTint);
                    else if (k < 0) EditorGUI.DrawRect(line, RemovedTint);
                    string prefix = kind == null ? "" : k > 0 ? "+ " : k < 0 ? "- " : "  ";
                    GUI.Label(line, prefix + text(i), _mono);
                }
            }
            GUI.EndScrollView();
        }

        private float _beforeWidth, _afterWidth;

        private float MeasureCurrent() => _tab == ListingTab.Before ? _beforeWidth : _afterWidth;

        private float MeasureWidth(IEnumerable<string> lines)
        {
            int longest = 0;
            foreach (string line in lines) longest = Math.Max(longest, line.Length);
            float character = _mono.CalcSize(new GUIContent("MMMMMMMMMM")).x / 10f;
            return (longest + 2) * character + 12;
        }

        private void LoadListing(QuicResult result)
        {
            string key = $"{result.Guid}:{result.Timestamp}";
            if (_listingKey == key) return;
            _listingKey = key;
            _diffCancel?.Cancel();
            _diffCancel = null;
            _diffReady = false;
            _diff = null;
            _before = ReadLines(QuicResults.ListingPath(result.Guid, false), result.HasListings);
            _after = ReadLines(QuicResults.ListingPath(result.Guid, true), result.HasListings);
            _beforeWidth = _before == null ? 0 : MeasureWidth(_before);
            _afterWidth = _after == null ? 0 : MeasureWidth(_after);
            if (_before == null || _after == null) return;

            var cancel = new CancellationTokenSource();
            _diffCancel = cancel;
            string[] before = _before, after = _after;
            Task.Run(() =>
            {
                DiffLine[] diff = QuicDiff.Lines(before, after, line => Hex.Replace(line, "0x"), cancel.Token);
                if (cancel.IsCancellationRequested) return;
                _pendingKey = key;
                _pendingDiff = diff;
                _diffReady = true;
            });
        }

        private static string[] ReadLines(string path, bool recorded)
        {
            if (!recorded || !File.Exists(path)) return null;
            try
            {
                return File.ReadAllText(path).Replace("\r\n", "\n").TrimEnd('\n').Split('\n');
            }
            catch (IOException)
            {
                return null;
            }
        }
    }
}
