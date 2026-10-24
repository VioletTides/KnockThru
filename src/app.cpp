#include "app.hpp"

#include "calibrator.hpp"
#include "config.hpp"
#include "diagnostics.hpp"
#include "haptics.hpp"
#include "install.hpp"
#include "knock_detector.hpp"
#include "overlay_texture.hpp"
#include "panel.hpp"
#include "perf_overlay.hpp"
#include "poses.hpp"
#include "report.hpp"
#include "report_server.hpp"
#include "room_view.hpp"
#include "sound.hpp"
#include "steamvr.hpp"
#include "style.hpp"
#include "update.hpp"

#include <openvr.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include <unistd.h>

namespace knockthru {
namespace {

namespace fs = std::filesystem;

// The KnockThru tab on the SteamVR dashboard: an overlay showing the panel
// image, with the laser pointer as a mouse.
class DashboardPanel {
public:
    explicit DashboardPanel(const fs::path& font) : panel_(font) {
        auto* overlay = vr::VROverlay();
        const auto error = overlay->CreateDashboardOverlay(kPanelKey, "KnockThru", &main_, &thumbnail_);
        if (error != vr::VROverlayError_None)
            throw std::runtime_error(std::string("Cannot add the dashboard panel: ")
                + overlay->GetOverlayErrorNameFromEnum(error));
        overlay->SetOverlayWidthInMeters(main_, 1.6f);
        overlay->SetOverlayInputMethod(main_, vr::VROverlayInputMethod_Mouse);
        const vr::HmdVector2_t scale{{Panel::kWidth, Panel::kHeight}};
        overlay->SetOverlayMouseScale(main_, &scale);
        try {
            texture_ = std::make_unique<OverlayTexture>(Panel::kWidth, Panel::kHeight);
        } catch (const std::exception& error) {
            log(std::string("Panel uses slower image uploads: ") + error.what());
        }
    }
    ~DashboardPanel() { vr::VROverlay()->DestroyOverlay(main_); }
    DashboardPanel(const DashboardPanel&) = delete;

    bool visible() const { return visible_; }

    // Handles overlay events. Returns the clicked buttons and sets `shown` when
    // the panel just opened.
    std::vector<PanelClick> poll(bool& shown) {
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

    void invalidate() { dirty_ = true; }

#ifdef KNOCKTHRU_TEST_HOOK
    // Stand-ins for the dashboard and the laser pointer. The next poll() reports them.
    void test_open() { visible_ = dirty_ = test_shown_ = true; }
    void test_close() { visible_ = false; }
    void test_click(float x, float y) {
        if (const auto click = panel_.click(x, y)) test_clicks_.push_back(*click);
        dirty_ = true;
    }
    void test_press(PanelClick click) { test_clicks_.push_back(click); }
    bool test_snapshot(const PanelState& state, double now, const fs::path& path) {
        return panel_.render(state, hover_, now).write_png(path);
    }
#endif

    // Sets the dashboard bar icon: the mark in the logo colors, or in white for Steam's tint.
    void set_bar_icon(bool classic) {
        if (bar_icon_classic_ == classic) return;
        bar_icon_classic_ = classic;
        Canvas icon(256, 256);
        Panel::draw_icon(icon, classic ? IconStyle::BarColor : IconStyle::BarWhite);
        vr::VROverlay()->SetOverlayRaw(
            thumbnail_, const_cast<std::uint8_t*>(icon.pixels().data()), 256, 256, 4);
    }

    // Renders 30 times a second while something moves, at most 20 times a second
    // after a change, and twice a second otherwise, for countdowns and changes
    // made outside KnockThru. Uploads only changed images.
    void draw(const PanelState& state, double now) {
        const double interval = panel_.animating() ? 1.0 / 30 : dirty_ ? 0.05 : 0.5;
        if (!visible_ || now - last_draw_ < interval) return;
        last_draw_ = now;
        dirty_ = false;
        upload(panel_.render(state, hover_, now));
    }

private:
    void upload(const Canvas& canvas) {
        if (canvas.pixels() == uploaded_) return;
        uploaded_ = canvas.pixels();
        if (texture_) {
            try {
                texture_->show(main_, uploaded_.data());
                return;
            } catch (const std::exception& error) {
                log(std::string("Panel uses slower image uploads: ") + error.what());
                texture_.reset();
            }
        }
        vr::VROverlay()->SetOverlayRaw(main_, uploaded_.data(), static_cast<uint32_t>(canvas.width()),
            static_cast<uint32_t>(canvas.height()), 4);
    }

    Panel panel_;
    vr::VROverlayHandle_t main_ = vr::k_ulOverlayHandleInvalid, thumbnail_ = vr::k_ulOverlayHandleInvalid;
    bool visible_ = false, dirty_ = true;
    double last_draw_ = -1;
    std::optional<std::pair<float, float>> hover_;
    std::optional<bool> bar_icon_classic_;
    std::vector<std::uint8_t> uploaded_;
    std::unique_ptr<OverlayTexture> texture_;
#ifdef KNOCKTHRU_TEST_HOOK
    bool test_shown_ = false;
    std::vector<PanelClick> test_clicks_;
#endif
};

// A Room View read or change running on another thread, because the debug
// port can take a second.
struct RoomViewResult {
    bool on;
    std::string message; // empty for a plain read
};

std::string today() {
    const auto now = std::time(nullptr);
    char date[16];
    std::strftime(date, sizeof date, "%F", std::localtime(&now));
    return date;
}

// "double" or "triple".
const char* sequence_name(const KnockEvent& event) { return event.count == 3 ? "triple" : "double"; }

// Whether a background job has a result to collect.
template <typename T> bool ready(const std::future<T>& job) {
    return job.valid() && job.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
}

// One line for the log: how each knock heard while recording ended.
std::string heard_summary(const Calibrator& calibrator) {
    using Heard = Calibrator::Heard;
    char line[192];
    std::snprintf(line, sizeof line,
        "Recorded %s knocks: %d counted, %d too soon, %d too light, %d other kind, %d ignored",
        to_string(calibrator.kind()), calibrator.heard(Heard::Counted), calibrator.heard(Heard::TooSoon),
        calibrator.heard(Heard::TooLight), calibrator.heard(Heard::OtherKind),
        calibrator.heard(Heard::Ignored));
    return line;
}

// The resident app. SteamVR starts it as a dashboard overlay. Each knock kind
// and count runs its action from knock_actions. By default, knocking twice turns
// Room View on or off, and knocking thrice switches translucent and opaque. The
// KnockThru dashboard tab has buttons for everything.
class App {
public:
    App();
    // Downloads stop when the app does, so quitting SteamVR never waits on one.
    // This runs before the members are destroyed, and the job futures wait for their jobs.
    ~App() { cancel_downloads_ = true; }
    App(const App&) = delete;

