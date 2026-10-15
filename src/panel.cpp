#include "panel.hpp"

#include "logo_shapes.hpp"

namespace knockthru {
namespace {

constexpr Color kBackground{18, 20, 24, 248};
constexpr Color kSurface{28, 31, 37};
constexpr Color kSurfaceHover{36, 40, 47};
constexpr Color kRaised{44, 48, 57};
constexpr Color kRaisedHover{54, 59, 69};
constexpr Color kHairline{46, 50, 58};
constexpr Color kText{236, 238, 241};
constexpr Color kDim{150, 156, 168};
constexpr Color kFaint{96, 102, 114};
constexpr Color kAccent{58, 145, 232};
constexpr Color kAccentHover{80, 160, 240};
constexpr Color kKnobOff{196, 200, 208};
constexpr Color kError{240, 110, 100};
// The knocks row is a tint of the accent while knocks act, so the passthrough tile keeps the full accent.
constexpr Color kActive{34, 62, 94};
constexpr Color kActiveHover{40, 72, 108};

// Containers use a 16 px radius. Switches and the style control are pills.
constexpr float kRadius = 16;
constexpr float kMargin = 40;

constexpr const char* kKindTitles[3] = {"Controllers together", "Controller on a surface", "Tap on the headset"};
constexpr const char* kKindHowTo[3] = {"Knock the controllers together 10 times, at least 1 second apart.",
                                       "Knock one controller on a table or wall 10 times, at least 1 second apart.",
                                       "Tap the side of the headset 10 times, at least 1 second apart."};

} // namespace

Panel::Panel(const std::filesystem::path& fonts)
    : bold_(fonts / "NotoSans-SemiBold.ttf"), regular_(fonts / "NotoSans-Regular.ttf"), canvas_(kWidth, kHeight) {}

bool Panel::hovered(Rect rect) const { return hover_ && rect.contains(hover_->first, hover_->second); }

void Panel::switch_control(float right, float cy, bool on, bool enabled, float scale) {
    const float h = 48 * scale, w = 80 * scale, r = h / 2;
    const Rect track{right - w, cy - r, w, h};
    canvas_.rounded_rect(track, r, !enabled ? kSurfaceHover : on ? kAccent : kRaisedHover);
    canvas_.circle(on ? track.x + track.w - r : track.x + r, cy, r - 6 * scale, !enabled ? kFaint : on ? kText : kKnobOff);
}

void Panel::button(Rect rect, const std::string& label, std::optional<PanelClick> click, std::optional<Screen> go_to,
                   bool primary) {
    const bool hover = hovered(rect);
    canvas_.rounded_rect(rect, kRadius, primary ? (hover ? kAccentHover : kAccent) : (hover ? kRaisedHover : kRaised));
    canvas_.text(bold_, label, rect.x + rect.w / 2, rect.y + rect.h / 2, 28, kText, Align::Center);
    targets_.push_back({rect, click, go_to});
}

void Panel::header(const std::string& title) {
    Canvas icon(56, 56);
    draw_icon(icon);
    canvas_.draw(icon, static_cast<int>(kMargin), 40);
    canvas_.text(bold_, title, kMargin + 76, 68, 38, kText);
}

void Panel::message(const PanelState& state, float y) {
    for (const auto& line : canvas_.wrap(regular_, state.message, 26, kWidth - 2 * kMargin, 2)) {
        canvas_.text(regular_, line, kMargin, y, 26, state.message_is_error ? kError : kDim);
        y += 36;
    }
}

void Panel::passthrough_tile(Rect rect, const PanelState& state) {
    const bool on = state.room_view.value_or(false);
    const bool broken = !state.room_view_error.empty();
    const bool hover = hovered(rect);
    canvas_.rounded_rect(rect, kRadius, on ? (hover ? kAccentHover : kAccent) : (hover ? kRaisedHover : kRaised));
    const float x = rect.x + 36, w = rect.w - 72;
    canvas_.text(bold_, "Passthrough", x, rect.y + 52, 30, kText);
    const std::string value = state.busy ? "Switching" : broken ? "Unavailable" : !state.room_view ? "Checking" : on ? "On" : "Off";
    canvas_.text(bold_, value, x, rect.y + 160, broken ? 64 : 96, kText);
    if (broken) {
        float y = rect.y + rect.h - 92;
        for (const auto& line : canvas_.wrap(regular_, state.room_view_error, 26, w, 2)) {
            canvas_.text(regular_, line, x, y, 26, kText);
            y += 36;
        }
    } else {
        const std::string hint = on ? "Select to turn off, or knock twice" : "Select to turn on, or knock twice";
        canvas_.text(regular_, hint, x, rect.y + rect.h - 48, 26, on ? kText : kDim);
    }
    targets_.push_back({rect, PanelClick{PanelCommand::RoomView}, std::nullopt});
}

void Panel::style_control(Rect rect, const PanelState& state) {
    canvas_.text(bold_, "Style", rect.x, rect.y + 16, 28, kText);
    canvas_.text(regular_, "or knock three times", rect.x + rect.w, rect.y + 16, 26, kDim, Align::Right);
    const Rect track{rect.x, rect.y + 46, rect.w, 84};
    canvas_.rounded_rect(track, track.h / 2, kSurface);
    const char* names[2] = {"Translucent", "Opaque"};
    for (int i = 0; i < 2; ++i) {
        const Rect half{track.x + 6 + i * (track.w - 12) / 2, track.y + 6, (track.w - 12) / 2, track.h - 12};
        const bool selected = state.opaque && *state.opaque == (i == 1);
        if (selected) {
            canvas_.rounded_rect(half, half.h / 2, kRaisedHover);
        } else {
            if (hovered(half)) canvas_.rounded_rect(half, half.h / 2, kSurfaceHover);
            if (state.opaque) targets_.push_back({half, PanelClick{PanelCommand::Style}, std::nullopt});
        }
        canvas_.text(bold_, names[i], half.x + half.w / 2, half.y + half.h / 2, 28, selected ? kText : kDim,
            Align::Center);
    }
    const std::string what = !state.opaque ? "" : *state.opaque ? "You see only the room." : "The room shows through your game.";
    canvas_.text(regular_, what, rect.x, track.y + track.h + 34, 26, kDim);
}

void Panel::switch_row(Rect rect, std::optional<PanelClick> click, bool on, const std::string& title,
                       const std::string& detail, bool enabled, bool divider) {
    if (enabled && hovered(rect)) canvas_.rounded_rect(rect, kRadius, kSurfaceHover);
    if (divider) canvas_.rounded_rect({rect.x + 24, rect.y, rect.w - 48, 1.5f}, 0, kHairline);
    const float title_y = detail.empty() ? rect.y + rect.h / 2 : rect.y + rect.h / 2 - 17;
    canvas_.text(bold_, canvas_.fit(bold_, title, 30, rect.w - 150), rect.x + 24, title_y, 30, enabled ? kText : kFaint);
    if (!detail.empty())
        canvas_.text(regular_, canvas_.fit(regular_, detail, 26, rect.w - 150), rect.x + 24, rect.y + rect.h / 2 + 21,
            26, enabled ? kDim : kFaint);
    switch_control(rect.x + rect.w - 24, rect.y + rect.h / 2, on, enabled, 1);
    if (enabled && click) targets_.push_back({rect, click, std::nullopt});
}

void Panel::knocks_row(Rect rect, const PanelState& state) {
    const bool any_kind = state.enabled[0] || state.enabled[1] || state.enabled[2];
    const bool active = !state.paused && any_kind;
    const bool hover = hovered(rect);
    canvas_.rounded_rect(rect, kRadius, active ? (hover ? kActiveHover : kActive) : (hover ? kRaisedHover : kRaised));
    const float cy = rect.y + rect.h / 2;
    const std::string title = state.paused ? "KnockThru Off" : "KnockThru On";
    canvas_.text(bold_, title, rect.x + 28, cy, 38, kText);
    switch_control(rect.x + rect.w - 28, cy, !state.paused, true, 1.25f);
    targets_.push_back({rect, PanelClick{PanelCommand::Pause}, std::nullopt});
}

void Panel::main_screen(const PanelState& state) {
    header("KnockThru");
    button({kMargin + 500 - 160, 36, 160, 64}, "Settings", std::nullopt, Screen::Settings);
    const float left_w = 500;
    passthrough_tile({kMargin, 128, left_w, 340}, state);
    style_control({kMargin, 500, left_w, 170}, state);

    const float x = kMargin + left_w + 48, w = kWidth - kMargin - x;
    canvas_.rounded_rect({x, 40, w, 648}, kRadius, kSurface);
    float y = 40;

    knocks_row({x + 8, y + 8, w - 16, 104}, state);
    y += 112;

    y += 24;
    for (int k = 0; k < 3; ++k, y += 88)
        switch_row({x, y, w, 88}, PanelClick{PanelCommand::Kind, static_cast<KnockKind>(k)}, state.enabled[k],
            kKindTitles[k], "", !state.paused, false);
    y += 8;

    if (state.app_key.empty()) {
        switch_row({x, y, w, 112}, std::nullopt, false, "This game", "No game running", false, true);
    } else {
        const auto name = state.app_name.empty() ? state.app_key : state.app_name;
        switch_row({x, y, w, 112}, PanelClick{PanelCommand::BlockApp}, !state.app_blocked, "Knocks in " + name,
            state.app_blocked ? "Off for this game only" : "On", true, true);
    }
    y += 112;

    canvas_.rounded_rect({x + 24, y, w - 48, 1.5f}, 0, kHairline);
    canvas_.text(bold_, "Knocks getting missed?", x + 24, y + 56, 30, kText);
    button({x + w - 24 - 170, y + 28, 170, 56}, "Calibrate", std::nullopt, Screen::Calibrate);

    // The tiles already show what worked, so the main screen only shows errors.
    if (state.message_is_error) message(state, 718);
}

void Panel::calibrate_screen(const PanelState& state) {
    if (state.calibrating) {
        const int k = static_cast<int>(*state.calibrating);
        header("Calibrating");
        canvas_.rounded_rect({kMargin, 128, kWidth - 2 * kMargin, 520}, kRadius, kSurface);
        const float x = kMargin + 48;
        canvas_.text(bold_, kKindTitles[k], x, 196, 34, kText);
        canvas_.text(regular_, kKindHowTo[k], x, 248, 28, kDim);
        canvas_.text(regular_, "Knock as softly as you want KnockThru to catch.", x, 292, 28, kDim);
        canvas_.text(regular_, "One buzz means a knock counted.", x, 336, 28, kDim);
        canvas_.text(bold_, std::to_string(state.calibration_count) + " of " + std::to_string(state.calibration_wanted),
            x, 430, 96, kText);
        for (int i = 0; i < state.calibration_wanted; ++i) {
            const float cx = x + 20 + i * 56;
            if (i < state.calibration_count) canvas_.circle(cx, 520, 18, kAccent);
            else canvas_.arc(cx, 520, 15, 18, 0, 7, kRaisedHover);
        }
        canvas_.text(regular_, std::to_string(state.calibration_seconds_left) + " s left", x, 588, 28, kDim);
        button({kWidth - kMargin - 48 - 200, 548, 200, 64}, "Cancel",
            PanelClick{PanelCommand::CancelCalibration, *state.calibrating}, std::nullopt);
        return;
    }

    button({kMargin, 36, 140, 64}, "Back", std::nullopt, Screen::Main);
    canvas_.text(bold_, "Calibrate", kMargin + 172, 68, 38, kText);
    const float top = 136, row = 128, w = kWidth - 2 * kMargin;
    canvas_.rounded_rect({kMargin, top, w, 3 * row}, kRadius, kSurface);
    for (int k = 0; k < 3; ++k) {
        const Rect rect{kMargin, top + k * row, w, row};
        const bool hover = hovered(rect);
        if (hover) canvas_.rounded_rect(rect, kRadius, kSurfaceHover);
        if (k) canvas_.rounded_rect({rect.x + 24, rect.y, rect.w - 48, 1.5f}, 0, kHairline);
        canvas_.text(bold_, kKindTitles[k], rect.x + 32, rect.y + row / 2 - 17, 30, kText);
        canvas_.text(regular_, kKindHowTo[k], rect.x + 32, rect.y + row / 2 + 21, 26, kDim);
        const Rect start{rect.x + rect.w - 32 - 170, rect.y + row / 2 - 30, 170, 60};
        canvas_.rounded_rect(start, kRadius, hover ? kAccentHover : kAccent);
        canvas_.text(bold_, "Start", start.x + start.w / 2, start.y + start.h / 2, 28, kText, Align::Center);
        // The whole row starts calibration, so there is one control per row.
        targets_.push_back({rect, PanelClick{PanelCommand::Calibrate, static_cast<KnockKind>(k)}, std::nullopt});
    }
    if (state.calibration_saved && !saved_dismissed_) {
        const Rect done{kMargin, top + 3 * row + 32, w, 120};
        canvas_.rounded_rect(done, kRadius, kActive);
        canvas_.text(bold_, "Saved", done.x + 32, done.y + done.h / 2 - 20, 38, kText);
        canvas_.text(regular_, "Knocks as soft as your softest one now count.", done.x + 32, done.y + done.h / 2 + 24, 26,
            kText);
        button({done.x + done.w - 32 - 170, done.y + 30, 170, 60}, "Done", std::nullopt, Screen::Main, true);
    } else {
        message(state, top + 3 * row + 50);
    }
}

void Panel::settings_screen(const PanelState& state) {
    button({kMargin, 36, 140, 64}, "Back", std::nullopt, Screen::Main);
    canvas_.text(bold_, "Settings", kMargin + 172, 68, 38, kText);
    const float top = 150, w = kWidth - 2 * kMargin;
    canvas_.rounded_rect({kMargin, top, w, 128}, kRadius, kSurface);
    switch_row({kMargin, top, w, 128}, PanelClick{PanelCommand::BarIcon}, state.classic_bar_icon, "Classic bar icon",
        "Show the logo colors in the dashboard bar", true, false);
    Canvas icon(64, 64);
    draw_icon(icon, state.classic_bar_icon ? IconStyle::BarColor : IconStyle::BarWhite);
    canvas_.draw(icon, static_cast<int>(kMargin + w - 24 - 80 - 40 - 64), static_cast<int>(top + 32));
    if (state.message_is_error) message(state, top + 128 + 50);
}

const Canvas& Panel::render(const PanelState& state, std::optional<std::pair<float, float>> hover) {
    hover_ = hover;
    targets_.clear();
    canvas_.clear({0, 0, 0, 0});
    canvas_.rounded_rect({0, 0, kWidth, kHeight}, 28, kBackground);
    if (state.calibrating) {
        screen_ = Screen::Calibrate;
        saved_dismissed_ = false;
    }
    if (screen_ == Screen::Main) main_screen(state);
    else if (screen_ == Screen::Settings) settings_screen(state);
    else calibrate_screen(state);
    return canvas_;
}

std::optional<PanelClick> Panel::click(float x, float y) {
    for (const auto& target : targets_) {
        if (!target.rect.contains(x, y)) continue;
        if (target.go_to) {
            // Leaving the calibration screen hides the Saved banner.
            if (screen_ == Screen::Calibrate) saved_dismissed_ = true;
            screen_ = *target.go_to;
        }
        return target.click;
    }
    return std::nullopt;
}

void Panel::draw_icon(Canvas& canvas, IconStyle style) {
    const bool tile = style == IconStyle::Tile;
    const float s = static_cast<float>(canvas.width());
    canvas.clear({0, 0, 0, 0});
    if (tile) canvas.rounded_rect({0, 0, s, s}, s * 0.22f, {kBackground.r, kBackground.g, kBackground.b});
    // Two Steam Frame controllers knocking heads, from logo_shapes.hpp on a 64 by 64 grid. The
    // mark spans x 7.3 to 56.7 and y 8.1 to 56.2. Without the tile, it fills the canvas.
    const float k = tile ? s / 64 : s / 50;
    const float shift = tile ? 0 : 7;
    // The bar version has wider gaps, so they stay visible at the bar's 38 px.
    const int* ring_sizes = tile ? logo::kRingSizes : logo::bar::kRingSizes;
    const float* point = tile ? logo::kPoints : logo::bar::kPoints;
    static_assert(std::size(logo::kShapes) == std::size(logo::bar::kShapes));
    for (std::size_t n = 0; n < std::size(logo::kShapes); ++n) {
        const auto& shape = tile ? logo::kShapes[n] : logo::bar::kShapes[n];
        std::vector<std::vector<Point>> rings;
        for (int r = shape.first_ring; r < shape.first_ring + shape.ring_count; ++r) {
            auto& ring = rings.emplace_back();
            for (int i = 0; i < ring_sizes[r]; ++i, point += 2) ring.push_back({(point[0] - shift) * k, (point[1] - shift) * k});
        }
        // The white bar icon takes Steam's tint: grey when idle, and white on hover. Its accent
        // parts are half transparent, so they stay apart from the controllers like the blue does.
        const bool accent = shape.fill == logo::Fill::Accent;
        canvas.polygon(rings, style == IconStyle::BarWhite ? Color{255, 255, 255, static_cast<std::uint8_t>(accent ? 128 : 255)}
                              : accent ? kAccent : kText);
    }
}

} // namespace knockthru
