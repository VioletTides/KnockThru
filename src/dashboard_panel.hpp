#pragma once

#include "overlay_texture.hpp"
#include "panel.hpp"

#include <openvr.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace knockthru {

// The KnockThru tab on the SteamVR dashboard: an overlay showing the panel
// image, with the laser pointer as a mouse.
class DashboardPanel {
public:
    explicit DashboardPanel(const std::filesystem::path& fonts);
    ~DashboardPanel();
    DashboardPanel(const DashboardPanel&) = delete;

    bool visible() const { return visible_; }

    // Handles overlay events. Returns the clicked buttons and sets `shown` when
    // the panel just opened.
    std::vector<PanelClick> poll(bool& shown);

    void invalidate() { dirty_ = true; }

    // The mark in the logo colors when `classic`, or in white for Steam's tint.
    void set_bar_icon(bool classic);

    // Renders 30 times a second while something moves, at most 20 times a second
    // after a change, and twice a second otherwise, for countdowns and changes
    // made outside KnockThru. Uploads only changed images.
    void draw(const PanelState& state, double now);

#ifdef KNOCKTHRU_TEST_HOOK
    // Stand-ins for the dashboard and the laser pointer. The next poll() reports them.
    void test_open() { visible_ = dirty_ = test_shown_ = true; }
    void test_close() { visible_ = false; }
    void test_click(float x, float y) {
        if (const auto click = panel_.click(x, y)) test_clicks_.push_back(*click);
        dirty_ = true;
    }
    void test_press(PanelClick click) { test_clicks_.push_back(click); }
    bool test_snapshot(const PanelState& state, double now, const std::filesystem::path& path) {
        return panel_.render(state, hover_, now).write_png(path);
    }
#endif

private:
    void upload(const Canvas& canvas);

    Panel panel_;
    vr::VROverlayHandle_t main_ = vr::k_ulOverlayHandleInvalid, thumbnail_ = vr::k_ulOverlayHandleInvalid;
    bool visible_ = false, dirty_ = true;
    double last_draw_ = -1;
    std::optional<std::pair<float, float>> hover_;
    std::optional<bool> bar_icon_classic_;
    std::optional<std::uint64_t> uploaded_hash_;
    std::unique_ptr<OverlayTexture> texture_; // unset after a Vulkan failure, so uploads use SetOverlayRaw
#ifdef KNOCKTHRU_TEST_HOOK
    bool test_shown_ = false;
    std::vector<PanelClick> test_clicks_;
#endif
};

} // namespace knockthru
