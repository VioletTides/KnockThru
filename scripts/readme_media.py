#!/usr/bin/env python3
"""Generates the README images in images/.

Usage: python3 scripts/readme_media.py [--build-dir build]

Needs Pillow, numpy, ffmpeg, Chromium, and a host build with the panel_preview target.
"""
import argparse
import math
import shutil
import subprocess
import tempfile
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFilter, ImageFont

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
ERROR = (240, 110, 100)
TABLE = (120, 92, 70)


def font(size, bold=False):
    name = "NotoSans-SemiBold.ttf" if bold else "NotoSans-Regular.ttf"
    return ImageFont.truetype(str(FONTS / name), size)


# Panel colors that each keep an exact palette entry in every GIF.
KEEP_COLORS = (ERROR,)

CORNER = 18  # rounded GIF corners, in pixels, so each GIF reads as a card on light and dark pages


def write_gif(frames, path, step=1):
    """Writes RGB frames to a looping GIF with transparent rounded corners. step 2 keeps every
    other frame, at half the frame rate, for a long GIF.

    ffmpeg picks the palette, which looks better than Pillow's. ffmpeg stores whole frames once
    the frames have transparency, though, so Pillow writes the final GIF: it keeps ffmpeg's colors
    exactly, makes the corners transparent, and marks what did not change as transparent too."""
    frames = frames[::step]
    with tempfile.TemporaryDirectory() as tmp:
        for i, frame in enumerate(frames):
            frame.save(f"{tmp}/{i:04d}.png")
        # A small label, like the red Reset, has too few pixels to win a palette entry and comes out
        # mustard. One extra frame of each such color, seen only by palettegen, gives it its own entry.
        for k, color in enumerate(KEEP_COLORS):
            Image.new("RGB", frames[0].size, color).save(f"{tmp}/{len(frames) + k:04d}.png")
        palette_png, opaque = Path(tmp) / "palette.png", Path(tmp) / "opaque.gif"
        subprocess.run(["ffmpeg", "-loglevel", "error", "-y", "-i", f"{tmp}/%04d.png",
                        "-vf", "palettegen=max_colors=127", str(palette_png)], check=True)
        subprocess.run(
            ["ffmpeg", "-loglevel", "error", "-y", "-framerate", str(FPS / step), "-i", f"{tmp}/%04d.png",
             "-i", str(palette_png), "-filter_complex", "[0][1]paletteuse=dither=none",
             "-frames:v", str(len(frames)), str(opaque)],
            check=True)
        decoded = Image.open(opaque)
        colored = []
        for i in range(decoded.n_frames):
            decoded.seek(i)
            colored.append(np.asarray(decoded.convert("RGB")))
    key = lambda pixels: (pixels[..., 0].astype(np.int64) << 16) | (pixels[..., 1].astype(np.int64) << 8) | pixels[..., 2]
    colors = np.unique(np.concatenate([np.unique(key(f)) for f in colored]))  # at most 127, so 255 stays free
    palette = [int(v) for c in colors for v in ((c >> 16) & 255, (c >> 8) & 255, c & 255)]
    palette += [0] * (768 - len(palette))
    corners = Image.new("L", frames[0].size, 0)
    ImageDraw.Draw(corners).rounded_rectangle([0, 0, frames[0].width - 1, frames[0].height - 1], CORNER, fill=255)
    outside = np.asarray(corners) < 128
    out = []
    previous = None
    for pixels in colored:
        index = np.searchsorted(colors, key(pixels)).astype(np.uint8)
        if previous is not None:
            # Unchanged pixels go transparent, which shows the frame below, so they compress to nothing.
            index[(index == previous) & ~outside] = 255
        index[outside] = 255
        previous = np.searchsorted(colors, key(pixels)).astype(np.uint8)
        image = Image.fromarray(index, "P")
        image.putpalette(palette)
        out.append(image)
    out[0].save(path, save_all=True, append_images=out[1:], duration=round(1000 * step / FPS), loop=0,
                transparency=255, disposal=1, optimize=False)
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


def knock_times(count, start=FIRST_KNOCK):
    return [start + i * KNOCK_GAP for i in range(count)]


def knock_motion(t, count, start=FIRST_KNOCK):
    """Returns how far the controller is from the contact point, from 0 (touching) to 1 (rest).

    Each knock pulls back a little, speeds up into the contact, and bounces off
    with a slowing rebound. After the last knock it settles back to rest.
    """
    contacts = knock_times(count, start)
    keys = [(contacts[0] - 0.3, 1.0, ease_in_out), (contacts[0] - 0.13, 1.18, ease_in_out), (contacts[0], 0.0, ease_in)]
    for contact in contacts[1:]:
        keys += [(contact - KNOCK_GAP / 2, 0.55, ease_out), (contact, 0.0, ease_in)]
    keys.append((contacts[-1] + 0.35, 1.0, ease_out_back))
    return keyframes(t, keys)


