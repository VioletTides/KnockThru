# KnockThru

A double knock toggles Steam Frame passthrough (Room View). A triple knock switches Room View between translucent and opaque. The app runs on the Frame as a SteamVR overlay app with no visible surface.

## Layout

- `src/knock_detector.*`: knock detection. Pure logic with no OpenVR types, so the host tests can replay recordings.
- `src/room_view.*`: clicks the Room View button through Steam's CEF debug port (8080 on the Frame).
- `src/style.*`: switches the SteamVR setting `camera.roomViewStyle` (3 is translucent, 4 is opaque).
- `src/config.*`: loads `assets/config.json`, then `~/.config/knockthru/config.json`, then `calibration.json`.
- `src/main.cpp`: the CLI commands and the resident app (`--run`).
- `tests/data/*.csv.gz`: pose recordings from the Frame. `docs/knock-signal.md` explains each one and how the detector was tuned.

## Commands

- Host build and tests: `cmake -S . -B build && cmake --build build && ctest --test-dir build`
- Frame build: `scripts/build-arm64.sh` (Zig 0.16.0), output in `build-arm64/bin/`
- Release ZIP and FrameDrop manifest: `node scripts/package.mjs --build-dir build-arm64 --version X.Y.Z`
- Tuning sweep: `KNOCKTHRU_TEST_SET="confirmed_peak=30" build/bin/detector_test`. Add `KNOCKTHRU_TEST_VERBOSE=1` to print every knock and event.

## Testing on the Frame

- `ssh frame` reaches the headset. The dev install lives in `~/knockthru-dev/`.
- To deploy, run `knockthru-ctl --uninstall` first, because a running binary cannot be overwritten. Then copy `knockthru` and `knockthru-ctl`, and run `./knockthru` to install and start.
- The app log is `~/.config/knockthru/application.log`.

## Rules

- `knockthru-ctl` must stay a byte-identical copy of `knockthru`. SteamVR identifies apps by executable path, so management commands re-exec through `knockthru-ctl` to keep the resident app's identity.
- Poses are valid only in the standing tracking universe, not the raw one.
- Any detector change must keep `ctest` passing: no actions in `play` and `noise`, and no wrong actions in `mixed`.
- A release goes out when a `v*` tag is pushed. Tag only with Robin's approval.
