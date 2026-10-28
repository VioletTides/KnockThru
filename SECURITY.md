# Security

## Reporting a vulnerability

Report it privately through **Report a vulnerability** on the repository's Security tab. Please don't open a public issue for it. Include the KnockThru version, from the top of Settings, and the steps that show the problem.

## What KnockThru exposes

KnockThru runs on the Steam Frame. Three parts of it talk to something outside the app:

- Bug report page: **Report** in Settings starts a small web server on the Frame's Wi-Fi network. A phone opens the report through a QR code. The server listens on a port the kernel picks, and every path needs a random 16-digit hex token from `std::random_device`. The server stops after 15 minutes. The page serves the report: versions, SteamVR state, settings, and the end of the log, plus 20 s of motion after **Record knocks**. Anyone on the same network who gets the link can read it while the server runs.
- Updates: once a day, KnockThru downloads the latest release manifest from GitHub. It installs a ZIP only from an `https://` URL, and only if the ZIP's SHA-256 matches the manifest. The checksum and the ZIP come from the same release, so the check catches broken downloads, not a replaced release.
- Steam's debug port: to switch passthrough, KnockThru needs CEF Remote Debugging turned on in the Frame's Developer settings. KnockThru connects to that port on 127.0.0.1 only. The port itself is a Steam feature, and Steam decides which addresses it listens on.

## Supported versions

Fixes ship in new releases. KnockThru checks for the latest release once a day and offers it in Settings.
