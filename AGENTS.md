# KnockThru

By default, a double knock toggles Steam Frame passthrough (Room View), and a triple knock switches Room View between translucent and opaque. `knock_actions` in `config.json` can give each kind of knock other actions (`src/knock_actions.*`). Knock actions in Settings sets every kind the same. The app runs on the Frame itself as a SteamVR overlay app.

Read `docs/README.md` before you change knock detection, calibration, or how the app switches passthrough. It links one page for each.

## Commands

- Host build and tests: `cmake -S . -B build && cmake --build build && ctest --test-dir build`
- Frame build: `scripts/build-arm64.sh` (Zig 0.16.0), output in `build-arm64/bin/`
- Release ZIP: `node scripts/package.mjs --build-dir build-arm64 --version X.Y.Z`
- Detector tuning sweep: `KNOCKTHRU_TEST_SET="confirmed_peak=30" build/bin/detector_test`. Add `KNOCKTHRU_TEST_VERBOSE=1` to print every knock and event.
- Replay a recording from a bug report: `build/bin/detector_test knockthru-knocks.csv.gz`
- Battery use on the Frame: `scripts/measure.sh 60` prints each thread's CPU and wakeups per second over 60 s.
- Panel frame time: `build/bin/panel_preview --bench`. Cross-compile it with Zig to time it on the Frame.
- Slower pose rate trial: `KNOCKTHRU_TEST_DECIMATE=2 build/bin/detector_test` keeps every second sample.

CI runs the host build, `ctest`, and the Frame build on every pull request.

## Rules

- Keep `src/knock_detector.*` free of OpenVR types. The host tests replay the recordings in `tests/data/` through it.
- After any detector change, `ctest` must pass: no actions in `play`, `noise`, or `straps`, and no wrong actions in `mixed`. If you change a threshold, update the docs page that explains it.
- Read poses in `vr::TrackingUniverseStanding`. Poses in the raw universe are wrong on the Frame.
- `knockthru` and `knockthru-ctl` ship as byte-identical copies. SteamVR identifies apps by executable path, so management commands re-exec through `knockthru-ctl` to keep the resident app's identity.
- Call every gesture a "knock" in code, docs, and UI text.
