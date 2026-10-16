#pragma once

#include "canvas.hpp"
#include "knock_detector.hpp"

#include <array>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace knockthru {

// A calibration in progress, filled in by the app.
struct CalibrationView {
    enum class Phase { Recording, Trying };
    KnockKind kind = KnockKind::Together;
    Phase phase = Phase::Recording;
    int wanted = 10, seconds_left = 0;
    std::vector<double> peaks;          // counted knocks, oldest first, in m/s^2
    std::vector<double> speeds;         // the same knocks' approach speeds, in m/s, which the chart shows
    double started_t = -1;              // when recording started
    double counted_t = -1;              // when the latest knock counted
    double knocked_t = -1;              // when that knock happened, which the pacing ring runs from
    std::optional<KnockKind> ignored;   // the latest knock of another kind
    double ignored_t = -1;
    double too_light_t = -1;            // when the latest knock of this kind was too light to count
    double too_soon_t = -1;             // when the latest knock came before the pacing ring filled
    std::optional<bool> trial_passed;   // unset until the try step hears a knock sequence
    std::string trial_heard;            // what the try step heard, for a miss
    double trial_t = -1;
};

// The first-run tour, filled in by the app. The knock steps really switch
// passthrough and style, and tick off when they hear their knock.
struct TourView {
    enum class Step { Welcome, Kinds, Calibrate, Double, Triple, Done };
    Step step = Step::Welcome;
    std::array<bool, 3> calibrated{}; // per KnockKind, calibrated before or during this tour
    double double_t = -1; // when the double knock step heard a double knock
    double triple_t = -1; // when the triple knock step heard a triple knock
};

// The update check in Settings, filled in by the app.
struct UpdateView {
    enum class Status { Idle, Checking, Current, Available, Installing, Failed };
    Status status = Status::Idle;
    std::string latest; // the newest release, once a check found one
    std::string error;  // why the check or the install failed
};

// Everything the dashboard panel shows. The app fills it in, the panel draws it.
// Times are seconds on the app clock, the same clock render() gets.
struct PanelState {
    std::optional<bool> room_view;   // unknown until the first debug port read
    std::string room_view_error;     // why Room View cannot be read or changed
    bool dashboard_changed = false;  // the Room View button is missing, likely after a Steam update
    std::optional<bool> opaque;      // Room View style, unknown without SteamVR
    bool busy = false;               // a Room View change is in progress
    bool paused = false;
    bool classic_bar_icon = false;   // the bar icon in the logo colors
    bool sounds = true;              // SteamVR's click on a triple knock and on panel buttons
    std::array<bool, 3> enabled{true, true, true}; // per KnockKind
    std::string app_key, app_name;   // the running game, empty when none
    bool app_blocked = false;
    std::optional<CalibrationView> calibration;
    std::optional<TourView> tour;
    std::optional<KnockKind> calibration_saved; // the kind the last calibration just saved
    std::string version;             // this KnockThru's version
    UpdateView update;
    std::string message;
    bool message_is_error = false;
};

enum class IconStyle {
    Tile,     // the logo on a dark rounded tile, for the app icon and the panel header
    BarWhite, // the mark in white, which Steam's tint turns grey
    BarColor, // the mark in the logo colors
};

enum class PanelCommand {
    RoomView, Style, Pause, Kind, BlockApp, BarIcon,
    Calibrate,          // starts recording a kind, or records it again
    CancelCalibration,
    SaveCalibration,
    Navigate,           // a button that only changes the screen, such as Back
    Sounds,
    StartTour, TourNext, TourBack,
    TourSkip,           // ends the tour from any step, including Done
    FactoryReset,
    CheckUpdate, InstallUpdate,
    SaveReport,         // writes the bug report to the home folder
};

struct PanelClick {
    PanelCommand command;
    KnockKind kind = KnockKind::Together; // for Kind and Calibrate
};

// The KnockThru dashboard panel, drawn into an RGBA image. It has a main
// screen, a settings screen with a factory reset confirm, a calibration
// walkthrough in four steps (pick a kind, learn the knock, record, and try
// it), and the first-run tour in six.
class Panel {
public:
    static constexpr int kWidth = 1200, kHeight = 770;

    // `fonts` holds NotoSans-SemiBold.ttf and NotoSans-Regular.ttf.
    explicit Panel(const std::filesystem::path& fonts);

    // Draws the panel at time `now`. `hover` is the pointer position in panel pixels.
    const Canvas& render(const PanelState& state, std::optional<std::pair<float, float>> hover = std::nullopt,
                         double now = 0);

    // Whether the last render had something moving, so the next frame differs.
    bool animating() const { return animating_; }

    // Handles a click at a point, using the controls from the last render.
    // Row 0 is the top. Switching screens happens here and returns nothing.
    std::optional<PanelClick> click(float x, float y);

    // Draws the KnockThru icon to fill a square canvas. The bar styles draw the mark alone on
    // transparency, as large as fits, with wider gaps for the dashboard bar's small size.
    static void draw_icon(Canvas& canvas, IconStyle style = IconStyle::Tile);

private:
    enum class Screen { Main, Settings, Reset, Pick, Learn, SkipTour };
    // What is on screen, for transitions: a screen, a calibration phase, or the tour.
    enum class View { Main, Settings, Reset, Pick, Learn, Record, Try, Tour };
    struct Target {
        Rect rect;
        std::optional<PanelClick> click = {};
        std::optional<Screen> go_to = {};
        std::optional<KnockKind> learn = {}; // the kind the Learn screen shows
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
    std::optional<TourView::Step> tour_step_; // the tour step on screen, so each step rises in
    double view_t_ = 0;

    bool hovered(Rect rect) const;
    void header(const std::string& title);
    void main_screen(const PanelState& state);
    void pick_screen(const PanelState& state);
    void learn_screen(const PanelState& state);
    void record_screen(const PanelState& state, const CalibrationView& view);
    void try_screen(const PanelState& state, const CalibrationView& view);
    void tour_screen(const PanelState& state, const TourView& tour);
    void tour_knock_step(const PanelState& state, const TourView& tour);
    void tour_buttons(bool first, bool last, bool complete);
    void confirm_card(const std::string& title, const std::string& text, const std::string& cancel, Screen cancel_to,
                      const std::string& confirm, PanelClick confirm_click, bool danger);
    void steps(int current, int total);
    void check_mark(float cx, float cy, float radius, float pop);
    void knock_scene(KnockKind kind, Rect box, float t, int knocks);
    void settings_screen(const PanelState& state);
    void knocks_row(Rect rect, const PanelState& state);
    void passthrough_tile(Rect rect, const PanelState& state);
    void style_control(Rect rect, const PanelState& state);
    void switch_row(Rect rect, std::optional<PanelClick> click, bool on, const std::string& title,
                    const std::string& detail, bool enabled, bool divider);
    void button_row(Rect rect, const std::string& title, const std::string& detail, const std::string& label,
                    std::optional<PanelClick> click, std::optional<Screen> go_to, bool danger, bool divider = true);
    void button(Rect rect, const std::string& label, std::optional<PanelClick> click, std::optional<Screen> go_to,
                bool primary = false, bool enabled = true, bool danger = false);
    void switch_control(float right, float cy, bool on, bool enabled, float scale);
    void message(const PanelState& state, float y);
};

} // namespace knockthru
