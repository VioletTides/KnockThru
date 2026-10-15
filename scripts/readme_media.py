#!/usr/bin/env python3
"""Generates the README images in images/.

Usage: python3 scripts/readme_media.py [--build-dir build]

Needs Pillow, ffmpeg, and a host build with the panel_preview target.
"""
import argparse
import math
import shutil
import subprocess
import tempfile
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parent.parent
IMAGES = ROOT / "images"
FONTS = ROOT / "assets" / "fonts"
FPS = 20

# Colors from src/panel.cpp.
BACKGROUND = (18, 20, 24)
SURFACE = (28, 31, 37)
RAISED = (44, 48, 57)
HAIRLINE = (46, 50, 58)
TEXT = (236, 238, 241)
DIM = (150, 156, 168)
ACCENT = (58, 145, 232)
TABLE = (120, 92, 70)


def font(size, bold=False):
    name = "NotoSans-SemiBold.ttf" if bold else "NotoSans-Regular.ttf"
    return ImageFont.truetype(str(FONTS / name), size)


def write_gif(frames, path):
    """Writes RGB frames to a looping GIF with a palette made for them."""
    with tempfile.TemporaryDirectory() as tmp:
        for i, frame in enumerate(frames):
            frame.save(f"{tmp}/{i:04d}.png")
        subprocess.run(
            ["ffmpeg", "-loglevel", "error", "-y", "-framerate", str(FPS), "-i", f"{tmp}/%04d.png",
             "-filter_complex", "split[a][b];[a]palettegen=max_colors=128[p];[b][p]paletteuse=dither=none",
             "-loop", "0", str(path)],
            check=True)
    print(f"Wrote {path.relative_to(ROOT)} ({path.stat().st_size // 1024} KB)")


def seconds(s):
    return round(s * FPS)


def ease(t):
    return 0.5 - 0.5 * math.cos(math.pi * max(0.0, min(1.0, t)))


# Controller and headset shapes, shared by the hero and the knock kinds loop.

def controller(draw, x, y, angle=0.0, scale=1.0):
    """Draws a controller as a ring on a handle. (x, y) is the ring center."""
    r = 22 * scale
    dx, dy = math.sin(angle), math.cos(angle)
    handle_end = (x + dx * 62 * scale, y + dy * 62 * scale)
    draw.line([(x, y), handle_end], fill=DIM, width=round(16 * scale))
    draw.ellipse([handle_end[0] - 8 * scale, handle_end[1] - 8 * scale,
                  handle_end[0] + 8 * scale, handle_end[1] + 8 * scale], fill=DIM)
    draw.ellipse([x - r, y - r, x + r, y + r], outline=TEXT, width=round(7 * scale))


def headset(draw, x, y, scale=1.0):
    w, h = 120 * scale, 64 * scale
    draw.rounded_rectangle([x - w / 2, y - h / 2, x + w / 2, y + h / 2], radius=22 * scale, fill=RAISED,
                           outline=DIM, width=round(4 * scale))
    for side in (-1, 1):
        cx = x + side * 28 * scale
        draw.ellipse([cx - 16 * scale, y - 12 * scale, cx + 16 * scale, y + 12 * scale], fill=SURFACE)


def burst(draw, x, y, strength, scale=1.0):
    """Draws impact lines around a point. strength runs from 1 down to 0."""
    if strength <= 0:
        return
    color = tuple(round(BACKGROUND[i] + (ACCENT[i] - BACKGROUND[i]) * strength) for i in range(3))
    for k in range(8):
        a = k * math.pi / 4 + math.pi / 8
        inner = (18 + 14 * (1 - strength)) * scale
        outer = inner + 16 * scale
        draw.line([(x + math.cos(a) * inner, y + math.sin(a) * inner),
                   (x + math.cos(a) * outer, y + math.sin(a) * outer)], fill=color, width=round(4 * scale))