def impact(t, count, start=FIRST_KNOCK):
    """Returns how far the latest impact has played, from 0 (the moment of contact) to 1 (faded), or None."""
    past = [contact for contact in knock_times(count, start) if contact <= t < contact + 0.3]
    return (t - past[-1]) / 0.3 if past else None


def pass_at(t):
    """Returns (knocks, time within the pass, pass length) for a time in the loop."""
    for count, length in PASSES:
        if t < length:
            return count, t, length
        t -= length
    return PASSES[-1][0], PASSES[-1][1], PASSES[-1][1]


def action_time(count):
    return knock_times(count)[-1] + WAIT


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

    def frame(t, state, after, count, buzzes, before=None):
        """One frame of a scene. state is (panel, caption, detail). after is the state once the knocks act.

        At the action the panel cross-fades. The old caption fades out, then the new one rises in.
        """
        action = knock_times(count)[-1] + WAIT if count else None
        fade = ease_in_out((t - action) / 0.3) if count else 0.0
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
        gap = knock_motion(t, count) if count else 1.0
        played = impact(t, count) if count else None
        shake = buzz(t, action, buzzes) if count else 0.0
        knock_pair(image, draw, width - 150, 50, 120, gap, played, 26, scale=0.8, shake=shake)
        return image

    frames = []

    def scene(state, duration, after=None, count=0, buzzes=0, before=None):
        frames.extend(frame(i / FPS, state, after or state, count, buzzes, before) for i in range(seconds(duration)))

    off = ("off", "You're in VR", "Passthrough is off.")
    knock_on = ("off", "Knock twice", "Knock your controllers together.")
    on = ("on", "Your room appears", "Passthrough on, translucent style.")
    knock_style = ("on", "Knock three times", "Switch the style.")
    opaque = ("opaque", "Only the room", "Opaque style hides the game.")
    knock_back = ("opaque", "Knock three times", "Switch back.")
    see_through = ("on", "Translucent again", "Passthrough on, translucent style.")
    knock_off = ("on", "Knock twice", "Back to the game.")

    def knock_scene(before, state, after, count, buzzes, hold):
        scene(state, knock_times(count)[-1] + WAIT + hold, after, count, buzzes, before)

    scene(off, 1.0)
    knock_scene(off, knock_on, on, 2, 1, 1.8)
    knock_scene(on, knock_style, opaque, 3, 2, 1.8)
    knock_scene(opaque, knock_back, see_through, 3, 2, 1.8)
    # The last knock returns to the opening frame, so the loop has no cut. Each knock
    # changes one thing: two knocks only turn passthrough on or off, three only switch the style.
    knock_scene(see_through, knock_off, off, 2, 1, 1.0)
    write_gif(frames, IMAGES / "hero.gif")


# Walkthroughs: the panel's own screens with a pointer. Calibrate goes from the Settings button
# to the saved banner. Update goes from the new-release dot to the restarted app.

def pointer_dot(image, x, y, press, ring, scale=1.0):
    """Draws SteamVR's dashboard pointer: a light blue dot, lighter at the top, in a dark ring.
    The colors and proportions match /opt/steamvr/resources/overlay_pointer_circle.png on the
    Frame. press shrinks the dot on a click. ring runs 0 to 1 as a click ring spreads, or is None.
    Sizes are in panel pixels, times scale."""
    k = 4 * scale  # drawn large, then shrunk, for smooth edges
    half = 40  # room for the widest click ring
    size = round(2 * half * k)
    dot = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    draw = ImageDraw.Draw(dot)
    c = size / 2
    if ring is not None:
        rr = (17 + 16 * ease_out(ring)) * k
        draw.ellipse([c - rr, c - rr, c + rr, c + rr], outline=(109, 186, 216, round(180 * (1 - ring))),
                     width=round(3 * k))
    outer = 14 * (1 - 0.15 * press) * k
    inner = outer * 18 / 27.5
    draw.ellipse([c - outer, c - outer, c + outer, c + outer], fill=(41, 45, 50, 237))
    top, bottom = (119, 197, 227), (100, 177, 207)
    gradient = Image.new("RGBA", (size, size))
    gradient.putdata([(*mix(bottom, ease_in_out(((row - c) / inner + 0.28) / 0.67), top), 255)
                      for row in range(size) for _ in range(size)])
    mask = Image.new("L", (size, size), 0)
    ImageDraw.Draw(mask).ellipse([c - inner, c - inner, c + inner, c + inner], fill=255)
    dot.paste(gradient, mask=mask)
    out = round(2 * half * scale)
    dot = dot.resize((out, out), Image.LANCZOS)
    image.alpha_composite(dot, (round(x - out / 2), round(y - out / 2)))


