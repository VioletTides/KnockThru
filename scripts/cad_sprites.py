#!/usr/bin/env python3
"""Renders Steam Frame CAD files into the sprites in images/cad/.

Usage: python3 scripts/cad_sprites.py CAD_DIRECTORY

CAD_DIRECTORY holds Valve's STEP files from
https://gitlab.steamos.cloud/SteamHardware/SteamFrame:
  Valve_Steam_Frame_Controller_L_20260926.stp
  Valve_Steam_Frame_Controller_R_20260926.stp
  Valve_Steam_frame_core_module_20261013.stp

Needs cadquery, numpy, and opencv-python. The sprites use the logo style: a flat
light shape, a gap between the controller head and grip, the controls cut out, and
an accent thumbstick. The pixel sizes below assume the 2048 px render.
"""
import sys
from pathlib import Path

import cadquery as cq
import cv2
import numpy as np

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / "images" / "cad"
N = 2048  # render size, in pixels
SPRITE_HEIGHT = 600  # saved sprite height, in pixels

INK = (236, 238, 241)  # kText in src/panel.cpp
ACCENT = (58, 145, 232)  # kAccent

CONTROLLERS = {
    "controller-left": "Valve_Steam_Frame_Controller_L_20260926.stp",
    "controller-right": "Valve_Steam_Frame_Controller_R_20260926.stp",
}
HEADSET = "Valve_Steam_frame_core_module_20261013.stp"


def disk(size):
    return cv2.getStructuringElement(cv2.MORPH_ELLIPSE, (size, size))


def render(path, toward=None, up=None):
    """Renders an orthographic view. Returns depth (toward the viewer) and CAD face ids.

    Without a direction, the thinnest bounding box axis faces the viewer.
    """
    shape = cq.importers.importStep(str(path))
    tris, fid = [], []
    for i, face in enumerate(shape.faces().vals()):
        vertices, indices = face.tessellate(0.05, 0.1)
        v = np.array([[p.x, p.y, p.z] for p in vertices])
        for t in indices:
            tris.append(v[list(t)])
            fid.append(i)
    tris, fid = np.array(tris), np.array(fid)
    if toward is None:
        order = np.argsort(np.ptp(tris.reshape(-1, 3), axis=0))
        toward, up = np.eye(3)[order[0]], np.eye(3)[order[1]]
    d = np.asarray(toward, float) / np.linalg.norm(toward)
    up = np.asarray(up, float) - d * np.dot(up, d)
    up /= np.linalg.norm(up)
    right = np.cross(up, d)
    p = np.stack([tris @ right, -(tris @ up), tris @ d], -1)
    lo, hi = p[..., :2].reshape(-1, 2).min(0), p[..., :2].reshape(-1, 2).max(0)
    scale = N * 0.92 / (hi - lo).max()
    xy = np.round(((p[..., :2] - lo) * scale + (N - (hi - lo) * scale) / 2) * 4).astype(np.int32)
    depth = np.full((N, N), -1e9, np.float32)
    ids = np.full((N, N), -1, np.int32)
    for k in np.argsort(p[..., 2].mean(1)):  # far to near
        cv2.fillConvexPoly(depth, xy[k], float(p[k, :, 2].mean()), shift=2)
        cv2.fillConvexPoly(ids, xy[k], int(fid[k]), shift=2)
    return depth, ids


def head_mask(depth):
    """The controller head: the high region bounded by depth jumps, without the grip collar."""
    sil = depth > -1e8
    d = np.where(sil, depth, depth[sil].min() - 20).astype(np.float32)
    gy, gx = np.gradient(d)
    jump = np.hypot(gx, gy) > 0.35
    top = d > np.percentile(d[sil], 60)
    _, lab = cv2.connectedComponents((sil & ~jump).astype(np.uint8))
    head = (lab == np.bincount(lab[top & (lab > 0)].ravel()).argmax()).astype(np.uint8)
    head = cv2.morphologyEx(head, cv2.MORPH_CLOSE, np.ones((25, 25), np.uint8))
    contours, _ = cv2.findContours(head, cv2.RETR_EXTERNAL, cv2.CHAIN_APPROX_NONE)
    head = np.zeros_like(head)
    cv2.drawContours(head, [max(contours, key=cv2.contourArea)], -1, 1, -1)
    # A 301 px opening keeps the round rim and drops the narrower collar.
    return cv2.morphologyEx(head, cv2.MORPH_OPEN, disk(301))


