# Settings

Put your settings in `~/.config/knockthru/config.json`. This file survives updates, and KnockThru reloads it within 2 s of a change. The dashboard tab writes to this file too.

```json
{
  "knock_types": ["together", "surface"],
  "blocked_apps": ["steam.app.620980"]
}
```

- `knock_types`: which knock kinds count. The default is all three.
- `blocked_apps`: SteamVR app keys where knocks do nothing. Run `knockthru --current-app` while a game runs to get its key.
- `debug_port`: the CEF Remote Debugging port. The default is 8080.
- `classic_bar_icon`: `true` shows the dashboard bar icon in the logo colors. The default `false` shows the logo in grey, and white on hover, like Steam's own icons.
- `check_updates`: `false` stops the daily update check. By default, KnockThru asks GitHub for the latest release once a day, a minute after it starts. **Check for updates** in Settings works either way.
- `sounds`: `false` silences the click. By default, KnockThru plays SteamVR's dashboard click when a knock switches the style or the performance graph, and on panel buttons. Steam already clicks when passthrough changes.
- `knock_actions`: what each kind of knock does twice (`"2"`) and thrice (`"3"`): `passthrough`, `style`, `perf_graph`, or `none`. Kinds and counts you leave out keep their default. **Knock actions** in Settings writes this key, and sets every kind the same. For example, this makes knocking thrice on the headset show and hide the performance graph:

  ```json
  {
    "knock_actions": { "headset": { "3": "perf_graph" } }
  }
  ```
- `paused`: `true` while the **KnockThru On** switch is off. Knocks do nothing then.
- `detector`: overrides for the knock detector's settings, such as `confirmed_peak` or `max_gap_s`. `src/knock_detector.hpp` gives each one with its unit. Calibration sets the peaks for you.
- `tour_done`: `true` once the first-run tour is finished or skipped. Set it to `false` to see the tour the next time you open the KnockThru tab. Settings also has a Replay button.

Calibration writes `~/.config/knockthru/calibration.json`, with one entry per calibrated kind. **Factory reset** in Settings deletes it along with `config.json`.
