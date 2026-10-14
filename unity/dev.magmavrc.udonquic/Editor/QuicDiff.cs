using System;
using System.Collections.Generic;
using System.Threading;

namespace Magma.VRC.UdonQuic
{
    /// <summary>One line of a diff: Kind is -1 for removed, 0 for kept and 1 for added.</summary>
    internal readonly struct DiffLine
    {
        public DiffLine(int kind, string text)
        {
            Kind = kind;
            Text = text;
        }

        public int Kind { get; }
        public string Text { get; }
    }

    /// <summary>A line diff using Myers' linear-space algorithm.</summary>
    internal static class QuicDiff
    {
        /// <summary>The shortest edit script turning before into after, comparing lines after normalize.</summary>
        /// <returns>The diff, or null when cancelled.</returns>
        public static DiffLine[] Lines(string[] before, string[] after, Func<string, string> normalize, CancellationToken cancel = default)
        {
            var ids = new Dictionary<string, int>(StringComparer.Ordinal);
            int[] a = Encode(before, normalize, ids), b = Encode(after, normalize, ids);
            var removed = new bool[a.Length];
            var added = new bool[b.Length];
            int max = a.Length + b.Length + 1;
            var down = new int[2 * max + 2];
            var up = new int[2 * max + 2];
            try
            {
                Compare(a, 0, a.Length, b, 0, b.Length, down, up, removed, added, cancel);
            }
            catch (OperationCanceledException)
            {
                return null;
            }

            var lines = new List<DiffLine>(Math.Max(a.Length, b.Length));
            int i = 0, j = 0;
            while (i < a.Length || j < b.Length)
            {
                if (i < a.Length && removed[i]) lines.Add(new DiffLine(-1, before[i++]));
                else if (j < b.Length && added[j]) lines.Add(new DiffLine(1, after[j++]));
                else
                {
                    lines.Add(new DiffLine(0, after[j]));
                    i++;
                    j++;
                }
            }
            return lines.ToArray();
        }

        private static int[] Encode(string[] lines, Func<string, string> normalize, Dictionary<string, int> ids)
        {
            var codes = new int[lines.Length];
            for (int i = 0; i < lines.Length; i++)
            {
                string key = normalize == null ? lines[i] : normalize(lines[i]);
                if (!ids.TryGetValue(key, out int id)) ids[key] = id = ids.Count;
                codes[i] = id;
            }
            return codes;
        }

        private static void Compare(int[] a, int lowA, int highA, int[] b, int lowB, int highB, int[] down, int[] up, bool[] removed, bool[] added, CancellationToken cancel)
        {
            while (true)
            {
                cancel.ThrowIfCancellationRequested();
                while (lowA < highA && lowB < highB && a[lowA] == b[lowB])
                {
                    lowA++;
                    lowB++;
                }
                while (lowA < highA && lowB < highB && a[highA - 1] == b[highB - 1])
                {
                    highA--;
                    highB--;
                }

                if (lowA == highA)
                {
                    while (lowB < highB) added[lowB++] = true;
                    return;
                }
                if (lowB == highB)
                {
                    while (lowA < highA) removed[lowA++] = true;
                    return;
                }

                (int x, int y) = MiddleSnake(a, lowA, highA, b, lowB, highB, down, up);
                Compare(a, lowA, x, b, lowB, y, down, up, removed, added, cancel);
                lowA = x;
                lowB = y;
            }
        }

        private static (int X, int Y) MiddleSnake(int[] a, int lowA, int highA, int[] b, int lowB, int highB, int[] down, int[] up)
        {
            int max = a.Length + b.Length + 1;
            int downK = lowA - lowB;
            int upK = highA - highB;
            int delta = (highA - lowA) - (highB - lowB);
            bool odd = (delta & 1) != 0;
            int downOffset = max - downK;
            int upOffset = max - upK;
            int maxD = (highA - lowA + highB - lowB) / 2 + 1;

            down[downOffset + downK + 1] = lowA;
            up[upOffset + upK - 1] = highA;

            for (int d = 0; d <= maxD; d++)
            {
                for (int k = downK - d; k <= downK + d; k += 2)
                {
                    int x;
                    if (k == downK - d) x = down[downOffset + k + 1];
                    else
                    {
                        x = down[downOffset + k - 1] + 1;
                        if (k < downK + d && down[downOffset + k + 1] >= x) x = down[downOffset + k + 1];
                    }
                    int y = x - k;
                    while (x < highA && y < highB && a[x] == b[y])
                    {
                        x++;
                        y++;
                    }
                    down[downOffset + k] = x;
                    if (odd && upK - d < k && k < upK + d && up[upOffset + k] <= down[downOffset + k])
                        return (down[downOffset + k], down[downOffset + k] - k);
                }

                for (int k = upK - d; k <= upK + d; k += 2)
                {
                    int x;
                    if (k == upK + d) x = up[upOffset + k - 1];
                    else
                    {
                        x = up[upOffset + k + 1] - 1;
                        if (k > upK - d && up[upOffset + k - 1] < x) x = up[upOffset + k - 1];
                    }
                    int y = x - k;
                    while (x > lowA && y > lowB && a[x - 1] == b[y - 1])
                    {
                        x--;
                        y--;
                    }
                    up[upOffset + k] = x;
                    if (!odd && downK - d <= k && k <= downK + d && up[upOffset + k] <= down[downOffset + k])
                        return (down[downOffset + k], down[downOffset + k] - k);
                }
            }
            throw new InvalidOperationException("the diff found no middle snake");
        }
    }
}
