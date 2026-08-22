#!/usr/bin/env python3
"""
Screen-layout checker and SVG previewer for the CoreS3 touch UI.

The display is only 320x240. LVGL aligns children inside the parent's content
area, so this model includes parent padding, representative worst-case text,
scroll clipping, the shared header, and every page built by ui.c.

The checker has no third-party dependencies. It exits non-zero for off-screen
objects, visible overlaps, clipped text, uncovered ui.c page builders, or
undersized ordinary controls. Compact keypad controls and keyboard/matrix
internals are documented exceptions.

Run:
    python -B tools/layout_check.py [out.svg]
    python -B tools/layout_check.py --no-svg
"""

from __future__ import annotations

import argparse
import html
import re
import sys
from dataclasses import dataclass, field
from pathlib import Path


W, H = 320, 240
HEADER = 44
PANEL_H = H - HEADER
PANEL_PAD = 6
BACK_STRIP = 56
BATT_W, BATT_H = 62, 24
MIN_TOUCH = 40

# LVGL's compiled Montserrat line heights.
LINE_H = {14: 16, 16: 18, 20: 22}

# Exact ASCII advances from lv_font_montserrat_14.c, in pixels. The 16- and
# 20-pixel fonts are the same face and scale closely enough for layout checks.
# Index zero is U+0020 (space), and the final entry is U+007E (~).
ADVANCE_14 = (
    3.75, 3.75, 5.50, 9.81, 8.69, 11.81, 9.62, 2.94, 4.69, 4.75,
    5.62, 8.12, 3.19, 5.38, 3.19, 4.94, 9.31, 5.19, 8.06, 8.00,
    9.38, 8.06, 8.62, 8.38, 9.00, 8.62, 3.19, 3.19, 8.12, 8.12,
    8.12, 8.00, 14.50, 10.25, 10.62, 10.12, 11.56, 9.38, 8.88, 10.81,
    11.38, 4.31, 7.19, 10.06, 8.31, 13.38, 11.38, 11.75, 10.12, 11.75,
    10.19, 8.69, 8.19, 11.06, 9.94, 15.75, 9.44, 9.06, 9.19, 4.69,
    4.94, 4.69, 8.19, 7.00, 8.38, 8.38, 9.56, 8.00, 9.56, 8.56,
    4.94, 9.69, 9.56, 3.88, 4.00, 8.62, 3.88, 14.81, 9.56, 8.88,
    9.56, 9.56, 5.75, 7.00, 5.81, 9.50, 7.81, 12.56, 7.75, 7.81,
    7.31, 4.94, 4.19, 4.94, 8.12,
)


@dataclass(frozen=True)
class Rect:
    x: float
    y: float
    w: float
    h: float

    @property
    def r(self) -> float:
        return self.x + self.w

    @property
    def b(self) -> float:
        return self.y + self.h

    def overlaps(self, other: "Rect") -> bool:
        return not (
            self.r <= other.x
            or other.r <= self.x
            or self.b <= other.y
            or other.b <= self.y
        )

    def intersection(self, other: "Rect") -> "Rect | None":
        x1, y1 = max(self.x, other.x), max(self.y, other.y)
        x2, y2 = min(self.r, other.r), min(self.b, other.b)
        if x2 <= x1 or y2 <= y1:
            return None
        return Rect(x1, y1, x2 - x1, y2 - y1)


@dataclass
class Box:
    name: str
    x: float
    y: float
    w: float
    h: float
    kind: str = "widget"
    text: str | None = None
    font: int | None = None
    text_mode: str | None = None  # wrap, dot, or scroll
    pad_x: int = 0
    pad_y: int = 0
    border: int = 0
    required_h: float | None = None
    ink: Rect | None = None
    touch: bool = False
    touch_size: tuple[float, float] | None = None
    compact: bool = False
    parent: str | None = None
    clip: str | None = None
    container: bool = False
    allow_overlap: set[str] = field(default_factory=set)

    @property
    def r(self) -> float:
        return self.x + self.w

    @property
    def b(self) -> float:
        return self.y + self.h

    @property
    def bounds(self) -> Rect:
        return Rect(self.x, self.y, self.w, self.h)

    def __repr__(self) -> str:
        return f"{self.name}({self.x:g},{self.y:g} {self.w:g}x{self.h:g})"


def glyph_width(ch: str, font: int) -> float:
    code = ord(ch)
    if 0x20 <= code <= 0x7E:
        return ADVANCE_14[code - 0x20] * font / 14
    # LVGL symbols are uncommon in modeled prose. This is conservative.
    return font * 0.9


def text_width(text: str, font: int) -> float:
    return sum(glyph_width(ch, font) for ch in text)


