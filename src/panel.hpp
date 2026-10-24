#pragma once

#include "canvas.hpp"
#include "knock_actions.hpp"
#include "knock_detector.hpp"

#include <array>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace knockthru {

// The app fills these in. Times are seconds on the clock render() gets, and -1 is "not yet".

struct CalibrationView {
    enum class Phase { Recording, Trying };
    KnockKind kind = KnockKind::Together;
    Phase phase = Phase::Recording;
    int wanted = 10, seconds_left = 0;
    std::vector<double> peaks, speeds; // counted knocks, oldest first: m/s^2 and m/s
    double started_t = -1;
    double counted_t = -1;
    double knocked_t = -1; // when the counted knock happened. The pacing ring runs from here.
    std::optional<KnockKind> ignored;
    double ignored_t = -1;
    double too_light_t = -1;
    double too_soon_t = -1;
    std::optional<bool> trial_passed; // unset until the try step hears a sequence
    std::string trial_heard; // shown on a miss
    double trial_t = -1;
};

// The tour's knock steps really switch passthrough and style.
struct TourView {
    enum class Step { Welcome, Kinds, Calibrate, Double, Triple, Done };
    Step step = Step::Welcome;
    std::array<bool, 3> calibrated{}; // per KnockKind, before or during this tour
    double double_t = -1;
    double triple_t = -1;
};

struct UpdateView {
    enum class Status {
        Idle,
        Checking,
        Current,
        Available,
        Installing,
        Installed,
        Restarting,
        Updated,
        Failed
    };
    Status status = Status::Idle;
    std::string latest; // the newest release. For Updated, the version before.
    std::string error;
};

// After an update, the new process shows success and the old one shows a failure.
struct UpdateNotice {
    bool failed = false;
    std::string text;
};

struct PanelState {
    std::optional<bool> room_view; // unset until the first debug port read
    std::string room_view_error;
    std::optional<bool> opaque; // unset without SteamVR
    bool busy = false; // a Room View change is in progress
    bool paused = false;
    bool sounds = true;
    std::array<bool, 3> enabled{true, true, true}; // per KnockKind
    KnockActions knock_actions;
    std::string app_key, app_name; // the running game, empty when none
    bool app_blocked = false;
    std::optional<CalibrationView> calibration;
    std::optional<TourView> tour;
    std::optional<KnockKind> calibration_saved;
    std::string version;
    UpdateView update;
    float update_progress = 0;
    std::optional<UpdateNotice> update_notice; // covers every screen until OK
    std::string report_url; // the QR code's link
    bool report_full = false; // report_url is the headset's page, not the bare GitHub form
    std::string report_saved;
    std::string report_error;
    int recording_left = -1; // seconds
    int recording_knocks = 0;
    bool recorded = false;
    std::string message;
    bool message_is_error = false;
};

enum class IconStyle {
    Tile, // on a dark rounded tile, for the app icon and the panel header
    BarWhite, // Steam's tint turns it grey
    BarColor,
};

enum class PanelCommand {
    RoomView,
    Style,
    Pause,
    Kind,
    BlockApp,
    Calibrate,
    CancelCalibration,
    SaveCalibration,
    Navigate, // only changes the screen, like Back
    Sounds,
    StartTour,
    TourNext,
    TourBack,
    TourSkip, // also Done on the last step
    FactoryReset,
    CheckUpdate,
    InstallUpdate,
    RestartForUpdate,
    DismissUpdateNotice,
    ShowReport, // starts the page the QR code opens
    CloseReport,
    SaveReport, // to the home folder
    RecordKnocks,
    SetKnockAction, // sets every kind at once
};

struct PanelClick {
    PanelCommand command;
    KnockKind kind = KnockKind::Together; // for Kind and Calibrate
    int count = 0; // for SetKnockAction
    KnockAction action = KnockAction::None;
};

// Draws the dashboard tab into an RGBA image.
class Panel {
public:
    static constexpr int kWidth = 1200, kHeight = 770;

