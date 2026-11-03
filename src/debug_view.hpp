#pragma once

#include "config.hpp"
#include "debug_model.hpp"
#include "knock_detector.hpp"

#include <array>
#include <cmath>
#include <optional>
#include <string>
#include <vector>

namespace knockthru {

// One detector number the Tuning tab can change.
struct Tunable {
    std::string label; // "Surface strength (m/s²)"
    double value, saved; // live and saved
    int decimals;
    std::string detail; // "From calibration", "Saved", "Saved value 40", or empty for defaults

    bool changed() const { return std::abs(value - saved) > 1e-9; }
};

// The 16 detector numbers in the order the tab shows them, the first five on its first page.
std::vector<Tunable> tunables(const DetectorConfig& live, const AppConfig& saved);
// Moves the number of tunable `index` one step down (-1) or up (+1), not below zero.
void step_tunable(DetectorConfig& live, int index, int direction);

// A line of the Status tab. `detail` is the dim line under the label.
struct StatusRow {
    std::string label, value, detail;
    bool bad = false;
};

// Everything the debug view shows. The app fills it in each frame.
struct DebugView {
    std::array<std::vector<float>, 3> accel; // per Device, 10 ms buckets, oldest first
    double threshold = 60; // the line on the graph, m/s²
    int set_knocks = 0; // knocks in the open set, 0 to 3 dots
    std::optional<FeedEntry> last; // the last knock tile
    std::vector<FeedEntry> recent; // the rows under it, newest first, at most 3
    std::vector<Tunable> tunables;
    std::vector<StatusRow> status_left,
        status_right; // the Status tab adds the test buttons under the right column
};

} // namespace knockthru
