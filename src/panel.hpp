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
    enum class Phase { Recording, Review, Trying };
    KnockKind kind = KnockKind::Together;
    Phase phase = Phase::Recording;
    int wanted = 10, seconds_left = 0;
    std::vector<double> peaks;          // counted knocks, oldest first, in m/s^2
    std::vector<double> speeds;         // the same knocks' approach speeds, in m/s, which the chart shows
    double started_t = -1;              // when recording started
    double counted_t = -1;              // when the latest knock counted
    std::optional<KnockKind> ignored;   // the latest knock of another kind
    double ignored_t = -1;
    double too_light_t = -1;            // when the latest knock of this kind was too light to count
    double sensitivity = 0.5;           // 0 is the least sensitive, 1 the most
    std::optional<bool> trial_passed;   // unset until the try step hears a knock sequence
    std::string trial_heard;            // what the try step heard, for a miss
    double trial_t = -1;
};

// Everything the dashboard panel shows. The app fills it in, the panel draws it.
// Times are seconds on the app clock, the same clock render() gets.
struct PanelState {
    std::optional<bool> room_view;   // unknown until the first debug port read
    std::string room_view_error;     // why Room View cannot be read or changed
    std::optional<bool> opaque;      // Room View style, unknown without SteamVR
    bool busy = false;               // a Room View change is in progress
    bool paused = false;
    bool classic_bar_icon = false;   // the bar icon in the logo colors
    std::array<bool, 3> enabled{true, true, true}; // per KnockKind
    std::string app_key, app_name;   // the running game, empty when none
    bool app_blocked = false;
    std::optional<CalibrationView> calibration;
    bool calibration_saved = false;  // the last calibration just saved
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
    TryCalibration,     // from the review step to the try step
    AdjustCalibration,  // from the try step back to the review step
    Sensitivity,        // the slider moved
    SaveCalibration,
};

struct PanelClick {
    PanelCommand command;
    KnockKind kind = KnockKind::Together; // for Kind and Calibrate
    double value = 0;                     // for Sensitivity
};

// The KnockThru dashboard panel, drawn into an RGBA image. It has a main
// screen, a settings screen, and a calibration walkthrough in five steps:
// pick a kind, learn the knock, record, adjust sensitivity, and try it.
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

    // Pointer input in panel pixels, row 0 at the top, using the controls from the
    // last render. A press on the slider starts a drag, and moves update it.
    void press(float x, float y);
    void move(float x, float y);
    // Handles a release: a click, or the end of a slider drag. Switching
    // screens happens here and returns nothing.
    std::optional<PanelClick> click(float x, float y);

    // Draws the KnockThru icon to fill a square canvas. The bar styles draw the mark alone on
    // transparency, as large as fits, with wider gaps for the dashboard bar's small size.
    static void draw_icon(Canvas& canvas, IconStyle style = IconStyle::Tile);

private:
    enum class Screen { Main, Settings, Pick, Learn };
    // What is on screen, for transitions: a screen, or a calibration phase.
    enum class View { Main, Settings, Pick, Learn, Record, Review, Try };
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
    std::vector<Target> targets_;
    std::optional<std::pair<float, float>> hover_;
    double now_ = 0;
    bool animating_ = false;
    std::optional<View> view_;
    double view_t_ = 0;
    // The sensitivity control is the threshold line on the knock chart. A press
    // anywhere on the chart drags it. While dragging, the panel shows its own
    // value. After a release near a notch, the line slides into it.
    Rect slider_{};                          // the chart, where a press starts a drag
    std::vector<float> slider_ys_;           // the line's height at sensitivity 0, 0.01, ... 1
    bool dragging_ = false;
    double drag_value_ = 0.5, snap_from_ = 0.5, snap_to_ = 0.5, snap_t_ = -1;

    bool hovered(Rect rect) const;
    void header(const std::string& title);
    void main_screen(const PanelState& state);
    void pick_screen(const PanelState& state);
    void learn_screen(const PanelState& state);
    void record_screen(const PanelState& state, const CalibrationView& view);
    void review_screen(const PanelState& state, const CalibrationView& view);
    void try_screen(const PanelState& state, const CalibrationView& view);
    void steps(int current);
    void knock_scene(KnockKind kind, Rect box, float t, int taps);
    double slider_value(const CalibrationView& view) const;
    double slider_at(float y) const;
    void settings_screen(const PanelState& state);
    void knocks_row(Rect rect, const PanelState& state);
    void passthrough_tile(Rect rect, const PanelState& state);
    void style_control(Rect rect, const PanelState& state);
    void switch_row(Rect rect, std::optional<PanelClick> click, bool on, const std::string& title,
                    const std::string& detail, bool enabled, bool divider);
    void button(Rect rect, const std::string& label, std::optional<PanelClick> click, std::optional<Screen> go_to,
                bool primary = false, bool enabled = true);
    void switch_control(float right, float cy, bool on, bool enabled, float scale);
    void message(const PanelState& state, float y);
};

} // namespace knockthru
