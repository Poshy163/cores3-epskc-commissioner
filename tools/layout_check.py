#!/usr/bin/env python3
"""
Screen-layout checker and previewer for the CoreS3 UI.

The display is 320x240 and every panel is placed with hand-computed offsets,
which is easy to get wrong: labels wrap to a second line, dropdown lists open
downward over whatever follows, and Back buttons end up on top of text. This
models the same geometry ui.c uses, reports overlaps before flashing, and
writes an SVG preview so a layout can be eyeballed without a board.

Keep it in step with ui.c when panels change.

Run:  python tools/layout_check.py [out.svg]
"""
import sys

W, H = 320, 240
HEADER = 44                  # title + wifi line sit above every panel
PANEL_H = H - HEADER         # mk_panel(): BSP_LCD_V_RES - 44, bottom-aligned
BACK_STRIP = 56              # height mk_scroll_area() leaves for the Back button

# Montserrat metrics, from the LVGL font files.
LINE_H = {14: 17, 16: 19, 20: 24}
CHAR_W = {14: 8.0, 16: 9.1, 20: 11.4}   # mean advance, mixed-case


class Box:
    def __init__(self, name, x, y, w, h, kind="widget"):
        self.name, self.x, self.y, self.w, self.h, self.kind = name, x, y, w, h, kind

    @property
    def r(self):
        return self.x + self.w

    @property
    def b(self):
        return self.y + self.h

    def overlaps(self, o):
        return not (self.r <= o.x or o.r <= self.x or self.b <= o.y or o.b <= self.y)

    def __repr__(self):
        return f"{self.name}({self.x},{self.y} {self.w}x{self.h})"


def align(a, w, h, dx, dy, cw=W, ch=PANEL_H):
    if a == "TOP_MID":        return (cw - w) // 2 + dx, dy
    if a == "TOP_LEFT":       return dx, dy
    if a == "TOP_RIGHT":      return cw - w + dx, dy
    if a == "BOTTOM_MID":     return (cw - w) // 2 + dx, ch - h + dy
    if a == "BOTTOM_LEFT":    return dx, ch - h + dy
    if a == "BOTTOM_RIGHT":   return cw - w + dx, ch - h + dy
    raise ValueError(a)


def label(name, text, font, a, dx, dy, wrap_w=None, kind="label"):
    lines = text.split("\n")
    if wrap_w:
        grown = []
        for ln in lines:
            per = max(1, int(wrap_w / CHAR_W[font]))
            grown += [ln[i:i + per] for i in range(0, len(ln), per)] or [""]
        lines = grown
    w = wrap_w or int(max((len(l) for l in lines), default=0) * CHAR_W[font]) + 2
    h = len(lines) * LINE_H[font]
    x, y = align(a, w, h, dx, dy)
    return Box(name, x, y, w, h, kind)


def button(name, a, dx, dy, w, h):
    x, y = align(a, w, h, dx, dy)
    return Box(name, x, y, w, h, "button")


def roller(name, a, dx, dy, w=132, rows=2):
    """Fixed size: unlike a dropdown, nothing opens over later widgets."""
    h = rows * 22 + 12
    x, y = align(a, w, h, dx, dy)
    return Box(name, x, y, w, h, "widget")


