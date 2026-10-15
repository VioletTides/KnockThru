#pragma once

namespace knockthru {

// Steam's own UI sounds, so a knock sounds like flipping the switch by hand.
enum class Sound { SwitchOn, SwitchOff, Button };

// Plays the sound from the Steam install with pw-play, without waiting for it.
// If the file or the player is missing, the app stays silent.
void play(Sound sound);

} // namespace knockthru
