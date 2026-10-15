#pragma once

namespace knockthru {

// Room View style, stored in the SteamVR setting camera.roomViewStyle.
// Values from KominoVR/frame-passthrough-shortcuts (MIT).
enum class RoomViewStyle { Translucent = 3, Opaque = 4 };

// Needs an active OpenVR session.
RoomViewStyle read_style();
// Switches translucent and opaque, checks that SteamVR kept the value, and
// returns the new style.
RoomViewStyle toggle_style();
const char* to_string(RoomViewStyle style);

} // namespace knockthru
