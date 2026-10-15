// Writes the dashboard panel in sample states to PNG files.
// Usage: panel_preview OUTPUT_DIRECTORY
// The calibration steps also get frame sequences in OUTPUT_DIRECTORY/frames, at 25
// frames a second, to review the animations.
#include "panel.hpp"

#include <iomanip>
#include <iostream>
#include <sstream>

using namespace knockthru;

// Writes `seconds` of frames, starting at `start`, as NAME-000.png and up.
void frames(Panel& panel, const PanelState& state, const std::filesystem::path& out, const std::string& name,
            double start, double seconds) {
    std::filesystem::create_directories(out / "frames");
    for (int i = 0; i < static_cast<int>(seconds * 25); ++i) {
        std::ostringstream file;
        file << name << "-" << std::setw(3) << std::setfill('0') << i << ".png";
        panel.render(state, std::nullopt, start + i / 25.0).write_png(out / "frames" / file.str());
    }
}

// Writes a frame at `t`, after one at `t` - 0.5 so a new screen has finished rising in.
void shot(Panel& panel, const PanelState& state, double t, const std::filesystem::path& path,
          std::optional<std::pair<float, float>> hover = std::nullopt) {
    panel.render(state, hover, t - 0.5);
    panel.render(state, hover, t).write_png(path);
}

// Walks through the five calibration steps, as clicks and the app would.
void calibration(const std::filesystem::path& out, const PanelState& idle) {
    Panel panel(KNOCKTHRU_FONTS);
    panel.render(idle, std::nullopt, 10);
    panel.click(1050, 620); // Calibrate on the main screen
    shot(panel, idle, 10.6, out / "calibrate-1-pick.png", std::make_pair(600.0f, 400.0f));
    frames(panel, idle, out, "pick", 20.8, 2.6);
    panel.click(600, 400); // the surface card
    shot(panel, idle, 30.6, out / "calibrate-2-learn.png");
    frames(panel, idle, out, "learn", 40, 2);

    PanelState state = idle;
    state.message.clear();
    CalibrationView view;
    view.kind = KnockKind::Surface;
    view.peaks = {92, 118, 76};
    view.counted_t = 50;
    view.seconds_left = 74;
    state.calibration = view;
    shot(panel, state, 50.6, out / "calibrate-3-record.png");
    frames(panel, state, out, "record", 50, 1.2);
    state.calibration->ignored = KnockKind::Together;
    state.calibration->ignored_t = 51.5;
    shot(panel, state, 51.8, out / "calibrate-3-record-hint.png");

    view.phase = CalibrationView::Phase::Review;
    view.peaks = {92, 118, 76, 104, 88, 131, 97, 69, 110, 101};
    state.calibration = view;
    shot(panel, state, 60.6, out / "calibrate-4-review.png");
    panel.press(810, 536);
    panel.move(1000, 536);
    shot(panel, state, 61, out / "calibrate-4-review-drag.png");
    panel.click(1050, 536); // released near the most sensitive notch
    frames(panel, state, out, "review-snap", 61, 0.4);

    view.phase = CalibrationView::Phase::Trying;
    view.sensitivity = 1;
    state.calibration = view;
    shot(panel, state, 70.6, out / "calibrate-5-try.png");
    frames(panel, state, out, "try", 71, 2.6);
    state.calibration->trial_passed = false;
    state.calibration->trial_heard = "a triple surface knock";
    state.calibration->trial_t = 74;
    shot(panel, state, 74.6, out / "calibrate-5-try-miss.png");
    state.calibration->trial_passed = true;
    shot(panel, state, 75.6, out / "calibrate-5-try-pass.png");

    state.calibration.reset();
    state.calibration_saved = true;
    shot(panel, state, 80, out / "calibrate-saved.png");
}

int main(int argc, char** argv) {
    const std::filesystem::path out = argc > 1 ? argv[1] : ".";
    Panel panel(KNOCKTHRU_FONTS);

    PanelState idle;
    idle.room_view = false;
    idle.opaque = false;
    idle.app_key = "steam.app.620980";
    idle.app_name = "Beat Saber";
    idle.message = "Passthrough off (double together knock)";
    panel.render(idle, std::make_pair(900.0f, 300.0f)).write_png(out / "panel-idle.png");

    PanelState busy = idle;
    busy.room_view = true;
    busy.opaque = true;
    busy.paused = true;
    busy.app_blocked = true;
    busy.enabled = {true, false, true};
    panel.render(busy).write_png(out / "panel-paused.png");

    PanelState broken;
    broken.room_view_error = "Turn on CEF Remote Debugging in the Frame's Developer settings.";
    broken.opaque = false;
    panel.render(broken).write_png(out / "panel-broken.png");

    calibration(out, idle);

    PanelState none = idle;
    none.enabled = {false, false, false};
    Panel back_panel(KNOCKTHRU_FONTS);
    back_panel.render(none).write_png(out / "panel-no-kinds.png");

    // Opens the settings screen through its button, as a click would.
    Panel settings_panel(KNOCKTHRU_FONTS);
    settings_panel.render(idle);
    settings_panel.click(460, 60);
    settings_panel.render(idle, std::make_pair(600.0f, 200.0f)).write_png(out / "panel-settings.png");
    idle.classic_bar_icon = true;
    settings_panel.render(idle).write_png(out / "panel-settings-color.png");

    Canvas icon(256, 256);
    Panel::draw_icon(icon);
    icon.write_png(out / "icon.png");
    Panel::draw_icon(icon, IconStyle::BarWhite);
    icon.write_png(out / "icon-dashboard.png");
    Panel::draw_icon(icon, IconStyle::BarColor);
    icon.write_png(out / "icon-dashboard-classic.png");
    std::cout << "Wrote panel previews to " << out << "\n";
}
