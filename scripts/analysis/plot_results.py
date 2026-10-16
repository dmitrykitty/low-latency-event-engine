#!/usr/bin/env python3
"""Render benchmark charts as static SVG (light and dark) from a run_clean.sh result.

    python3 scripts/analysis/plot_results.py <result dir> <output dir>

The result directory must contain latency_runs.csv and throughput_runs.csv. Standard
library only. Writes latency-p50, latency-p99 and throughput-1024 charts, each as
<name>-light.svg and <name>-dark.svg.
"""

import csv
import math
import os
import statistics
import sys
from html import escape

QUEUES = ["lle", "rigtorp", "boost"]
NAMES = {"lle": "LLE", "rigtorp": "rigtorp", "boost": "Boost"}
SIZES = [16, 64, 256, 1024]
FONT = 'system-ui, -apple-system, "Segoe UI", sans-serif'

# categorical slots 1-3 of the validated reference palette, plus chart chrome tokens.
THEMES = {
    "light": {
        "surface": "#fcfcfb", "ink": "#0b0b0b", "ink2": "#52514e", "muted": "#898781",
        "grid": "#e1e0d9", "base": "#c3c2b7",
        "series": {"lle": "#2a78d6", "rigtorp": "#eb6834", "boost": "#1baf7a"},
    },
    "dark": {
        "surface": "#1a1a19", "ink": "#f0efec", "ink2": "#c3c2b7", "muted": "#898781",
        "grid": "#2c2c2a", "base": "#383835",
        "series": {"lle": "#3987e5", "rigtorp": "#d95926", "boost": "#199e70"},
    },
}


def t_quantile(df):
    # two-sided 95% Student t; table for small df, Cornish-Fisher expansion otherwise.
    table = {1: 12.706, 2: 4.303, 3: 3.182}
    if df in table:
        return table[df]
    z = 1.959964
    return (z + (z**3 + z) / (4 * df) + (5 * z**5 + 16 * z**3 + 3 * z) / (96 * df**2)
            + (3 * z**7 + 19 * z**5 + 17 * z**3 - 15 * z) / (384 * df**3))


def mean_ci(values):
    mean = statistics.fmean(values)
    half = t_quantile(len(values) - 1) * statistics.stdev(values) / math.sqrt(len(values))
    return mean, half


def nice_ticks(lo, hi, count=4):
    raw = (hi - lo) / count
    magnitude = 10 ** math.floor(math.log10(raw))
    step = min((m * magnitude for m in (1, 2, 2.5, 5, 10) if m * magnitude >= raw))
    start = math.floor(lo / step) * step
    stop = math.ceil(hi / step) * step  # the last tick must cover the largest value
    return [round(start + i * step, 6) for i in range(round((stop - start) / step) + 1)]


def fmt(value):
    return f"{value:,.0f}" if value >= 100 else f"{value:g}"


class Svg:
    def __init__(self, width, height, theme, title):
        self.t = THEMES[theme]
        self.width, self.height = width, height
        self.parts = [
            f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" '
            f'viewBox="0 0 {width} {height}" font-family=\'{FONT}\' role="img">',
            f"<title>{escape(title)}</title>",
            f'<rect width="{width}" height="{height}" rx="8" fill="{self.t["surface"]}"/>',
        ]

    def text(self, x, y, content, size=12, color="ink2", anchor="start", weight=400):
        self.parts.append(
            f'<text x="{x:.1f}" y="{y:.1f}" font-size="{size}" fill="{self.t[color]}" '
            f'text-anchor="{anchor}" font-weight="{weight}">{escape(content)}</text>')

    def line(self, x1, y1, x2, y2, color, width=1, opacity=1):
        stroke = self.t[color] if color in self.t else color
        self.parts.append(
            f'<line x1="{x1:.1f}" y1="{y1:.1f}" x2="{x2:.1f}" y2="{y2:.1f}" stroke="{stroke}" '
            f'stroke-width="{width}" stroke-linecap="round" opacity="{opacity}"/>')

    def raw(self, fragment):
        self.parts.append(fragment)

    def legend(self, x, y):
        for queue in QUEUES:
            color = self.t["series"][queue]
            self.raw(f'<circle cx="{x + 5:.1f}" cy="{y - 4:.1f}" r="5" fill="{color}"/>')
            self.text(x + 15, y, NAMES[queue], size=12, color="ink2")
            x += 26 + 7.5 * len(NAMES[queue])

    def save(self, path):
        with open(path, "w") as out:
            out.write("\n".join(self.parts + ["</svg>"]) + "\n")


