#!/usr/bin/env python3
"""Draw the engine's event path (shared memory -> UDP -> shared memory) as SVG.

    python3 scripts/analysis/draw_architecture.py <output dir>

Writes architecture-light.svg and architecture-dark.svg. Standard library only.
"""

import os
import sys
from html import escape

FONT = 'system-ui, -apple-system, "Segoe UI", sans-serif'
THEMES = {
    "light": {"surface": "#fcfcfb", "band": "#f3f2ee", "ink": "#0b0b0b", "ink2": "#52514e",
              "muted": "#898781", "line": "#a8a69e", "card": "#ffffff", "border": "#e1e0d9",
              "engine": "#2a78d6", "shm": "#eb6834"},
    "dark": {"surface": "#1a1a19", "band": "#222220", "ink": "#f0efec", "ink2": "#c3c2b7",
             "muted": "#898781", "line": "#6f6e69", "card": "#262624", "border": "#383835",
             "engine": "#3987e5", "shm": "#d95926"},
}

WIDTH, HEIGHT = 960, 352
ROW_Y = 150  # top of the main row of cards
CARD_H = 104


class Canvas:
    def __init__(self, theme):
        self.t = THEMES[theme]
        self.parts = [
            f'<svg xmlns="http://www.w3.org/2000/svg" width="{WIDTH}" height="{HEIGHT}" '
            f'viewBox="0 0 {WIDTH} {HEIGHT}" font-family=\'{FONT}\' role="img">',
            "<title>Event path: producer, ingress ring, Sender, UDP, Receiver, egress ring, "
            "consumer</title>",
            '<defs><marker id="arrow" viewBox="0 0 10 10" refX="9" refY="5" markerWidth="7" '
            'markerHeight="7" orient="auto-start-reverse"><path d="M0,0 L10,5 L0,10 z" '
            f'fill="{self.t["line"]}"/></marker></defs>',
            f'<rect width="{WIDTH}" height="{HEIGHT}" rx="8" fill="{self.t["surface"]}"/>',
        ]

    def text(self, x, y, content, size=12, color="ink2", anchor="start", weight=400,
             opacity=1.0):
        self.parts.append(
            f'<text x="{x:.1f}" y="{y:.1f}" font-size="{size}" fill="{self.t[color]}" '
            f'text-anchor="{anchor}" font-weight="{weight}" opacity="{opacity}">'
            f"{escape(content)}</text>")

    def rect(self, x, y, w, h, fill, rx=6, opacity=1.0, stroke=None):
        border = f' stroke="{self.t[stroke]}" stroke-width="1"' if stroke else ""
        self.parts.append(f'<rect x="{x:.1f}" y="{y:.1f}" width="{w:.1f}" height="{h:.1f}" '
                          f'rx="{rx}" fill="{fill}" fill-opacity="{opacity}"{border}/>')

    def arrow(self, x1, y1, x2, y2, opacity=1.0, dashed=False):
        dash = ' stroke-dasharray="4 4"' if dashed else ""
        self.parts.append(
            f'<path d="M{x1:.1f},{y1:.1f} C{(x1 + x2) / 2:.1f},{y1:.1f} {(x1 + x2) / 2:.1f},'
            f'{y2:.1f} {x2:.1f},{y2:.1f}" fill="none" stroke="{self.t["line"]}" '
            f'stroke-width="1.5" opacity="{opacity}" marker-end="url(#arrow)"{dash}/>')

    def card(self, x, w, accent, title, lines, kind):
        t = self.t
        y = ROW_Y
        self.rect(x, y, w, CARD_H, t["card"], stroke="border")
        if accent:
            # role colour as a top bar; the text stays in ink.
            self.parts.append(f'<path d="M{x:.1f},{y + 6:.1f} Q{x:.1f},{y:.1f} {x + 6:.1f},'
                              f'{y:.1f} H{x + w - 6:.1f} Q{x + w:.1f},{y:.1f} {x + w:.1f},'
                              f'{y + 6:.1f} V{y + 4:.1f} H{x:.1f} Z" fill="{t[accent]}"/>')
            self.rect(x, y, w, 4, t[accent], rx=2)
        self.text(x + 10, y + 26, title, size=13, color="ink", weight=600)
        self.text(x + 10, y + 42, kind, size=11, color="muted")
        for i, line in enumerate(lines):
            self.text(x + 10, y + 62 + i * 15, line, size=11.5, color="ink2")

    def ring(self, x, w, title, lines):
        t = self.t
        y = ROW_Y
        self.rect(x, y, w, CARD_H, t["shm"], opacity=0.12, stroke="border")
        self.text(x + 10, y + 22, title, size=13, color="ink", weight=600)
        self.text(x + 10, y + 38, "POSIX shared memory", size=11, color="muted")
        cells, gap = 8, 3
        cell_w = (w - 20 - gap * (cells - 1)) / cells
        for i in range(cells):
            filled = 2 <= i <= 4  # a few queued events between the two cursors
            self.rect(x + 10 + i * (cell_w + gap), y + 48, cell_w, 14, t["shm"],
                      rx=2, opacity=0.85 if filled else 0.25)
        for i, line in enumerate(lines):
            self.text(x + 10, y + 80 + i * 14, line, size=11, color="ink2")

    def save(self, path):
        with open(path, "w") as out:
            out.write("\n".join(self.parts + ["</svg>"]) + "\n")


