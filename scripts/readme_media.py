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


# Controller and headset sprites, rendered from Valve's CAD by scripts/cad_sprites.py.

CAD = IMAGES / "cad"
HEAD = (0.5, 0.29)  # controller head center, as a fraction of the sprite size
_sprites = {}


def sprite(name, height, angle=0.0):
    """Returns a sprite scaled to a height and rotated about its anchor, plus that anchor.

    The anchor is the head center on a controller and the middle on the headset.
    angle is in degrees, counterclockwise.
    """
    key = (name, height, round(angle, 1))
    if key not in _sprites:
        image = Image.open(CAD / f"{name}.png").convert("RGBA")
        scale = height / image.height
        image = image.resize((round(image.width * scale), height), Image.LANCZOS)
        fx, fy = HEAD if name.startswith("controller") else (0.5, 0.5)
        ax, ay = fx * image.width, fy * image.height
        # Pad to a square centered on the anchor so rotation keeps every corner.
        side = 2 * math.ceil(max(math.hypot(x - ax, y - ay) for x in (0, image.width) for y in (0, image.height)))
        square = Image.new("RGBA", (side, side))
        square.paste(image, (round(side / 2 - ax), round(side / 2 - ay)))
        _sprites[key] = square.rotate(angle, resample=Image.BICUBIC)
    return _sprites[key]


def place(image, name, x, y, height, angle=0.0):
    """Pastes a sprite with its anchor at (x, y)."""
    s = sprite(name, height, angle)
    image.paste(s, (round(x - s.width / 2), round(y - s.height / 2)), s)


def head_radius(height):
    return 0.288 * height  # measured on the controller sprites


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
        cx, cy, h = width - 150, 50, 120
        gap, strength = tap_offset(t_knock, taps) if t_knock is not None else (1.0, 0.0)
        offset = head_radius(h) * 0.9 + 26 * gap
        place(image, "controller-left", cx - offset, cy, h, angle=-30)
        place(image, "controller-right", cx + offset, cy, h, angle=30)
        burst(draw, cx, cy, strength, scale=0.8)
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
    cell, height = 340, 300
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
        cx, cy, h = cell // 2, 95, 150
        offset = head_radius(h) * 0.95 + 30 * gap
        place(image, "controller-left", cx - offset, cy, h, angle=-30)
        place(image, "controller-right", cx + offset, cy, h, angle=30)
        burst(draw, cx, cy, strength)

        # One controller on a surface.
        cx, table_y, h = cell + cell // 2, 210, 150
        place(image, "controller-right", cx, table_y - head_radius(h) * 0.95 - 45 * gap, h, angle=165)
        draw.rectangle([cx - 130, table_y, cx + 130, table_y + 16], fill=TABLE)
        burst(draw, cx, table_y, strength)

        # The left controller taps the left side of the headset.
        cx, cy, hw, h = 2 * cell + cell // 2, 110, 160, 130
        hx = cx + 70  # headset center
        place(image, "headset-front", hx, cy, round(hw * 600 / 1110))
        edge = hx - hw / 2
        place(image, "controller-left", edge - head_radius(h) * 0.95 - 30 * gap, cy, h, angle=-60)
        burst(draw, edge, cy, strength)

        for c, text in enumerate(("Controllers together", "Controller on a surface", "Tap on the headset")):
            w = draw.textlength(text, font=label)
            draw.text((c * cell + (cell - w) / 2, 248), text, font=label, fill=TEXT)
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
