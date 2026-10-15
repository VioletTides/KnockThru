# <img src="images/logo-wordmark.svg" alt="KnockThru" width="400">

Knock twice to turn Steam Frame passthrough (Room View) on or off. Knock three times to switch Room View between translucent and opaque.

[![Install with FrameDrop](images/framedrop-button.svg)](https://framedropvr.com/install?manifest=https%3A%2F%2Fgithub.com%2FVioletTides%2FKnockThru%2Freleases%2Flatest%2Fdownload%2Fknockthru.framedrop.json)

## Setup

1. On the Frame, turn on Developer Mode, then turn on CEF Remote Debugging in the Developer settings. KnockThru clicks the Room View button through this debug port.
2. Install with the FrameDrop button above.
3. Launch **KnockThru** once from **Library → Non-Steam**. This registers it with SteamVR and turns on autostart.

From then on, KnockThru starts with SteamVR.

## Knocks

| Knock | What counts |
| --- | --- |
| Together | The two controllers hit each other |
| Surface | One controller hits a table, wall, or other solid surface |
| Headset | A tap on the headset |

- Two knocks of the same kind: Room View on or off. Both controllers buzz once.
- Three knocks: translucent or opaque. Both controllers buzz twice.
- Four or more knocks, or knocks of mixed kinds, do nothing.
- Three buzzes mean the action failed. The reason is in `~/.config/knockthru/application.log`.

Knock at a steady pace, with a short pause before you start. A double fires about 0.4 s after the second knock, because KnockThru waits to see if a third knock follows.

## Settings

Put your settings in `~/.config/knockthru/config.json`. This file survives updates, and KnockThru reloads it within 2 s of a change.

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

## Calibration

If soft knocks do not register, calibrate that knock kind over SSH:

```sh
<install folder>/knockthru --calibrate together
```

Knock 10 times within 90 s. Each knock KnockThru hears gives one buzz. The new threshold goes to `~/.config/knockthru/calibration.json`, and the running app picks it up within 2 s.

## Commands

Run these on the Frame over SSH.

| Command | What it does |
| --- | --- |
| `knockthru` or `--install` | Register with SteamVR, turn on autostart, and start |
| `--uninstall` | Stop and remove from SteamVR. Keeps `~/.config/knockthru` |
| `--status` | Show registration, the running process, and Room View state |
| `--toggle`, `--style` | Turn Room View on or off, or switch its style, once |
| `--detect` | Print knocks live without acting on them |

## Manual installation

Copy the Linux ARM64 ZIP from the releases page to the Frame, then run:

```sh
unzip -oq knockthru-0.1.0-linux-arm64.zip -d ~/KnockThru
~/KnockThru/knockthru --install
```

## Build

Host build and detector tests (any Linux with CMake and a C++17 compiler):

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build
```

Steam Frame build (needs Zig 0.16.0) and release ZIP (needs Node.js):

```sh
scripts/build-arm64.sh
node scripts/package.mjs --build-dir build-arm64 --version 0.1.0
```

`docs/knock-signal.md` explains how knock detection works and how it was tuned.

## Credits

- [KominoVR/frame-passthrough-shortcuts](https://github.com/KominoVR/frame-passthrough-shortcuts) (MIT): app scaffolding, style switch, and packaging.
- [toorux/steam-frame-passthrough-toggle](https://github.com/toorux/steam-frame-passthrough-toggle) (MIT): the Room View button click through the debug port.

Room View control uses Steam's internal dashboard UI. A Steam update can break it.

[MIT licensed](LICENSE). Developed with help from coding agents.