def wrap_physical_line(line: str, font: int, width: float) -> list[str]:
    """Approximate LV_LABEL_LONG_WRAP, preferring whitespace boundaries."""
    if line == "":
        return [""]
    out: list[str] = []
    rest = line
    while text_width(rest, font) > width:
        fit = 0
        last_space = -1
        used = 0.0
        for i, ch in enumerate(rest):
            advance = glyph_width(ch, font)
            if used + advance > width:
                break
            used += advance
            fit = i + 1
            if ch.isspace():
                last_space = i
        if fit == 0:
            fit = 1
        cut = last_space if last_space > 0 else fit
        out.append(rest[:cut].rstrip())
        rest = rest[cut:].lstrip()
    out.append(rest)
    return out


def text_lines(text: str, font: int, width: float, mode: str) -> list[str]:
    if mode in {"dot", "scroll"}:
        return text.split("\n")
    lines: list[str] = []
    for physical in text.split("\n"):
        lines.extend(wrap_physical_line(physical, font, width))
    return lines or [""]


def align(
    where: str,
    w: float,
    h: float,
    dx: float,
    dy: float,
    *,
    cw: float = W,
    ch: float = PANEL_H,
    pad: float = PANEL_PAD,
) -> tuple[float, float]:
    """Match LVGL alignment inside a parent's padded content area."""
    content_w, content_h = cw - 2 * pad, ch - 2 * pad
    if where == "TOP_MID":
        return pad + (content_w - w) / 2 + dx, pad + dy
    if where == "TOP_LEFT":
        return pad + dx, pad + dy
    if where == "TOP_RIGHT":
        return pad + content_w - w + dx, pad + dy
    if where == "BOTTOM_MID":
        return pad + (content_w - w) / 2 + dx, pad + content_h - h + dy
    if where == "BOTTOM_LEFT":
        return pad + dx, pad + content_h - h + dy
    if where == "BOTTOM_RIGHT":
        return pad + content_w - w + dx, pad + content_h - h + dy
    raise ValueError(where)


def label(
    name: str,
    text: str,
    font: int,
    where: str,
    dx: float,
    dy: float,
    *,
    object_w: float = W - 20,
    fixed_h: float | None = None,
    mode: str = "wrap",
    text_align: str = "center",
    pad_x: int = 0,
    pad_y: int = 0,
    border: int = 0,
    background: bool = False,
    kind: str = "label",
    parent_pad: float = PANEL_PAD,
    parent_w: float = W,
    parent_h: float = PANEL_H,
    parent: str | None = None,
    clip: str | None = None,
) -> Box:
    inner_w = max(1.0, object_w - 2 * (pad_x + border))
    lines = text_lines(text, font, inner_w, mode)
    required_text_h = len(lines) * LINE_H[font]
    required_h = required_text_h + 2 * (pad_y + border)
    object_h = fixed_h if fixed_h is not None else required_h
    x, y = align(
        where, object_w, object_h, dx, dy,
        cw=parent_w, ch=parent_h, pad=parent_pad,
    )

    if background:
        ink = Rect(x, y, object_w, object_h)
    else:
        widths = [min(inner_w, text_width(line, font)) for line in lines]
        ink_w = max(widths, default=0.0)
        if text_align == "left":
            ink_x = x + pad_x + border
        elif text_align == "right":
            ink_x = x + object_w - pad_x - border - ink_w
        else:
            ink_x = x + (object_w - ink_w) / 2
        ink = Rect(ink_x, y + pad_y + border, ink_w, required_text_h)

    return Box(
        name, x, y, object_w, object_h, kind,
        text=text, font=font, text_mode=mode,
        pad_x=pad_x, pad_y=pad_y, border=border,
        required_h=required_h, ink=ink,
        parent=parent, clip=clip,
    )


def label_at(
    name: str,
    text: str,
    font: int,
    x: float,
    y: float,
    w: float,
    *,
    mode: str = "wrap",
    parent: str | None = None,
    clip: str | None = None,
) -> Box:
    lines = text_lines(text, font, w, mode)
    h = len(lines) * LINE_H[font]
    ink_w = min(w, max((text_width(line, font) for line in lines), default=0.0))
    return Box(
        name, x, y, w, h, "label",
        text=text, font=font, text_mode=mode, required_h=h,
        ink=Rect(x, y, ink_w, h), parent=parent, clip=clip,
    )


def button(
    name: str,
    where: str,
    dx: float,
    dy: float,
    w: float,
    h: float,
    *,
    compact: bool = False,
) -> Box:
    x, y = align(where, w, h, dx, dy)
    return Box(name, x, y, w, h, "button", touch=True, compact=compact)


def container(name: str, w: float, h: float, where: str, dx: float, dy: float, kind: str) -> Box:
    x, y = align(where, w, h, dx, dy)
    return Box(name, x, y, w, h, kind, container=True)


