// Writes the dashboard panel in sample states to PNG files.
// Usage: panel_preview OUTPUT_DIRECTORY
//        panel_preview --readme OUTPUT_DIRECTORY
// The calibration steps also get frame sequences in OUTPUT_DIRECTORY/frames, at 25
// frames a second, to review the animations. The --readme form writes clean states for
// the README images instead.
#include "panel.hpp"

#include <cmath>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

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

// The tour's six steps, with the knock and calibrate steps before and after they finish.
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
    state.tour->step = TourView::Step::Triple;
    panel.render(state, std::nullopt, 89);
    panel.click(856, 692); // Skip opens the confirm card
    shot(panel, state, 90, out / "tour-skip-confirm.png");
    panel.click(388, 496); // Keep going
    state.tour->step = TourView::Step::Kinds;
    state.enabled = {false, false, false};
    state.message = "Settings not saved: Cannot write config.json";
    state.message_is_error = true;
    shot(panel, state, 100, out / "tour-2-kinds-none.png");
}

// Writes 25 frames a second of one screen, numbered on from earlier calls, and moves the clock on.
struct Recorder {
    Panel& panel;
    std::filesystem::path dir;
    double t = 100;
    int index = 0;
    std::vector<int> cuts; // frames where the clock jumps, for the README script to cross-fade

    void play(const PanelState& state, double seconds, std::optional<std::pair<float, float>> hover = std::nullopt) {
        until(state, t + seconds, hover);
    }
    void until(const PanelState& state, double end, std::optional<std::pair<float, float>> hover = std::nullopt) {
        for (; t < end - 1e-6; t += 1 / 25.0) {
            std::ostringstream file;
            file << std::setw(4) << std::setfill('0') << index++ << ".png";
            panel.render(state, hover, t).write_png(dir / file.str());
        }
    }
    // The first time after the clock where `phase` seconds into a `loop` long animation comes round.
    double next(double loop, double phase) const { return t + std::fmod(phase - std::fmod(t, loop) + 2 * loop, loop); }
};

// A surface calibration from the Settings button to the saved banner, as frames in `dir`.
// Counted knocks land on the contacts of the panel's own knock drawing.
void readme_calibration(const std::filesystem::path& dir) {
    std::filesystem::create_directories(dir);
    Panel panel(KNOCKTHRU_FONTS);
    PanelState state;
    state.room_view = false;
    state.opaque = false;
    state.app_key = "steam.app.620980";
    state.app_name = "Beat Saber";
    Recorder rec{panel, dir};
    rec.play(state, 1.0, std::make_pair(460.0f, 60.0f));
    panel.click(460, 60); // Settings
    rec.play(state, 1.4, std::make_pair(1050.0f, 184.0f));
    panel.click(1050, 184); // Calibrate
    rec.play(state, 2.2, std::make_pair(600.0f, 400.0f));
    panel.click(600, 400); // the surface card
    rec.play(state, 2.6, std::make_pair(1040.0f, 652.0f));
    panel.click(1040, 652); // Start, which the app answers by recording

    // Robin's surface calibration on the Frame, 2026-10-15, as in calibration().
    const std::vector<double> peaks{80, 401, 115, 342, 446, 91, 64, 234, 64, 147};
    const std::vector<double> speeds{1.21, 0.85, 0.71, 0.81, 0.62, 0.77, 0.63, 0.53, 0.72, 1.71};
    CalibrationView view;
    view.kind = KnockKind::Surface;
    view.started_t = rec.t - 2.0; // most of the get-ready time skipped
    view.seconds_left = 88;
    state.calibration = view;
    const auto knock = [&](std::size_t i) {
        auto& v = *state.calibration;
        rec.until(state, rec.next(2.0, 0.5)); // the record screen knocks once every 2 s
        v.peaks.push_back(peaks[i]);
        v.speeds.push_back(speeds[i]);
        v.knocked_t = v.counted_t = rec.t;
        v.seconds_left = 90 - static_cast<int>(rec.t - v.started_t);
    };
    for (std::size_t i = 0; i < 3; ++i) knock(i);
    rec.play(state, 1.2);
    // Skip ahead to the eighth knock.
    rec.cuts.push_back(rec.index);
    rec.t += 10;
    for (std::size_t i = 3; i < 8; ++i) state.calibration->peaks.push_back(peaks[i]), state.calibration->speeds.push_back(speeds[i]);
    state.calibration->knocked_t = state.calibration->counted_t = rec.t - 1.2;
    for (std::size_t i = 8; i < peaks.size(); ++i) knock(i);
    rec.play(state, 0.9);

    state.calibration->phase = CalibrationView::Phase::Trying;
    rec.play(state, 1.0);
    rec.until(state, rec.next(2.6, 1.2)); // the try screen knocks twice every 2.6 s, then waits 0.4 s
    state.calibration->trial_passed = true;
    state.calibration->trial_t = rec.t;
    rec.play(state, 1.8, std::make_pair(1010.0f, 692.0f));
    panel.click(1010, 692); // Save
    state.calibration.reset();
    state.calibration_saved = KnockKind::Surface;
    rec.play(state, 2.6);

    std::ofstream cuts(dir / "cuts.txt");
    for (int cut : rec.cuts) cuts << cut << "\n";
}

