#pragma once

namespace knockthru {

// Plays the click that SteamVR's dashboard plays for every button and switch,
// the Room View switch included, without waiting for it. The sound comes from
// the SteamVR install through pw-play. If the file or the player is missing,
// the app stays silent.
void play_click();

} // namespace knockthru