def draw(theme, path):
    c = Canvas(theme)
    t = c.t
    c.text(24, 32, "Event path: shared memory → UDP → shared memory", size=15, color="ink",
           weight=600)
    c.text(24, 52, "One producer and one consumer per ring; one LLEP DATA packet per event; "
           "the Sender fans each packet out to every receiver.", size=12)

    # host and network bands
    bands = [(16, 408, "Sender host"), (432, 100, "Network"), (540, 404, "Receiver host (one of N)")]
    for x, w, label in bands:
        c.rect(x, 72, w, 266, t["band"], rx=10)
        c.text(x + 12, 94, label, size=12, color="ink", weight=600)

    # main row: producer, ingress ring, Sender | Receiver, egress ring, consumer
    ring_lines = ["in: try_publish (copy)", "out: try_acquire (view)"]
    c.card(28, 96, None, "Producer", ["publishes", "events"], "application")
    c.ring(148, 140, "Ingress ring", ring_lines)
    c.card(312, 100, "engine", "Sender", ["encode LLEP", "release slot"], "lle::Sender")
    c.card(552, 104, "engine", "Receiver", ["decode, validate", "gap / dup detect"],
           "lle::Receiver")
    c.ring(680, 140, "Egress ring", ring_lines)
    c.card(844, 88, None, "Consumer", ["reads events", "in order"], "application")

    mid = ROW_Y + CARD_H / 2
    for x1, x2 in ((124, 148), (288, 312), (656, 680), (820, 844)):
        c.arrow(x1 + 1, mid, x2 - 2, mid)

    # UDP fan-out: the main receiver plus faded ones behind it
    c.arrow(412, mid, 552, mid)
    c.text(482, mid - 44, "LLEP DATA", size=11.5, color="ink", anchor="middle", weight=600)
    c.text(482, mid - 29, "over UDP,", size=11, color="ink2", anchor="middle")
    c.text(482, mid - 14, "unicast fan-out", size=11, color="ink2", anchor="middle")
    for i, label in enumerate(("receiver 2", "receiver N")):
        y = ROW_Y + CARD_H + 18 + i * 30
        c.arrow(414, mid + 8, 552, y + 11, opacity=0.6, dashed=True)
        c.rect(552, y, 104, 22, t["card"], rx=5, stroke="border", opacity=0.9)
        c.text(562, y + 15, label, size=11, color="muted")

    # what each side guarantees
    notes = [
        (28, ["The producer creates the ingress SHM; the Sender attaches.",
              "A full ring returns Full on publish; nothing is overwritten.",
              "Packets carry a session_id and a sequence that grows by 1."]),
        (680, ["The Receiver creates the egress SHM;",
               "the consumer attaches. Malformed packets",
               "and events that do not fit are dropped",
               "and counted."]),
    ]
    for x, lines in notes:
        for i, line in enumerate(lines):
            c.text(x, ROW_Y + CARD_H + 30 + i * 16, line, size=11, color="ink2")
    c.save(path)


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    os.makedirs(sys.argv[1], exist_ok=True)
    for theme in THEMES:
        draw(theme, os.path.join(sys.argv[1], f"architecture-{theme}.svg"))


if __name__ == "__main__":
    main()
