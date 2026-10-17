# The test recordings

`tests/data/` holds 10 recordings of real motion from a Steam Frame. Each one is every headset and controller pose SteamVR reported, about once a millisecond. The detector tests replay them on every change, so a change that misses real knocks, or finds knocks that aren't there, fails the build.

## What's in each recording

Knocks on controllers, desks, and the headset:

| Recording | Length | What happens |
| --- | --- | --- |
| `together` | 45 s | Controllers knocked together in pairs, about 20 pairs |
| `surface` | 45 s | One controller knocked on a table in pairs, about 20 pairs |
| `headset` | 45 s | A controller knocked on the headset in pairs, about 20 pairs |
| `fast` | 30 s | Quick together doubles, 89 to 168 ms between knocks |

Knocks on a leg:

| Recording | Length | What happens |
| --- | --- | --- |
| `leg` | 70 s | Doubles and triples knocked on a leg with each hand, headset worn |
| `leg-doubles` | 106 s | About 40 doubles on a leg, a few seconds apart, mostly with the right hand |

Everyday movement, where nothing should happen:

| Recording | Length | What happens |
| --- | --- | --- |
| `play` | 45 s | Normal play |
| `noise` | 45 s | Clapping runs and waving, made to fool the detector |
| `straps` | 21 s | Fitting the controller straps in Into the Radius 2, with 19 bumps in 5 s |

A known routine:

| Recording | Length | What happens |
| --- | --- | --- |
| `mixed` | 60 s | 4 doubles of each kind, then 4 triples of each kind, 24 sets in all |

The `straps` clip comes from a 40 minute session. The other 18.7 minutes of play in it gave no actions.

## What the tests check

`tests/detector_test.cpp` replays each recording at the default thresholds. Exact knock counts are unknown for most recordings, so each test sets a floor a little under what the detector finds:

| Recording | The test needs | Found today |
| --- | --- | --- |
| `together`, `surface`, `headset` | At least 18, 16, and 15 doubles of that kind | 20, 18, and 22 |
| `fast` | At least 12 together doubles, nothing else | 13 |
| `leg`, `leg-doubles` | At least 14 and 23 surface actions, nothing else | 18 and 25 |
| `play`, `noise`, `straps` | No actions | None |
| `mixed` | At least 20 of 24 right, none wrong | 23 right, 0 wrong |

A wrong kind still switches passthrough, so the knock files allow a few. `surface` gives 1 together double, and `headset` gives 2 surface doubles.

The same file also tests calibration on the recordings. [How calibration works](calibration.md) has those results.

## Running the tests

On any Linux machine with CMake and a C++17 compiler:

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build
```

To try a threshold against every recording without changing code, set it through `KNOCKTHRU_TEST_SET`. Add `KNOCKTHRU_TEST_VERBOSE=1` to print every knock and action:

```sh
KNOCKTHRU_TEST_SET="confirmed_peak=30" build/bin/detector_test
```

## Replaying a bug report

When knocks go wrong, the bug report flow has a **Record knocks** button. It records 20 s of motion while the person knocks the way that fails, and adds it to `knockthru-report.zip` as `knockthru-knocks.csv.gz`. Unzip the report and replay the recording like this:

```sh
build/bin/detector_test knockthru-knocks.csv.gz
```

The report includes the reporter's `calibration.json`. To replay with their thresholds, pass them through `KNOCKTHRU_TEST_SET`. Surface knocks use `controller_peak` and `min_approach_surface`, together knocks `confirmed_peak` and `min_approach_together`, and headset knocks `headset_knock_peak` and `min_approach_headset`.

## Making a new recording

On the Frame, in Konsole or over SSH:

```sh
<install folder>/knockthru --record my-knocks.csv --seconds 45
gzip my-knocks.csv
```

`knockthru --detect` prints knocks live without acting on them. Add `--save FILE` to keep the poses from that session too.

Each row of a recording is one device at one moment: the time in seconds, the device (`hmd`, `left`, or `right`), whether the pose was valid, then position, velocity, and angular velocity, each as x, y, and z.