def tap_offset(t, taps, period=0.3, swing=1.0):
    """Returns (distance from the contact point, burst strength) at time t.

    Each tap swings in and out over one period, and touches at its middle.
    """
    for i in range(taps):
        local = (t - i * period) / period
        if 0 <= local < 1:
            return abs(math.cos(math.pi * local)) * swing, max(0.0, 1 - abs(local - 0.5) * 4)
    return swing, 0.0


# Hero: the dashboard panel reacting to knocks.

def hero(panel_dir):
    width = 800
    panel_height = round(770 * width / 1200)
    caption_height = 150
    panels = {}
    for name in ("off", "on", "opaque"):
        image = Image.open(panel_dir / f"readme-{name}.png").convert("RGBA")
        image = image.resize((width, panel_height), Image.LANCZOS)
        flat = Image.new("RGB", image.size, BACKGROUND)
        flat.paste(image, mask=image)
        panels[name] = flat

    big = font(34, bold=True)
    small = font(22)

    def frame(panel, caption, detail, t_knock=None, taps=0):
        image = Image.new("RGB", (width, caption_height + panel_height), BACKGROUND)
        image.paste(panels[panel], (0, caption_height))
        draw = ImageDraw.Draw(image)
        draw.text((40, 34), caption, font=big, fill=TEXT)
        draw.text((40, 86), detail, font=small, fill=DIM)
        # Two controllers knocking together on the right.
        cx, cy = width - 130, 70
        gap, strength = tap_offset(t_knock, taps) if t_knock is not None else (1.0, 0.0)
        offset = 18 + 30 * gap
        controller(draw, cx - offset, cy - 10, angle=0.5, scale=0.8)
        controller(draw, cx + offset, cy - 10, angle=-0.5, scale=0.8)
        burst(draw, cx, cy - 10, strength, scale=0.8)
        return image

    frames = []

    def hold(panel, caption, detail, duration):
        frames.extend(frame(panel, caption, detail) for _ in range(seconds(duration)))

    def knock(panel, caption, detail, taps):
        # The action fires about 0.4 s after the last knock.
        duration = taps * 0.3 + 0.4
        frames.extend(frame(panel, caption, detail, i / FPS, taps) for i in range(seconds(duration)))

    hold("off", "You're in VR", "Passthrough is off.", 1.4)
    knock("off", "Knock twice", "Tap your controllers together.", 2)
    hold("on", "Your room appears", "Passthrough on, see-through style.", 2.0)
    knock("on", "Knock three times", "Switch the style.", 3)
    hold("opaque", "Only the room", "Opaque style hides the game.", 2.0)
    knock("opaque", "Knock twice", "Back to the game.", 2)
    hold("off", "Back in the game", "Passthrough is off.", 1.4)
    write_gif(frames, IMAGES / "hero.gif")


# Knock kinds: three small loops side by side.

def knock_kinds():
    cell, height = 300, 280
    label = font(22, bold=True)
    frames = []
    duration = 1.6
    for i in range(seconds(duration)):
        t = i / FPS - 0.2
        image = Image.new("RGB", (cell * 3, height), BACKGROUND)
        draw = ImageDraw.Draw(image)
        for c in range(3):
            draw.rounded_rectangle([c * cell + 10, 10, c * cell + cell - 10, height - 10], radius=18, fill=SURFACE)
        gap, strength = tap_offset(t, 2)

        # Controllers together.
        cx, cy = cell // 2, 110
        offset = 24 + 34 * gap
        controller(draw, cx - offset, cy, angle=0.5)
        controller(draw, cx + offset, cy, angle=-0.5)
        burst(draw, cx, cy, strength)

        # One controller on a surface.
        cx = cell + cell // 2
        table_y = 170
        draw.rectangle([cx - 110, table_y, cx + 110, table_y + 16], fill=TABLE)
        controller(draw, cx, table_y - 22 - 50 * gap, angle=math.pi)
        burst(draw, cx, table_y, strength)

        # A tap on the headset.
        cx = 2 * cell + cell // 2
        headset(draw, cx - 30, 110)
        hx = cx + 30
        controller(draw, hx + 22 + 50 * gap, 110, angle=-1.2)
        burst(draw, hx, 110, strength)

        for c, text in enumerate(("Controllers together", "Controller on a surface", "Tap on the headset")):
            w = draw.textlength(text, font=label)
            draw.text((c * cell + (cell - w) / 2, 220), text, font=label, fill=TEXT)
        frames.append(image)
    write_gif(frames, IMAGES / "knock-kinds.gif")