def control_mask(ids, head):
    """The buttons and D-pad: CAD faces on the head that are not large dome faces."""
    u, c = np.unique(ids[head > 0], return_counts=True)
    dome = u[c > 8000]
    feat = ((head > 0) & ~np.isin(ids, dome)).astype(np.uint8)
    dist = cv2.distanceTransform(head, cv2.DIST_L2, 5)
    n, lab, st, cen = cv2.connectedComponentsWithStats(cv2.erode(feat, np.ones((7, 7), np.uint8)))
    filled = np.zeros_like(feat)
    for i in range(1, n):
        if st[i, 4] < 300 or dist[int(cen[i][1]), int(cen[i][0])] < 90:
            continue
        piece = cv2.dilate((lab == i).astype(np.uint8), np.ones((7, 7), np.uint8)) & feat
        contours, _ = cv2.findContours(piece, cv2.RETR_EXTERNAL, cv2.CHAIN_APPROX_NONE)
        cv2.drawContours(filled, contours, -1, 1, -1)
    # Open each control with the largest disk that keeps 70% of it, to strip bezel tails.
    n, lab = cv2.connectedComponents(filled)
    out = np.zeros_like(filled)
    for i in range(1, n):
        piece = (lab == i).astype(np.uint8)
        for k in (91, 61, 45, 35, 27, 21, 15, 9):
            opened = cv2.morphologyEx(piece, cv2.MORPH_OPEN, disk(k))
            if opened.sum() >= 0.7 * piece.sum():
                break
        m, ml = cv2.connectedComponents(opened)
        if m > 2:
            opened = (ml == 1 + np.argmax(np.bincount(ml[ml > 0].ravel())[1:])).astype(np.uint8)
        out |= opened
    return out, dome


def thumbstick(depth, ids, head, dome):
    """Returns the stick center, the stick top radius, and the well radius."""
    ys, xs = np.nonzero(depth > depth.max() - 1.0)
    cx, cy = xs.mean(), ys.mean()
    angles = np.linspace(0, 2 * np.pi, 90, endpoint=False)

    def at(r, a):
        return int(cy + r * np.sin(a)), int(cx + r * np.cos(a))

    profile = np.array([[depth[at(r, a)] for r in range(400)] for a in angles])
    r_top = int(np.median(np.argmin(np.diff(profile, axis=1), axis=1)))
    dome = set(dome)
    wells = []
    for a in angles:
        r = r_top + 3
        while r < 400 and ids[at(r, a)] not in dome:
            r += 1
        wells.append(r)
    return cx, cy, r_top, int(np.median(wells))


def controller_sprite(depth, ids):
    sil = depth > -1e8
    head = head_mask(depth)
    controls, dome = control_mask(ids, head)
    cx, cy, r_top, r_well = thumbstick(depth, ids, head, dome)

    # Drop control pieces that belong to the stick well or sit on the rim.
    well = np.zeros(sil.shape, np.uint8)
    cv2.circle(well, (int(cx), int(cy)), r_well + 8, 1, -1)
    pieces = cv2.morphologyEx(controls & (1 - well), cv2.MORPH_OPEN, disk(21))
    dist = cv2.distanceTransform(head, cv2.DIST_L2, 5)
    n, lab, st, cen = cv2.connectedComponentsWithStats(pieces)
    keep = np.zeros(sil.shape, bool)
    for i in range(1, n):
        if st[i, 4] <= 1500 or dist[int(cen[i][1]), int(cen[i][0])] <= 130:
            continue
        piece = (lab == i).astype(np.uint8)
        if np.hypot(cen[i][0] - cx, cen[i][1] - cy) < r_well + 60:
            piece = cv2.morphologyEx(piece, cv2.MORPH_OPEN, disk(101))  # bezel crescent beside a button
        keep |= piece > 0

    # Same layers as the logo: the grip stops 35 px short of the head.
    body = (sil & ~cv2.dilate(head, disk(71)).astype(bool)) | (head > 0)
    body &= ~keep
    body &= ~cv2.circle(np.zeros(sil.shape, np.uint8), (int(cx), int(cy)), r_well, 1, -1).astype(bool)
    stick = cv2.circle(np.zeros(sil.shape, np.uint8), (int(cx), int(cy)), r_top, 1, -1).astype(bool)
    return compose(body, stick)