// Writes the passthrough off, on, and opaque states with no hover highlight.
int readme(const std::filesystem::path& out) {
    Panel panel(KNOCKTHRU_FONTS);
    PanelState state;
    state.room_view = false;
    state.opaque = false;
    state.app_key = "steam.app.620980";
    state.app_name = "Beat Saber";
    shot(panel, state, 10, out / "readme-off.png");
    state.room_view = true;
    shot(panel, state, 20, out / "readme-on.png");
    state.opaque = true;
    shot(panel, state, 30, out / "readme-opaque.png");
    readme_calibration(out / "calibrate");
    std::cout << "Wrote README panel states to " << out << "\n";
    return 0;
}

int main(int argc, char** argv) {
    if (argc > 2 && std::string(argv[1]) == "--readme") return readme(argv[2]);
    const std::filesystem::path out = argc > 1 ? argv[1] : ".";
    Panel panel(KNOCKTHRU_FONTS);

    PanelState idle;
    idle.room_view = false;
    idle.opaque = false;
    idle.app_key = "steam.app.620980";
    idle.app_name = "Beat Saber";
    idle.message = "Passthrough off (double together knock)";
    panel.render(idle, std::make_pair(900.0f, 300.0f)).write_png(out / "panel-idle.png");

    PanelState ready = idle;
    ready.update = {UpdateView::Status::Available, "0.2.0", ""};
    panel.render(ready).write_png(out / "panel-update-ready.png");

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
    broken.dashboard_changed = true;
    broken.room_view_error = "A Steam update moved the Room View button. Update KnockThru in Settings.";
    panel.render(broken).write_png(out / "panel-dashboard-changed.png");

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
    PanelState update = idle;
    update.version = "0.1.0";
    settings_panel.render(update).write_png(out / "panel-settings-update-idle.png");
    update.update = {UpdateView::Status::Checking, "", ""};
    settings_panel.render(update).write_png(out / "panel-settings-update-checking.png");
    update.update = {UpdateView::Status::Available, "0.2.0", ""};
    settings_panel.render(update, std::make_pair(1000.0f, 68.0f)).write_png(out / "panel-settings-update-available.png");
    update.update = {UpdateView::Status::Current, "0.1.0", ""};
    settings_panel.render(update).write_png(out / "panel-settings-update-current.png");
    update.update = {UpdateView::Status::Installing, "0.2.0", ""};
    update.update_progress = 0.45f;
    settings_panel.render(update).write_png(out / "panel-settings-update-installing.png");
    update.update_progress = 0;
    update.update = {UpdateView::Status::Installed, "0.2.0", ""};
    settings_panel.render(update).write_png(out / "panel-settings-update-installed.png");
    update.update = {UpdateView::Status::Restarting, "0.2.0", ""};
    settings_panel.render(update).write_png(out / "panel-settings-update-restarting.png");
    update.version = "0.2.0";
    update.update = {UpdateView::Status::Updated, "0.1.0", ""};
    settings_panel.render(update).write_png(out / "panel-settings-update-updated.png");
    {
        PanelState updated = idle;
        updated.version = "0.2.0";
        updated.update = {UpdateView::Status::Updated, "0.1.0", ""};
        updated.update_notice = UpdateNotice{false, "You now have version 0.2.0. You had version 0.1.0."};
        Panel main_panel(KNOCKTHRU_FONTS);
        main_panel.render(updated).write_png(out / "panel-updated.png");
        updated.update_notice = UpdateNotice{true, "Version 0.2.0 was not installed. The downloaded ZIP does not match "
            "its SHA-256. Nothing changed. You still have version 0.1.0."};
        main_panel.render(updated).write_png(out / "panel-update-failed.png");
    }
    update.version = "0.1.0";
    update.update = {UpdateView::Status::Failed, "", "Cannot reach GitHub. Check the Wi-Fi connection."};
    update.message = "Update check failed: Cannot reach GitHub. Check the Wi-Fi connection.";
    update.message_is_error = true;
    settings_panel.render(update).write_png(out / "panel-settings-update-failed.png");
    update.update = {};
    update.message.clear();
    update.message_is_error = false;
    update.report_url = "http://192.168.2.222:39841/5f0c9a2e81d34b7a";
    update.report_full = true;
    settings_panel.click(1050, 542); // Report on the Something wrong? row
    settings_panel.render(update, std::nullopt, 0);
    settings_panel.render(update).write_png(out / "panel-report.png");
    update.recording_left = 14;
    update.recording_knocks = 3;
    settings_panel.render(update).write_png(out / "panel-report-recording.png");
    update.recording_left = -1;
    update.recorded = true;
    update.report_saved = "/home/steamos/knockthru-report.txt";
    settings_panel.render(update).write_png(out / "panel-report-saved.png");
    update.report_saved.clear();
    update.report_full = false;
    update.report_url = "https://github.com/VioletTides/KnockThru/issues/new?template=bug.yml&version=0.1.0"
                        "&steamvr=2.18.2&passthrough=on";
    settings_panel.render(update).write_png(out / "panel-report-offline.png");
    settings_panel.click(900, 638); // Done
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
