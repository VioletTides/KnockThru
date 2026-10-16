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
an accent thumbstick. The headset also gets a turn from its front to its face side,
as frames in images/cad/headset-turn/. The pixel sizes below assume the 2048 px
render, except in headset_turn(), which renders at TURN_SIZE.
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


def tessellate(path):
    """Imports a STEP file. Returns its triangles and each triangle's CAD face id."""
    shape = cq.importers.importStep(str(path))
    tris, fid = [], []
    for i, face in enumerate(shape.faces().vals()):
        vertices, indices = face.tessellate(0.05, 0.1)
        v = np.array([[p.x, p.y, p.z] for p in vertices])
        for t in indices:
            tris.append(v[list(t)])
            fid.append(i)
    return np.array(tris), np.array(fid)


def view_basis(toward, up):
    d = np.asarray(toward, float) / np.linalg.norm(toward)
    up = np.asarray(up, float) - d * np.dot(up, d)
    up /= np.linalg.norm(up)
    return np.cross(up, d), up, d


def rasterize(tris, fid, toward, up, size=N, center=None, scale=None):
    """Draws triangles in an orthographic view. Returns depth (toward the viewer) and CAD face
    ids. Without center and scale, the drawing fills 92% of the image."""
    right, up, d = view_basis(toward, up)
    p = np.stack([tris @ right, -(tris @ up), tris @ d], -1)
    if scale is None:
        lo, hi = p[..., :2].reshape(-1, 2).min(0), p[..., :2].reshape(-1, 2).max(0)
        scale = size * 0.92 / (hi - lo).max()
        offset = (size - (hi - lo) * scale) / 2 - lo * scale
    else:
        c = np.array([center @ right, -(center @ up)])
        offset = size / 2 - c * scale
    xy = np.round((p[..., :2] * scale + offset) * 4).astype(np.int32)
    depth = np.full((size, size), -1e9, np.float32)
    ids = np.full((size, size), -1, np.int32)
    for k in np.argsort(p[..., 2].mean(1)):  # far to near
        cv2.fillConvexPoly(depth, xy[k], float(p[k, :, 2].mean()), shift=2)
        cv2.fillConvexPoly(ids, xy[k], int(fid[k]), shift=2)
    return depth, ids


def render(path, toward=None, up=None):
    """Renders an orthographic view. Returns depth (toward the viewer) and CAD face ids.

    Without a direction, the thinnest bounding box axis faces the viewer.
    """
    tris, fid = tessellate(path)
    if toward is None:
        order = np.argsort(np.ptp(tris.reshape(-1, 3), axis=0))
        toward, up = np.eye(3)[order[0]], np.eye(3)[order[1]]
    return rasterize(tris, fid, toward, up)


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


TURN_SIZE = 1024  # render size for the turn, in pixels
TURN_STEP = 6  # degrees between turn frames
TURN_HEIGHT = 480  # saved turn frame height, in pixels
LENS = (20, 24, 32)  # lens glass in the turn frames, which readme_media.py finds by color


def smooth(mask, sigma=3):
    return cv2.GaussianBlur(mask.astype(np.float32), (0, 0), sigma) > 0.5


def drop_specks(mask, area=400):
    """Removes separate pieces smaller than area pixels, and keeps thin lines whole."""
    n, labels, stats, _ = cv2.connectedComponentsWithStats(mask.astype(np.uint8))
    return np.isin(labels, [i for i in range(1, n) if stats[i, 4] >= area])


