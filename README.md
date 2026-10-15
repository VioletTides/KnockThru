# <img src="images/logo-wordmark.svg" alt="KnockThru" width="400">

Knock twice to see your room without taking off your Steam Frame. Knock three times to switch between seeing the room through your game and seeing only the room.

![Knocking twice turns passthrough on in the KnockThru panel, and knocking three times switches it to opaque](images/hero.gif)

[![Install with FrameDrop](images/framedrop-button.svg)](https://framedropvr.com/install?manifest=https%3A%2F%2Fgithub.com%2FVioletTides%2FKnockThru%2Freleases%2Flatest%2Fdownload%2Fknockthru.framedrop.json)

## Install

1. On the Frame, turn on **Developer Mode**. Then turn on **CEF Remote Debugging** in the Developer settings. KnockThru needs it to press the Room View button for you.
2. Click the **Install with FrameDrop** button above.
3. Open **Library → Non-Steam** and launch **KnockThru** once.

From now on KnockThru starts with SteamVR and runs in the background.

## How to knock

Any of these three knocks works. Use the same kind for every knock in a set.

![Three ways to knock: tap the controllers together, tap one controller on a table, or tap the headset](images/knock-kinds.gif)

Pause for a moment, then knock at a steady pace. After two knocks, KnockThru waits a moment in case a third one follows.

![Timeline: knock, knock, a wait of about 0.4 seconds, then Room View turns on and both controllers buzz](images/timing.gif)

| You knock | What happens | You feel |
| --- | --- | --- |
| Twice | Passthrough (Room View) turns on or off | One buzz |
| Three times | The room switches between see-through and solid | Two buzzes |
| Four or more times, or mixed kinds | Nothing, so steady clapping does not set it off | Nothing |

Three buzzes mean something went wrong. See [Something not working?](#something-not-working) below.

## The dashboard tab

Open the SteamVR dashboard and pick the **KnockThru** tab to do everything by hand.

![The KnockThru dashboard tab](images/dashboard.png)

- **Passthrough**: turns Room View on or off.
- **Style**: **Translucent** shows the room through your game. **Opaque** shows only the room.
- **Knocks**: turns knocking off for a while without quitting the app.
- **Count these knocks**: picks which of the three knock kinds count.
- **Knocks in** your game: turns knocks off in just the game you're playing, for games where you hit things on purpose.
- **Calibrate**: teaches KnockThru how hard you knock. Pick a knock kind, then knock 10 times, about 1 s apart.

## Something not working?

- **Soft knocks don't register.** Open the dashboard tab and press **Calibrate**.
- **Three buzzes after a knock.** KnockThru heard you but could not change Room View. The usual cause is that **CEF Remote Debugging** is off. Turn it back on in the Frame's Developer settings. The dashboard tab also shows the reason.
- **Knocks do nothing in one game.** Check the **Knocks in** switch for that game in the dashboard tab.

KnockThru presses the Room View button through Steam's own dashboard. A Steam update can break that until KnockThru gets an update too.

## For tinkerers

These need SSH access to the Frame.

<details>
<summary>Settings file</summary>

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
- `sounds`: `false` silences the click. By default, KnockThru plays SteamVR's dashboard click on a triple knock and on panel buttons. Steam already clicks when a double knock changes passthrough.
- `tour_done`: `true` once the first-run tour is finished or skipped. Set it to `false` to see the tour the next time you open the KnockThru tab. Settings also has a Replay button.

</details>

<details>
<summary>Calibrate over SSH</summary>

```sh
<install folder>/knockthru --calibrate together
```

Use `together`, `surface`, or `headset`. Knock 10 times within 90 s. Each knock KnockThru hears gives one buzz. The new threshold goes to `~/.config/knockthru/calibration.json`, and the running app picks it up within 2 s.

</details>

<details>
<summary>Commands</summary>

Run these on the Frame over SSH.

| Command | What it does |
| --- | --- |
| `knockthru` or `--install` | Register with SteamVR, turn on autostart, and start |
| `--uninstall` | Stop and remove from SteamVR. Keeps `~/.config/knockthru` |
| `--status` | Show registration, the running process, and Room View state |
| `--toggle`, `--style` | Turn Room View on or off, or switch its style, once |
| `--detect` | Print knocks live without acting on them |

The log is `~/.config/knockthru/application.log`.

</details>

<details>
<summary>Manual installation</summary>

Copy the Linux ARM64 ZIP from the releases page to the Frame, then run:

```sh
unzip -oq knockthru-0.1.0-linux-arm64.zip -d ~/KnockThru
~/KnockThru/knockthru --install
```

</details>

<details>
<summary>Build</summary>

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

README images (needs Pillow and ffmpeg, after the host build):

```sh
python3 scripts/readme_media.py
```

The controller and headset sprites in `images/cad/` come from Valve's CAD files. To render them again, download the STEP files from [Valve's Steam Frame repository](https://gitlab.steamos.cloud/SteamHardware/SteamFrame) into one folder, then run this (needs cadquery and opencv-python, and takes several minutes):

```sh
python3 scripts/cad_sprites.py <CAD folder>
```

`docs/knock-signal.md` explains how knock detection works and how it was tuned.

</details>

## Credits

- [KominoVR/frame-passthrough-shortcuts](https://github.com/KominoVR/frame-passthrough-shortcuts) (MIT): app scaffolding, style switch, and packaging.
- [toorux/steam-frame-passthrough-toggle](https://github.com/toorux/steam-frame-passthrough-toggle) (MIT): the Room View button click through the debug port.

- The controller and headset drawings are rendered from [Valve's Steam Frame CAD files](https://gitlab.steamos.cloud/SteamHardware/SteamFrame) (CC BY-NC-SA 4.0). Those images keep that license. See [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md). KnockThru is not associated with or endorsed by Valve.

[MIT licensed](LICENSE). Developed with help from coding agents.