def add_list_rows(
    boxes: list[Box], list_name: str, texts: list[str], label_width: int,
    *, label_offset: int = 30,
) -> None:
    owner = next(box for box in boxes if box.name == list_name)
    # Default-theme small-display list padding is 13 px horizontally and zero
    # vertically at the CoreS3 DPI. ui.c fixes each button at 40 px high.
    x, y = owner.x + 13, owner.y
    row_w = owner.w - 26
    for i, text in enumerate(texts):
        boxes.append(Box(
            f"row:{i + 1}", x, y + i * 40, row_w, 40, "list-row",
            text=text, font=14, text_mode="scroll", required_h=LINE_H[14],
            ink=Rect(x + label_offset, y + i * 40 + 11, label_width, LINE_H[14]),
            pad_x=(row_w - label_width) // 2,
            touch=True, parent=list_name, clip=list_name,
        ))


def content_box(name: str = "content") -> Box:
    return container(name, W - 12, PANEL_H - BACK_STRIP - 6, "TOP_MID", 0, 2, "scroll")


def flex_settings(name: str, rows: list[tuple[str, int, str]]) -> list[Box]:
    """Model mk_content(..., true), mk_row(), and right-side controls."""
    content = content_box(f"{name}:content")
    boxes = [content]
    x = content.x + 4
    y = content.y + 4
    row_w = content.w - 8
    for i, (caption, row_h, control_type) in enumerate(rows):
        if control_type == "slider":
            control_w, control_h = 150, 20  # includes knob overhang
            touch_size = (182, 40)  # 16 px extended click area around 150x8
        elif control_type == "switch":
            control_w, control_h = 50, 26
            touch_size = (64, 40)  # seven-pixel extended click area
        elif control_type == "roller":
            control_w, control_h = 118, 56
            touch_size = (control_w, control_h)
        else:
            raise ValueError(control_type)
        label_w = row_w - control_w - 8
        wrapped = text_lines(caption, 14, label_w, "wrap")
        label_h = len(wrapped) * LINE_H[14]
        boxes.append(label_at(
            f"caption:{caption}", caption, 14, x, y + (row_h - label_h) / 2,
            label_w, parent=content.name, clip=content.name,
        ))
        boxes.append(Box(
            f"{control_type}:{i + 1}", x + row_w - control_w,
            y + (row_h - control_h) / 2, control_w, control_h, "widget",
            touch=True, touch_size=touch_size,
            parent=content.name, clip=content.name,
        ))
        y += row_h + 6
    boxes.append(button("Back", "BOTTOM_MID", 0, -6, 110, 40))
    return boxes


def header_boxes(title: str, maximum_wifi: bool = True) -> list[Box]:
    wifi = (
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ123456  |  255.255.255.255"
        if maximum_wifi else "Workshop Wi-Fi  |  192.168.100.254"
    )
    title_box = label(
        "header:title", title, 16, "TOP_LEFT", 8, 3,
        object_w=W - 84, mode="dot", text_align="left",
        parent_pad=0, parent_h=HEADER,
    )
    wifi_box = label(
        "header:wifi", wifi, 14, "TOP_LEFT", 8, 24,
        object_w=W - 84, mode="dot", text_align="left",
        parent_pad=0, parent_h=HEADER,
    )
    batt_x, batt_y = align("TOP_RIGHT", BATT_W, BATT_H, -8, 4, ch=HEADER, pad=0)
    battery_text = "⚡100%"
    battery_text_w = text_width(battery_text, 14)
    battery_text_x = batt_x + 4 + (BATT_W - 8 - battery_text_w) / 2
    return [
        title_box,
        wifi_box,
        Box(
            "header:battery", batt_x, batt_y, BATT_W, BATT_H, "battery",
            container=True,
        ),
        Box(
            "header:battery-label", battery_text_x, batt_y + 4,
            battery_text_w, LINE_H[14], "label",
            text=battery_text, font=14, text_mode="dot",
            required_h=LINE_H[14],
            ink=Rect(battery_text_x, batt_y + 4, battery_text_w, LINE_H[14]),
            parent="header:battery",
        ),
        Box("header:battery-nub", batt_x + BATT_W + 1, batt_y + 7, 3, 10, "battery"),
    ]


PANEL_TITLES = {
    "home": "Thread Commissioner",
    "border_agents": "Join a network",
    "keypad": "Enter share code",
    "qr_camera": "Scan share code",
    "result": "Commissioning",
    "wifi": "Wi-Fi networks",
    "wifi_password": "Wi-Fi password",
    "network": "Thread network",
    "thread_activity": "Thread activity",
    "share": "Share credentials",
    "dataset_qr": "Dataset QR",
    "settings": "Settings",
    "display": "Display",
    "power": "Power",
    "thread": "Thread & REST",
    "device_name": "Device name",
    "diagnostics": "Diagnostics",
}