def load_latency(directory):
    runs = {}
    for row in csv.DictReader(open(os.path.join(directory, "latency_runs.csv"))):
        runs.setdefault((int(row["payload_bytes"]), row["queue"]), []).append(row)
    return runs


def load_throughput(directory, slots):
    runs = {}
    for row in csv.DictReader(open(os.path.join(directory, "throughput_runs.csv"))):
        if int(row["slots"]) == slots:
            runs.setdefault((int(row["payload_bytes"]), row["queue"]), []).append(
                float(row["items_per_second"]) / 1e6)
    return runs


def latency_chart(runs, metric, label, theme, path):
    width, panel_w, panel_h = 920, 430, 168
    top = 92
    svg = Svg(width, top + 2 * panel_h + 36, theme, f"Ping-pong round-trip latency, {label}")
    t = svg.t
    svg.text(24, 32, f"Ping-pong round-trip latency, {label} (ns, lower is better)",
             size=15, color="ink", weight=600)
    svg.text(24, 52, "Each small dot is one process with a random memory layout (32 per "
             "queue); large dot = mean, whisker = 95% confidence interval.", size=12)
    svg.legend(24, 76)
    for index, size in enumerate(SIZES):
        x0 = 24 + (index % 2) * (panel_w + 18)
        y0 = top + (index // 2) * panel_h
        plot_l, plot_r = x0 + 74, x0 + panel_w - 96
        values = [float(r[metric]) for q in QUEUES for r in runs[(size, q)]]
        ticks = nice_ticks(min(values), max(values))
        lo, hi = ticks[0], ticks[-1]
        scale = lambda v: plot_l + (v - lo) / (hi - lo) * (plot_r - plot_l)
        svg.text(x0, y0 + 14, f"{size} B payload", size=12, color="ink", weight=600)
        rows_top, row_h = y0 + 30, 34
        axis_y = rows_top + row_h * len(QUEUES) + 4
        for tick in ticks:
            svg.line(scale(tick), rows_top - 6, scale(tick), axis_y, "grid")
            svg.text(scale(tick), axis_y + 15, fmt(tick), size=11, color="muted", anchor="middle")
        svg.line(plot_l, axis_y, plot_r, axis_y, "base")
        for row, queue in enumerate(QUEUES):
            cy = rows_top + row * row_h + row_h / 2
            color = t["series"][queue]
            svg.text(x0 + 66, cy + 4, NAMES[queue], size=12, color="ink2", anchor="end")
            for i, record in enumerate(runs[(size, queue)]):
                value = float(record[metric])
                jitter = ((i * 7) % 11 - 5) * 1.6
                svg.raw(f'<circle cx="{scale(value):.1f}" cy="{cy + jitter:.1f}" r="4" '
                        f'fill="{color}" fill-opacity="0.35" stroke="{t["surface"]}" '
                        f'stroke-width="1"><title>{NAMES[queue]} {size} B, process '
                        f'{escape(record["run"])}: {value:.0f} ns</title></circle>')
            mean, half = mean_ci([float(r[metric]) for r in runs[(size, queue)]])
            svg.line(scale(mean - half), cy, scale(mean + half), cy, color, width=2)
            for end in (mean - half, mean + half):
                svg.line(scale(end), cy - 5, scale(end), cy + 5, color, width=2)
            svg.raw(f'<circle cx="{scale(mean):.1f}" cy="{cy:.1f}" r="6.5" fill="{color}" '
                    f'stroke="{t["surface"]}" stroke-width="2"><title>{NAMES[queue]} {size} B: '
                    f'mean {mean:.0f} ± {half:.0f} ns</title></circle>')
            svg.text(plot_r + 12, cy + 4, f"{mean:.0f} ± {half:.0f}", size=12, color="ink2")
    svg.save(path)


def throughput_chart(runs, slots, theme, path):
    width, height = 920, 420
    svg = Svg(width, height, theme, f"Throughput, {slots}-slot queue")
    t = svg.t
    svg.text(24, 32, f"Throughput, {slots}-slot queue (million events/s, higher is better)",
             size=15, color="ink", weight=600)
    svg.text(24, 52, "Mean of 10 processes with random memory layouts; whisker = 95% "
             "confidence interval. The consumer copies every payload.", size=12)
    svg.legend(24, 76)
    plot_l, plot_r, plot_t, plot_b = 84, width - 24, 104, height - 56
    peak = max(mean_ci(v)[0] + mean_ci(v)[1] for v in runs.values())
    ticks = nice_ticks(0, peak, count=5)
    scale = lambda v: plot_b - v / ticks[-1] * (plot_b - plot_t)
    for tick in ticks:
        svg.line(plot_l, scale(tick), plot_r, scale(tick), "grid" if tick else "base")
        svg.text(plot_l - 10, scale(tick) + 4, fmt(tick), size=11, color="muted", anchor="end")
    group_w = (plot_r - plot_l) / len(SIZES)
    bar_w, gap = 24, 2
    for g, size in enumerate(SIZES):
        center = plot_l + group_w * (g + 0.5)
        svg.text(center, plot_b + 22, f"{size} B", size=12, color="ink2", anchor="middle")
        first = center - (len(QUEUES) * bar_w + (len(QUEUES) - 1) * gap) / 2
        for i, queue in enumerate(QUEUES):
            mean, half = mean_ci(runs[(size, queue)])
            x, y = first + i * (bar_w + gap), scale(mean)
            r = min(4, plot_b - y)
            color = t["series"][queue]
            # square at the baseline, 4px rounded data end.
            svg.raw(f'<path d="M{x:.1f},{plot_b:.1f} V{y + r:.1f} Q{x:.1f},{y:.1f} {x + r:.1f},'
                    f'{y:.1f} H{x + bar_w - r:.1f} Q{x + bar_w:.1f},{y:.1f} {x + bar_w:.1f},'
                    f'{y + r:.1f} V{plot_b:.1f} Z" fill="{color}"><title>{NAMES[queue]} '
                    f'{size} B: {mean:.2f} ± {half:.2f} M events/s</title></path>')
            cx = x + bar_w / 2
            svg.line(cx, scale(mean + half), cx, scale(max(mean - half, 0)), "ink2", width=1.5)
            for end in (mean + half, max(mean - half, 0)):
                svg.line(cx - 4, scale(end), cx + 4, scale(end), "ink2", width=1.5)
            if queue == "lle":
                svg.text(cx, scale(mean + half) - 8, f"{mean:.1f}", size=12, color="ink",
                         anchor="middle", weight=600)
    svg.save(path)


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    source, target = sys.argv[1], sys.argv[2]
    os.makedirs(target, exist_ok=True)
    latency = load_latency(source)
    throughput = load_throughput(source, 1024)
    for theme in THEMES:
        latency_chart(latency, "p50_ns", "p50", theme,
                      os.path.join(target, f"latency-p50-{theme}.svg"))
        latency_chart(latency, "p99_ns", "p99", theme,
                      os.path.join(target, f"latency-p99-{theme}.svg"))
        throughput_chart(throughput, 1024, theme,
                         os.path.join(target, f"throughput-1024-{theme}.svg"))


if __name__ == "__main__":
    main()