def load_walkthrough(frames_dir, width):
    """Reads a panel_preview frame sequence: its cuts, clicks, pointer log, and the frames at `width`."""
    marks = dict(line.split(maxsplit=1) if " " in line else (line, "")
                 for line in (frames_dir / "marks.txt").read_text().splitlines())
    cuts = [int(n) for n in marks["cuts"].split()]
    clicks = [int(n) for n in marks["clicks"].split()]
    pointer = [tuple(map(float, line.split())) for line in (frames_dir / "pointer.txt").read_text().splitlines()]
    frames = []
    for path in sorted(frames_dir.glob("*.png")):
        image = Image.open(path).convert("RGBA").resize((width, round(770 * width / 1200)), Image.LANCZOS)
        flat = Image.new("RGB", image.size, BACKGROUND)
        flat.paste(image, mask=image)
        frames.append(flat)
    return cuts, clicks, pointer, frames


def add_pointer(frames, pointer, clicks, scale):
    """Draws the pointer on each frame. A hand held out drifts a little. Each drift wave fits a
    whole number of times into the loop, so the loop has no jump."""
    count = len(frames)

    def wave(i, per_second, phase=0.0):
        cycles = max(1, round(count / FPS * per_second))
        return math.sin(2 * math.pi * cycles * i / count + phase)

    for i, frame in enumerate(frames):
        x, y = pointer[i]
        x += 1.6 * wave(i, 0.27) + 0.8 * wave(i, 0.65, 1)
        y += 1.4 * wave(i, 0.37, 2) + 0.7 * wave(i, 0.59)
        since = [i - c for c in clicks if 0 <= i - c < 8]
        before = [c - i for c in clicks if 0 < c - i <= 3]
        press = 1 - since[0] / 8 if since else (1 - before[0] / 4 if before else 0)
        ring = since[0] / 8 if since else None
        image = frame.convert("RGBA")
        pointer_dot(image, x * scale, y * scale, press, ring, scale)
        frames[i] = image.convert("RGB")


def loop_fade(frames):
    """Cross-fades the last 0.4 s into the first frame, stopping one step short, so the loop has no cut."""
    fade = seconds(0.4)
    for k in range(fade):
        i = len(frames) - fade + k
        frames[i] = Image.blend(frames[i], frames[0], ease_in_out((k + 1) / (fade + 1)))


def walkthrough(frames_dir, name):
    """Turns a panel_preview frame sequence with a pointer log into images/NAME.gif."""
    width = 800  # the same panel size as the hero
    cuts, clicks, pointer, frames = load_walkthrough(frames_dir, width)
    # Where the clock skips ahead, cross-fade instead of cutting.
    fade = seconds(0.4)
    for cut in cuts:
        before = frames[cut - 1]
        for k in range(fade):
            frames[cut + k] = Image.blend(before, frames[cut + k], ease_in_out((k + 1) / (fade + 1)))
    loop_fade(frames)
    add_pointer(frames, pointer, clicks, width / 1200)  # after the fades, so it never doubles
    write_gif(frames, IMAGES / f"{name}.gif")


# Report card: its sample QR code gets blurred in the GIF, so nobody scans it.

def hide_qr(image):
    """Blurs a QR code past reading, at any size, and softens its contrast."""
    soft = image.filter(ImageFilter.GaussianBlur(image.width / 28))
    return Image.blend(soft, Image.new("RGB", soft.size, (150, 156, 168)), 0.35)


def qr_box(flat):
    """The QR code's box on a report card render: the only large white square in the panel."""
    white = flat.point(lambda v: 255 if v > 250 else 0).convert("L").point(lambda v: 255 if v == 255 else 0)
    return white.getbbox()


# Phone: the other half of the report GIF. Screens are drawn at twice a 320 by 694 CSS pixel
# phone. The KnockThru page is the real one the headset serves, captured with Chromium.

PHONE_CSS = (320, 694)
BAR, URL_BAR = 40, 40  # status bar and browser address bar, in CSS pixels
PAGE_LIGHT = (244, 245, 247)
FORM_BLUE = (38, 112, 188)  # the phone page's button blue


