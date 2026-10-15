# Knock signal findings

Source: five 45 second recordings in `tests/data/`, made with `knockthru --record` on a Steam Frame on 2026-10-14. In the three knock files, the wearer knocked in pairs for the full 45 seconds. Exact counts are unknown, and the estimate is about 20 pairs per file.

## What the Frame provides

- `GetDeviceToAbsoluteTrackingPose` returns valid poses only in `TrackingUniverseStanding`. `TrackingUniverseRawAndUncalibrated` returns `bPoseIsValid = false` for every device.
- Headset velocity updates at 1000 Hz. Each controller updates at about 233 Hz (one sample every 4.3 ms).
- The Frame has no `/dev/hidraw*` devices, so raw controller accelerometer data is not reachable that way.
- Acceleration below is the velocity difference between consecutive changed samples, divided by their time gap.

## What separates a knock from motion

Spike size alone does not work. Normal play reached 697 m/s², above every recorded knock.

| Feature | Knocks | Play and waving |
|---|---|---|
| Speed 30 ms after the spike, median | 0.1 to 0.2 m/s | 1.6 to 1.7 m/s |
| Speed 30 ms before the spike, median | 0.5 to 1.1 m/s | 2.3 to 2.5 m/s |
| Spike width at 233 Hz | 1 to 3 samples | 1 to 2 samples |

The controller nearly stops after a knock. During motion it keeps moving. Spike width does not separate them at this sample rate.

Kinds:

- `headset`: the headset spikes above 25 m/s² within 30 ms of a controller spike. Headset taps reach 40 to 215 m/s². Head shaking reaches 22 m/s², and controller knocks move the headset less than 10 m/s².
- `together`: the other controller spikes above 20 m/s² within 40 ms, with the controllers less than 15 cm apart. The second controller often spikes much weaker than the first.
- `surface`: anything else. Both knocks of a double must land within 3 cm of each other, because the table does not move.

## Prototype rule and results

A controller spike counts as a knock when all of these hold:

- Peak above 60 m/s².
- Speed 30 ms after the peak below 0.4 m/s.
- Speed 30 ms before the peak below 1.8 m/s.

Spikes less than 80 ms apart merge into one knock, which absorbs bounces. A sequence is knocks of the same kind and the same device, 120 to 450 ms apart. A sequence of 2 is a double, 3 is a triple, and 4 or more is ignored (steady clapping).

Prototype results:

| Recording | Doubles found | Wrong kind | False doubles |
|---|---|---|---|
| `together` | 20 | 0 | |
| `surface` | 18 | 1 (as `together`) | |
| `headset` | 18 | 2 (as `surface`) | |
| `play` | | | 0 |
| `noise` | | | 0 |

A double with the wrong kind still toggles passthrough, unless that kind is turned off in `knock_types`.

The C++ detector (`src/knock_detector.cpp`) adds two rules. Without them, one false double appeared at 18.9 s in `noise`, from two weak claps (peaks 63 and 60 m/s²) 500 ms after a 3 second clapping run.

- Every knock within 450 ms of the previous one joins the same burst. A burst that mixes kinds, sources, or surface spots is rejected as a whole. It does not restart.
- A burst must start at least 0.8 s after the previous burst ended.

## Live session tuning

Two 60 second live sessions with `knockthru --detect` drove these changes. The first one was not saved. It led to two changes:

- The 1 s cooldown after an action is gone. It swallowed the first knock of the next sequence. The 0.8 s stillness rule already keeps sequences apart.
- Surface knocks less than 150 ms apart count as one knock. A table bounce landed 120 ms after a real knock, while real knocks were about 215 ms apart. Together and headset knocks use a shorter gap (see "Fast doubles" below).

The second session is `tests/data/mixed.csv.gz`, with a known routine: 4 each of together, surface, and headset doubles, then 4 of each as triples. The build at that point got 15 of 24 right and read 4 triples as doubles. These changes fixed that:

- A together or headset knock needs only 40 m/s², because the other controller or the headset confirms it. Weak third knocks peaked at 42 to 48 m/s².
- A surface knock may still move at 0.55 m/s 30 ms later, because hard table knocks rebound.
- Knocks in a sequence must be at most 350 ms apart (was 450). Real gaps were 170 to 290 ms. The wider window let two weak spikes 357 ms apart in `play` form a false double.

Result: 22 of 24 right, 0 wrong. The 2 misses are surface doubles where hard hits bounced several times.

## Fast doubles

`tests/data/fast.csv.gz` is 30 s of fast together doubles. Its knock pairs were 89 to 168 ms apart, so the 150 ms gap dropped half of them. Rebounds in the other recordings land 83 to 99 ms after a real knock. Peak ratio (0.15 to 0.99 for real pairs, 0.39 for a rebound) and controller separation (1.3 to 3.6 cm for real pairs, 1.4 cm for a rebound) do not tell them apart.

- Together and headset knocks need only 105 ms between them (`fast_gap_s`). Surface knocks keep 150 ms.
- At the default thresholds, `fast` gives 13 doubles (was 7), and every other recording keeps its result.
- With together or headset thresholds under 20 m/s², soft rebounds pass the 105 ms gap and turn doubles into triples. Calibration never sets them below 20.

Background on tap detection: [ST design tip DT0101](https://www.st.com/resource/en/design_tip/dt0101-setting-up-singletap-and-doubletap-recognition-with-sts-mems-accelerometers-stmicroelectronics.pdf) and [NXP AN3919](https://www.nxp.com/docs/en/application-note/AN3919.pdf) describe threshold, pulse width, quiet time, and latency windows for double taps.