# Timing strip: knock, knock, short wait, action.

def timing():
    width, height = 900, 230
    left, right, axis_y = 60, width - 60, 120
    span = 1.4  # seconds shown on the strip
    knocks = (0.15, 0.45)
    action = knocks[-1] + 0.4
    bold, small = font(22, bold=True), font(18)

    def x_at(t):
        return left + (right - left) * t / span

    frames = []
    total = 2.6
    for i in range(seconds(total)):
        now = i / FPS * span / 1.8  # the playhead crosses the strip in 1.8 s, then holds
        image = Image.new("RGB", (width, height), BACKGROUND)
        draw = ImageDraw.Draw(image)
        draw.line([(left, axis_y), (right, axis_y)], fill=HAIRLINE, width=4)
        draw.line([(left, axis_y), (x_at(min(now, span)), axis_y)], fill=DIM, width=4)

        for n, k in enumerate(knocks):
            if now >= k:
                x = x_at(k)
                burst(draw, x, axis_y, max(0.0, 1 - (now - k) * 3))
                draw.ellipse([x - 9, axis_y - 9, x + 9, axis_y + 9], fill=TEXT)
                draw.text((x, axis_y - 48), "Knock", font=bold, fill=TEXT, anchor="mm")
        if now >= knocks[-1]:
            a, b = x_at(knocks[-1]), x_at(min(now, action))
            draw.line([(a, axis_y + 34), (b, axis_y + 34)], fill=ACCENT, width=3)
            draw.text(((a + x_at(action)) / 2, axis_y + 60), "about 0.4 s", font=small, fill=DIM, anchor="mm")
            draw.text(((a + x_at(action)) / 2, axis_y + 84), "waiting for a third knock", font=small, fill=DIM,
                      anchor="mm")
        if now >= action:
            x = x_at(action)
            draw.rounded_rectangle([x - 8, axis_y - 26, x + 8, axis_y + 26], radius=6, fill=ACCENT)
            draw.text((x + 26, axis_y - 48), "Room View on", font=bold, fill=ACCENT, anchor="lm")
            buzz = math.sin((now - action) * 40) * max(0.0, 1 - (now - action) * 4)
            draw.text((x + 26 + buzz * 4, axis_y - 16), "both controllers buzz", font=small, fill=DIM, anchor="lm")
        if now < action:
            x = x_at(now)
            draw.line([(x, axis_y - 70), (x, axis_y + 20)], fill=HAIRLINE, width=2)
        frames.append(image)
    write_gif(frames, IMAGES / "timing.gif")


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--build-dir", default="build")
    args = parser.parse_args()
    preview = ROOT / args.build_dir / "bin" / "panel_preview"
    if not preview.exists():
        raise SystemExit(f"{preview} is missing. Run: cmake --build {args.build_dir} --target panel_preview")
    if not shutil.which("ffmpeg"):
        raise SystemExit("ffmpeg is missing.")
    with tempfile.TemporaryDirectory() as tmp:
        subprocess.run([str(preview), "--readme", tmp], check=True, stdout=subprocess.DEVNULL)
        shutil.copy(Path(tmp) / "readme-off.png", IMAGES / "dashboard.png")
        print("Wrote images/dashboard.png")
        hero(Path(tmp))
    knock_kinds()
    timing()


if __name__ == "__main__":
    main()