def panels() -> dict[str, list[Box]]:
    p: dict[str, list[Box]] = {}
    bw, bh, col = 148, 46, 78

    p["home"] = [
        label(
            "status", "Wi-Fi failed - previous network restored", 16,
            "TOP_LEFT", 8, 2, object_w=W - 20, mode="dot", text_align="left",
        ),
        label(
            "network-card",
            "ABCDEFGHIJKLMNOP | ch 26 | PAN FFFF\n"
            "child | -128 dBm | 63 routers known",
            14, "TOP_MID", 0, 26, object_w=W - 12, fixed_h=52,
            mode="wrap", text_align="left", pad_x=6, pad_y=6, border=1,
            background=True, kind="card",
        ),
        button("Join existing", "BOTTOM_MID", -col, -58, bw, bh),
        button("Create network", "BOTTOM_MID", col, -58, bw, bh),
        button("Manage network", "BOTTOM_MID", -col, -6, bw, bh),
        button("Settings", "BOTTOM_MID", col, -6, bw, bh),
    ]

    p["border_agents"] = [
        container("agents:list", W - 20, 118, "TOP_MID", 0, 2, "list"),
        button("Rescan", "BOTTOM_MID", -78, -6, 148, 40),
        button("Back", "BOTTOM_MID", 78, -6, 148, 40),
    ]
    add_list_rows(
        p["border_agents"], "agents:list",
        [
            "BorderAgent-XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX  (255.255.255.255)",
            "Living-room border router  (192.168.100.254)",
            "Third active share code  (10.255.255.254)",
        ], 236,
    )

    matrix_x, matrix_y = align("BOTTOM_MID", W - 24, 118, 0, -2)
    p["keypad"] = [
        label(
            "target", "BorderAgent-XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX:65535", 14,
            "TOP_LEFT", 2, 4, object_w=W - 80, mode="dot", text_align="left",
        ),
        label("code", "123456789", 20, "TOP_MID", 0, 28),
        Box(
            "numeric keypad (4 rows)", matrix_x, matrix_y, W - 24, 118,
            "keypad", compact=True, container=True,
        ),
        button("QR", "TOP_RIGHT", -2, 26, 56, 30, compact=True),
        button("Back", "TOP_LEFT", 2, 26, 56, 30, compact=True),
    ]
    key_names = ["1", "2", "3", "4", "5", "6", "7", "8", "9", "erase", "0", "OK"]
    key_w = (W - 28) / 3
    key_h = 114 / 4
    for key_i, key_name in enumerate(key_names):
        key_col, key_row = key_i % 3, key_i // 3
        p["keypad"].append(Box(
            f"key:{key_name}", matrix_x + 2 + key_col * key_w,
            matrix_y + 2 + key_row * key_h, key_w, key_h,
            "keypad-key", touch=True, compact=True,
            parent="numeric keypad (4 rows)",
        ))

    preview_x, preview_y = align("TOP_MID", 320, 150, 0, -6)
    p["qr_camera"] = [
        Box("camera preview", preview_x, preview_y, 320, 150, "media"),
        label(
            "camera hint", "Scanned, but no 9-digit code", 14,
            "BOTTOM_MID", 0, -50,
        ),
        button("Retry", "BOTTOM_MID", -78, 0, 148, 40),
        button("Cancel", "BOTTOM_MID", 78, 0, 148, 40),
    ]
    p["qr_camera"][1].allow_overlap.add("camera preview")

    p["result"] = [
        label("result title", "Credentials saved", 20, "TOP_MID", 0, 16),
        label(
            "result body",
            "ABCDEFGHIJKLMNOP\nchannel 26   pan 0xffff\n\n"
            "Still attaching. Check Network or Diagnostics.",
            14, "TOP_MID", 0, 54,
        ),
        button("Done", "BOTTOM_MID", 0, -6, 110, 40),
    ]

    p["wifi"] = [
        container("wifi:list", W - 20, 134, "TOP_MID", 0, 2, "list"),
        button("Rescan", "BOTTOM_LEFT", 4, -6, 96, 40),
        button("Leave", "BOTTOM_MID", 0, -6, 96, 40),
        button("Back", "BOTTOM_RIGHT", -4, -6, 96, 40),
    ]
    add_list_rows(
        p["wifi"], "wifi:list",
        [
            "ABCDEFGHIJKLMNOPQRSTUVWXYZ123456  (-128)",
            "Workshop devices  (-67)",
            "Open guest network  (-89)",
        ], 236,
    )

    text_x, text_y = align("TOP_MID", W - 24, 40, 0, 20)
    keyboard_x, keyboard_y = align("BOTTOM_MID", W - 12, 122, 0, 0)
    p["wifi_password"] = [
        label(
            "SSID", "ABCDEFGHIJKLMNOPQRSTUVWXYZ123456", 14,
            "TOP_LEFT", 2, 0, object_w=W - 24,
            mode="dot", text_align="left",
        ),
        Box(
            "password field", text_x, text_y, W - 24, 40, "text-area",
            text="W" * 64, font=14, text_mode="scroll", touch=True,
        ),
        Box("keyboard", keyboard_x, keyboard_y, W - 12, 122, "keyboard", compact=True),
    ]

    p["network"] = [
        label(
            "network-card",
            "ABCDEFGHIJKLMNOP\nchannel 26   pan 0xffff\n"
            "role: child | attached 49710d\n"
            "parent 0xffff | -128 dBm | LQI 3",
            14, "TOP_MID", 0, 2, object_w=W - 12, fixed_h=80,
            mode="wrap", text_align="left", pad_x=6, pad_y=6, border=1,
            background=True, kind="card",
        ),
        button("Activity", "BOTTOM_MID", -104, -56, 96, 44),
        button("Share", "BOTTOM_MID", 0, -56, 96, 44),
        button("Dataset", "BOTTOM_MID", 104, -56, 96, 44),
        button("Forget network", "BOTTOM_MID", -78, -6, 148, 44),
        button("Back", "BOTTOM_MID", 78, -6, 148, 44),
    ]

    p["thread_activity"] = [
        label(
            "activity-card",
            "Attached: child | 49710d\n"
            "Parent: 0xFFFF | signal pending | LQI 3\n"
            "Local table: 63 routers\n"
            "Seen partial: 65598 devices | 63R 65535C\n"
            "Frames/60s: TX 9.9M RX 9.9M\n"
            "Topology: try Refresh mesh",
            14, "TOP_MID", 0, 2, object_w=W - 12, fixed_h=124,
            mode="wrap", text_align="left", pad_x=6, pad_y=6, border=1,
            background=True, kind="card",
        ),
        button("Refresh mesh", "BOTTOM_MID", -78, -6, 148, 44),
        button("Back", "BOTTOM_MID", 78, -6, 148, 44),
    ]

    p["share"] = [
        label("hint", "Enter this code on the commissioner", 14, "TOP_MID", 0, 8),
        label("code", "123 456 789", 20, "TOP_MID", 0, 44),
        label("state", "Used or expired - close and share again", 14, "TOP_MID", 0, 90),
        button("New code", "BOTTOM_MID", -78, -6, 148, 40),
        button("Close", "BOTTOM_MID", 78, -6, 148, 40),
    ]

    qr_x, qr_y = align("TOP_LEFT", 140, 140, 2, 2)
    p["dataset_qr"] = [
        Box("dataset QR", qr_x, qr_y, 140, 140, "qrcode"),
        label(
            "secret warning", "Dataset TLVs - contains the network key", 14,
            "TOP_RIGHT", -2, 8, object_w=W - 166,
            text_align="left",
        ),
        button("Back", "BOTTOM_RIGHT", -2, -6, 110, 40),
    ]

    p["settings"] = [
        container("settings:list", W - 20, 134, "TOP_MID", 0, 2, "list"),
        button("Back", "BOTTOM_MID", 0, -6, 110, 40),
    ]
    add_list_rows(
        p["settings"], "settings:list",
        [
            "Wi-Fi", "Screen", "Power", "Thread", "Device name", "Diagnostics",
            "Reboot", "Factory reset",
        ], 258, label_offset=8,
    )

    p["display"] = flex_settings("display", [
        ("Brightness", 40, "slider"),
        ("Sleep after", 62, "roller"),
        ("Stay on when plugged in", 40, "switch"),
    ])

    power_content = content_box("power:content")
    p["power"] = [power_content]
    p["power"].append(label_at(
        "power details",
        "Battery  100%   4.16 V   constant current\n"
        "USB      present   4.68 V\nSystem   4.37 V\n"
        "Temp     PMIC 40 C   ESP32 37 C\n\n"
        "Discharge  measuring (10 min on battery)\n\n"
        "The AXP2101 has no current sensor,\nso there is no live mW figure.",
        14, power_content.x + 4, power_content.y + 4, W - 28,
        parent=power_content.name, clip=power_content.name,
    ))
    p["power"].append(button("Back", "BOTTOM_MID", 0, -6, 110, 40))

    p["thread"] = flex_settings("thread", [
        ("Prefer router role", 40, "switch"),
        ("REST API (LAN, no auth)", 40, "switch"),
        ("ePSKc over REST", 40, "switch"),
        ("New network channel", 62, "roller"),
        ("Share code lifetime", 62, "roller"),
    ])

    name_x, name_y = align("TOP_MID", W - 24, 40, 0, 20)
    name_keyboard_x, name_keyboard_y = align("BOTTOM_MID", W - 12, 122, 0, 0)
    p["device_name"] = [
        label(
            "caption", "Device name (mDNS)", 14,
            "TOP_LEFT", 8, 0, object_w=180, text_align="left",
        ),
        Box(
            "name field", name_x, name_y, W - 24, 40, "text-area",
            text="ABCDEFGHIJKLMNOPQRSTUVWXYZ123456", font=14,
            text_mode="scroll", touch=True,
        ),
        Box("keyboard", name_keyboard_x, name_keyboard_y, W - 12, 122, "keyboard", compact=True),
    ]

    about_content = content_box("diagnostics:content")
    p["diagnostics"] = [about_content]
    p["diagnostics"].append(label_at(
        "diagnostic details",
        "Firmware   824e3d5-dirty\nESP-IDF    v5.5.4\n"
        "IP         255.255.255.255\nMAC        ff:ff:ff:ff:ff:ff\n"
        "Uptime     9999h 59m 59s\nLast reset interrupt watchdog\n"
        "Free RAM   999 KB internal, 8192 KB PSRAM",
        14, about_content.x + 4, about_content.y + 4, W - 28,
        parent=about_content.name, clip=about_content.name,
    ))
    p["diagnostics"].append(button("Back", "BOTTOM_MID", 0, -6, 110, 40))

    return p


