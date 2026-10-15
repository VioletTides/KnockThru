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
FPS = 25  # GIF frame delays are whole centiseconds, so 25 fps keeps exact timing

# One knock timeline shared by every animation, in seconds, so the GIFs stay in step.
FIRST_KNOCK = 0.5  # the first contact in a loop
KNOCK_GAP = 0.3  # between contacts
WAIT = 0.4  # from the last knock to the action, as in the README
# The knock kinds and timing loops play a double, then a triple: (knocks, pass length).
PASSES = ((2, 3.0), (3, 3.6))
LOOP = sum(length for _, length in PASSES)

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


def mix(color, amount, base=BACKGROUND):
    """Blends a color toward the background. amount 1 is the full color."""
    return tuple(round(base[i] + (color[i] - base[i]) * amount) for i in range(3))


# Easing curves. Each maps 0..1 to 0..1.

def clamp(t):
    return max(0.0, min(1.0, t))


def ease_in(t):
    return clamp(t) ** 3


def ease_out(t):
    return 1 - (1 - clamp(t)) ** 3


def ease_in_out(t):
    t = clamp(t)
    return 0.5 - 0.5 * math.cos(math.pi * t)


def ease_out_back(t, overshoot=1.7):
    t = clamp(t) - 1
    return 1 + t * t * ((overshoot + 1) * t + overshoot)


def keyframes(t, keys):
    """Interpolates (time, value, easing) keys. Each easing shapes the segment into its key."""
    if t <= keys[0][0]:
        return keys[0][1]
    for (t0, v0, _), (t1, v1, curve) in zip(keys, keys[1:]):
        if t <= t1:
            return v0 + (v1 - v0) * curve((t - t0) / (t1 - t0))
    return keys[-1][1]


def knock_times(taps, start=FIRST_KNOCK):
    return [start + i * KNOCK_GAP for i in range(taps)]


def knock_motion(t, taps, start=FIRST_KNOCK):
    """Returns how far the controller is from the contact point, from 0 (touching) to 1 (rest).

    Each knock pulls back a little, speeds up into the contact, and bounces off
    with a slowing rebound. After the last knock it settles back to rest.
    """
    hits = knock_times(taps, start)
    keys = [(hits[0] - 0.3, 1.0, ease_in_out), (hits[0] - 0.13, 1.18, ease_in_out), (hits[0], 0.0, ease_in)]
    for hit in hits[1:]:
        keys += [(hit - KNOCK_GAP / 2, 0.55, ease_out), (hit, 0.0, ease_in)]
    keys.append((hits[-1] + 0.35, 1.0, ease_out_back))
    return keyframes(t, keys)


def impact(t, taps, start=FIRST_KNOCK):
    """Returns how far the latest impact has played, from 0 (just hit) to 1 (faded), or None."""
    past = [hit for hit in knock_times(taps, start) if hit <= t < hit + 0.3]
    return (t - past[-1]) / 0.3 if past else None


def pass_at(t):
    """Returns (knocks, time within the pass, pass length) for a time in the loop."""
    for taps, length in PASSES:
        if t < length:
            return taps, t, length
        t -= length
    return PASSES[-1][0], PASSES[-1][1], PASSES[-1][1]


def action_time(taps):
    return knock_times(taps)[-1] + WAIT


def pass_fade(lt, length, edge=0.25):
    """1 inside a pass, easing to 0 at its start and end."""
    return ease_in_out(min(lt / edge, (length - lt) / edge, 1.0))


def buzz(t, action, count):
    """A small side-to-side shake after the action, one burst per buzz, in pixels."""
    for i in range(count):
        local = t - action - i * 0.18
        if 0 <= local < 0.12:
            return 3 * math.sin(local * 2 * math.pi * 25) * (1 - local / 0.12)
    return 0.0


def burst(draw, x, y, played, scale=1.0, base=BACKGROUND):
    """Draws impact lines that fly outward and fade into base. played runs from 0 to 1."""
    if played is None:
        return
    spread, fade = ease_out(played), 1 - ease_in_out(played)
    color = mix(ACCENT, fade, base)
    for k in range(8):
        a = k * math.pi / 4 + math.pi / 8
        inner = (16 + 18 * spread) * scale
        outer = inner + (18 - 8 * spread) * scale
        draw.line([(x + math.cos(a) * inner, y + math.sin(a) * inner),
                   (x + math.cos(a) * outer, y + math.sin(a) * outer)], fill=color, width=max(1, round(4 * scale)))


