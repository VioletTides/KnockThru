// Debug view render test: a graph value above the full scale, or one that is not a number, draws as a
// clipped spike inside the graph.
#include "check.hpp"
#include "panel.hpp"

#include <cfloat>
#include <cmath>
#include <iostream>
#include <vector>

using namespace knockthru;

namespace {

// The Live tab's graph tile, in canvas pixels.
constexpr int kTileLeft = 40, kTileRight = 616, kTileTop = 220, kTileBottom = 734;

// A panel on the debug view's Live tab, reached by clicking Settings and then the Debug view row.
struct DebugScreen {
    DebugView view;
    PanelState state;
    Panel panel{KNOCKTHRU_FONTS};

    DebugScreen() {
        state.debug = &view;
        panel.render(state);
        panel.click(460, 60);
        panel.render(state);
        panel.click(1050, 560);
    }

    // The screen with the left controller steady at 30 m/s² except for `middle` halfway along.
    const Canvas& live(float middle) {
        view.accel[static_cast<int>(Device::Left)] = std::vector<float>(500, 30.0f);
        view.accel[static_cast<int>(Device::Left)][250] = middle;
        return panel.render(state);
    }
};

void off_scale_tests() {
    std::cout << "off the scale\n";
    DebugScreen screen;
    const auto baseline = screen.live(30).pixels();
    const auto spike = screen.live(1e9f).hash();
    const auto flat = screen.live(30).hash();
    check(spike != flat, "a value above the full scale changes the graph");
    for (const float value : {101.0f, 450.0f, 1e30f, FLT_MAX, HUGE_VALF, std::nanf("")})
        check(screen.live(value).hash() == spike,
            "off the scale draws the same spike, " + std::to_string(value));

    // The spike stays inside the graph tile.
    const auto& canvas = screen.live(450);
    bool outside = false;
    int changed = 0;
    for (int y = 0; y < canvas.height(); ++y) {
        for (int x = 0; x < canvas.width(); ++x) {
            const auto at = (static_cast<std::size_t>(y) * canvas.width() + x) * 4;
            bool same = true;
            for (int c = 0; c < 4; ++c) same = same && canvas.pixels()[at + c] == baseline[at + c];
            if (same) continue;
            ++changed;
            outside = outside || x < kTileLeft || x >= kTileRight || y < kTileTop || y >= kTileBottom;
        }
    }
    check(changed > 0 && !outside, "the spike draws only inside the graph tile");
    check(screen.live(100).hash() != spike, "a value at the full scale is not a spike");
}

} // namespace

int main() {
    off_scale_tests();
    return finish();
}