def headset_sprite(depth, ids):
    """The headset front: the visor face, with a gap to the frame around it."""
    sil = depth > -1e8
    u, c = np.unique(ids[sil], return_counts=True)
    visor = np.isin(ids, u[c >= 0.5 * c.max()]).astype(np.uint8)
    visor = cv2.morphologyEx(visor, cv2.MORPH_CLOSE, disk(15))
    contours, _ = cv2.findContours(visor, cv2.RETR_EXTERNAL, cv2.CHAIN_APPROX_NONE)
    visor = np.zeros_like(visor)
    cv2.drawContours(visor, [max(contours, key=cv2.contourArea)], -1, 1, -1)
    visor = cv2.erode(visor, disk(121))
    body = (sil & ~cv2.dilate(visor, disk(61)).astype(bool)) | (visor > 0)
    # The two front cameras: small round faces inside the visor.
    cams = np.zeros(sil.shape, bool)
    n, lab, st, cen = cv2.connectedComponentsWithStats(((ids >= 0) & (visor > 0) & ~np.isin(ids, u[c >= 0.5 * c.max()])).astype(np.uint8))
    for i in range(1, n):
        piece = (lab == i).astype(np.uint8)
        contours, _ = cv2.findContours(piece, cv2.RETR_EXTERNAL, cv2.CHAIN_APPROX_NONE)
        perimeter = cv2.arcLength(contours[0], True)
        if st[i, 4] > 400 and 4 * np.pi * st[i, 4] / perimeter ** 2 > 0.8:  # round pieces only
            cams |= piece > 0
    body &= ~cv2.dilate(cams.astype(np.uint8), disk(9)).astype(bool)
    return compose(body, np.zeros_like(body))


def headset_back_sprite(depth, ids):
    """The headset from the face side: the body, with the two lenses cut out behind a gap,
    so a drawing can show the screen through them."""
    sil = depth > -1e8
    u, c = np.unique(ids[sil], return_counts=True)
    glass = np.isin(ids, u[np.argsort(c)[-2:]]).astype(np.uint8)  # the two lens faces are the largest
    contours, _ = cv2.findContours(glass, cv2.RETR_EXTERNAL, cv2.CHAIN_APPROX_NONE)
    lenses = np.zeros_like(glass)
    for contour in sorted(contours, key=cv2.contourArea)[-2:]:
        (x, y), r = cv2.minEnclosingCircle(contour)
        cv2.circle(lenses, (round(x), round(y)), round(r), 1, -1)
    body = sil & ~cv2.dilate(lenses, disk(61)).astype(bool)
    return compose(body, np.zeros_like(body))


def compose(body, accent):
    rgba = np.zeros(body.shape + (4,), np.uint8)
    rgba[body] = (*INK[::-1], 255)  # cv2 writes BGRA
    rgba[accent] = (*ACCENT[::-1], 255)
    ys, xs = np.nonzero(rgba[..., 3])
    rgba = rgba[ys.min():ys.max() + 1, xs.min():xs.max() + 1]
    scale = SPRITE_HEIGHT / rgba.shape[0]
    # Premultiply so the edges do not pick up the transparent pixels' color.
    a = rgba[..., 3:].astype(np.float32) / 255
    pre = np.dstack([rgba[..., :3] * a, a * 255])
    small = cv2.resize(pre, (round(rgba.shape[1] * scale), SPRITE_HEIGHT), interpolation=cv2.INTER_AREA)
    alpha = small[..., 3:] / 255
    out = np.dstack([small[..., :3] / np.maximum(alpha, 1e-6), small[..., 3:]])
    return np.clip(out, 0, 255).astype(np.uint8)


def main():
    if len(sys.argv) != 2:
        raise SystemExit(__doc__)
    cad = Path(sys.argv[1])
    OUT.mkdir(parents=True, exist_ok=True)
    jobs = [(name, step, (0, 0, 1), (0, 1, 0), controller_sprite) for name, step in CONTROLLERS.items()]
    jobs.append(("headset-front", HEADSET, None, None, headset_sprite))
    jobs.append(("headset-back", HEADSET, (0, 0, -1), (0, 1, 0), headset_back_sprite))
    for name, step, toward, up, make in jobs:
        print(name, flush=True)
        depth, ids = render(cad / step, toward, up)
        path = OUT / f"{name}.png"
        cv2.imwrite(str(path), make(depth, ids))
        print(f"  wrote {path.relative_to(ROOT)}")


if __name__ == "__main__":
    main()
