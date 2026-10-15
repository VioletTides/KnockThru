# Calibration redo plan

Branch `calibration-redo`, worktree `../KnockThru-calibration`. The UI comes first. The logic changes come second and stay small: sensitivity and threshold handling.

## What exists now

- `src/calibrator.*` collects 10 knocks of one kind within 90 s with lowered thresholds. It saves `0.6 * weakest peak` to `calibration.json`, clamped to 50..150 m/s² for surface and 20..150 for together and headset.
- `src/config.cpp` `apply_calibration` maps the saved values onto the detector. Together and headset share `confirmed_peak`, so the softer of the two wins.
- `src/panel.cpp` `calibrate_screen` has two states. One is a list of three kinds with a Start button each. The other is a live screen with a "3 of 10" counter, ten dots, a seconds countdown, and Cancel. A "Saved" banner follows.
- Calibration counts single knocks, not doubles or triples. `Calibrator::add` takes every knock from `take_knocks()`, which logs any knock more than `merge_s` (80 ms) after the last one. The current screen says "10 times, at least 1 second apart" but never says "one knock at a time", so it reads as if it wants doubles.
- The panel is static. `Panel::render` takes no time input. `DashboardPanel::draw` in `src/main.cpp` redraws at most every 50 ms when dirty and every 500 ms otherwise.
- `tests/panel_preview.cpp` writes PNGs of sample states. Nothing previews motion.

## Phase 1: UI

### Prototype on the readme branch

Branch `readme-revamp` (worktree `../KnockThru-readme`, local only, still in progress) already scripts the knock animations in Python for the README GIFs. The panel animations port from it, so the motion matches the README.

- `scripts/readme_media.py` has the easing curves (`ease_in`, `ease_out`, `ease_in_out`, `ease_out_back`) and a `keyframes` interpolator.
- The same file has the knock motion: `knock_motion` (pull back, hit, rebound, settle), `impact` and `burst` (accent lines flying out from the contact), `buzz` (a shake per haptic buzz), and `knock_pair` (two controllers swinging from the wrist). One timeline drives them: first knock at 0.5 s, 0.3 s between knocks, 0.4 s to the action.
- `knock_kinds()` in that file draws one card per knock kind, which is the layout the "Pick a kind" step needs.
- The README loops play a double, then a triple. Calibration wants one knock, then a rest. `knock_motion` already takes the knock count, so the calibration loop calls it with one knock and a longer pause. Only the "Try it" step shows a double.
- `scripts/cad_sprites.py` renders the controller and headset sprites in `images/cad/` from Valve's CAD files, in the logo style.
- The CAD-derived drawings use CC BY-NC-SA 4.0, like the logo. `THIRD_PARTY_NOTICES.md` on that branch says so. Any sprite shipped in the app needs the same notice.

`Canvas` cannot load PNGs or draw rotated images. It can fill polygons, and `logo_shapes.hpp` already bakes the logo into polygons. Tracing the sprites into a polygon header the same way lets the panel scale and rotate them by transforming points. Loading PNGs would instead need a vendored image decoder and a rotated blit.

### Animation groundwork

`calibration-redo` never merges `readme-revamp` and never copies its files. The README work stays unpublished until it is ready on its own. The readme worktree is a read-only reference.

Release order: `calibration-redo` merges and ships first, with the traced sprites in the panel. `readme-revamp` follows and adds README sections for calibration. `panel_preview` frames from this branch can feed those sections.

1. Port the easing curves, `keyframes`, and the knock motion to C++ by hand, beside the canvas code. `scripts/readme_media.py` in the readme worktree stays the reference for timing.
2. Write a tracer that reads the sprite PNGs from a path given on the command line, such as `../KnockThru-readme/images/cad/`. It writes a polygon header, as `logo_shapes.hpp` does for the logo. Only the header lands on `calibration-redo`.
3. Add the new header to the "Logo" section of `THIRD_PARTY_NOTICES.md`, which already covers CAD-derived drawings under CC BY-NC-SA 4.0.

Then the render loop:

1. Pass the current time into `Panel::render`.
2. Add an "animating" flag that the panel reports back. While it is set, `DashboardPanel::draw` redraws at the 50 ms rate even without input.
3. Log how long one `render` plus texture upload takes on the Frame. The redraw rate depends on that number, which nobody has measured yet.
4. Extend `panel_preview` to write a frame sequence for each animated state, so motion can be reviewed on the host.

### Wizard steps