    // Runs until SteamVR quits, a signal arrives, or the install folder is removed.
    void run();

private:
    using Phase = CalibrationView::Phase;
    using Step = TourView::Step;
    using UpdateStatus = UpdateView::Status;

    // Each returns false when the app should exit.
    bool slow_check(double t);
    void handle_click(const PanelClick& click, double t);
    void record_knocks(double t, const std::deque<Knock>& knocks, const std::optional<KnockEvent>& event);
    void handle_knocks(double t, const std::deque<Knock>& knocks, const std::optional<KnockEvent>& event);
    void collect_jobs(double t);
    void draw_panel(double t);
#ifdef KNOCKTHRU_TEST_HOOK
    void run_test_commands(double t, std::deque<Knock>& knocks, std::optional<KnockEvent>& event);
    void run_test_command(
        const std::string& line, double t, std::deque<Knock>& knocks, std::optional<KnockEvent>& event);
#endif

    void note_versions();
    void load_haptics();
    void save_app_state();
    std::string config_stamp() const;
    void show(const std::string& message, bool error = false);
    void reload();
    void play_click();
    void start_tour();
    void read_room_view();
    void read_opaque();
    bool toggle_room_view(const std::string& why);
    bool toggle_style(const std::string& why);
    bool toggle_perf_overlay(const std::string& why);
    void run_action(KnockAction action, const KnockEvent& event, const std::string& why, double t);
    bool save(const std::function<void()>& write);
    void start_check();
    void restart_failed(const std::string& version);
    void end_calibration();
    std::string room_view_text() const;

    AppState app_state_ = load_state();
    std::optional<std::string>
        updated_from_; // set when an update ran since the last start. Empty when the old version is unknown.
    Haptics haptics_;
    vr::VRActiveActionSet_t active_{}; // its action set stays invalid without the action manifest
    std::unique_ptr<DashboardPanel> panel_;

    const std::array<fs::path, 3> config_files_{install_directory() / "config.json",
        user_directory() / "config.json", user_directory() / "calibration.json"};
    AppConfig config_ = load_config(install_directory());
    std::string stamp_ = config_stamp();
    KnockDetector detector_{config_.detector};
    std::optional<Calibrator> calibrator_;
    std::optional<CalibrationView> calibration_; // the walkthrough the panel shows
    std::optional<TourView> tour_; // the first-run tour, while the panel shows it

    Clock clock_;
    PoseThread poses_{clock_};
    PanelState state_;
    std::string last_error_; // for the report link

    std::future<RoomViewResult> room_view_job_;
    bool room_view_job_toggles_ = false;
    bool dashboard_changed_ = false; // the last Room View read did not find the button, so the log has it
    std::future<void> tint_job_;

    std::atomic<bool> cancel_downloads_{false};
    std::atomic<float> install_progress_{0}; // the install job writes it, from 0 to 1
    const fs::path update_work_ = user_directory() / "update";
    std::future<Release> check_job_;
    bool check_is_automatic_ = false; // the daily check, which never shows errors
    std::future<void> install_job_;
    std::optional<Release> latest_; // the newer release a check found
    double restart_started_ = -1; // when this process asked knockthru-ctl to restart into the new version

    double next_slow_check_ = 0;
    const fs::path binary_ = executable();
    double binary_missing_since_ = -1;