def effective_rect(box: Box, boxes_by_name: dict[str, Box]) -> Rect | None:
    rect = box.ink or box.bounds
    if box.clip:
        owner = boxes_by_name.get(box.clip)
        if owner is None:
            return None
        return rect.intersection(owner.bounds)
    return rect


def check(boxes: list[Box], *, width: int = W, height: int = PANEL_H) -> tuple[list[str], int, int]:
    problems: list[str] = []
    truncations = 0
    compact_controls = 0
    by_name = {box.name: box for box in boxes}

    for box in boxes:
        if box.clip:
            owner = by_name.get(box.clip)
            if owner is None:
                problems.append(f"  BAD-CLIP  {box.name} references missing {box.clip}")
            elif box.x < owner.x or box.r > owner.r:
                problems.append(f"  OFF-VIEWPORT  {box} outside horizontal clip {owner}")
        elif box.x < 0 or box.y < 0 or box.r > width or box.b > height:
            problems.append(f"  OFF-BOUNDS  {box} (area is {width}x{height})")

        if box.parent and not box.clip:
            owner = by_name.get(box.parent)
            if owner is None:
                problems.append(f"  BAD-PARENT  {box.name} references missing {box.parent}")
            elif (
                box.x < owner.x or box.y < owner.y
                or box.r > owner.r or box.b > owner.b
            ):
                problems.append(f"  CHILD-BOUNDS  {box} outside {owner}")

        if box.required_h is not None and box.required_h > box.h + 0.01:
            problems.append(
                f"  TEXT-CLIP  {box.name} needs {box.required_h:g}px high, has {box.h:g}px"
            )
        if box.text_mode in {"dot", "scroll"} and box.text and "\n" in box.text:
            problems.append(f"  MULTILINE-{box.text_mode.upper()}  {box.name}")
        if box.text_mode in {"dot", "scroll"} and box.text and box.font:
            available = max(1, box.w - 2 * (box.pad_x + box.border))
            if text_width(box.text, box.font) > available:
                truncations += 1

        touch_w, touch_h = box.touch_size or (box.w, box.h)
        if box.touch and (touch_w < MIN_TOUCH or touch_h < MIN_TOUCH):
            if box.compact:
                compact_controls += 1
            else:
                problems.append(
                    f"  SMALL-TARGET  {box.name} has {touch_w:g}x{touch_h:g}px hit area "
                    f"(minimum {MIN_TOUCH}x{MIN_TOUCH})"
                )

    for i, left in enumerate(boxes):
        for right in boxes[i + 1:]:
            if left.parent == right.name or right.parent == left.name:
                continue
            if right.name in left.allow_overlap or left.name in right.allow_overlap:
                continue
            lrect = effective_rect(left, by_name)
            rrect = effective_rect(right, by_name)
            if lrect is not None and rrect is not None and lrect.overlaps(rrect):
                problems.append(f"  OVERLAP  {left} <-> {right}")

    return problems, truncations, compact_controls