def headset_turn(cad, angles=None, mesh=None, folder=OUT / "headset-turn"):
    """Renders the headset turning from the front (0 degrees) to the face side (180), one
    frame every TURN_STEP degrees, into images/cad/headset-turn/.

    Each frame keeps one scale and center, so the frames play as a turn. The visor outline is
    traced once on the front view and the lens outlines once on the face side. Both are lifted
    into 3D, onto a surface fitted to the visor and a plane fitted to each lens, then projected
    at every angle. A lens shows only where no part of the shell is in front of its plane.

    To test a fix, pass a few angles, the (triangles, face ids) from tessellate() as mesh so
    the STEP file is read once, and a scratch folder. The crop then fits only those angles.
    """
    size = TURN_SIZE
    tris, fid = mesh if mesh is not None else tessellate(cad / HEADSET)
    points = tris.reshape(-1, 3)
    center = points.mean(0)
    scale = size * 0.92 / (2 * np.linalg.norm(points - center, axis=1).max())  # fits at every angle
    up = np.array([0.0, 1.0, 0.0])

    def toward(degrees):
        a = np.radians(degrees)
        return np.array([np.sin(a), 0.0, np.cos(a)])

    def view(degrees):
        """Depth here is measured from the headset's center, as lift() expects."""
        depth, ids = rasterize(tris, fid, toward(degrees), up, size, center, scale)
        return np.where(depth > -1e8, depth - center @ toward(degrees), depth), ids

    def lift(xy, depth, degrees):
        """Image points with depths at one angle, as 3D points."""
        right, _, d = view_basis(toward(degrees), up)
        x, y = (xy[:, 0] - size / 2) / scale, (xy[:, 1] - size / 2) / scale
        return center + x[:, None] * right - y[:, None] * up + depth[:, None] * d

    def outline(mask):
        contours, _ = cv2.findContours(mask.astype(np.uint8), cv2.RETR_EXTERNAL, cv2.CHAIN_APPROX_NONE)
        return max(contours, key=cv2.contourArea)[:, 0, :].astype(float)

    def project(points3, degrees):
        right, _, _ = view_basis(toward(degrees), up)
        q = points3 - center
        xy = np.stack([q @ right * scale + size / 2, -(q @ up) * scale + size / 2], -1)
        mask = np.zeros((size, size), np.uint8)
        cv2.fillPoly(mask, [np.round(xy * 4).astype(np.int32)], 1, shift=2)
        return mask > 0

    # The visor: its outline and gap, as in headset_sprite, on a smooth surface fitted to its depth.
    depth, ids = view(0)
    u, c = np.unique(ids[ids >= 0], return_counts=True)
    visor = np.isin(ids, u[c >= 0.5 * c.max()])
    ys, xs = np.nonzero(visor)
    terms = lambda x, y: np.stack([x ** i * y ** j for i in range(5) for j in range(5 - i)], -1)
    norm = lambda v: (v - size / 2) / size
    coef, *_ = np.linalg.lstsq(terms(norm(xs), norm(ys)), depth[ys, xs], rcond=None)
    shape = np.zeros((size, size), np.uint8)
    cv2.drawContours(shape, [outline(cv2.morphologyEx(visor.astype(np.uint8), cv2.MORPH_CLOSE, disk(9))).astype(np.int32)],
                     -1, 1, -1)
    inner = smooth(cv2.erode(shape, disk(61)), 4).astype(np.uint8)
    edges = []
    for mask in (inner, smooth(cv2.dilate(inner, disk(31))).astype(np.uint8)):
        xy = outline(mask)
        edges.append(lift(xy, terms(norm(xy[:, 0]), norm(xy[:, 1])) @ coef, 0))
    visor_inner, visor_outer = edges

    # The lenses: outline, rim, and hole, each on a plane fitted to its lens.
    depth, ids = view(180)
    u, c = np.unique(ids[ids >= 0], return_counts=True)
    glass_ids = u[np.argsort(c)[-2:]]  # the two lens faces are the largest
    holes = np.zeros((size, size), np.uint8)
    lenses = []
    for gid in glass_ids:
        ys, xs = np.nonzero(ids == gid)
        a = np.stack([xs, ys, np.ones_like(xs)], -1).astype(float)
        plane, *_ = np.linalg.lstsq(a, depth[ys, xs], rcond=None)
        on_plane = lift(np.stack([xs, ys], -1).astype(float), a @ plane, 180)
        mask = smooth(ids == gid, 2).astype(np.uint8)
        holes |= cv2.dilate(mask, disk(61))

        def lifted(m):
            xy = outline(m)
            return lift(xy, np.stack([xy[:, 0], xy[:, 1], np.ones(len(xy))], -1) @ plane, 180)
        lenses.append(dict(normal=np.linalg.svd(on_plane - on_plane.mean(0))[2][2], point=on_plane.mean(0),
                           glass=lifted(mask), rim_in=lifted(cv2.dilate(mask, disk(15))),
                           rim_out=lifted(cv2.dilate(mask, disk(31))), hole=lifted(cv2.dilate(mask, disk(61)))))
    assembly = np.unique(ids[(holes > 0) & (ids >= 0)])  # the lens barrels, which never hide a lens

    yy, xx = np.mgrid[0:size, 0:size]
    frames = []
    angles = list(angles if angles is not None else range(0, 181, TURN_STEP))
    for degrees in angles:
        depth, ids = view(degrees)
        sil = smooth(depth > -1e8)
        body = sil.copy()
        glass = np.zeros_like(sil)
        if degrees <= 78:  # past this, the gap is seen edge on and breaks into specks
            gap = (project(visor_outer, degrees) & ~project(visor_inner, degrees)).astype(np.uint8)
            body &= ~drop_specks(gap)
        if degrees >= 102:  # before this, the lenses are seen edge on and break into specks
            right, _, d = view_basis(toward(degrees), up)
            rays = center + ((xx - size / 2) / scale)[..., None] * right - ((yy - size / 2) / scale)[..., None] * up
            blocker = (depth > -1e8) & ~np.isin(ids, assembly)
            for lens in lenses:
                plane = ((lens["point"] - rays) @ lens["normal"]) / (d @ lens["normal"])
                clear = smooth(~(blocker & (depth > plane + 1.0)), 1.5) & sil
                rim = project(lens["rim_out"], degrees) & ~project(lens["rim_in"], degrees)
                body = (body & ~drop_specks(project(lens["hole"], degrees) & clear)) | (rim & clear)
                glass |= project(lens["glass"], degrees) & clear
        frames.append((body, glass))
        print(f"  {degrees} degrees", flush=True)

    # One crop for every frame, so the headset stays put while it turns.
    ys, xs = np.nonzero(np.any([b | g for b, g in frames], axis=0))
    box = (slice(ys.min(), ys.max() + 1), slice(xs.min(), xs.max() + 1))
    folder.mkdir(parents=True, exist_ok=True)
    for degrees, (body, glass) in zip(angles, frames):
        rgba = np.zeros(body.shape + (4,), np.uint8)
        rgba[body] = (*INK[::-1], 255)
        rgba[glass] = (*LENS[::-1], 255)
        rgba = rgba[box]
        height = TURN_HEIGHT
        width = round(rgba.shape[1] * height / rgba.shape[0])
        cv2.imwrite(str(folder / f"{degrees:03d}.png"), cv2.resize(rgba, (width, height), interpolation=cv2.INTER_AREA))
    print(f"  wrote {folder}/")


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
    for name, step, toward, up, make in jobs:
        print(name, flush=True)
        depth, ids = render(cad / step, toward, up)
        path = OUT / f"{name}.png"
        cv2.imwrite(str(path), make(depth, ids))
        print(f"  wrote {path.relative_to(ROOT)}")
    print("headset-turn", flush=True)
    headset_turn(cad)


if __name__ == "__main__":
    main()
