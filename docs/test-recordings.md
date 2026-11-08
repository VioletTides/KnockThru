# The test recordings

`tests/data/` holds 10 recordings of real motion from a Steam Frame: every headset and controller pose SteamVR reported, about once a millisecond. The tests replay all of them whenever the code changes, so a change that misses real knocks, or hears knocks that aren't there, fails the build.

## What's in each recording

| Recording | Length | What happens |
| --- | --- | --- |
| `together` | 45 s | About 20 doubles, knocking the controllers together |
| `surface` | 45 s | About 20 doubles, knocking one controller on a table |
| `headset` | 45 s | About 20 doubles, knocking a controller on the headset |
| `fast` | 30 s | Quick together doubles, as little as 89 ms between knocks |
| `leg` | 70 s | Doubles and triples knocked on a leg with each hand |
| `leg-doubles` | 106 s | About 40 doubles on a leg, mostly with the right hand |
| `play` | 45 s | Normal play |
| `noise` | 45 s | Clapping and waving, trying to fool the detector |
| `straps` | 21 s | Fitting the controller straps, which bumps them together 19 times |
| `mixed` | 60 s | 4 doubles of each kind, then 4 triples of each kind, 24 sets in all |

## What the tests check

Each test sets a minimum, a little under what the detector finds now:

| Recording | The test needs |
| --- | --- |
| `together`, `surface`, `headset` | At least 18, 16, and 15 doubles of that kind |
| `fast` | At least 12 together doubles, and nothing else |
| `leg`, `leg-doubles` | At least 14 and 23 surface actions, and nothing else |
| `play`, `noise`, `straps` | No actions at all |
| `mixed` | At least 20 of the 24 sets right, and none wrong |

The first three allow a few doubles of the wrong kind, since those still switch passthrough. The same tests also run calibration on the recordings.

## Running the tests

On any Linux machine with CMake and a C++17 compiler:

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build
```

To try a different threshold against every recording without changing any code, set it through `KNOCKTHRU_TEST_SET`. Add `KNOCKTHRU_TEST_VERBOSE=1` to print every knock and action:

```sh
KNOCKTHRU_TEST_SET="confirmed_peak=30" build/bin/detector_test
```

## Replaying a bug report

When knocks go wrong, the bug report has a **Record knocks** button. It records 20 s of motion while the person knocks the way that fails, and adds it to `knockthru-report.zip` as `knockthru-knocks.csv.gz`. Unzip the report and replay it:

```sh
build/bin/detector_test knockthru-knocks.csv.gz
```

The report also has the person's `calibration.json`. To replay with their thresholds, pass them through `KNOCKTHRU_TEST_SET`. Surface knocks use `controller_peak` and `min_approach_surface`, together knocks use `confirmed_peak` and `min_approach_together`, and headset knocks use `headset_knock_peak` and `min_approach_headset`.

## Making a new recording

On the Frame, in Konsole or over SSH:

```sh
<install folder>/knockthru --detect --save my-knocks.csv --seconds 45
gzip my-knocks.csv
```

`--detect` prints each knock it hears without acting on it, and `--save` writes the motion to the file. Each row is one device at one moment: the time in seconds, the device (`hmd`, `left`, or `right`), whether the pose was valid, then position, velocity, and angular velocity, each as x, y, and z.
