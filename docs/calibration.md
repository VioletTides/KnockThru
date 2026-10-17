# How calibration works

Everyone knocks a little differently. Some people knock hard, some barely touch. The default thresholds catch most knocks, but soft ones can slip under them. Calibration measures 10 of your knocks and sets the thresholds for that kind of knock to fit you.

The code is in `src/calibrator.cpp`. The saved result goes to `~/.config/knockthru/calibration.json`.

## The walkthrough

![Calibrating a surface knock: pick the kind, read how to knock, knock 10 times while the bars fill, then knock twice to try it and save](../images/calibrate.gif)

1. **Pick** the kind of knock to calibrate: together, surface, or headset.
2. **Learn** how to knock it, then press Start.
3. **Record** 10 single knocks within 90 s. A ring fills before each one. One buzz means the knock counted.
4. **Try** the new thresholds with a double knock. Save unlocks only if the try works. The try never switches passthrough.

The first 3 s after Start don't count, because the hand coming down from pressing Start once counted as a knock. After each counted knock, the next one has to wait 1 s, which is how long the ring takes to fill.

You can also calibrate from a terminal on the Frame with `knockthru --calibrate together` (or `surface`, or `headset`).

## Catching soft knocks without catching everything else

To hear soft knocks, calibration lowers the spike thresholds while it records. Surface knocks need 30 m/s² instead of 60, and together and headset knocks need 12 instead of 40.

At those thresholds, almost anything looks like a knock. The `play` and `noise` recordings gave 84 and 82 knock candidates in 45 s each, about one a second. Two more problems showed up during a real surface calibration on the Frame:

- Starting a hard swing from rest spiked at 37 m/s², 0.09 to 0.14 s before the real knock. That early spike took the count instead of the knock, which spiked at 146 and 461.
- A slow hand movement counted as a knock.

So calibration is pickier about which knocks count. Knocks less than 0.25 s apart form a burst, and the burst is judged by its knock with the fastest approach. A burst counts when:

1. Nothing else happened in the 0.6 s before it. Waving makes clusters of spikes, and people wait between calibration knocks.
2. Its knock approached at 0.25 m/s or faster. Approach speed is the controller's fastest speed in the 100 ms before the spike.
3. For surface knocks, the controller was nearly still beforehand. It must have averaged under 0.6 m/s from 0.6 to 0.15 s before the knock. Real surface knocks averaged 0.02 to 0.51 m/s there, and false ones in `play` and `noise` mostly 0.64 to 2.8 m/s.
4. Its spike reaches the floor for its kind (see below). A knock under the floor could never trigger anything, so the walkthrough says "too light" instead of counting it.

Together and headset knocks skip the stillness check. Their false knocks never got past the first two rules, and a headset knock starts with your arm rising.

With these rules, calibrating on the 90 s of `play` and `noise` counts no knocks at all. Calibrating on the `together`, `surface`, and `headset` recordings counts 10 of 10. In the Frame session above, the hard knocks count and the swing starts and slow movement don't.

## Why approach speed matters

The spike is a noisy measure of how hard you knocked. It divides a velocity change by the time between two controller updates, and that time varies from 2 to 7 ms. In the knock recordings, the hardest tenth of spikes ran 2.8 to 8.3 times the softest tenth.

Approach speed is steadier. The fastest tenth of approaches ran only 1.7 to 2.5 times the slowest tenth. Slow movement also falls well under it. That's why calibration sets an approach threshold as well as a spike threshold.

## Turning 10 knocks into thresholds

Each kind of knock gets two thresholds:

- **Spike threshold:** 60 percent of your weakest recorded spike. It never goes below the floor for that kind, and never above 150 m/s².
- **Approach threshold:** 55 percent of your second slowest approach. The second slowest, not the slowest, so one odd knock can't drag it down.

The floors come from replaying the recordings:

| Kind | Floor | What goes wrong below it |
| --- | --- | --- |
| Surface | 50 m/s² | Replays split real surface doubles apart |
| Together | 30 m/s² | The strap fitting bumps in the `straps` recording form a double |
| Headset | 20 m/s² | Soft rebounds slip past the 105 ms gap and turn doubles into triples |

A calibration file saved under an older, lower floor loads with the current one.

To use the approach threshold, a set of knocks needs at least one knock that reaches it. Two other ways failed on the recordings:

- Dropping slow knocks before grouping them let leftover spikes form false doubles in `play` and `noise`. Slow knocks still join their set, so clapping runs stay rejected.
- Making every knock in a set reach the threshold lost most together doubles, because the second knock of a quick double starts from the rebound and comes in slow.

### The slider that got cut

An earlier version had a sensitivity slider from 30 to 80 percent, in place of the fixed 55. In the replays, anything stricter than 55 percent only lost real doubles. 30 percent matched 55 on surface and headset knocks and did a little better on together knocks, but it leaves less room for movement the recordings don't have. The slider didn't earn its place, so the walkthrough uses 55 percent.

## How well it works

The tests calibrate each kind on its own recording, apply the result, and replay every recording. These are the results today:

| Kind | Spike threshold | Approach threshold | Doubles found | `mixed` right, of 24 |
| --- | --- | --- | --- | --- |
| Together | 31.2 m/s² | 0.24 m/s | 19 | 22 |
| Surface | 55.6 m/s² | 0.68 m/s | 18 | 23 |
| Headset | 20 m/s² | 0.20 m/s | 22 | 23 |

Every row gives no actions in `play` and `noise`, and no wrong actions in `mixed`. With the default thresholds, the same recordings give 20 together, 18 surface, and 22 headset doubles, and 23 of 24 right in `mixed`.
