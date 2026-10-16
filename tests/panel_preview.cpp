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
    panel.click(460, 60); // Settings on the main screen
    panel.render(idle, std::nullopt, 10.2);
    panel.click(1050, 184); // Calibrate in Settings
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
    view.speeds = {1.1, 1.5, 0.8};
    view.counted_t = 50;
    view.knocked_t = 49.7;
    view.seconds_left = 74;
    state.calibration = view;
    shot(panel, state, 50.6, out / "calibrate-3-record.png");
    frames(panel, state, out, "record", 50, 1.2);
    state.calibration->ignored = KnockKind::Together;
    state.calibration->ignored_t = 51.5;
    shot(panel, state, 51.8, out / "calibrate-3-record-hint.png");
    // The get-ready ring after Start, with a knock that came too soon.
    PanelState ready = state;
    ready.calibration->peaks.clear();
    ready.calibration->speeds.clear();
    ready.calibration->started_t = 55;
    ready.calibration->too_soon_t = 55.8;
    shot(panel, ready, 56.2, out / "calibrate-3-record-ready.png");

    // Robin's surface calibration on the Frame, 2026-10-15, with its one too-light knock raised to 64.
    view.peaks = {80, 401, 115, 342, 446, 91, 57, 234, 64, 147};
    view.speeds = {1.21, 0.85, 0.71, 0.81, 0.62, 0.77, 0.63, 0.53, 0.72, 1.71};
    view.phase = CalibrationView::Phase::Trying;
    state.calibration = view;
    shot(panel, state, 70.6, out / "calibrate-4-try.png");
    frames(panel, state, out, "try", 71, 2.6);
    state.calibration->trial_passed = false;
    state.calibration->trial_heard = "a triple surface knock";
    state.calibration->trial_t = 74;
    shot(panel, state, 74.6, out / "calibrate-4-try-miss.png");
    state.calibration->trial_passed = true;
    shot(panel, state, 75.6, out / "calibrate-4-try-pass.png");

    state.calibration.reset();
    state.calibration_saved = KnockKind::Surface;
    shot(panel, state, 80, out / "calibrate-saved.png");
    state.enabled[static_cast<int>(KnockKind::Surface)] = false;
    shot(panel, state, 81, out / "calibrate-saved-kind-off.png");
}

// The tour's five steps, with the knock steps before and after their knock.
void tour(const std::filesystem::path& out, const PanelState& idle) {
    Panel panel(KNOCKTHRU_FONTS);
    PanelState state = idle;
    state.message.clear();
    TourView view;
    state.tour = view;
    shot(panel, state, 10, out / "tour-1-welcome.png");
    state.tour->step = TourView::Step::Kinds;
    state.enabled = {true, true, false};
    shot(panel, state, 20, out / "tour-2-kinds.png");
    state.tour->step = TourView::Step::Calibrate;
    shot(panel, state, 30, out / "tour-3-calibrate.png", std::make_pair(600.0f, 400.0f));
    panel.click(600, 400); // the surface card opens the Learn screen
    shot(panel, state, 32, out / "tour-3-calibrate-learn.png");
    panel.click(100, 60); // Back returns to the tour
    state.tour->calibrated[static_cast<int>(KnockKind::Surface)] = true;
    shot(panel, state, 34, out / "tour-3-calibrate-saved.png");
    state.tour->step = TourView::Step::Double;
    shot(panel, state, 40, out / "tour-4-double.png");
    state.tour->double_t = 49;
    state.room_view = true;
    shot(panel, state, 50, out / "tour-4-double-heard.png");
    state.tour->step = TourView::Step::Triple;
    shot(panel, state, 60, out / "tour-5-triple.png");
    state.tour->triple_t = 69;
    state.opaque = true;
    shot(panel, state, 70, out / "tour-5-triple-heard.png");
    state.tour->step = TourView::Step::Done;
    shot(panel, state, 80, out / "tour-6-done.png");
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
    settings_panel.click(1050, 632); // Reset on the factory reset row
    settings_panel.render(idle, std::make_pair(800.0f, 500.0f)).write_png(out / "panel-reset-confirm.png");
    idle.classic_bar_icon = false;

    tour(out, idle);

    Canvas icon(256, 256);
    Panel::draw_icon(icon);
    icon.write_png(out / "icon.png");
    Panel::draw_icon(icon, IconStyle::BarWhite);
    icon.write_png(out / "icon-dashboard.png");
    Panel::draw_icon(icon, IconStyle::BarColor);
    icon.write_png(out / "icon-dashboard-classic.png");
    std::cout << "Wrote panel previews to " << out << "\n";
}