def scroll_area(name="scroll"):
    """mk_scroll_area(): fills the panel except the Back-button strip."""
    w, h = W - 12, PANEL_H - BACK_STRIP
    return Box(name, (W - w) // 2, 2, w, h, "scroll")


def panels():
    P = {}
    bw, bh, col = 148, 46, 78

    P["main"] = [
        label("status", "Ready", 20, "TOP_MID", 0, 8),
        label("network", "CoreS3-FC09  ch 20  pan 0xa78b\nthread: leader  -43 dBm  2 devices",
              14, "TOP_MID", 0, 40, wrap_w=W - 20),
        button("Scan", "BOTTOM_MID", -col, -58, bw, bh),
        button("New network", "BOTTOM_MID", col, -58, bw, bh),
        button("Network", "BOTTOM_MID", -col, -6, bw, bh),
        button("Settings", "BOTTOM_MID", col, -6, bw, bh),
    ]

    P["settings"] = [
        Box("list", 10, 2, W - 20, 140, "scroll"),
        button("Back", "BOTTOM_MID", 0, -6, 110, 40),
    ]

    P["screen"] = [
        label("cap:Brightness", "Brightness", 14, "TOP_LEFT", 8, 4),
        Box("slider", 20, 26, W - 40, 10),
        label("cap:Sleep", "Sleep after", 14, "TOP_LEFT", 8, 52),
        roller("roll:sleep", "TOP_RIGHT", -8, 46),
        label("cap:KeepOn", "Stay on when\nplugged in", 14, "TOP_LEFT", 8, 104),
        Box("sw:keepon", W - 58, 104, 50, 28),
        button("Back", "BOTTOM_MID", 0, -6, 110, 40),
    ]

    P["power"] = [
        scroll_area(),
        label("info",
              "Battery  92%   4.16 V   constant current\n"
              "USB      present   4.68 V\n"
              "System   4.37 V\n"
              "Temp     PMIC 40 C   ESP32 37 C\n\n"
              "Discharge  measuring (10 min on battery)\n\n"
              "The AXP2101 has no current sensor,\nso there is no live mW figure.",
              14, "TOP_LEFT", 10, 6, wrap_w=W - 28, kind="scrolled"),
        button("Back", "BOTTOM_MID", 0, -6, 110, 40),
    ]

    P["thread"] = [
        label("cap:Router", "Prefer router role", 14, "TOP_LEFT", 8, 8),
        Box("sw:router", W - 58, 2, 50, 28),
        label("cap:Chan", "New network\nchannel", 14, "TOP_LEFT", 8, 42),
        roller("roll:chan", "TOP_RIGHT", -8, 38),
        label("cap:Share", "Share code\nlifetime", 14, "TOP_LEFT", 8, 98),
        roller("roll:share", "TOP_RIGHT", -8, 94),
        button("Back", "BOTTOM_MID", 0, -6, 110, 40),
    ]

    P["about"] = [
        scroll_area(),
        label("info",
              "Firmware   824e3d5\nESP-IDF    v5.5.4\nIP         192.168.1.58\n"
              "MAC        68:ee:8f:d8:34:6c\nUptime     0h 12m 30s\n"
              "Last reset power on\nFree RAM   25 KB internal, 7954 KB PSRAM",
              14, "TOP_LEFT", 10, 6, wrap_w=W - 28, kind="scrolled"),
        button("Back", "BOTTOM_MID", 0, -6, 110, 40),
    ]

    P["net"] = [
        label("info", "CoreS3-FC09\nchannel 20   pan 0xa78b\n"
                      "role: leader   2 devices\nparent link -43 dBm",
              14, "TOP_MID", 0, 6, wrap_w=W - 20),
        button("Share", "BOTTOM_MID", -117, -6, 72, 40),
        button("QR", "BOTTOM_MID", -39, -6, 72, 40),
        button("Forget", "BOTTOM_MID", 39, -6, 72, 40),
        button("Back", "BOTTOM_MID", 117, -6, 72, 40),
    ]

    P["share"] = [
        label("hint", "Enter this code on the commissioner", 14, "TOP_MID", 0, 8, wrap_w=W - 20),
        label("code", "032 315 950", 20, "TOP_MID", 0, 44),
        label("state", "Waiting for a commissioner   4:58", 14, "TOP_MID", 0, 90, wrap_w=W - 20),
        button("Close", "BOTTOM_MID", 0, -6, 110, 40),
    ]
    return P


def check(name, boxes):
    """Overlaps and out-of-bounds. Content marked 'scrolled' lives inside a
    'scroll' box, so it may legitimately be taller than the panel."""
    problems = []
    for b in boxes:
        if b.kind == "scrolled":
            continue
        if b.x < 0 or b.y < 0 or b.r > W or b.b > PANEL_H:
            problems.append(f"  OFF-PANEL  {b} (panel is {W}x{PANEL_H})")

    for i, a in enumerate(boxes):
        for b in boxes[i + 1:]:
            pair = {a.kind, b.kind}
            if pair == {"scroll", "scrolled"}:
                continue          # content inside its own viewport
            if "scrolled" in pair and "button" in pair:
                # The viewport clips it, so only flag if the viewport itself
                # reaches the button.
                continue
            if a.overlaps(b):
                problems.append(f"  OVERLAP  {a} <-> {b}")
    return problems


def svg(P, path):
    pad, cols = 16, 2
    rows = (len(P) + cols - 1) // cols
    colour = {"button": "#D9904F", "label": "#4ECBB8", "scrolled": "#4ECBB8",
              "scroll": "#8A937F", "widget": "#7aa2f7"}
    out = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{cols*(W+pad)+pad}" '
           f'height="{rows*(H+pad+22)+pad}" style="background:#222">']
    for i, (name, boxes) in enumerate(P.items()):
        ox = pad + (i % cols) * (W + pad)
        oy = pad + (i // cols) * (H + pad + 22)
        out.append(f'<text x="{ox}" y="{oy-4}" fill="#eee" font-size="12" '
                   f'font-family="monospace">{name}</text>')
        out.append(f'<rect x="{ox}" y="{oy}" width="{W}" height="{H}" fill="#101410" stroke="#555"/>')
        out.append(f'<rect x="{ox}" y="{oy}" width="{W}" height="{HEADER}" fill="#1B2119"/>')
        out.append(f'<text x="{ox+6}" y="{oy+16}" fill="#8A937F" font-size="10" '
                   f'font-family="monospace">header</text>')
        for b in boxes:
            dash = ' stroke-dasharray="3,2"' if b.kind == "scroll" else ""
            out.append(f'<rect x="{ox+b.x}" y="{oy+HEADER+b.y}" width="{b.w}" height="{b.h}" '
                       f'fill="{colour[b.kind]}" fill-opacity="0.45" '
                       f'stroke="{colour[b.kind]}"{dash}/>')
            out.append(f'<text x="{ox+b.x+2}" y="{oy+HEADER+b.y+10}" fill="#fff" font-size="8" '
                       f'font-family="monospace">{b.name}</text>')
    out.append("</svg>")
    open(path, "w", encoding="utf-8").write("\n".join(out))


def main():
    P = panels()
    bad = 0
    for name, boxes in P.items():
        probs = check(name, boxes)
        if probs:
            bad += len(probs)
            print(f"[{name}]")
            print("\n".join(probs))
    print(f"{bad} problem(s); panel area is {W}x{PANEL_H} px below a {HEADER} px header")
    out = sys.argv[1] if len(sys.argv) > 1 else "layout.svg"
    svg(P, out)
    print(f"preview written to {out}")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
