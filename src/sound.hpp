#pragma once

namespace knockthru {

// The click that SteamVR's dashboard plays for every button and switch. It
// plays from the SteamVR install through PulseAudio (PipeWire on the Frame).
// If the file or libpulse-simple is missing, the app stays silent.
//
// Opening an audio stream takes about 250 ms on the Frame, and an open stream
// costs PipeWire about 2% of a core. Like Steam's own UI, a background thread
// opens the stream when needed and closes it after 10 s without a click.

// Opens the stream ahead of a click, for example when the panel appears.
void prepare_click();
// Plays the click without waiting for it.
void play_click();

} // namespace knockthru
