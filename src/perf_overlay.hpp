#pragma once

namespace knockthru {

// SteamVR's in-headset performance graph, stored in the SteamVR setting
// perfcheck.perfGraphInHMD. Steam's Quick Access menu calls it "Show Perf
// Overlay in VR".

// Needs an active OpenVR session.
bool read_perf_overlay();
// Shows or hides the graph, checks that SteamVR kept the value, and returns
// whether it shows now.
bool toggle_perf_overlay();

} // namespace knockthru
