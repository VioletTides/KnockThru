#pragma once

namespace knockthru {

// Registers with SteamVR, turns on autostart, and starts the resident app. A
// running resident app stops first, so an update takes effect.
int install();

// Stops the resident app and removes it from SteamVR. With `purge`, also deletes
// ~/.config/knockthru, SteamVR's file for the app, and the report files in the home
// folder. The install folder stays, because FrameDrop or the person who unpacked it owns it.
int uninstall(bool purge);

// Installs the latest release over this one, then restarts KnockThru.
int update();

// Removes KnockThru's manifest and autostart from SteamVR. Needs a SteamVR session.
void unregister();

} // namespace knockthru