    // `fonts` holds NotoSans-SemiBold.ttf and NotoSans-Regular.ttf.
    explicit Panel(const std::filesystem::path& fonts);

    // `hover` is the pointer position in panel pixels.
    const Canvas& render(
        const PanelState& state, std::optional<std::pair<float, float>> hover = std::nullopt, double now = 0);

    bool animating() const { return animating_; }

    // Hits the controls from the last render, row 0 at the top. Screen changes happen here.
    std::optional<PanelClick> click(float x, float y);

    // Fills a square canvas. The bar styles leave wider gaps, for the bar's small size.
    static void draw_icon(Canvas& canvas, IconStyle style = IconStyle::Tile);

private:
    enum class Screen { Main, Settings, Reset, Report, Pick, Learn, SkipTour, Actions };
    // What is on screen, for transitions: a screen, a calibration phase, or the tour.
    enum class View { Main, Settings, Reset, Report, Pick, Learn, Record, Try, Tour, Actions };
    struct Target {
        Rect rect;
        std::optional<PanelClick> click = {};
        std::optional<Screen> go_to = {};
        std::optional<KnockKind> learn = {};
    };

    Font bold_, regular_;
    Canvas canvas_, scratch_, background_;
    Screen screen_ = Screen::Main;
    KnockKind learn_kind_ = KnockKind::Together;
    bool saved_dismissed_ = false;
    bool was_calibrating_ = false;
    bool was_touring_ = false;
    std::vector<Target> targets_;
    std::optional<std::pair<float, float>> hover_;
    double now_ = 0;
    bool animating_ = false;
    std::optional<View> view_;
    std::optional<TourView::Step> tour_step_;
    double view_t_ = 0;
    std::map<std::pair<int, IconStyle>, Canvas> icons_;

    const Canvas& icon(int size, IconStyle style);
    bool hovered(Rect rect) const;
    float paragraph(Font& font, const std::string& text, float x, float y, float size, float leading,
        float width, int max_lines, Color color);
    void header(const std::string& title);
    void main_screen(const PanelState& state);
    void pick_screen(const PanelState& state);
    void learn_screen(const PanelState& state);
    void record_screen(const PanelState& state, const CalibrationView& view);
    void try_screen(const PanelState& state, const CalibrationView& view);
    void tour_screen(const PanelState& state, const TourView& tour);
    void tour_knock_step(const PanelState& state, const TourView& tour);
    void tour_buttons(bool first, bool last, bool complete);
    void confirm_card(const std::string& title, const std::string& text, const std::string& cancel,
        Screen cancel_to, const std::string& confirm, PanelClick confirm_click, bool danger);
    void steps(int current, int total);
    void check_mark(float cx, float cy, float radius, float pop);
    void knock_scene(KnockKind kind, Rect box, float t, int knocks);
    void settings_screen(const PanelState& state);
    void actions_screen(const PanelState& state);
    void action_list(Rect rect, const PanelState& state, int count);
    void update_control(const PanelState& state);
    void report_card(const PanelState& state);
    void update_card(const UpdateNotice& notice);
    void alert_mark(float cx, float cy, float scale, Color color);
    void progress_button(Rect rect, const std::string& label, float progress);
    void knocks_row(Rect rect, const PanelState& state);
    void passthrough_tile(Rect rect, const PanelState& state);
    void style_control(Rect rect, const PanelState& state);
    void switch_row(Rect rect, std::optional<PanelClick> click, bool on, const std::string& title,
        const std::string& detail, bool enabled, bool divider);
    void button_row(Rect rect, const std::string& title, const std::string& detail, const std::string& label,
        std::optional<PanelClick> click, std::optional<Screen> go_to, bool danger, bool divider = true);
    void button(Rect rect, const std::string& label, std::optional<PanelClick> click,
        std::optional<Screen> go_to, bool primary = false, bool enabled = true, bool danger = false);
    void switch_control(float right, float cy, bool on, bool enabled, float scale);
    void message(const PanelState& state, float y);
};

} // namespace knockthru