def capture(html, path, height):
    """Screenshots an HTML file at the phone's width with headless Chromium, at 2x."""
    browser = shutil.which("chromium") or shutil.which("google-chrome") or shutil.which("chromium-browser")
    if not browser:
        raise SystemExit("Chromium is missing. It renders the phone page.")
    subprocess.run([browser, "--headless", "--disable-gpu", "--hide-scrollbars", "--force-device-scale-factor=2",
                    f"--window-size={PHONE_CSS[0]},{height}", f"--screenshot={path}", html.as_uri()],
                   check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    return Image.open(path).convert("RGB")


def browser_chrome(screen, host, dark=False):
    """Draws a status bar and an address bar showing host over the top of a 2x screen."""
    draw = ImageDraw.Draw(screen)
    w = screen.width
    top = (12, 13, 16) if dark else PAGE_LIGHT
    ink = (240, 240, 240) if dark else (22, 24, 28)
    draw.rectangle([0, 0, w, 2 * (BAR + URL_BAR)], fill=top)
    draw.text((44, 2 * BAR / 2 + 4), "12:00", font=font(26, bold=True), fill=ink, anchor="lm")
    for k in range(4):  # signal bars
        x = w - 120 + k * 12
        draw.rounded_rectangle([x, 2 * BAR / 2 + 12 - 6 * k, x + 7, 2 * BAR / 2 + 14], 2, fill=ink)
    draw.rounded_rectangle([w - 64, 2 * BAR / 2 - 6, w - 30, 2 * BAR / 2 + 14], 5, outline=ink, width=3)
    draw.rounded_rectangle([w - 60, 2 * BAR / 2 - 2, w - 40, 2 * BAR / 2 + 10], 2, fill=ink)
    if host:
        draw.rounded_rectangle([24, 2 * BAR + 6, w - 24, 2 * (BAR + URL_BAR) - 10], 20,
                               fill=(226, 228, 233) if not dark else (40, 42, 48))
        draw.text((w / 2, 2 * BAR + URL_BAR - 2), host, font=font(24), fill=(70, 75, 85), anchor="mm")


def touch(screen, x, y, played):
    """A fingertip touch: a soft gray circle that fades. played runs 0 to 1."""
    if played is None or not 0 <= played <= 1:
        return
    layer = Image.new("RGBA", screen.size, (0, 0, 0, 0))
    r = 44 * (1 - 0.25 * ease_out(played))
    ImageDraw.Draw(layer).ellipse([x - r, y - r, x + r, y + r], fill=(90, 95, 105, round(110 * (1 - played))))
    screen.alpha_composite(layer)


def camera_screen(qr_image, t):
    """The phone camera aimed at the headset lens: the QR code drifts in, the focus box
    closes on it, and a link to the headset's page pops up. t is seconds since Report."""
    w, h = 2 * PHONE_CSS[0], 2 * PHONE_CSS[1]
    screen = Image.new("RGBA", (w, h), (24, 26, 30, 255))
    glow = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    ImageDraw.Draw(glow).ellipse([-w * 0.3, h * 0.12, w * 1.3, h * 0.82], fill=(52, 56, 64, 255))
    screen.alpha_composite(glow.filter(ImageFilter.GaussianBlur(80)))
    aim = ease_in_out((t - 0.5) / 0.7)
    if aim > 0:
        size = round(w * (0.42 + 0.14 * aim))
        code = qr_image.resize((size, size), Image.LANCZOS).convert("RGBA")
        code.putalpha(round(255 * aim))
        cx, cy = w / 2 + 120 * (1 - aim), h * 0.42 + 60 * (1 - aim)
        screen.alpha_composite(code, (round(cx - size / 2), round(cy - size / 2)))
        focus = ease_out_back((t - 1.2) / 0.4)
        if t >= 1.2:
            half = size / 2 + 26 + 40 * (1 - clamp(focus))
            draw = ImageDraw.Draw(screen)
            for sx, sy in ((-1, -1), (1, -1), (-1, 1), (1, 1)):
                x, y = cx + sx * half, cy + sy * half
                draw.line([(x, y), (x - sx * 46, y)], fill=(250, 204, 40), width=6)
                draw.line([(x, y), (x, y - sy * 46)], fill=(250, 204, 40), width=6)
    pop = ease_out_back((t - 1.7) / 0.35)
    if t >= 1.7:
        draw = ImageDraw.Draw(screen)
        pw, ph = 420 * min(max(pop, 0.0), 1.1), 76
        y = h * 0.78
        draw.rounded_rectangle([w / 2 - pw / 2, y - ph / 2, w / 2 + pw / 2, y + ph / 2], 38, fill=(250, 204, 40))
        if pop > 0.6:
            draw.text((w / 2, y), "192.168.2.222", font=font(28, bold=True), fill=(24, 26, 30), anchor="mm")
    # The shutter button, as in a camera app.
    draw = ImageDraw.Draw(screen)
    draw.ellipse([w / 2 - 58, h - 190, w / 2 + 58, h - 74], outline=(236, 238, 241), width=8)
    draw.ellipse([w / 2 - 44, h - 176, w / 2 + 44, h - 88], fill=(236, 238, 241))
    browser_chrome(screen, None, dark=True)
    touch(screen, w / 2, h * 0.78, (t - 2.5) / 0.4)
    return screen


FORM_TITLE = "Surface knocks stop in Beat Saber"
FORM_WHAT = "Table knocks work in the menu, but not once the game starts."


def issue_form(report, title, what, pasted, pressed):
    """A plain issue form with the fields of .github/ISSUE_TEMPLATE/bug.yml, at 2x. Returns the
    full-height page and the y of the Report box and of the Create button."""
    w = 2 * PHONE_CSS[0]
    page = Image.new("RGBA", (w, 2600), PAGE_LIGHT + (255,))
    draw = ImageDraw.Draw(page)
    ink, dim, line = (22, 24, 28), (84, 91, 104), (213, 217, 224)
    label, body, small = font(26, bold=True), font(26), font(22)
    mono = ImageFont.truetype("/usr/share/fonts/noto/NotoSansMono-Regular.ttf", 20) \
        if Path("/usr/share/fonts/noto/NotoSansMono-Regular.ttf").exists() else font(20)
    x, y = 32, 2 * (BAR + URL_BAR) + 28
    draw.text((x, y), "VioletTides / KnockThru", font=small, fill=dim)
    y += 44
    draw.text((x, y), "Something wrong", font=font(40, bold=True), fill=ink)
    y += 76

    def field(name, value, lines=1, monospace=False):
        nonlocal y
        draw.text((x, y), name, font=label, fill=ink)
        y += 44
        height = 22 + 38 * lines
        draw.rounded_rectangle([x, y, w - x, y + height], 14, fill=(255, 255, 255), outline=line, width=2)
        text_font = mono if monospace else body
        for k, part in enumerate(value.split("\n")[:lines]):
            draw.text((x + 18, y + 12 + 38 * k), part, font=text_font, fill=ink)
        top = y
        y += height + 30
        return top

    def wrap(text, width_px, text_font):
        words, rows, row = text.split(), [], ""
        for word in words:
            test = (row + " " + word).strip()
            if draw.textlength(test, font=text_font) > width_px and row:
                rows.append(row)
                row = word
            else:
                row = test
        if row:
            rows.append(row)
        return "\n".join(rows)

    field("Title", title)
    field("What went wrong?", wrap(what, w - 2 * x - 36, body), lines=3)
    field("KnockThru version", "0.2.0")
    field("SteamVR version", "2.18.2")
    field("Passthrough status", "off")
    field("Last error KnockThru showed", "")
    report_y = field("Report", report if pasted else "", lines=7, monospace=True)
    button_y = y + 10
    fill = tuple(round(c * (0.8 if pressed else 1)) for c in FORM_BLUE)
    draw.rounded_rectangle([x, button_y, w - x, button_y + 92], 28, fill=fill)
    draw.text((w / 2, button_y + 46), "Create", font=font(30, bold=True), fill=(245, 247, 250), anchor="mm")
    return page.crop((0, 0, w, button_y + 92 + 300)), report_y, button_y + 46


def report_phone(panel_dir):
    """Returns phone(t): the phone at t seconds into its story, which starts with the camera open."""
    frames_dir = panel_dir / "report"
    with tempfile.TemporaryDirectory() as tmp:
        page_html = panel_dir / "readme-phone.html"
        copied_html = Path(tmp) / "copied.html"
        copied_html.write_text(page_html.read_text().replace(
            '<p id="status" role="status"></p>', '<p id="status" role="status">Copied. Opening GitHub...</p>'))
        view = PHONE_CSS[1] - BAR - URL_BAR
        page = capture(page_html, Path(tmp) / "page.png", view)
        copied = capture(copied_html, Path(tmp) / "copied.png", view)
    # The Copy button is the only large run of the button blue.
    pixels = page.load()
    rows = [y for y in range(page.height) if pixels[page.width // 2, y] == FORM_BLUE]
    copy_y = (rows[0] + rows[-1]) / 2 + 2 * (BAR + URL_BAR) if rows else 600
    report = "KnockThru bug report, 2026-10-15 14:02:11\n\nKnockThru: 0.2.0\nSteamVR: 2.18.2\n" \
             "Registered with SteamVR: yes\nStarts with SteamVR: yes\nCurrent app: steam.app.620980"
    card = Image.open(panel_dir / "readme-report.png").convert("RGBA")
    flat = Image.new("RGB", card.size, BACKGROUND)
    flat.paste(card, mask=card)
    qr = hide_qr(flat.crop(qr_box(flat)))  # as blurred as on the panel

    def web(shot, host):
        screen = Image.new("RGBA", (2 * PHONE_CSS[0], 2 * PHONE_CSS[1]), PAGE_LIGHT + (255,))
        screen.paste(shot, (0, 2 * (BAR + URL_BAR)))
        browser_chrome(screen, host)
        return screen

    def form_screen(t):
        typed_title = FORM_TITLE[:round(len(FORM_TITLE) * clamp((t - 6.3) / 1.0))]
        typed_what = FORM_WHAT[:round(len(FORM_WHAT) * clamp((t - 7.6) / 1.2))]
        full, report_y, button_y = issue_form(report, typed_title, typed_what, t >= 10.1, 11.4 <= t < 11.7)
        view_h = 2 * PHONE_CSS[1]
        down = min(report_y - 2 * (BAR + URL_BAR) - 120, full.height - view_h)
        end = full.height - view_h
        scroll = down * ease_in_out((t - 9.0) / 0.7) + (end - down) * ease_in_out((t - 10.6) / 0.5)
        screen = full.crop((0, round(scroll), full.width, round(scroll) + view_h))
        if 10.1 <= t < 10.6:  # the pasted report flashes once
            flash = Image.new("RGBA", screen.size, (0, 0, 0, 0))
            y = report_y + 44 - scroll
            ImageDraw.Draw(flash).rounded_rectangle([32, y, screen.width - 32, y + 22 + 38 * 7], 14,
                                                     fill=FORM_BLUE + (round(60 * (1 - (t - 10.1) / 0.5)),))
            screen.alpha_composite(flash)
        browser_chrome(screen, "github.com")
        w = screen.width
        touch(screen, w * 0.5, 2 * (BAR + URL_BAR) + 200 - scroll, (t - 6.2) / 0.4)  # Title
        touch(screen, w * 0.5, 2 * (BAR + URL_BAR) + 330 - scroll, (t - 7.5) / 0.4)  # What went wrong?
        touch(screen, w * 0.5, report_y + 120 - scroll, (t - 9.9) / 0.4)
        touch(screen, w * 0.5, button_y - scroll, (t - 11.4) / 0.4)
        return screen

    def screen_at(t):
        if t < 2.8:
            return camera_screen(qr, t)
        if t < 5.5:
            shot = web(copied if t >= 4.6 else page, "192.168.2.222")
            touch(shot, shot.width / 2, copy_y, (t - 4.4) / 0.4)
            if t < 3.2:  # the page slides up over the camera
                under = camera_screen(qr, t)
                rise = ease_out((t - 2.8) / 0.4)
                under.alpha_composite(shot, (0, round(under.height * (1 - rise))))
                return under
            return shot
        form = form_screen(t)
        if t < 5.9:  # GitHub slides in from the right
            under = web(copied, "192.168.2.222")
            under.alpha_composite(form, (round(form.width * (1 - ease_in_out((t - 5.5) / 0.4))), 0))
            return under
        return form

    phone_w = 260
    bezel, radius = 12, 40
    screen_w = phone_w - 2 * bezel
    screen_h = round(screen_w * PHONE_CSS[1] / PHONE_CSS[0])
    phone_h = screen_h + 2 * bezel
    body = Image.new("RGBA", (phone_w, phone_h), (0, 0, 0, 0))
    ImageDraw.Draw(body).rounded_rectangle([0, 0, phone_w - 1, phone_h - 1], radius, fill=(10, 11, 14, 255),
                                           outline=(70, 74, 84, 255), width=2)
    mask = Image.new("L", (screen_w, screen_h), 0)
    ImageDraw.Draw(mask).rounded_rectangle([0, 0, screen_w - 1, screen_h - 1], radius - bezel, fill=255)
    island = round(phone_w * 0.28)

    def phone(t):
        screen = screen_at(max(t, -1.0)).convert("RGB").resize((screen_w, screen_h), Image.LANCZOS)
        image = body.copy()
        image.paste(screen, (bezel, bezel), mask)
        ImageDraw.Draw(image).rounded_rectangle([(phone_w - island) / 2, bezel + 8, (phone_w + island) / 2, bezel + 28],
                                                10, fill=(10, 11, 14))
        return image

    return phone


# The report GIF: the panel's report card, then the headset taken off and turned around, with
# the card in both lenses. The phone comes up to a lens to scan the code, then goes through the
# phone page and the issue form beside it. Times are seconds after the Report click.
TAKE_OFF = (2.3, 2.8)       # after a moment to read the card, the panel shrinks away and the headset appears
TURN = (2.8, 4.0)           # the headset turns to show its lenses
PHONE_TO_LENS = (4.1, 4.9)  # the phone moves over the left lens
STORY_START = 4.4           # the phone story's time 0, so its link tap lands while at the lens
PHONE_BACK = (7.2, 7.8)     # the phone goes back beside the headset once the link opens
PUT_ON = (16.5, 17.3)       # the view zooms into the left lens and back to the panel
LENS = (20, 24, 32)         # the lens glass color in images/cad/headset-turn/, from cad_sprites.py


def lens_regions(sprite):
    """The visible lens glass in a turn frame, as one (mask, box) per lens, left first."""
    pixels = np.asarray(sprite)
    glass = (np.abs(pixels[..., :3].astype(int) - LENS).sum(-1) < 40) & (pixels[..., 3] > 128)
    columns = np.nonzero(glass.any(0))[0]
    if not len(columns):
        return []
    # Lenses are separate runs of columns. Two runs when both show, one when only one does.
    splits = np.nonzero(np.diff(columns) > 2)[0]
    runs = np.split(columns, splits + 1)
    regions = []
    for run in runs:
        part = np.zeros_like(glass)
        part[:, run[0]:run[-1] + 1] = glass[:, run[0]:run[-1] + 1]
        rows = np.nonzero(part.any(1))[0]
        regions.append((Image.fromarray((part * 255).astype(np.uint8)), (run[0], rows[0], run[-1] + 1, rows[-1] + 1)))
    return regions


def headset_frame(sprite, content, size):
    """A turn frame with content shown in each visible lens. Each lens shows the whole content,
    squeezed sideways as the lens turns away."""
    image = Image.new("RGB", sprite.size, BACKGROUND)
    image.paste(sprite, (0, 0), sprite)
    full_width = None
    for mask, (left, top, right, bottom) in lens_regions(sprite):
        height = bottom - top
        width = right - left
        shown = content.resize((max(1, width), max(1, round(height * content.height / content.width))), Image.LANCZOS)
        screen = Image.new("RGB", sprite.size, (8, 9, 11))
        screen.paste(shown, (left, round((top + bottom) / 2 - shown.height / 2)))
        image.paste(screen, (0, 0), mask)
    return image.resize(size, Image.LANCZOS)


def report_gif(panel_dir):
    width = 640
    scale = width / 1200
    cuts, clicks, pointer, frames = load_walkthrough(panel_dir / "report", width)
    report_click, done_click = clicks[-2], clicks[-1]
    card = Image.open(panel_dir / "readme-report.png").convert("RGBA")
    flat = Image.new("RGB", card.size, BACKGROUND)
    flat.paste(card, mask=card)
    qr = tuple(round(v * scale) for v in qr_box(flat))
    qr_area = (qr[2] - qr[0]) * (qr[3] - qr[1])
    # Blur the code only in the frames that show it, wherever the card has risen to. Its white
    # square is the only large pure white area. Shrinking the frames makes text edges overshoot
    # past white too, so the mask is eroded first, which keeps only the code's solid white border.
    erode = 7
    for frame in frames:
        mask = Image.fromarray(((np.asarray(frame).min(-1) > 250) * 255).astype(np.uint8))
        white = np.asarray(mask.filter(ImageFilter.MinFilter(erode))) > 0
        if white.sum() < 0.05 * qr_area:
            continue
        rows, columns = np.nonzero(white.any(1))[0], np.nonzero(white.any(0))[0]
        grow = erode // 2 + 3
        box = (int(columns[0]) - grow, int(rows[0]) - grow, int(columns[-1]) + 1 + grow, int(rows[-1]) + 1 + grow)
        frame.paste(hide_qr(frame.crop(box)), box)
    add_pointer(frames, pointer, clicks, scale)  # before the lenses, so it shows inside them

    phone = report_phone(panel_dir)
    turn = [Image.open(path).convert("RGBA") for path in sorted((IMAGES / "cad" / "headset-turn").glob("*.png"))]
    area_w, area_h = width, frames[0].height
    k = min(520 / turn[0].width, (area_h - 70) / turn[0].height)
    head_size = (round(turn[0].width * k), round(turn[0].height * k))
    head_at = ((area_w - head_size[0]) // 2, (area_h - head_size[1]) // 2 + 22)  # below the caption
    lens_box = lens_regions(turn[-1])[0][1]
    lens_center = (head_at[0] + (lens_box[0] + lens_box[2]) / 2 * k, head_at[1] + (lens_box[1] + lens_box[3]) / 2 * k)
    lens_size = (lens_box[2] - lens_box[0]) * k
    caption = font(22, bold=True)

    def left_side(t, panel):
        take_off = ease_in_out((t - TAKE_OFF[0]) / (TAKE_OFF[1] - TAKE_OFF[0]))
        if take_off <= 0:
            return panel
        put_on = ease_in_out((t - PUT_ON[0]) / (PUT_ON[1] - PUT_ON[0]))
        angle = ease_in_out((t - TURN[0]) / (TURN[1] - TURN[0]))
        sprite = turn[round(angle * (len(turn) - 1))]
        view = Image.new("RGB", (area_w, area_h), BACKGROUND)
        grow = 1.12 - 0.12 * take_off  # the headset settles into place as it appears
        size = (round(head_size[0] * grow), round(head_size[1] * grow))
        head = headset_frame(sprite, panel, size)
        view.paste(head, (round(area_w / 2 - size[0] / 2), round(head_at[1] + head_size[1] / 2 - size[1] / 2)))
        draw = ImageDraw.Draw(view)
        shown = ease_in_out((t - TURN[1] + 0.2) / 0.4) * clamp(1 - (t - PUT_ON[0]) / 0.3)
        draw.text((area_w / 2, 30 + 6 * (1 - shown)), "Take off the headset. Scan either lens with your phone.",
                  font=caption, fill=mix(TEXT, shown), anchor="mm")
        if take_off < 1:  # the panel shrinks away as the headset comes off
            small = panel.resize((round(area_w * (1 - 0.4 * take_off)), round(area_h * (1 - 0.4 * take_off))), Image.LANCZOS)
            under = Image.new("RGB", (area_w, area_h), BACKGROUND)
            under.paste(small, ((area_w - small.width) // 2, (area_h - small.height) // 2))
            view = Image.blend(under, view, take_off)
        if put_on > 0:  # zoom into the left lens, and through it back to the panel
            zoom = 1 + (area_w / lens_size * 1.3 - 1) * put_on
            cx = lens_center[0] + (area_w / 2 - lens_center[0]) * put_on
            cy = lens_center[1] + (area_h / 2 - lens_center[1]) * put_on
            box = (lens_center[0] - cx / zoom, lens_center[1] - cy / zoom,
                   lens_center[0] + (area_w - cx) / zoom, lens_center[1] + (area_h - cy) / zoom)
            view = view.resize((area_w, area_h), Image.LANCZOS, box=box)
            view = Image.blend(view, panel, ease_in_out((put_on - 0.4) / 0.6))
        return view

    phone_slot = None
    out = []
    for i, frame in enumerate(frames):
        t = (i - report_click) / FPS
        left = left_side(t, frame)
        full = phone(t - STORY_START)
        hand = full
        if phone_slot is None:
            size = (20 + width + 36 + full.width + 24, max(area_h, full.height) + 40)
            origin = (20, (size[1] - area_h) // 2)
            phone_slot = (20 + width + 36, (size[1] - full.height) // 2)
            # Over the lens, held a little farther away: the camera view, in the upper half
            # of the screen, sits on the left lens.
            near = 0.72
            phone_lens = (round(origin[0] + lens_center[0] - full.width * near / 2),
                          round(origin[1] + lens_center[1] - full.height * near * 0.42))
        image = Image.new("RGB", size, BACKGROUND)
        image.paste(left, origin)
        go = ease_in_out((t - PHONE_TO_LENS[0]) / (PHONE_TO_LENS[1] - PHONE_TO_LENS[0])) \
            - ease_in_out((t - PHONE_BACK[0]) / (PHONE_BACK[1] - PHONE_BACK[0]))
        x = phone_slot[0] + (phone_lens[0] - phone_slot[0]) * go
        y = phone_slot[1] + (phone_lens[1] - phone_slot[1]) * go
        if go > 0:
            factor = 1 + (near - 1) * go
            hand = full.resize((round(full.width * factor), round(full.height * factor)), Image.LANCZOS)
            shadow = Image.new("RGBA", hand.size, (0, 0, 0, 0))
            shadow.putalpha(hand.getchannel("A").point(lambda a: round(a * 0.5 * go)))
            image.paste((0, 0, 0), (round(x) + 10, round(y) + 14), shadow.filter(ImageFilter.GaussianBlur(14)))
        image.paste(hand, (round(x), round(y)), hand)
        out.append(image)
    loop_fade(out)
    write_gif(out, IMAGES / "report.gif", step=2)

# Knock kinds: three small loops side by side, on the shared timeline.

def knock_kinds():
    cell, height, header = 340, 300, 56
    label, title = font(22, bold=True), font(24, bold=True)
    titles = {2: "Knock twice: passthrough turns on or off", 3: "Knock three times: the style switches"}
    frames = []
    for i in range(seconds(LOOP)):
        count, t, length = pass_at(i / FPS)
        image = Image.new("RGB", (cell * 3, height + header), BACKGROUND)
        draw = ImageDraw.Draw(image)
        shown = pass_fade(t, length)
        w = draw.textlength(titles[count], font=title)
        draw.text(((cell * 3 - w) / 2, 14 + 6 * (1 - shown)), titles[count], font=title, fill=mix(TEXT, shown))

        cards = Image.new("RGB", (cell * 3, height), BACKGROUND)
        draw = ImageDraw.Draw(cards)
        for c in range(3):
            draw.rounded_rectangle([c * cell + 10, 10, c * cell + cell - 10, height - 10], radius=18, fill=SURFACE)
        gap, played, shake = knock_motion(t, count), impact(t, count), buzz(t, action_time(count), count - 1)

        # Controllers together.
        knock_pair(cards, draw, cell // 2, 95, 150, gap, played, 30, shake=shake, base=SURFACE)

        # One controller on a surface, swinging down from the wrist.
        cx, table_y, h = cell + cell // 2, 210, 150
        burst(draw, cx, table_y, played, base=SURFACE)
        place(cards, "controller-right", cx + shake, table_y - head_radius(h) * 0.95 - 45 * gap, h,
              angle=round(135 - 12 * gap))
        draw.rectangle([cx - 130, table_y, cx + 130, table_y + 16], fill=TABLE)

        # The left controller count the left side of the headset.
        cx, cy, hw, h = 2 * cell + cell // 2, 110, 160, 130
        hx = cx + 70  # headset center
        edge = hx - hw / 2
        burst(draw, edge, cy, played, base=SURFACE)
        place(cards, "headset-front", hx, cy, round(hw * 600 / 1110))
        place(cards, "controller-left", edge - head_radius(h) * 0.95 - 30 * gap + shake, cy, h,
              angle=round(-30 - 8 * gap))

        for c, text in enumerate(("Controllers together", "Controller on a surface", "Knock on the headset")):
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
    words = {2: ("third", "Passthrough on", "both controllers buzz"),
             3: ("fourth", "Style switches", "both controllers buzz twice")}

    def x_at(t):
        return left + (right - left) * t / span

    def strip(now, count):
        knocks = knock_times(count)
        action = action_time(count)
        next_knock, done, buzzed = words[count]
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
            draw.text((x + 26 - slide + buzz(now, action, count - 1) * 1.5, axis_y - 14), buzzed,
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
        count, now, length = pass_at(i / FPS)
        image = strip(now, count)
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
        walkthrough(Path(tmp) / "calibrate", "calibrate")
        walkthrough(Path(tmp) / "update", "update")
        report_gif(Path(tmp))
    knock_kinds()
    timing()


if __name__ == "__main__":
    main()
