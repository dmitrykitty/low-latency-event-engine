#!/usr/bin/env python3
"""Draw the SPSC ring's shared-memory layout with its 128-byte prefetch pairs.

    python3 scripts/analysis/draw_ring_layout.py <output dir>

Writes ring-layout-light.svg and ring-layout-dark.svg. Standard library only.
"""

import os
import sys
from html import escape

FONT = 'system-ui, -apple-system, "Segoe UI", sans-serif'
THEMES = {
    "light": {"surface": "#fcfcfb", "ink": "#0b0b0b", "ink2": "#52514e", "muted": "#898781",
              "base": "#c3c2b7", "neutral": "#e1e0d9", "producer": "#2a78d6",
              "consumer": "#eb6834"},
    "dark": {"surface": "#1a1a19", "ink": "#f0efec", "ink2": "#c3c2b7", "muted": "#898781",
             "base": "#383835", "neutral": "#2c2c2a", "producer": "#3987e5",
             "consumer": "#d95926"},
}

# (offset, title, detail, role); role decides the colour: who writes the line per event.
LINES = [
    (0, "preamble", "geometry, state", "neutral"),
    (64, "producer cursor", "write_position", "producer"),
    (128, "consumer cursor", "read_position", "consumer"),
    (192, "slot 0", "producer writes", "slot"),
    (256, "slot 1", "", "slot"),
    (320, "slot 2", "", "slot"),
]

BOX_W, GAP, LEFT = 118, 2, 150


def x_of(offset):
    return LEFT + offset / 64 * (BOX_W + GAP)


def draw(theme, path):
    t = THEMES[theme]
    width, height = 920, 470
    out = [
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" '
        f'viewBox="0 0 {width} {height}" font-family=\'{FONT}\' role="img">',
        "<title>SPSC ring layout in shared memory and its 128-byte prefetch pairs</title>",
        f'<rect width="{width}" height="{height}" rx="8" fill="{t["surface"]}"/>',
    ]

    def text(x, y, content, size=12, color="ink2", anchor="start", weight=400):
        out.append(f'<text x="{x:.1f}" y="{y:.1f}" font-size="{size}" fill="{t[color]}" '
                   f'text-anchor="{anchor}" font-weight="{weight}">{escape(content)}</text>')

    text(24, 32, "SPSC ring layout in shared memory", size=15, color="ink", weight=600)
    text(24, 52, "Each box is one 64-byte cache line. Brackets mark the 128-byte line pairs "
         "that Intel's L2 spatial prefetcher fetches together.", size=12)

    legend_x = 24
    for role, label, opacity in (
            ("producer", "producer core writes on every event", 1.0),
            ("consumer", "consumer core writes on every event", 1.0),
            ("producer", "slot data: producer writes, consumer reads", 0.16)):
        out.append(f'<rect x="{legend_x}" y="68" width="12" height="12" rx="3" '
                   f'fill="{t[role]}" fill-opacity="{opacity}"/>')
        text(legend_x + 18, 78, label, size=12)
        legend_x += 30 + 6.3 * len(label)

    def lane(top, title, subtitle, base_shift):
        text(24, top, title, size=13, color="ink", weight=600)
        text(24, top + 18, subtitle, size=12)
        box_y = top + 52
        for offset, name, detail, role in LINES:
            x = x_of(offset)
            text(x, box_y - 8, f"+{offset}", size=11, color="muted")
            if role in ("producer", "consumer"):
                fill, opacity = t[role], 1.0
                ink = "#ffffff"
            elif role == "slot":
                fill, opacity, ink = t["producer"], 0.16, t["ink"]
            else:
                fill, opacity, ink = t["neutral"], 1.0, t["ink"]
            out.append(f'<rect x="{x:.1f}" y="{box_y}" width="{BOX_W}" height="44" rx="4" '
                       f'fill="{fill}" fill-opacity="{opacity}"/>')
            out.append(f'<text x="{x + 8:.1f}" y="{box_y + 19}" font-size="12" fill="{ink}" '
                       f'font-weight="600">{escape(name)}</text>')
            if detail:
                out.append(f'<text x="{x + 8:.1f}" y="{box_y + 35}" font-size="11" '
                           f'fill="{ink}" opacity="0.85">{escape(detail)}</text>')
        text(x_of(384) + 6, box_y + 27, "…", size=14, color="muted")
        # 128-byte pairs, aligned to the absolute address, so they shift with the base.
        bracket_y = box_y + 58
        pairs = [(-64 + base_shift + 128 * i) for i in range(4)]
        for start in pairs:
            lo, hi = max(start, -64), start + 128
            if hi <= 0 or lo >= 384:
                continue
            x1 = x_of(max(lo, 0)) + (0 if lo >= 0 else -BOX_W / 2)
            x2 = x_of(min(hi, 384)) - GAP
            contended = lo == 64 and hi == 192
            stroke = t["ink"] if contended else t["base"]
            sw = 2.5 if contended else 1.5
            out.append(f'<path d="M{x1 + 2:.1f},{bracket_y} v8 H{x2 - 2:.1f} v-8" fill="none" '
                       f'stroke="{stroke}" stroke-width="{sw}" stroke-linejoin="round"/>')
            mid = (x1 + x2) / 2
            if contended:
                text(mid, bracket_y + 26, "✕ one pair, written by both cores on every event",
                     size=12, color="ink", anchor="middle", weight=600)
            elif lo == 0 and hi == 128:
                text(mid, bracket_y + 26, "✓ only the producer writes here", size=12,
                     anchor="middle")
            elif lo == 128 and hi == 256:
                text(mid, bracket_y + 26, "✓ slot 0 is written once per lap", size=12,
                     anchor="middle")

    lane(118, "Ring memory on a 128-byte boundary (required)",
         "The two cursors fall into different pairs, so neither core's prefetches pull in "
         "the other core's hot line.", 64)
    lane(296, "Ring memory at 64 modulo 128 (rejected by initialize() and attach())",
         "Both cursors fall into one pair. Measured: p50 round trip 7-22% higher than on a "
         "128-byte boundary (16 B to 1 KB).", 0)

    with open(path, "w") as svg:
        svg.write("\n".join(out + ["</svg>"]) + "\n")


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    os.makedirs(sys.argv[1], exist_ok=True)
    for theme in THEMES:
        draw(theme, os.path.join(sys.argv[1], f"ring-layout-{theme}.svg"))


if __name__ == "__main__":
    main()
