# How the app fits together

KnockThru is one program that runs on the Steam Frame itself, not on a PC. SteamVR starts it as an overlay app, the same kind of app as a desktop or chat overlay. It shows up as a tab on the SteamVR dashboard.

The code is C++. Each part of the app lives in its own file pair under `src/`.

## From a knock to passthrough

1. **Reading motion:** a thread reads the headset and controller poses from SteamVR about once a millisecond (`src/poses.cpp`). Every knock needs a controller that moves first, so the thread reads less often when it can (`src/pose_rate.hpp`). While no controller is tracked, such as in standby, it reads 20 times a second. While both controllers stay under 0.1 m/s, it reads 100 times a second. It goes back to every millisecond on the first read that sees a controller move, and stays there for 0.5 s after the last movement. Poses are only valid in SteamVR's standing tracking space on the Frame, so KnockThru reads them there.
2. **Finding knocks:** the main loop feeds the poses to the knock detector (`src/knock_detector.cpp`). [How knock detection works](knock-detection.md) explains it.
3. **Acting:** the action for each kind of knock and count comes from `knock_actions` in `config.json` (`src/knock_actions.cpp`). **Knock actions** in Settings sets every kind the same. By default, knocking twice turns passthrough on or off, and knocking thrice switches its style. The other actions show or hide SteamVR's performance graph, or do nothing. Both controllers buzz once after knocking twice, and two times after knocking thrice. Three buzzes mean something failed.

Knocks do nothing while KnockThru is paused, or in a game where you turned knocks off.

## Switching passthrough

KnockThru turns passthrough (Room View) on and off by pressing the Room View button on the SteamVR dashboard bar, just as you would.

The Steam client draws the dashboard bar as a web page, and it has a browser debug port (CEF Remote Debugging, port 8080 on the Frame). KnockThru connects to that port, finds the Room View button on the bar page, and clicks it (`src/room_view.cpp`). Then it reads the button's state back to check that passthrough really changed. This is why CEF Remote Debugging must be on in the Frame's Developer settings. The approach comes from [toorux/steam-frame-passthrough-toggle](https://github.com/toorux/steam-frame-passthrough-toggle).

The style is simpler. Translucent and opaque are the SteamVR setting `camera.roomViewStyle`, 3 for translucent and 4 for opaque (`src/style.cpp`). KnockThru writes the setting and checks that SteamVR kept it. The values come from [KominoVR/frame-passthrough-shortcuts](https://github.com/KominoVR/frame-passthrough-shortcuts).

The performance graph works the same way. It is the SteamVR setting `perfcheck.perfGraphInHMD` (`src/perf_overlay.cpp`), which Steam's Quick Access menu calls **Show Perf Overlay in VR**. It shows over games streamed from a PC too.

If a Steam update moves the Room View button, KnockThru can't find it any more. The dashboard tab then says "A Steam update moved the Room View button. Update KnockThru in Settings."

## The dashboard tab

KnockThru draws its own dashboard tab (`src/panel.cpp`). It draws text and shapes into an image in memory (`src/canvas.cpp`) and hands that image to SteamVR as a texture (`src/overlay_texture.cpp`). The tab holds the switches, Settings, the calibration walkthrough, and the first-run tour.

## Two copies of one program

The install folder has `knockthru` and `knockthru-ctl`, and they're the same file. SteamVR tells apps apart by the path of their program. The resident app that SteamVR starts is `knockthru`. Commands like `--uninstall` run through `knockthru-ctl`, so SteamVR never mistakes a command for a second copy of the app.

## Settings and files

| File | What it holds |
| --- | --- |
| `<install folder>/config.json` | The defaults that ship with each release |
| `~/.config/knockthru/config.json` | Your settings. The dashboard tab writes here too |
| `~/.config/knockthru/calibration.json` | Your calibrated thresholds, one entry per kind of knock |
| `~/.config/knockthru/state.json` | What KnockThru remembers between runs, like the last update check |
| `~/.config/knockthru/application.log` | The log |

KnockThru checks for changed settings every 2 s and reloads them. Your files live outside the install folder, so updates never touch them. [Settings](settings.md) lists every key.

## Updates

Once a day, KnockThru asks GitHub for the latest release's manifest (`src/update.cpp`). The manifest lists the release ZIP and its SHA-256 checksum. To install an update, KnockThru:

1. Downloads the ZIP over HTTPS.
2. Checks the ZIP's SHA-256 against the manifest, and stops if they differ.
3. Unpacks it and checks that `knockthru-ctl` is an identical copy of `knockthru`.
4. Copies each file in beside the old one under a temporary name, then renames it into place. A failed copy leaves the old version working.
5. Restarts into the new version.

## Bug reports

**Report** in Settings writes a report with versions, SteamVR state, settings, and the end of the log (`src/report.cpp`). To get the report to your phone, it shows a QR code. The code links to a small web page the headset serves on your Wi-Fi (`src/report_server.cpp`). The link carries a random token, and the server stops after 15 minutes. On your phone, the page copies the report and opens a prefilled GitHub issue.

If knocks are the problem, **Record knocks** adds 20 s of motion to the report. [The test recordings](test-recordings.md) shows how to replay it.
