#!/usr/bin/env python3
"""Prints the update rate and the largest acceleration spikes in a knockthru --record CSV."""
import csv
import math
import sys
from collections import defaultdict


def main(path: str, top: int = 20) -> None:
    rows = defaultdict(list)
    with open(path, newline="") as handle:
        for row in csv.DictReader(handle):
            if row["valid"] == "1":
                rows[row["device"]].append(row)

    for device, samples in rows.items():
        fresh, spikes = [], []
        last = None
        for row in samples:
            velocity = tuple(float(row[k]) for k in ("vx", "vy", "vz"))
            if last is None or velocity != last[1]:
                t = float(row["t_s"])
                if last is not None and t > last[0]:
                    dv = math.dist(velocity, last[1])
                    spikes.append((dv / (t - last[0]), t))
                fresh.append(t)
                last = (t, velocity)
        if len(fresh) < 2:
            print(f"{device}: not enough valid samples")
            continue
        gaps = sorted(b - a for a, b in zip(fresh, fresh[1:]))
        span = fresh[-1] - fresh[0]
        print(f"{device}: {len(fresh)} fresh samples, {len(fresh) / span:.0f} Hz, "
              f"median gap {gaps[len(gaps) // 2] * 1000:.2f} ms")
        print("  top spikes (m/s^2 at time s):")
        for accel, t in sorted(spikes, reverse=True)[:top]:
            print(f"    {accel:9.1f}  {t:9.3f}")


if __name__ == "__main__":
    if len(sys.argv) < 2:
        sys.exit("Usage: analyze.py <recording.csv> [top]")
    main(sys.argv[1], int(sys.argv[2]) if len(sys.argv) > 2 else 20)
