#include "dashboard_panel.hpp"

#include "steamvr.hpp"

#include <stdexcept>
#include <string>

namespace knockthru {

DashboardPanel::DashboardPanel(const std::filesystem::path& fonts) : panel_(fonts) {
    auto* overlay = vr::VROverlay();
    const auto error = overlay->CreateDashboardOverlay(kPanelKey, "KnockThru", &main_, &thumbnail_);
    if (error != vr::VROverlayError_None)
        throw std::runtime_error(
            std::string("Cannot add the dashboard panel: ") + overlay->GetOverlayErrorNameFromEnum(error));
    overlay->SetOverlayWidthInMeters(main_, 1.6f);
    overlay->SetOverlayInputMethod(main_, vr::VROverlayInputMethod_Mouse);
    const vr::HmdVector2_t scale{{Panel::kWidth, Panel::kHeight}};
    overlay->SetOverlayMouseScale(main_, &scale);
    const float hz = vr::VRSystem()->GetFloatTrackedDeviceProperty(
        vr::k_unTrackedDeviceIndex_Hmd, vr::Prop_DisplayFrequency_Float);
    if (hz > 0) frame_ = 1.0 / hz;
    try {
        texture_ = std::make_unique<OverlayTexture>(Panel::kWidth, Panel::kHeight);
    } catch (const std::exception& error) {
        log(std::string("Panel uses slower image uploads: ") + error.what());
    }
}

DashboardPanel::~DashboardPanel() { vr::VROverlay()->DestroyOverlay(main_); }

std::vector<PanelClick> DashboardPanel::poll(bool& shown) {
    std::vector<PanelClick> clicks;
#ifdef KNOCKTHRU_TEST_HOOK
    clicks.swap(test_clicks_);
    if (test_shown_) shown = true;
    test_shown_ = false;
#endif
    vr::VREvent_t event{};
    while (vr::VROverlay()->PollNextOverlayEvent(main_, &event, sizeof event)) {
        // OpenVR puts the mouse origin at the bottom left.
        const float x = event.data.mouse.x, y = Panel::kHeight - event.data.mouse.y;
        switch (event.eventType) {
        case vr::VREvent_OverlayShown: visible_ = shown = dirty_ = true; break;
        case vr::VREvent_OverlayHidden:
            visible_ = false;
            hover_.reset();
            break;
        case vr::VREvent_MouseMove:
            hover_ = std::make_pair(x, y);
            dirty_ = true;
            break;
        case vr::VREvent_MouseButtonUp:
            if (event.data.mouse.button == vr::VRMouseButton_Left)
                if (const auto click = panel_.click(x, y)) clicks.push_back(*click);
            dirty_ = true;
            break;
        default: break;
        }
    }
    return clicks;
}

void DashboardPanel::set_bar_icon(bool classic) {
    if (bar_icon_classic_ == classic) return;
    bar_icon_classic_ = classic;
    Canvas icon(256, 256);
    Panel::draw_icon(icon, classic ? IconStyle::BarColor : IconStyle::BarWhite);
    vr::VROverlay()->SetOverlayRaw(thumbnail_, const_cast<std::uint8_t*>(icon.pixels().data()), 256, 256, 4);
}

void DashboardPanel::draw(const PanelState& state, double now) {
    double interval = 0.5;
    if (panel_.transitioning())
        interval = frame_;
    else if (panel_.animating())
        interval = 1.0 / 30;
    else if (dirty_)
        interval = 0.05;
    // The main loop wakes on whole milliseconds, so a frame may come up to 1 ms early.
    if (!visible_ || now - last_draw_ < interval - 0.001) return;
    last_draw_ = now;
    dirty_ = false;
    const auto& canvas = panel_.render(state, hover_, now);
    // Even animated screens repeat about half their frames. On the Frame the hash
    // takes 0.2 ms, and an upload copies 3.7 MB to the GPU.
    const auto hash = canvas.hash();
    if (hash == uploaded_hash_) return;
    uploaded_hash_ = hash;
    upload(canvas);
}

void DashboardPanel::upload(const Canvas& canvas) {
    if (texture_) {
        try {
            texture_->show(main_, canvas.pixels().data());
            return;
        } catch (const std::exception& error) {
            log(std::string("Panel uses slower image uploads: ") + error.what());
            texture_.reset();
        }
    }
    vr::VROverlay()->SetOverlayRaw(main_, const_cast<std::uint8_t*>(canvas.pixels().data()),
        static_cast<uint32_t>(canvas.width()), static_cast<uint32_t>(canvas.height()), 4);
}

} // namespace knockthru