MODELED_BUILDERS = {
    "build_main", "build_list", "build_keypad", "build_result", "build_qr",
    "build_wifi", "build_wpass", "build_net", "build_thread_activity",
    "build_share", "build_dsqr",
    "build_settings", "build_screen", "build_power", "build_tset",
    "build_name", "build_about",
}


def source_sync_problems() -> list[str]:
    """Fail when ui.c changes assumptions without updating this model."""
    source_path = Path(__file__).resolve().parents[1] / "main" / "ui.c"
    try:
        source = source_path.read_text(encoding="utf-8")
    except OSError as exc:
        return [f"  SOURCE  cannot read {source_path}: {exc}"]

    problems: list[str] = []
    constants = {
        "UI_HEADER_H": HEADER,
        "UI_BACK_STRIP": BACK_STRIP,
        "BATT_W": BATT_W,
        "BATT_H": BATT_H,
    }
    for symbol, expected in constants.items():
        match = re.search(rf"#define\s+{symbol}\s+(\d+)", source)
        if match is None or int(match.group(1)) != expected:
            found = match.group(1) if match else "missing"
            problems.append(f"  SOURCE  {symbol} is {found}; model expects {expected}")

    pad = re.search(r"lv_obj_set_style_pad_all\(p,\s*(\d+),\s*LV_PART_MAIN\)", source)
    if pad is None or int(pad.group(1)) != PANEL_PAD:
        found = pad.group(1) if pad else "missing"
        problems.append(f"  SOURCE  panel padding is {found}; model expects {PANEL_PAD}")

    builders = set(re.findall(r"static void (build_[a-z0-9_]+)\(void\)", source))
    missing = sorted(builders - MODELED_BUILDERS)
    stale = sorted(MODELED_BUILDERS - builders)
    if missing:
        problems.append(f"  SOURCE  unmodeled page builder(s): {', '.join(missing)}")
    if stale:
        problems.append(f"  SOURCE  stale modeled builder(s): {', '.join(stale)}")

    for symbol in ("lbl_title", "lbl_wifi", "lbl_status", "lbl_target", "lbl_wpass_ssid"):
        pattern = rf"lv_label_set_long_mode\({symbol},\s*LV_LABEL_LONG_DOT\)"
        if re.search(pattern, source) is None:
            problems.append(f"  SOURCE  {symbol} is modeled as single-line ellipsized")

    for symbol in ("wpass_ta", "name_ta"):
        pattern = rf"lv_textarea_set_one_line\({symbol},\s*true\)"
        if re.search(pattern, source) is None:
            problems.append(f"  SOURCE  {symbol} is modeled as a one-line field")

    geometry = {
        "home card": (
            r"lv_obj_set_size\(lbl_network,\s*BSP_LCD_H_RES\s*-\s*12,\s*52\)"
        ),
        "network card": (
            r"lv_obj_set_size\(lbl_net_info,\s*BSP_LCD_H_RES\s*-\s*12,\s*80\)"
        ),
        "activity card": (
            r"lv_obj_set_size\(lbl_activity_info,\s*BSP_LCD_H_RES\s*-\s*12,\s*124\)"
        ),
        "network three-button row": (
            r"const int top_w\s*=\s*96,\s*h\s*=\s*44,\s*top_col\s*=\s*104"
        ),
        "activity footer": (
            r"lv_obj_align\(btn_activity_refresh,\s*LV_ALIGN_BOTTOM_MID,\s*-78,\s*-6\)"
        ),
        "content viewport": (
            r"lv_obj_set_size\(box,\s*BSP_LCD_H_RES\s*-\s*12,\s*"
            r"UI_PANEL_H\s*-\s*UI_BACK_STRIP\s*-\s*6\)"
        ),
        "Wi-Fi list": (
            r"lv_obj_set_size\(wifi_list_w,\s*BSP_LCD_H_RES\s*-\s*20,\s*134\)"
        ),
        "settings list": (
            r"lv_obj_set_size\(list,\s*BSP_LCD_H_RES\s*-\s*20,\s*134\)"
        ),
        "camera preview offset": (
            r"lv_obj_align\(qr_canvas,\s*LV_ALIGN_TOP_MID,\s*0,\s*-6\)"
        ),
        "camera Retry footer": (
            r"lv_obj_align\(retry,\s*LV_ALIGN_BOTTOM_MID,\s*-78,\s*0\)"
        ),
        "dataset QR placement": (
            r"lv_obj_align\(dsqr_widget,\s*LV_ALIGN_TOP_LEFT,\s*2,\s*2\)"
        ),
        "two-row roller height": (
            r"lv_obj_set_style_text_line_space\(r,\s*10,\s*LV_PART_MAIN\)\s*;\s*"
            r"lv_roller_set_visible_row_count\(r,\s*2\)"
        ),
        "switch hit area": (
            r"lv_obj_set_ext_click_area\(sw,\s*7\)"
        ),
        "slider hit area": (
            r"lv_obj_set_ext_click_area\(sl,\s*16\)"
        ),
    }
    for name, pattern in geometry.items():
        if re.search(pattern, source) is None:
            problems.append(f"  SOURCE  {name} geometry no longer matches the model")

    if re.search(
        r"p\s*==\s*panel_activity\)\s*title\s*=\s*\"Thread activity\"", source,
    ) is None:
        problems.append("  SOURCE  Thread activity header title is not synchronized")

    if len(re.findall(
        r"lv_obj_set_size\(kb,\s*BSP_LCD_H_RES\s*-\s*12,\s*122\)", source,
    )) != 2:
        problems.append("  SOURCE  expected two modeled 308x122 keyboards")
    if len(re.findall(
        r"lv_obj_set_size\((?:wpass_ta|name_ta),\s*"
        r"BSP_LCD_H_RES\s*-\s*24,\s*40\)", source,
    )) != 2:
        problems.append("  SOURCE  expected two modeled 296x40 one-line fields")
    if len(re.findall(r"lv_obj_set_height\(b,\s*40\)", source)) != 3:
        problems.append("  SOURCE  expected 40px settings, border-agent, and Wi-Fi list rows")

    return problems


