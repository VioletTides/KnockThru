#pragma once

namespace knockthru {

// Runs the resident app until SteamVR quits or a signal arrives. SteamVR starts it with --run.
int run_app();

} // namespace knockthru
