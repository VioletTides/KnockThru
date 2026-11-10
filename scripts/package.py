#!/usr/bin/env python3
"""Builds the release ZIP, its .sha256 file, and the FrameDrop manifest. Never runs the binary.

    python3 scripts/package.py --build-dir build-arm64 --version 1.0.0 [--output-dir dist]
        [--base-url https://github.com/OWNER/REPO/releases/download/v1.0.0/]

The archive layout comes from KominoVR/frame-passthrough-shortcuts (MIT).
"""
import argparse
import hashlib
import json
import re
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
EXTRAS = ["README.md", "LICENSE", "third_party/openvr/LICENSE", "third_party/json/LICENSE.MIT",
          "third_party/vulkan/MIT.txt", "THIRD_PARTY_NOTICES.md", "images/framedrop-button.svg"]


def files_under(folder):
    return sorted(path for path in folder.rglob("*") if path.is_file())


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--build-dir", type=Path, default=ROOT / "build-arm64")
    parser.add_argument("--version", required=True)
    parser.add_argument("--output-dir", type=Path, default=ROOT / "dist")
    parser.add_argument("--base-url", help="the https folder the release files download from")
    args = parser.parse_args()
    if not re.fullmatch(r"\d+\.\d+\.\d+(-[0-9A-Za-z.-]+)?", args.version):
        parser.error("--version must look like 1.0.0 or 1.0.0-rc1")
    if args.base_url and not args.base_url.startswith("https://"):
        parser.error("--base-url must start with https://, because the app installs from nothing else")

    binaries = args.build_dir / "bin"
    # The app refuses an update whose knockthru-ctl is not a copy of knockthru.
    if (binaries / "knockthru").read_bytes() != (binaries / "knockthru-ctl").read_bytes():
        parser.error("knockthru-ctl differs from knockthru. Run scripts/build-arm64.sh again")

    assets = ROOT / "assets"
    licenses = ROOT / "third_party/toolchain-licenses"
    entries = [(name, binaries / name, 0o755) for name in ("knockthru", "knockthru-ctl")]
    entries += [(path.relative_to(assets).as_posix(), path, 0o644) for path in files_under(assets)]
    entries += [(name, ROOT / name, 0o644) for name in EXTRAS]
    entries += [(path.relative_to(ROOT).as_posix(), path, 0o644) for path in files_under(licenses)]

    name = f"knockthru-{args.version}-linux-arm64.zip"
    args.output_dir.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(args.output_dir / name, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
        for entry, path, mode in entries:
            # A fixed date, so the same files always give the same ZIP.
            info = zipfile.ZipInfo(entry, (1980, 1, 1, 0, 0, 0))
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = (0o100000 | mode) << 16
            archive.writestr(info, path.read_bytes())

    sha256 = hashlib.sha256((args.output_dir / name).read_bytes()).hexdigest()
    (args.output_dir / f"{name}.sha256").write_text(f"{sha256}  {name}\n")
    if args.base_url:
        manifest = {"schema": "framedrop.install/v1", "name": "KnockThru",
                    "files": [{"url": args.base_url.rstrip("/") + "/" + name, "sha256": sha256}]}
        (args.output_dir / "knockthru.framedrop.json").write_text(json.dumps(manifest, indent=2) + "\n")
    print(f"{args.output_dir / name}\nsha256 {sha256}")


if __name__ == "__main__":
    main()
