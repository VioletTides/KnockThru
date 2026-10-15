#pragma once

#include "canvas.hpp"
#include "knock_detector.hpp"

#include <array>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace knockthru {

// Everything the dashboard panel shows. The app fills it in, the panel draws it.
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
    std::optional<KnockKind> calibrating;
    int calibration_count = 0, calibration_wanted = 10, calibration_seconds_left = 0;
    bool calibration_saved = false;  // the last calibration just saved
    std::string message;
    bool message_is_error = false;
};

enum class IconStyle {
    Tile,     // the logo on a dark rounded tile, for the app icon and the panel header
    BarWhite, // the mark in white, which Steam's tint turns grey
    BarColor, // the mark in the logo colors
};

enum class PanelCommand { RoomView, Style, Pause, Kind, BlockApp, Calibrate, CancelCalibration, BarIcon };

struct PanelClick {
    PanelCommand command;
    KnockKind kind = KnockKind::Together; // for Kind and Calibrate
};

// The KnockThru dashboard panel, drawn into an RGBA image. It has a main
// screen, a calibration screen, and a settings screen.
class Panel {
public:
    static constexpr int kWidth = 1200, kHeight = 770;

    // `fonts` holds NotoSans-SemiBold.ttf and NotoSans-Regular.ttf.
    explicit Panel(const std::filesystem::path& fonts);

    // Draws the panel. `hover` is the pointer position in panel pixels.
    const Canvas& render(const PanelState& state, std::optional<std::pair<float, float>> hover = std::nullopt);

    // Handles a click at a point, using the controls from the last render.
    // Row 0 is the top. Switching screens happens here and returns nothing.
    std::optional<PanelClick> click(float x, float y);

    // Draws the KnockThru icon to fill a square canvas. The bar styles draw the mark alone on
    // transparency, as large as fits, with wider gaps for the dashboard bar's small size.
    static void draw_icon(Canvas& canvas, IconStyle style = IconStyle::Tile);

private:
    enum class Screen { Main, Calibrate, Settings };
    struct Target {
        Rect rect;
        std::optional<PanelClick> click;
        std::optional<Screen> go_to;
    };

    Font bold_, regular_;
    Canvas canvas_;
    Screen screen_ = Screen::Main;
    bool saved_dismissed_ = false;
    std::vector<Target> targets_;
    std::optional<std::pair<float, float>> hover_;

    bool hovered(Rect rect) const;
    void header(const std::string& title);
    void main_screen(const PanelState& state);
    void calibrate_screen(const PanelState& state);
    void settings_screen(const PanelState& state);
    void knocks_row(Rect rect, const PanelState& state);
    void passthrough_tile(Rect rect, const PanelState& state);
    void style_control(Rect rect, const PanelState& state);
    void switch_row(Rect rect, std::optional<PanelClick> click, bool on, const std::string& title,
                    const std::string& detail, bool enabled, bool divider);
    void button(Rect rect, const std::string& label, std::optional<PanelClick> click, std::optional<Screen> go_to,
                bool primary = false);
    void switch_control(float right, float cy, bool on, bool enabled, float scale);
    void message(const PanelState& state, float y);
};

} // namespace knockthru