def knock_pair(image, draw, cx, cy, h, gap, played, swing, scale=1.0, shake=0.0, base=BACKGROUND):
    """Two controllers knocking heads at (cx, cy). Each one swings from its wrist."""
    offset = head_radius(h) * 0.95 + swing * gap
    tilt = 30 + 10 * gap  # a wider angle while pulled back
    burst(draw, cx, cy, played, scale, base)  # behind the controllers
    place(image, "controller-left", cx - offset + shake, cy, h, angle=-round(tilt))
    place(image, "controller-right", cx + offset - shake, cy, h, angle=round(tilt))


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
    big, small = font(34, bold=True), font(22)

    def frame(t, state, after, taps, buzzes, before=None):
        """One frame of a scene. state is (panel, caption, detail). after is the state once the knocks act.

        At the action the panel cross-fades. The old caption fades out, then the new one rises in.
        """
        action = knock_times(taps)[-1] + WAIT if taps else None
        fade = ease_in_out((t - action) / 0.3) if taps else 0.0
        image = Image.new("RGB", (width, caption_height + panel_height), BACKGROUND)
        panel = panels[state[0]] if fade == 0 else Image.blend(panels[state[0]], panels[after[0]], fade)
        image.paste(panel, (0, caption_height))
        draw = ImageDraw.Draw(image)
        enter = ease_in_out(t / 0.3) if before else 1.0  # from the previous scene's caption
        if enter < 1:
            caption, shown = (before, 1 - 2 * enter) if enter < 0.5 else (state, 2 * enter - 1)
            rising = enter >= 0.5
        else:
            caption, shown = (state, 1 - 2 * fade) if fade < 0.5 else (after, 2 * fade - 1)
            rising = fade >= 0.5
        rise = 8 * (1 - ease_out(shown)) if rising else 0
        draw.text((40, 34 + rise), caption[1], font=big, fill=mix(TEXT, shown))
        draw.text((40, 86 + rise), caption[2], font=small, fill=mix(DIM, shown))
        gap = knock_motion(t, taps) if taps else 1.0
        played = impact(t, taps) if taps else None
        shake = buzz(t, action, buzzes) if taps else 0.0
        knock_pair(image, draw, width - 150, 50, 120, gap, played, 26, scale=0.8, shake=shake)
        return image

    frames = []

    def scene(state, duration, after=None, taps=0, buzzes=0, before=None):
        frames.extend(frame(i / FPS, state, after or state, taps, buzzes, before) for i in range(seconds(duration)))

    off = ("off", "You're in VR", "Passthrough is off.")
    knock_on = ("off", "Knock twice", "Tap your controllers together.")
    on = ("on", "Your room appears", "Passthrough on, see-through style.")
    knock_style = ("on", "Knock three times", "Switch the style.")
    opaque = ("opaque", "Only the room", "Opaque style hides the game.")
    knock_back = ("opaque", "Knock three times", "Switch back.")
    see_through = ("on", "See-through again", "Passthrough on, see-through style.")
    knock_off = ("on", "Knock twice", "Back to the game.")

    def knock_scene(before, state, after, taps, buzzes, hold):
        scene(state, knock_times(taps)[-1] + WAIT + hold, after, taps, buzzes, before)

    scene(off, 1.0)
    knock_scene(off, knock_on, on, 2, 1, 1.8)
    knock_scene(on, knock_style, opaque, 3, 2, 1.8)
    knock_scene(opaque, knock_back, see_through, 3, 2, 1.8)
    # The last knock returns to the opening frame, so the loop has no cut. Each knock
    # changes one thing: two knocks only turn passthrough on or off, three only switch the style.
    knock_scene(see_through, knock_off, off, 2, 1, 1.0)
    write_gif(frames, IMAGES / "hero.gif")


# Knock kinds: three small loops side by side, on the shared timeline.