1. **Pick a kind:** three cards, each with a short looping animation of that knock, ported from `knock_kinds()`.
2. **Show how:** one screen per kind with the single-knock animation large. The instruction says "one knock, then wait" in plain words. Ask for a normal knock, not the softest one.
3. **Record:** a pacing cue (a ring that fills over about a second) shows when the next knock can come. Each counted knock pops a dot and pulses a ring. A live strength bar shows each knock's peak against the current threshold. A knock of the wrong kind gets a hint ("That looked like a surface knock") instead of silence.
4. **Review:** plot the recorded peaks with the threshold line. The sensitivity slider sits under the plot, and the line moves as the slider moves.
5. **Try it:** this step is required before saving. The user does one double knock at the chosen threshold. The panel shows whether it would have toggled passthrough, without toggling it. A pass unlocks Save. A miss offers to raise sensitivity or redo the recording.

### Sensitivity slider

- The slider is continuous, with notches at minimum, middle, and maximum.
- Releasing near a notch snaps the knob onto it, with an eased slide.
- Dragging works through the overlay's mouse events. `DashboardPanel` currently turns only clicks into panel commands, so it needs press, move, and release.

Cancel and Back stay on every step. Leaving the panel mid-recording keeps the current behavior: the recording continues until done or timed out.

### UI acceptance

- Every step renders in `panel_preview`, including at least one mid-animation frame per step.
- On the Frame, a full calibration of each kind works from the panel alone.

### Checkpoint, 2026-10-15

Phase 1 runs on the Frame, and Robin checked it in the headset. Testing it turned up problems beyond the UI, fixed on this branch:

- Pose sampling runs on its own thread, so panel drawing cannot stretch the gaps between samples.
- Calibration counts only deliberate knocks, as `docs/knock-signal.md` describes under "Calibration knocks".
- The record bars show approach speed, because the top tenth of spikes ran 2.8 to 8.3 times the bottom tenth in the knock recordings.
- Panel frames take about 1.5 ms on the host, and screen fades about 3.7 ms.

The review chart, the threshold, and the slider still use the spike size. The app logs every knock heard while calibrating, for tuning phase 2.

## Phase 2: logic

1. Replace "60 percent of the weakest knock" with a sensitivity value from 0 to 1. It scales a low percentile of the recorded peaks, not the single minimum. A rebound lands 83 to 99 ms after a knock (`docs/knock-signal.md`), past the 80 ms `merge_s`, so it can count as a weak extra knock and drag the minimum down. The test data sets the fractions at 0, 0.5, and 1.
2. Store the recorded peaks and the sensitivity in `calibration.json`, so the slider can move the threshold later without a new recording.
3. Give together and headset knocks separate thresholds. Today `judge` in `src/knock_detector.cpp` checks both against `confirmed_peak`, and `apply_calibration` keeps the softer of the two. Add a headset threshold to `DetectorConfig`, keep `confirmed_peak` for together knocks, and keep the existing `config.json` keys working.
4. Let the user change sensitivity later without recording again.
5. Lower the knock count if the robust statistic allows it. The test data decides the number.

Later: a "Reset to defaults" action that deletes the saved thresholds.

The existing floors stay: surface never below 50 m/s², together and headset never below 20. `docs/knock-signal.md` explains why.

### Phase 2 status, 2026-10-15

Robin picked "drag the line" (option 1 of two mockups) for the sensitivity step, and asked for whatever testing showed best against movement. Done on this branch:

- The chart, the line, and the slider use approach speed. Each kind saves a spike threshold and an approach threshold, plus the slider position and the recorded knocks.
- Together and headset knocks have separate thresholds.
- `docs/knock-signal.md` ("Calibrated thresholds") has the replay results at each notch.

Later the same day, testing showed the slider earned little: its middle and most sensitive ends gave nearly the same replay results, and its least sensitive end only lost real knocks. Robin chose to drop it. The walkthrough is four steps (pick, learn, record, try), and the approach threshold is fixed at 55 percent of the second slowest knock.

Not done: lowering the knock count.

### Logic acceptance

- `ctest` passes: no actions in `play` and `noise`, and no wrong actions in `mixed`.
- A host test calibrates each kind on its recording, applies the saved thresholds, and replays every recording.
- `mixed` still sorts together and headset knocks correctly with the split thresholds.
- `--calibrate` on the command line keeps working with the new logic.

## Decisions

- No sensitivity slider. The approach threshold is 55 percent of the second slowest recorded knock (see "Phase 2 status").
- The "Try it" step is required before saving.
- Together and headset knocks get separate thresholds.
- Recording opens with a 3 s get-ready ring, and each knock must wait for the 1 s pacing ring.
