#!/usr/bin/env python3
"""Renders Steam Frame CAD files into the sprites in images/cad/.

Usage: python3 scripts/cad_sprites.py CAD_DIRECTORY

CAD_DIRECTORY holds Valve's STEP files from
https://gitlab.steamos.cloud/SteamHardware/SteamFrame:
  Valve_Steam_Frame_Controller_L_20260926.stp
  Valve_Steam_Frame_Controller_R_20260926.stp
  Valve_Steam_frame_core_module_20261013.stp

Needs cadquery, numpy, and opencv-python. Each sprite is an orthographic view,
shaded between two panel colors, on a transparent background.
"""
import sys
from pathlib import Path

import cadquery as cq
import cv2
import numpy as np

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / "images" / "cad"
SIZE = 1024  # render size before trimming, in pixels

# Shading runs from kRaised to kKnobOff in src/panel.cpp.
DARK = np.array((44, 48, 57), np.float32)
LIGHT = np.array((196, 200, 208), np.float32)
EDGE = (18, 20, 24)  # kBackground, for the outline

# name: (STEP file, direction toward the viewer, up direction on screen)
VIEWS = {
    "controller-left": ("Valve_Steam_Frame_Controller_L_20260926.stp", (0, 0, 1), (0, 1, 0)),
    "controller-right": ("Valve_Steam_Frame_Controller_R_20260926.stp", (0, 0, 1), (0, 1, 0)),
    "headset-front": ("Valve_Steam_frame_core_module_20261013.stp", None, None),
}


def triangles(shape):
    tris = []
    for face in shape.faces().vals():
        vertices, indices = face.tessellate(0.03, 0.05)
        v = np.array([[p.x, p.y, p.z] for p in vertices])
        tris.extend(v[list(t)] for t in indices)
    return np.array(tris)


def headset_axes(tris):
    """Picks the front view of the headset: the thinnest box axis faces the viewer."""
    pts = tris.reshape(-1, 3)
    extent = pts.max(0) - pts.min(0)
    print("  headset extent (mm):", np.round(extent, 1))
    order = np.argsort(extent)
    toward = np.eye(3)[order[0]]
    up = np.eye(3)[order[1]]
    return toward, up


def render(tris, toward, up, flip=1):
    d = np.asarray(toward, float) * flip
    d /= np.linalg.norm(d)
    up = np.asarray(up, float) - d * np.dot(up, d)
    up /= np.linalg.norm(up)
    right = np.cross(up, d)
    p = np.stack([tris @ right, -(tris @ up), tris @ d], -1)
    lo, hi = p[..., :2].reshape(-1, 2).min(0), p[..., :2].reshape(-1, 2).max(0)
    scale = SIZE * 0.96 / (hi - lo).max()
    xy = (p[..., :2] - lo) * scale + (SIZE - (hi - lo) * scale) / 2

    n = np.cross(tris[:, 1] - tris[:, 0], tris[:, 2] - tris[:, 0])
    n /= np.linalg.norm(n, axis=1, keepdims=True) + 1e-12
    light = -0.45 * right + 0.6 * up + 0.66 * d
    shade = np.clip(np.abs(n @ light), 0, 1)

    tone = np.zeros((SIZE, SIZE), np.float32)
    mask = np.zeros((SIZE, SIZE), np.uint8)
    for k in np.argsort(p[..., 2].mean(1)):  # far to near
        poly = np.round(xy[k] * 4).astype(np.int32)
        cv2.fillConvexPoly(tone, poly, float(shade[k]), shift=2)
        cv2.fillConvexPoly(mask, poly, 255, shift=2)
    return tone, mask


def finish(tone, mask):
    """Smooths out tessellation noise, maps shading to panel colors, and adds a dark outline."""
    inside = mask > 0
    m = inside.astype(np.float32)
    tone = cv2.GaussianBlur(tone * m, (0, 0), 2.5) / np.maximum(cv2.GaussianBlur(m, (0, 0), 2.5), 1e-3)
    t = np.clip(tone, 0, 1)[..., None] ** 1.3
    rgb = DARK + (LIGHT - DARK) * t
    outline = inside & ~cv2.erode(mask, np.ones((7, 7), np.uint8)).astype(bool)
    rgb[outline] = EDGE
    alpha = cv2.GaussianBlur(mask, (3, 3), 0)
    image = np.dstack([rgb[..., ::-1], alpha]).astype(np.uint8)  # BGRA for cv2
    ys, xs = np.nonzero(inside)
    return image[ys.min():ys.max() + 1, xs.min():xs.max() + 1]


def main():
    if len(sys.argv) != 2:
        raise SystemExit(__doc__)
    cad = Path(sys.argv[1])
    OUT.mkdir(parents=True, exist_ok=True)
    for name, (step, toward, up) in VIEWS.items():
        print(name, flush=True)
        tris = triangles(cq.importers.importStep(str(cad / step)))
        if toward is None:
            toward, up = headset_axes(tris)
        tone, mask = render(tris, toward, up)
        path = OUT / f"{name}.png"
        cv2.imwrite(str(path), finish(tone, mask))
        print(f"  wrote {path.relative_to(ROOT)}")


if __name__ == "__main__":
    main()