COLOURS = {
    "button": "#D9904F",
    "label": "#4ECBB8",
    "card": "#4ECBB8",
    "list": "#8A937F",
    "list-row": "#B1BAA5",
    "scroll": "#8A937F",
    "widget": "#7AA2F7",
    "text-area": "#7AA2F7",
    "keyboard": "#8067A8",
    "keypad": "#8067A8",
    "media": "#5A6A78",
    "qrcode": "#F4F4F4",
    "battery": "#4ECBB8",
}


def svg(panel_map: dict[str, list[Box]], path: Path) -> None:
    pad, cols = 16, 2
    rows = (len(panel_map) + cols - 1) // cols
    canvas_w = cols * (W + pad) + pad
    canvas_h = rows * (H + pad + 22) + pad
    out = [
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{canvas_w}" '
        f'height="{canvas_h}" style="background:#222">'
    ]

    for i, (panel_name, boxes) in enumerate(panel_map.items()):
        ox = pad + (i % cols) * (W + pad)
        oy = pad + (i // cols) * (H + pad + 22)
        out.append(
            f'<text x="{ox}" y="{oy - 4}" fill="#eee" font-size="12" '
            f'font-family="monospace">{html.escape(panel_name)}</text>'
        )
        out.append(
            f'<rect x="{ox}" y="{oy}" width="{W}" height="{H}" '
            'fill="#101410" stroke="#555"/>'
        )
        out.append(
            f'<rect x="{ox}" y="{oy}" width="{W}" height="{HEADER}" '
            'fill="#1B2119"/>'
        )

        all_boxes = header_boxes(PANEL_TITLES[panel_name], maximum_wifi=False) + boxes
        panel_lookup = {box.name: box for box in boxes}
        for box in all_boxes:
            is_header = box.name.startswith("header:")
            y_offset = 0 if is_header else HEADER
            rect = box.ink if box.kind == "label" and box.ink else box.bounds
            if box.clip and box.clip in panel_lookup:
                clipped = rect.intersection(panel_lookup[box.clip].bounds)
                if clipped is None:
                    continue
                rect = clipped
            colour = COLOURS.get(box.kind, "#7AA2F7")
            dash = ' stroke-dasharray="3,2"' if box.container else ""
            out.append(
                f'<rect x="{ox + rect.x:g}" y="{oy + y_offset + rect.y:g}" '
                f'width="{rect.w:g}" height="{rect.h:g}" '
                f'fill="{colour}" fill-opacity="0.38" stroke="{colour}"{dash}/>'
            )
            if rect.h < 11 or rect.w < 12 or box.name == "header:battery-nub":
                continue
            annotation = box.name.removeprefix("header:")
            if box.name == "header:battery-label":
                annotation = "100%"
            max_chars = max(1, int((rect.w - 4) / 4.8))
            if len(annotation) > max_chars:
                annotation = annotation[:max(1, max_chars - 3)] + "..."
            out.append(
                f'<text x="{ox + rect.x + 2:g}" y="{oy + y_offset + rect.y + 10:g}" '
                f'fill="#fff" font-size="8" font-family="monospace">'
                f'{html.escape(annotation)}</text>'
            )

    out.append("</svg>")
    path.write_text("\n".join(out), encoding="utf-8")


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "output", nargs="?", default="layout.svg",
        help="SVG preview path (default: layout.svg)",
    )
    parser.add_argument(
        "--no-svg", action="store_true",
        help="run checks without writing a preview (useful in CI)",
    )
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(sys.argv[1:] if argv is None else argv)
    panel_map = panels()
    total_problems = 0
    truncations = 0
    compact_controls = 0

    sync = source_sync_problems()
    if sync:
        print("[source sync]")
        print("\n".join(sync))
        total_problems += len(sync)

    header_problems, header_trunc, header_compact = check(
        header_boxes("Thread Commissioner"), height=HEADER,
    )
    if header_problems:
        print("[header]")
        print("\n".join(header_problems))
        total_problems += len(header_problems)
    truncations += header_trunc
    compact_controls += header_compact

    for panel_name, boxes in panel_map.items():
        problems, panel_trunc, panel_compact = check(boxes)
        if problems:
            print(f"[{panel_name}]")
            print("\n".join(problems))
            total_problems += len(problems)
        truncations += panel_trunc
        compact_controls += panel_compact

    print(
        f"{total_problems} problem(s); modeled {len(panel_map)} panels plus header; "
        f"panel area {W}x{PANEL_H} below {HEADER}px header"
    )
    print(
        f"maximum-text cases exercise {truncations} one-line truncation(s); "
        f"minimum ordinary touch target is {MIN_TOUCH}px"
    )
    if compact_controls:
        print(f"{compact_controls} intentional compact touch target(s) exempted")

    if not args.no_svg:
        output = Path(args.output)
        svg(panel_map, output)
        print(f"preview written to {output}")

    return 1 if total_problems else 0


if __name__ == "__main__":
    sys.exit(main())