def knock_kinds():
    cell, height, header = 340, 300, 56
    label, title = font(22, bold=True), font(24, bold=True)
    titles = {2: "Knock twice: Room View turns on or off", 3: "Knock three times: the style switches"}
    frames = []
    for i in range(seconds(LOOP)):
        taps, t, length = pass_at(i / FPS)
        image = Image.new("RGB", (cell * 3, height + header), BACKGROUND)
        draw = ImageDraw.Draw(image)
        shown = pass_fade(t, length)
        w = draw.textlength(titles[taps], font=title)
        draw.text(((cell * 3 - w) / 2, 14 + 6 * (1 - shown)), titles[taps], font=title, fill=mix(TEXT, shown))

        cards = Image.new("RGB", (cell * 3, height), BACKGROUND)
        draw = ImageDraw.Draw(cards)
        for c in range(3):
            draw.rounded_rectangle([c * cell + 10, 10, c * cell + cell - 10, height - 10], radius=18, fill=SURFACE)
        gap, played, shake = knock_motion(t, taps), impact(t, taps), buzz(t, action_time(taps), taps - 1)

        # Controllers together.
        knock_pair(cards, draw, cell // 2, 95, 150, gap, played, 30, shake=shake, base=SURFACE)

        # One controller on a surface, swinging down from the wrist.
        cx, table_y, h = cell + cell // 2, 210, 150
        burst(draw, cx, table_y, played, base=SURFACE)
        place(cards, "controller-right", cx + shake, table_y - head_radius(h) * 0.95 - 45 * gap, h,
              angle=round(165 - 12 * gap))
        draw.rectangle([cx - 130, table_y, cx + 130, table_y + 16], fill=TABLE)

        # The left controller taps the left side of the headset.
        cx, cy, hw, h = 2 * cell + cell // 2, 110, 160, 130
        hx = cx + 70  # headset center
        edge = hx - hw / 2
        burst(draw, edge, cy, played, base=SURFACE)
        place(cards, "headset-front", hx, cy, round(hw * 600 / 1110))
        place(cards, "controller-left", edge - head_radius(h) * 0.95 - 30 * gap + shake, cy, h,
              angle=round(-30 - 8 * gap))

        for c, text in enumerate(("Controllers together", "Controller on a surface", "Tap on the headset")):
            w = draw.textlength(text, font=label)
            draw.text((c * cell + (cell - w) / 2, 248), text, font=label, fill=TEXT)
        image.paste(cards, (0, header))
        frames.append(image)
    write_gif(frames, IMAGES / "knock-kinds.gif")


# Timing strip: knock, knock, short wait, action, on the shared timeline.

def timing():
    width, height = 1000, 240
    left, right, axis_y = 60, width - 60, 130
    span = 2.0  # seconds shown on the strip; the playhead moves in real time
    bold, small = font(22, bold=True), font(18)
    words = {2: ("third", "Room View on", "both controllers buzz"),
             3: ("fourth", "Style switches", "both controllers buzz twice")}

    def x_at(t):
        return left + (right - left) * t / span

    def strip(now, taps):
        knocks = knock_times(taps)
        action = action_time(taps)
        next_knock, done, buzzed = words[taps]
        image = Image.new("RGB", (width, height), BACKGROUND)
        draw = ImageDraw.Draw(image)
        for k in range(round(span * 10) + 1):  # a tick every 0.1 s
            x = x_at(k / 10)
            draw.line([(x, axis_y - 4), (x, axis_y + 4)], fill=HAIRLINE, width=2)
        draw.line([(left, axis_y), (right, axis_y)], fill=HAIRLINE, width=4)
        draw.line([(left, axis_y), (x_at(min(now, span)), axis_y)], fill=DIM, width=4)

        # The wait: a bracket that grows with the playhead, then its label fades in.
        if now >= knocks[-1]:
            a, b = x_at(knocks[-1]), x_at(min(now, action))
            y = axis_y + 34
            draw.line([(a, y), (b, y)], fill=ACCENT, width=3)
            draw.line([(a, y - 6), (a, y + 6)], fill=ACCENT, width=3)
            if now >= action:
                draw.line([(b, y - 6), (b, y + 6)], fill=ACCENT, width=3)
            shown = ease_out((now - knocks[-1]) / 0.3)
            mid = (a + x_at(action)) / 2
            draw.text((mid, y + 26 + 6 * (1 - shown)), "about 0.4 s", font=small, fill=mix(DIM, shown), anchor="mm")
            draw.text((mid, y + 50 + 6 * (1 - shown)), f"waiting for a {next_knock} knock", font=small,
                      fill=mix(DIM, shown), anchor="mm")

        # Knocks: the dot pops in, the label rises into place.
        for k in knocks:
            if now >= k:
                x = x_at(k)
                burst(draw, x, axis_y, (now - k) / 0.3 if now < k + 0.3 else None)
                r = 9 * ease_out_back((now - k) / 0.2)
                draw.ellipse([x - r, axis_y - r, x + r, axis_y + r], fill=TEXT)
                shown = ease_out((now - k) / 0.25)
                draw.text((x, axis_y - 44 + 8 * (1 - shown)), "Knock", font=bold, fill=mix(TEXT, shown), anchor="mm")

        # The action: the marker grows, the labels slide in, and the buzz shakes.
        if now >= action:
            x = x_at(action)
            grow = ease_out_back((now - action) / 0.25)
            draw.rounded_rectangle([x - 8, axis_y - 26 * grow, x + 8, axis_y + 26 * grow], radius=6, fill=ACCENT)
            shown = ease_out((now - action) / 0.3)
            slide = 12 * (1 - shown)
            draw.text((x + 26 - slide, axis_y - 44), done, font=bold, fill=mix(ACCENT, shown), anchor="lm")
            draw.text((x + 26 - slide + buzz(now, action, taps - 1) * 1.5, axis_y - 14), buzzed,
                      font=small, fill=mix(DIM, shown), anchor="lm")

        # Playhead: a light line with a small marker on top, until the action.
        if now < action:
            x = x_at(now)
            draw.line([(x, axis_y - 70), (x, axis_y + 16)], fill=mix(DIM, 0.6), width=2)
            draw.polygon([(x - 6, axis_y - 78), (x + 6, axis_y - 78), (x, axis_y - 70)], fill=DIM)
        return image

    frames = []
    empty = strip(0.0, 2)
    for i in range(seconds(LOOP)):
        taps, now, length = pass_at(i / FPS)
        image = strip(now, taps)
        if now > length - 0.3:  # each pass fades to an empty strip before the next
            image = Image.blend(image, empty, ease_in_out((now - (length - 0.3)) / 0.3))
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