    std::optional<KnockRecording> recording_; // while the report card records knocks
    std::future<void> recording_job_; // writes a finished recording
    std::unique_ptr<ReportServer> report_server_; // while the report card shows
#ifdef KNOCKTHRU_TEST_HOOK
    fs::path test_commands_; // the command file
    std::uintmax_t test_read_ = 0; // bytes of it already run
#endif
};

App::App() {
    note_versions();
#ifdef KNOCKTHRU_TEST_HOOK
    test_commands_ = user_directory() / "test-commands";
    std::ofstream(test_commands_, std::ios::app).close();
    test_read_ = fs::file_size(test_commands_); // a restart never repeats old commands
    log("Test hook reads " + test_commands_.string());
#endif

    const auto pid = static_cast<uint32_t>(getpid());
    std::array<char, vr::k_unMaxApplicationKeyLength> key{};
    vr::VRApplications()->GetApplicationKeyByProcessId(pid, key.data(), static_cast<uint32_t>(key.size()));
    if (std::string(key.data()) != kAppKey && vr::VRApplications()->IsApplicationInstalled(kAppKey))
        vr::VRApplications()->IdentifyApplication(pid, kAppKey);

    load_haptics();
    try {
        panel_ = std::make_unique<DashboardPanel>(install_directory() / "fonts");
    } catch (const std::exception& error) {
        log(error.what());
    }

    // Someone with settings or a calibration already knows KnockThru, so the tour
    // only shows itself to new users. Settings can replay it.
    if (!config_.tour_done
        && (fs::exists(user_directory() / "config.json")
            || fs::exists(user_directory() / "calibration.json"))) {
        try {
            save_tour_done(true);
            config_ = load_config(install_directory());
            stamp_ = config_stamp();
        } catch (const std::exception& error) {
            log(std::string("Tour not marked done: ") + error.what());
        }
    }

    state_.version = kVersion;
    if (updated_from_) {
        state_.update = {UpdateStatus::Updated, *updated_from_, ""};
        const auto had = updated_from_->empty() ? "" : " You had version " + *updated_from_ + ".";
        state_.update_notice =
            UpdateNotice{false, std::string("You now have version ") + kVersion + "." + had};
    }
}

// A SteamVR update is the usual cause when Room View control breaks, so the log marks each one.
// A newer KnockThru than at the last start means an update ran, from Settings or FrameDrop. The panel says
// so.
void App::note_versions() {
    // A state file without knockthru_version comes from an older KnockThru.
    const bool ran_before = !app_state_.steamvr_version.empty();
    const auto before = app_state_.steamvr_version;
    const auto previous = note_steamvr_version(app_state_, steamvr_version(), today());
    if (!previous.empty()) log("SteamVR changed from " + previous + " to " + app_state_.steamvr_version);
    if (app_state_.steamvr_version != before) save_app_state();
    if (app_state_.knockthru_version == kVersion) return;
    if (app_state_.knockthru_version.empty() ? ran_before
                                             : compare_versions(kVersion, app_state_.knockthru_version) > 0) {
        updated_from_ = app_state_.knockthru_version;
        log("Updated from " + (updated_from_->empty() ? "an older version" : *updated_from_));
    }
    app_state_.knockthru_version = kVersion;
    save_app_state();
}

void App::load_haptics() {
    vr::VRActionSetHandle_t action_set = vr::k_ulInvalidActionSetHandle;
    vr::VRActionHandle_t left = vr::k_ulInvalidActionHandle, right = vr::k_ulInvalidActionHandle;
    const auto actions = (install_directory() / "actions.json").u8string();
    if (vr::VRInput()->SetActionManifestPath(actions.c_str()) == vr::VRInputError_None
        && vr::VRInput()->GetActionSetHandle("/actions/knockthru", &action_set) == vr::VRInputError_None
        && vr::VRInput()->GetActionHandle("/actions/knockthru/out/haptic_left", &left)
            == vr::VRInputError_None
        && vr::VRInput()->GetActionHandle("/actions/knockthru/out/haptic_right", &right)
            == vr::VRInputError_None) {
        haptics_.use_actions(left, right);
        active_.ulActionSet = action_set;
    } else {
        log("SteamVR input actions unavailable. Using direct haptic pulses.");
    }
}

void App::run() {
    std::string actions;
    for (const int count : kKnockCounts) {
        const auto shared = config_.knock_actions.shared(count);
        actions += std::string(" Knocking ") + count_word(count) + ": "
            + (shared ? to_string(*shared) : "different for each kind") + ".";
    }
    log("Ready." + actions);
    while (!stopping) {
        const double t = clock_.now();
        vr::VREvent_t event{};
        while (vr::VRSystem()->PollNextEvent(&event, sizeof event)) {
            if (event.eventType == vr::VREvent_Quit) {
                vr::VRSystem()->AcknowledgeQuit_Exiting();
                stopping = 1;
            }
        }
        if (active_.ulActionSet != vr::k_ulInvalidActionSetHandle)
            vr::VRInput()->UpdateActionState(&active_, sizeof active_, 1);

        if (t >= next_slow_check_ && !slow_check(t)) break;
        if (panel_) {
            bool shown = false;
            for (const auto& click : panel_->poll(shown)) handle_click(click, t);
            if (shown) {
                if (!config_.tour_done && !tour_ && !calibrator_) {
                    log("Starting the first-run tour");
                    start_tour();
                }
                if (config_.sounds) prepare_click();
                read_room_view();
                read_opaque();
            }
        }

        for (const auto& sample : poses_.take()) {
            detector_.push(sample.device, sample.t, sample.position, sample.velocity);
            if (recording_) recording_->samples.push_back(sample);
        }
        auto knock_event = detector_.poll(t);
        auto knocks = detector_.take_knocks();
#ifdef KNOCKTHRU_TEST_HOOK
        run_test_commands(t, knocks, knock_event);
#endif
        if (recording_) record_knocks(t, knocks, knock_event);
        handle_knocks(t, knocks, knock_event);
        collect_jobs(t);
        draw_panel(t);
        haptics_.update(t);
        // Knocks are judged 50 ms late anyway, so a hidden panel lets the loop wake every 5 ms.
        // The pose thread still samples every millisecond.
        clock_.tick(std::chrono::milliseconds(panel_ && panel_->visible() ? 1 : 5));
    }
    log("KnockThru stopped");
}

// Runs every 2 s: the bar icon tint, the app in front, the daily update check,
// changed settings files, and a removed install folder.
bool App::slow_check(double t) {
    next_slow_check_ = t + 2;
    // Keeps the click stream open while the panel shows, so a press clicks at once.
    if (config_.sounds && panel_ && panel_->visible()) prepare_click();
    // Restyles the bar icon while the dashboard is open, in case the bar page
    // reloaded. If this fails, the icon stays undimmed. The classic icon has no tint.
    if (vr::VROverlay()->IsDashboardVisible() && (!tint_job_.valid() || ready(tint_job_))) {
        if (tint_job_.valid()) try {
                tint_job_.get();
            } catch (const std::exception&) {}
        const RoomView bar(config_.debug_port);
        const bool tint = !config_.classic_bar_icon;
        tint_job_ = std::async(std::launch::async, [bar, tint] { bar.tint_icon(kPanelKey, tint); });
    }
    state_.app_key = scene_app_key();
    state_.app_name = app_name(state_.app_key);
    // The daily check waits a minute after start, for the Wi-Fi. It fails quietly.
    const auto now_epoch = static_cast<long long>(std::time(nullptr));
    if (config_.check_updates && t > 60 && !check_job_.valid() && !install_job_.valid()
        && (state_.update.status == UpdateStatus::Idle || state_.update.status == UpdateStatus::Updated)
        && now_epoch - app_state_.last_update_check >= 24 * 3600) {
        app_state_.last_update_check = now_epoch;
        save_app_state();
        check_is_automatic_ = true;
        log("Daily update check");
        start_check();
    }
    if (const auto now_stamp = config_stamp(); now_stamp != stamp_) {
        stamp_ = now_stamp;
        try {
            reload();
            log("Settings reloaded");
        } catch (const std::exception& error) {
            show(std::string("Settings not reloaded: ") + error.what(), true);
        }
    }
    if (panel_ && panel_->visible()) {
        read_room_view(); // Room View can also change from the bar or the side button
        read_opaque();
    }
    // FrameDrop updates delete the install directory, then copy the new
    // one. A binary gone for 15 s means the app was removed.
    if (fs::exists(binary_)) {
        binary_missing_since_ = -1;
    } else if (binary_missing_since_ < 0) {
        binary_missing_since_ = t;
    } else if (t - binary_missing_since_ > 15) {
        log("Install directory removed. Unregistering and exiting.");
        unregister();
        return false;
    }
    return true;
}

void App::handle_click(const PanelClick& click, double t) {
    if (click.command != PanelCommand::RoomView) play_click();
    const auto kind_name = std::string(to_string(click.kind));
    const auto kind = static_cast<int>(click.kind);
    switch (click.command) {
    case PanelCommand::RoomView: toggle_room_view(""); break;
    case PanelCommand::Style: toggle_style(""); break;
    case PanelCommand::Navigate: break;
    case PanelCommand::Pause:
        if (save([&] { save_paused(!config_.paused); }))
            show(config_.paused ? "KnockThru off" : "KnockThru on");
        break;
    case PanelCommand::Sounds:
        if (save([&] { save_sounds(!config_.sounds); })) show(config_.sounds ? "Sounds on" : "Sounds off");
        break;
    case PanelCommand::SetKnockAction: {
        auto actions = config_.knock_actions;
        actions.set_all(click.count, click.action);
        if (save([&] { save_knock_actions(actions); }))
            show(std::string("Knocking ") + count_word(click.count) + ": " + to_string(click.action));
        break;
    }
    case PanelCommand::StartTour: start_tour(); break;
    case PanelCommand::TourNext:
        if (tour_ && tour_->step != Step::Done)
            tour_->step = static_cast<Step>(static_cast<int>(tour_->step) + 1);
        break;
    case PanelCommand::TourBack:
        if (tour_ && tour_->step != Step::Welcome)
            tour_->step = static_cast<Step>(static_cast<int>(tour_->step) - 1);
        break;
    case PanelCommand::TourSkip:
        // The tour closes even if the flag cannot be saved, so it never traps anyone.
        if (!config_.tour_done) save([&] { save_tour_done(true); });
        if (tour_) log(tour_->step == Step::Done ? "Tour finished" : "Tour skipped");
        tour_.reset();
        break;
    case PanelCommand::FactoryReset:
        calibrator_.reset();
        calibration_.reset();
        try {
            reset_user_settings();
            reload();
            log("Factory reset: settings and calibration deleted");
            state_.message.clear();
            state_.message_is_error = false;
            state_.calibration_saved.reset();
            start_tour();
        } catch (const std::exception& error) {
            detector_ = KnockDetector(config_.detector);
            show(std::string("Factory reset failed: ") + error.what(), true);
        }
        break;
    case PanelCommand::CheckUpdate:
        // A daily check already running answers this press too.
        if (check_job_.valid() && check_is_automatic_) {
            check_is_automatic_ = false;
            state_.update = {UpdateStatus::Checking, "", ""};
        }
        if (check_job_.valid() || install_job_.valid()) break;
        check_is_automatic_ = false;
        state_.update = {UpdateStatus::Checking, "", ""};
        start_check();
        break;
    case PanelCommand::RestartForUpdate:
        if (state_.update.status != UpdateStatus::Installed) break;
        log("Restarting into KnockThru " + state_.update.latest);
        try {
            // knockthru-ctl --install stops this process and starts the new one.
            start_detached({(install_directory() / "knockthru-ctl").string(), "--install"});
            state_.update.status = UpdateStatus::Restarting;
            restart_started_ = t;
        } catch (const std::exception& error) {
            log(std::string("Restart failed: ") + error.what());
            restart_failed(state_.update.latest);
        }
        break;
    case PanelCommand::DismissUpdateNotice: state_.update_notice.reset(); break;
    case PanelCommand::InstallUpdate:
        if (!latest_ || check_job_.valid() || install_job_.valid()) break;
        log("Installing KnockThru " + latest_->version + " from " + latest_->url);
        state_.update.status = UpdateStatus::Installing;
        install_progress_ = 0;
        install_job_ = std::async(std::launch::async,
            [release = *latest_, work = update_work_, cancel = &cancel_downloads_,
                progress = &install_progress_] {
                install_release(release, install_directory(), work, cancel, progress);
            });
        break;
    case PanelCommand::ShowReport: {
        // The QR code opens a page this headset serves, with the full report. Without a
        // network, it opens the GitHub form directly, which only has room for the versions.
        const auto issue = issue_url({kVersion, steamvr_version(), room_view_text(), last_error_});
        report_server_.reset();
        try {
            report_server_ = std::make_unique<ReportServer>(report_text(true, room_view_text()), issue);
            state_.report_url = report_server_->url();
            state_.report_full = true;
            // A recording made before the card closed belongs to this report too.
            std::error_code missing;
            if (state_.recorded && fs::exists(recording_path(), missing))
                report_server_->set_recording(recording_path());
            log("Serving the bug report at " + state_.report_url);
        } catch (const std::exception& error) {
            state_.report_url = issue;
            state_.report_full = false;
            log(std::string("Bug report page unavailable: ") + error.what());
        }
        state_.report_saved.clear();
        state_.report_error.clear();
        break;
    }
    case PanelCommand::RecordKnocks:
        if (recording_ || recording_job_.valid()) break;
        recording_.emplace();
        recording_->start = t;
        recording_->samples.reserve(70000);
        state_.recording_knocks = 0;
        state_.recorded = false;
        log("Recording knocks for a bug report");
        break;
    case PanelCommand::CloseReport: report_server_.reset(); break;
    case PanelCommand::SaveReport:
        try {
            state_.report_saved = write_report(true, room_view_text()).string();
            state_.report_error.clear();
            log("Bug report saved to " + state_.report_saved);
        } catch (const std::exception& error) {
            state_.report_error = error.what();
            log(std::string("Bug report not saved: ") + error.what());
        }
        break;
    case PanelCommand::Kind: {
        auto enabled = config_.detector.enabled;
        enabled[kind] = !enabled[kind];
        if (save([&] { save_knock_types(enabled); }))
            show(kind_name + " knocks " + (config_.detector.enabled[kind] ? "on" : "off"));
        break;
    }
    case PanelCommand::BlockApp: {
        if (state_.app_key.empty()) break;
        auto blocked = config_.blocked_apps;
        if (!blocked.erase(state_.app_key)) blocked.insert(state_.app_key);
        const auto name = state_.app_name.empty() ? state_.app_key : state_.app_name;
        if (save([&] { save_blocked_apps(blocked); }))
            show(std::string("Knocks ") + (config_.blocked_apps.count(state_.app_key) ? "off" : "on") + " in "
                + name);
        break;
    }
    case PanelCommand::Calibrate:
        calibrator_.emplace(click.kind, config_.detector, t);
        detector_ = KnockDetector(calibrator_->detector_config());
        calibration_.emplace();
        calibration_->kind = click.kind;
        calibration_->wanted = Calibrator::kWanted;
        calibration_->started_t = t;
        show("Calibrating " + kind_name + ": knock " + std::to_string(Calibrator::kWanted)
            + " times, about 1 s apart");
        break;
    case PanelCommand::CancelCalibration:
        if (calibrator_ && calibration_ && calibration_->phase == Phase::Recording)
            log(heard_summary(*calibrator_));
        end_calibration();
        show("Calibration canceled. Nothing saved.");
        break;
    case PanelCommand::SaveCalibration:
        if (!calibrator_ || !calibration_ || !calibration_->trial_passed.value_or(false)) break;
        try {
            const auto saved = calibrator_->save();
            log("Calibrated " + kind_name + " knocks: " + thresholds_text(saved));
            calibrator_.reset();
            calibration_.reset();
            reload();
            show("Calibration saved");
            // The tour's Calibrate step marks its card, so the Saved banner is only for Settings.
            if (tour_)
                tour_->calibrated[kind] = true;
            else
                state_.calibration_saved = click.kind;
            haptics_.buzz(2, t);
        } catch (const std::exception& error) {
            show(std::string("Calibration not saved: ") + error.what(), true);
        }
        break;
    }
}

// Adds each knock and action to the bug report recording, and saves it when its time is up.
void App::record_knocks(double t, const std::deque<Knock>& knocks, const std::optional<KnockEvent>& event) {
    char line[160];
    for (const auto& knock : knocks) {
        std::snprintf(line, sizeof line, "%6.2f s  %s knock, peak %.0f m/s^2", knock.t - recording_->start,
            to_string(knock.kind), knock.peak);
        recording_->heard.push_back(line);
    }
    if (event) {
        std::snprintf(line, sizeof line, "%6.2f s  %s %s knock", event->t - recording_->start,
            sequence_name(*event), to_string(event->kind));
        recording_->heard.push_back(line);
    }
    state_.recording_knocks += static_cast<int>(knocks.size());
    const double left = KnockRecording::kSeconds - (t - recording_->start);
    state_.recording_left = static_cast<int>(std::ceil(std::max(left, 0.0)));
    if (left <= 0) {
        state_.recording_left = -1;
        log("Recorded " + std::to_string(recording_->samples.size()) + " samples and "
            + std::to_string(recording_->heard.size()) + " knocks and actions");
        recording_job_ = std::async(
            std::launch::async, [finished = std::move(*recording_)] { write_recording(finished); });
        recording_.reset();
    }
    if (panel_) panel_->invalidate();
}

// Feeds knocks to a running calibration, judges the try step, or acts on a double or triple knock.
void App::handle_knocks(double t, const std::deque<Knock>& knocks, const std::optional<KnockEvent>& event) {
    // Opens the click stream on the first knock, so it is ready if a triple knock follows.
    if (config_.sounds && !config_.paused && !knocks.empty()) prepare_click();

    if (calibrator_ && calibration_ && calibration_->phase == Phase::Recording) {
        for (const auto& knock : knocks) calibrator_->hear(knock);
        while (const auto result = calibrator_->next(t)) {
            const auto& knock = result->knock;
            switch (result->heard) {
            case Calibrator::Heard::Counted:
                haptics_.buzz(1, t);
                calibration_->peaks = calibrator_->peaks();
                calibration_->speeds = calibrator_->speeds();
                calibration_->counted_t = t;
                calibration_->knocked_t = knock.t;
                break;
            case Calibrator::Heard::OtherKind:
                calibration_->ignored = knock.kind;
                calibration_->ignored_t = t;
                break;
            case Calibrator::Heard::TooLight: calibration_->too_light_t = t; break;
            case Calibrator::Heard::TooSoon: calibration_->too_soon_t = t; break;
            default: break;
            }
        }
        calibration_->seconds_left = static_cast<int>(std::ceil(calibrator_->seconds_left(t)));
        if (calibrator_->done()) {
            // Straight on to the try step, which judges knocks at the new thresholds.
            calibration_->phase = Phase::Trying;
            detector_ = KnockDetector(calibrator_->trial_config(config_.detector));
            haptics_.buzz(2, t + 0.3);
            log(heard_summary(*calibrator_)
                + ". Trying thresholds: " + thresholds_text(calibrator_->result()));
        } else if (calibrator_->seconds_left(t) <= 0) {
            log(heard_summary(*calibrator_));
            show("Time ran out after " + std::to_string(calibrator_->count()) + " of "
                    + std::to_string(Calibrator::kWanted) + " knocks. Nothing saved.",
                true);
            end_calibration();
        }
        return;
    }
    if (!event) return;
    const std::string what =
        std::string(" (") + sequence_name(*event) + " " + to_string(event->kind) + " knock)";
    if (calibrator_ && calibration_ && calibration_->phase == Phase::Trying) {
        // The try step judges knocks at the new threshold and never switches passthrough.
        const bool passed = event->kind == calibrator_->kind() && event->count == 2;
        calibration_->trial_passed = passed;
        calibration_->trial_heard =
            std::string("a ") + sequence_name(*event) + " " + to_string(event->kind) + " knock";
        calibration_->trial_t = t;
        log("Try step heard " + calibration_->trial_heard + (passed ? ": passed" : ": missed"));
        haptics_.buzz(passed ? 1 : 3, t);
        return;
    }
    if (calibrator_) return;
    // The tour's knock steps act even when paused or off in this game, so the user can practice.
    // Only while the panel shows them, so a tour left open never overrides the settings in a game.
    const bool practicing =
        tour_ && (tour_->step == Step::Double || tour_->step == Step::Triple) && panel_ && panel_->visible();
    if (config_.paused && !practicing) {
        log("Ignored" + what + ": paused");
    } else if (config_.blocked_apps.count(state_.app_key) && !practicing) {
        log("Ignored" + what + ": knocks are off in " + state_.app_key);
    } else {
        // The tour teaches the default actions, whatever Knock actions says.
        run_action(practicing ? KnockActions::default_for(event->count)
                              : config_.knock_actions.get(event->kind, event->count),
            *event, what, t);
    }
}

// Takes the results of finished background jobs: the recording, Room View, the update check, and the install.
void App::collect_jobs(double t) {
    if (ready(recording_job_)) {
        try {
            recording_job_.get();
            state_.recorded = true;
            log("Knock recording saved to " + recording_path().string());
            if (report_server_) {
                report_server_->set_report(report_text(true, room_view_text()));
                report_server_->set_recording(recording_path());
            }
        } catch (const std::exception& error) {
            state_.report_error = std::string("recording not saved: ") + error.what();
            log(std::string("Knock recording not saved: ") + error.what());
        }
        if (panel_) panel_->invalidate();
    }

    if (ready(room_view_job_)) {
        const bool toggled = room_view_job_toggles_;
        room_view_job_toggles_ = false;
        try {
            const auto result = room_view_job_.get();
            if (state_.room_view != result.on && panel_) panel_->invalidate();
            state_.room_view = result.on;
            state_.room_view_error.clear();
            dashboard_changed_ = false;
            if (!result.message.empty()) show(result.message);
        } catch (const DashboardChanged& error) {
            // Logged once per change, with the SteamVR version, for bug reports.
            if (!dashboard_changed_)
                log(std::string(error.what()) + ". SteamVR " + steamvr_version()
                    + ". A Steam update may have changed the dashboard.");
            dashboard_changed_ = true;
            state_.room_view.reset();
            state_.room_view_error =
                "A Steam update moved the Room View button. Update KnockThru in Settings.";
            if (toggled) {
                show("Passthrough not changed: Room View button not found", true);
                haptics_.buzz(3, t);
            }
        } catch (const std::exception& error) {
            state_.room_view.reset();
            const std::string reason = error.what();
            state_.room_view_error = reason.find("debug port") != std::string::npos
                ? "Turn on CEF Remote Debugging in the Frame's Developer settings."
                : reason;
            if (toggled) {
                show("Passthrough not changed: " + reason, true);
                haptics_.buzz(3, t);
            } else if (state_.message != reason) {
                show(reason, true);
            }
        }
    }

    if (ready(check_job_)) {
        try {
            const auto release = check_job_.get();
            const bool newer = compare_versions(release.version, kVersion) > 0;
            if (newer)
                latest_ = release;
            else
                latest_.reset();
            // An automatic check that finds nothing leaves the header as it was.
            if (newer || !check_is_automatic_)
                state_.update = {
                    newer ? UpdateStatus::Available : UpdateStatus::Current, release.version, ""};
            log("Update check: " + release.version + (newer ? " is available" : " is the latest"));
        } catch (const std::exception& error) {
            if (check_is_automatic_) {
                // Try again in an hour, not a day, since the Wi-Fi may only have been down.
                app_state_.last_update_check = static_cast<long long>(std::time(nullptr)) - 23 * 3600;
                save_app_state();
                log(std::string("Daily update check failed: ") + error.what());
            } else {
                state_.update = {UpdateStatus::Failed, "", error.what()};
                show(std::string("Update check failed: ") + error.what(), true);
            }
        }
        if (panel_) panel_->invalidate();
    }

    // The Installing button fills as the download runs. Whole percents are enough to redraw for.
    if (install_job_.valid() && std::abs(install_progress_ - state_.update_progress) >= 0.01f) {
        state_.update_progress = install_progress_;
        if (panel_) panel_->invalidate();
    }
    if (ready(install_job_)) {
        const auto version = latest_ ? latest_->version : "";
        try {
            install_job_.get();
            // The Restart button in Settings starts the new version now. Otherwise SteamVR starts it at its
            // next launch.
            log("Installed KnockThru " + version);
            state_.update = {UpdateStatus::Installed, version, ""};
        } catch (const std::exception& error) {
            std::string reason = error.what();
            if (!reason.empty() && reason.back() != '.') reason += '.';
            state_.update = {UpdateStatus::Failed, version, error.what()};
            state_.update_notice = UpdateNotice{true,
                "Version " + version + " was not installed. " + reason + " You still have version " + kVersion
                    + "."};
            log("Update not installed: " + reason);
        }
        if (panel_) panel_->invalidate();
    }

    // knockthru-ctl stops this process within seconds, so one still running after 30 s missed its restart.
    if (restart_started_ >= 0 && t - restart_started_ > 30) {
        restart_started_ = -1;
        log("Restart after the update did not happen");
        restart_failed(latest_ ? latest_->version : "");
        if (panel_) panel_->invalidate();
    }
}

void App::draw_panel(double t) {
    if (!panel_) return;
    state_.busy = room_view_job_.valid() && room_view_job_toggles_;
    state_.paused = config_.paused;
    state_.sounds = config_.sounds;
    state_.knock_actions = config_.knock_actions;
    state_.tour = tour_;
    panel_->set_bar_icon(config_.classic_bar_icon);
    state_.enabled = config_.detector.enabled;
    state_.app_blocked = config_.blocked_apps.count(state_.app_key) > 0;
    state_.calibration = calibration_;
    panel_->draw(state_, t);
}

#ifdef KNOCKTHRU_TEST_HOOK
// Development builds only (KNOCKTHRU_TEST_HOOK). Runs each line appended to
// ~/.config/knockthru/test-commands, so a script can drive the app over SSH:
//   open | close             the dashboard opens or closes the panel
//   click X Y                the laser pointer clicks panel pixel (X, Y), from the top left
//   press COMMAND [KIND]     a panel button by its PanelCommand name, such as CheckUpdate
//   knock KIND PEAK APPROACH one knock, with its spike in m/s^2 and approach speed in m/s
//   action KIND COUNT        a double (2) or triple (3) knock
//   snapshot PATH            saves the panel as a PNG
// Reopens the file for each read, because glibc keeps a stream at end of file
// once it got there, so an open stream never sees lines added later.
void App::run_test_commands(double t, std::deque<Knock>& knocks, std::optional<KnockEvent>& event) {
    std::error_code error;
    const auto size = fs::file_size(test_commands_, error);
    if (error || size <= test_read_) return;
    std::ifstream file(test_commands_, std::ios::binary);
    file.seekg(static_cast<std::streamoff>(test_read_));
    std::string text(size - test_read_, '\0');
    file.read(text.data(), static_cast<std::streamsize>(text.size()));
    const auto end = text.rfind('\n'); // a line still being written waits for the next loop
    if (end == std::string::npos) return;
    test_read_ += end + 1;
    std::istringstream lines(text.substr(0, end));
    for (std::string line; std::getline(lines, line);)
        if (!line.empty()) run_test_command(line, t, knocks, event);
}

void App::run_test_command(
    const std::string& line, double t, std::deque<Knock>& knocks, std::optional<KnockEvent>& event) {
    static const std::map<std::string, PanelCommand> commands{{"RoomView", PanelCommand::RoomView},
        {"Style", PanelCommand::Style}, {"Pause", PanelCommand::Pause}, {"Kind", PanelCommand::Kind},
        {"BlockApp", PanelCommand::BlockApp}, {"Calibrate", PanelCommand::Calibrate},
        {"CancelCalibration", PanelCommand::CancelCalibration},
        {"SaveCalibration", PanelCommand::SaveCalibration}, {"Sounds", PanelCommand::Sounds},
        {"StartTour", PanelCommand::StartTour}, {"TourNext", PanelCommand::TourNext},
        {"TourBack", PanelCommand::TourBack}, {"TourSkip", PanelCommand::TourSkip},
        {"FactoryReset", PanelCommand::FactoryReset}, {"CheckUpdate", PanelCommand::CheckUpdate},
        {"InstallUpdate", PanelCommand::InstallUpdate}, {"RestartForUpdate", PanelCommand::RestartForUpdate},
        {"DismissUpdateNotice", PanelCommand::DismissUpdateNotice}, {"ShowReport", PanelCommand::ShowReport},
        {"CloseReport", PanelCommand::CloseReport}, {"SaveReport", PanelCommand::SaveReport},
        {"RecordKnocks", PanelCommand::RecordKnocks}};
    log("Test: " + line);
    std::istringstream in(line);
    std::string verb, name;
    in >> verb;
    const auto kind_of = [&](const std::string& text) {
        const auto kind = parse_knock_kind(text);
        if (!kind) throw std::runtime_error("unknown knock kind " + text);
        return *kind;
    };
    try {
        if (!panel_ && verb != "knock" && verb != "action")
            throw std::runtime_error("the panel did not load");
        if (verb == "open") {
            panel_->test_open();
        } else if (verb == "close") {
            panel_->test_close();
        } else if (verb == "click") {
            float x = 0, y = 0;
            if (!(in >> x >> y)) throw std::runtime_error("click needs X and Y");
            panel_->test_click(x, y);
        } else if (verb == "press") {
            in >> name;
            const auto command = commands.find(name);
            if (command == commands.end()) throw std::runtime_error("unknown command " + name);
            PanelClick click{command->second};
            if (std::string kind; in >> kind) click.kind = kind_of(kind);
            panel_->test_press(click);
        } else if (verb == "knock") {
            double peak = 0, approach = 0;
            if (!(in >> name >> peak >> approach)) throw std::runtime_error("knock needs KIND PEAK APPROACH");
            const auto kind = kind_of(name);
            const int source = kind == KnockKind::Together ? -1 : kind == KnockKind::Headset ? 0 : 2;
            knocks.push_back(Knock{t, kind, source, {0, 0, 0}, peak, 0, approach});
        } else if (verb == "action") {
            int count = 0;
            if (!(in >> name >> count) || (count != 2 && count != 3))
                throw std::runtime_error("action needs KIND 2 or 3");
            event = KnockEvent{t, kind_of(name), count};
        } else if (verb == "snapshot") {
            std::string path;
            std::getline(in >> std::ws, path);
            if (!panel_->test_snapshot(state_, t, path)) throw std::runtime_error("cannot write " + path);
        } else {
            throw std::runtime_error("unknown test command");
        }
    } catch (const std::exception& error) {
        log(std::string("Test failed: ") + error.what());
    }
}
#endif

void App::save_app_state() {
    try {
        save_state(app_state_);
    } catch (const std::exception& error) {
        log(std::string("State not saved: ") + error.what());
    }
}

// The modification times of the settings files, to notice an edit.
std::string App::config_stamp() const {
    std::string stamp;
    for (const auto& path : config_files_) {
        std::error_code error;
        stamp += std::to_string(
                     static_cast<long long>(fs::last_write_time(path, error).time_since_epoch().count()))
            + ",";
    }
    return stamp;
}

void App::show(const std::string& message, bool error) {
    log(message);
    if (error) last_error_ = message;
    state_.message = message;
    state_.message_is_error = error;
    state_.calibration_saved.reset();
    if (panel_) panel_->invalidate();
}

void App::reload() {
    config_ = load_config(install_directory());
    stamp_ = config_stamp();
    if (!calibrator_) detector_ = KnockDetector(config_.detector);
}

// SteamVR's dashboard click. Steam already clicks when the Room View button
// is pressed, so a double knock and the panel's Room View button stay silent.
void App::play_click() {
    if (config_.sounds) knockthru::play_click();
}

// A new tour counts kinds calibrated earlier as done, so a replay only asks for the rest.
void App::start_tour() {
    tour_.emplace();
    try {
        auto calibrated = load_calibration();
        for (const auto kind : {KnockKind::Together, KnockKind::Surface, KnockKind::Headset})
            tour_->calibrated[static_cast<int>(kind)] = calibrated[kind].has_value();
    } catch (const std::exception& error) {
        log(error.what());
    }
}

void App::read_room_view() {
    if (room_view_job_.valid()) return;
    const RoomView room_view(config_.debug_port);
    room_view_job_toggles_ = false;
    room_view_job_ =
        std::async(std::launch::async, [room_view] { return RoomViewResult{room_view.active(), ""}; });
}

void App::read_opaque() {
    try {
        state_.opaque = read_style() == RoomViewStyle::Opaque;
    } catch (const std::exception&) {
        state_.opaque.reset();
    }
}

bool App::toggle_room_view(const std::string& why) {
    if (room_view_job_.valid() && room_view_job_toggles_) {
        log("Ignored" + why + ": passthrough is still switching");
        return false;
    }
    if (room_view_job_.valid()) room_view_job_.wait(); // a plain read, at most a few ms
    const RoomView room_view(config_.debug_port);
    room_view_job_toggles_ = true;
    room_view_job_ = std::async(std::launch::async, [room_view, why] {
        const bool on = room_view.toggle();
        return RoomViewResult{on, std::string("Passthrough ") + (on ? "on" : "off") + why};
    });
    return true;
}

// Runs a knock action. Success buzzes once after knocking twice, and two times after knocking thrice.
// A failure buzzes 3 times.
void App::run_action(KnockAction action, const KnockEvent& event, const std::string& why, double t) {
    const int buzzes = event.count - 1;
    const auto tick_tour = [&] {
        if (!tour_) return;
        if (tour_->step == Step::Double && event.count == 2) tour_->double_t = t;
        if (tour_->step == Step::Triple && event.count == 3) tour_->triple_t = t;
    };
    switch (action) {
    case KnockAction::Passthrough:
        if (toggle_room_view(why)) {
            // Buzz now, because the switch can take a second. A failure buzzes 3 times later.
            haptics_.buzz(buzzes, t);
            tick_tour();
        }
        return;
    case KnockAction::Style:
    case KnockAction::PerfGraph: {
        // Steam clicks when passthrough switches, so KnockThru clicks only for these.
        const bool changed = action == KnockAction::Style ? toggle_style(why) : toggle_perf_overlay(why);
        if (changed) play_click();
        haptics_.buzz(changed ? buzzes : 3, t);
        if (changed) tick_tour();
        return;
    }
    case KnockAction::None: log("Ignored" + why + ": Knock actions says do nothing"); return;
    }
}

bool App::toggle_perf_overlay(const std::string& why) {
    try {
        show(std::string("Performance graph ") + (knockthru::toggle_perf_overlay() ? "on" : "off") + why);
        return true;
    } catch (const std::exception& error) {
        show(std::string("Performance graph not changed: ") + error.what(), true);
        return false;
    }
}

bool App::toggle_style(const std::string& why) {
    try {
        const auto style = knockthru::toggle_style();
        state_.opaque = style == RoomViewStyle::Opaque;
        show(std::string("Style ") + to_string(style) + why);
        return true;
    } catch (const std::exception& error) {
        show(std::string("Style not changed: ") + error.what(), true);
        return false;
    }
}

// Runs `write`, then reloads the settings. Shows the error if `write` throws.
bool App::save(const std::function<void()>& write) {
    try {
        write();
        reload();
        return true;
    } catch (const std::exception& error) {
        show(std::string("Settings not saved: ") + error.what(), true);
        return false;
    }
}

void App::start_check() {
    check_job_ = std::async(std::launch::async,
        [work = update_work_, cancel = &cancel_downloads_] { return fetch_latest_release(work, cancel); });
}

// The new files stay in place when a restart fails, so Settings keeps its Restart button.
void App::restart_failed(const std::string& version) {
    state_.update = {UpdateStatus::Installed, version, ""};
    state_.update_notice = UpdateNotice{true,
        "Version " + version
            + " is installed, but KnockThru did not restart. Press Restart in Settings to try again, or "
              "restart SteamVR."};
}

// Leaves the calibration walkthrough and goes back to the saved thresholds.
void App::end_calibration() {
    calibrator_.reset();
    calibration_.reset();
    detector_ = KnockDetector(config_.detector);
}

std::string App::room_view_text() const {
    return !state_.room_view_error.empty() ? state_.room_view_error
        : !state_.room_view                ? "not read yet"
        : *state_.room_view                ? "on"
                                           : "off";
}

} // namespace

int run_app() {
    open_log();
    log(std::string("KnockThru ") + kVersion + " starting");
    VrSession session(vr::VRApplication_Overlay, true);
    log("SteamVR " + steamvr_version());
    App().run();
    return 0;
}

} // namespace knockthru
