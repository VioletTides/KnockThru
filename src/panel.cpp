#include "panel.hpp"
#include "panel_style.hpp"
#include "logo_shapes.hpp"

#include <algorithm>
#include <cmath>

namespace knockthru {

using namespace panel_style;

Panel::Panel(const std::filesystem::path& fonts)
    : bold_(fonts / "NotoSans-SemiBold.ttf"), regular_(fonts / "NotoSans-Regular.ttf"),
      canvas_(kWidth, kHeight), scratch_(kWidth, kHeight), background_(kWidth, kHeight) {
    background_.rounded_rect({0, 0, kWidth, kHeight}, 28, kBackground);
}

// Drawing the icon from its outlines is slow, so each size is drawn once and kept.
const Canvas& Panel::icon(int size) {
    auto found = icons_.find(size);
    if (found == icons_.end()) {
        found = icons_.emplace(size, Canvas(size, size)).first;
        draw_icon(found->second);
    }
    return found->second;
}

// Draws `text` wrapped to `width` in at most `max_lines` lines, `leading` pixels apart.
// Returns the y after the last line.
float Panel::paragraph(Font& font, const std::string& text, float x, float y, float size, float leading,
    float width, int max_lines, Color color) {
    for (const auto& line : canvas_.wrap(font, text, size, width, max_lines)) {
        canvas_.text(font, line, x, y, size, color);
        y += leading;
    }
    return y;
}

bool Panel::hovered(Rect rect) const { return hover_ && rect.contains(hover_->first, hover_->second); }

void Panel::switch_control(float right, float cy, bool on, bool enabled, float scale) {
    const float h = 48 * scale, w = 80 * scale, r = h / 2;
    const Rect track{right - w, cy - r, w, h};
    Color fill = kRaisedHover, knob = kKnobOff;
    if (!enabled) {
        fill = kSurfaceHover;
        knob = kDim;
    } else if (on) {
        fill = kAccent;
        knob = kText;
    }
    canvas_.rounded_rect(track, r, fill);
    canvas_.circle(on ? track.x + track.w - r : track.x + r, cy, r - 6 * scale, knob);
}

void Panel::button(Rect rect, const std::string& label, std::optional<PanelClick> click,
    std::optional<Screen> go_to, Look look, float size) {
    if (look == Look::Off) {
        canvas_.rounded_rect(rect, kRadius, kSurfaceHover);
        canvas_.text(bold_, label, rect.x + rect.w / 2, rect.y + rect.h / 2, size, kDim, Align::Center);
        return;
    }
    const bool hover = hovered(rect);
    Color fill = hover ? kRaisedHover : kRaised, ink = look == Look::Danger ? kError : kText;
    if (look == Look::PrimaryDanger) {
        fill = hover ? kErrorHover : kError;
        ink = kOnError;
    } else if (look == Look::Primary) {
        fill = hover ? kAccentHover : kAccent;
    }
    canvas_.rounded_rect(rect, kRadius, fill);
    canvas_.text(bold_, label, rect.x + rect.w / 2, rect.y + rect.h / 2, size, ink, Align::Center);
    targets_.push_back({rect, click, go_to});
}

// A pill track with one segment per name. The selected segment is filled with `raised` and the hovered one
// lights up. Returns the segments, for the caller's targets.
std::vector<Rect> Panel::segments(Rect track, const std::vector<std::string>& names,
    std::optional<int> selected, float size, Color raised) {
    canvas_.rounded_rect(track, track.h / 2, kSurface);
    const float w = (track.w - 12) / names.size();
    std::vector<Rect> out;
    for (std::size_t i = 0; i < names.size(); ++i) {
        const Rect segment{track.x + 6 + i * w, track.y + 6, w, track.h - 12};
        const bool on = selected == static_cast<int>(i);
        if (on)
            canvas_.rounded_rect(segment, segment.h / 2, raised);
        else if (hovered(segment))
            canvas_.rounded_rect(segment, segment.h / 2, kSurfaceHover);
        canvas_.text(bold_, names[i], segment.x + segment.w / 2, segment.y + segment.h / 2, size,
            on ? kText : kDim, Align::Center);
        out.push_back(segment);
    }
    return out;
}

float Panel::pill_width(const std::string& label, float padding) {
    return canvas_.measure(bold_, label, 22) + 2 * padding;
}

// The Back button and a title, at the top of every screen below Settings or Main.
void Panel::back_header(const std::string& title, Screen back_to) {
    button({kMargin, 36, 140, 64}, "Back", std::nullopt, back_to);
    canvas_.text(bold_, title, kMargin + 172, 68, 38, kText);
}

void Panel::message(const PanelState& state, float y) {
    paragraph(regular_, state.message, kMargin, y, 26, 36, kWidth - 2 * kMargin, 2,
        state.message_is_error ? kError : kDim);
}

void Panel::switch_row(Rect rect, std::optional<PanelClick> click, bool on, const std::string& title,
    const std::string& detail, bool enabled, bool divider) {
    if (enabled && hovered(rect)) canvas_.rounded_rect(rect, kRadius, kSurfaceHover);
    if (divider) canvas_.rounded_rect({rect.x + 24, rect.y, rect.w - 48, 1.5f}, 0, kHairline);
    const float title_y = detail.empty() ? rect.y + rect.h / 2 : rect.y + rect.h / 2 - 17;
    canvas_.text(
        bold_, canvas_.fit(bold_, title, 30, rect.w - 150), rect.x + 24, title_y, 30, enabled ? kText : kDim);
    if (!detail.empty())
        canvas_.text(regular_, canvas_.fit(regular_, detail, 26, rect.w - 150), rect.x + 24,
            rect.y + rect.h / 2 + 21, 26, kDim);
    switch_control(rect.x + rect.w - 24, rect.y + rect.h / 2, on, enabled, 1);
    if (enabled && click) targets_.push_back({rect, click, std::nullopt});
}

// A settings row with a title and a button at the right.
void Panel::button_row(Rect rect, const std::string& title, const std::string& label,
    std::optional<PanelClick> click, std::optional<Screen> go_to, bool danger, bool divider) {
    if (divider) canvas_.rounded_rect({rect.x + 24, rect.y, rect.w - 48, 1.5f}, 0, kHairline);
    canvas_.text(bold_, title, rect.x + 24, rect.y + rect.h / 2, 30, kText);
    button({rect.x + rect.w - 24 - 170, rect.y + rect.h / 2 - 30, 170, 60}, label, click, go_to,
        danger ? Look::Danger : Look::Plain);
}

// One bar per step at the top right. Steps up to `current` (from 1) are filled.
void Panel::steps(int current, int total) {
    const float w = 44, gap = 10, right = kWidth - kMargin, y = 64;
    const float left = right - total * w - (total - 1) * gap;
    canvas_.text(regular_, "Step " + std::to_string(current) + " of " + std::to_string(total), left - 20,
        y + 4, 24, kDim, Align::Right);
    for (int i = 0; i < total; ++i)
        canvas_.rounded_rect({left + i * (w + gap), y, w, 8}, 4, i < current ? kAccent : kRaised);
}

// A check mark drawn as two bars on an accent circle, or on `fill`. `pop` scales it from 0 to 1.
void Panel::check_mark(float cx, float cy, float radius, float pop, std::optional<Color> fill) {
    canvas_.circle(cx, cy, radius * pop, fill.value_or(kAccent));
    const float s = pop * radius / 34;
    canvas_.polygon({{{cx - 16 * s, cy}, {cx - 6 * s, cy + 11 * s}, {cx - 6 * s, cy + 20 * s},
                        {cx - 21 * s, cy + 5 * s}}},
        kText);
    canvas_.polygon({{{cx - 6 * s, cy + 11 * s}, {cx + 16 * s, cy - 13 * s}, {cx + 21 * s, cy - 8 * s},
                        {cx - 6 * s, cy + 20 * s}}},
        kText);
}

// A button that fills from the left with a blue tint as `progress` goes from 0 to 1. It takes no clicks.
void Panel::progress_button(Rect rect, const std::string& label, float progress) {
    canvas_.rounded_rect(rect, kRadius, kRaised);
    // The button's outline with every point right of the fill's edge pulled back to it. The outline
    // is convex, so this is the button shape cut off at the edge.
    const float edge = rect.x + rect.w * std::clamp(progress, 0.0f, 1.0f);
    if (edge > rect.x) {
        std::vector<Point> outline;
        const std::array<std::array<float, 3>, 4> corners{
            {{rect.x + rect.w - kRadius, rect.y + kRadius, -anim::kPi / 2},
                {rect.x + rect.w - kRadius, rect.y + rect.h - kRadius, 0},
                {rect.x + kRadius, rect.y + rect.h - kRadius, anim::kPi / 2},
                {rect.x + kRadius, rect.y + kRadius, anim::kPi}}};
        for (const auto& [cx, cy, start] : corners)
            for (int i = 0; i <= 8; ++i) {
                const float a = start + anim::kPi / 2 * static_cast<float>(i) / 8;
                outline.push_back({std::min(cx + kRadius * std::cos(a), edge), cy + kRadius * std::sin(a)});
            }
        canvas_.polygon({outline}, Color{kAccent.r, kAccent.g, kAccent.b, 110});
    }
    canvas_.text(bold_, label, rect.x + rect.w / 2, rect.y + rect.h / 2, 28, kText, Align::Center);
}

// Steam's alert mark from its Settings icon, a tapered bar over a dot, centered on (cx, cy).
// At scale 1 it is about 17 px tall.
void Panel::alert_mark(float cx, float cy, float s, Color color) {
    const float top = cy - 8.37f * s;
    canvas_.polygon({{{cx - 3.37f * s, top}, {cx + 3.37f * s, top}, {cx + 2.22f * s, top + 8 * s},
                        {cx - 2.22f * s, top + 8 * s}}},
        color);
    canvas_.circle(cx, top + 13.87f * s, 2.87f * s, color);
}

const Canvas& Panel::render(
    const PanelState& state, std::optional<std::pair<float, float>> hover, double now) {
    hover_ = hover;
    now_ = now;
    targets_.clear();
    canvas_ = background_;
    const auto& calibration = state.calibration;
    if (calibration) {
        saved_dismissed_ = false;
    } else if (was_calibrating_) {
        screen_ = Screen::Pick; // canceled or saved
    }
    was_calibrating_ = calibration.has_value();
    if ((screen_ == Screen::AdvancedWarning || debug_open()) && !state.debug) screen_ = Screen::Settings;
    if (state.tour.has_value() != was_touring_) screen_ = Screen::Main; // started, finished, or skipped
    was_touring_ = state.tour.has_value();
    // The tour's Calibrate step hands over to the Learn screen, whose Back returns to the tour.
    const bool tour_on_screen = state.tour && screen_ != Screen::Learn;

    Screen view = screen_ == Screen::SkipTour ? Screen::Learn : screen_;
    std::optional<TourView::Step> tour_step;
    if (calibration) {
        using Phase = CalibrationView::Phase;
        view = calibration->phase == Phase::Recording ? Screen::Record : Screen::Try;
        if (view == Screen::Record)
            record_screen(state, *calibration);
        else
            try_screen(state, *calibration);
    } else if (tour_on_screen) {
        view = Screen::Tour;
        tour_step = state.tour->step;
        tour_screen(state, *state.tour);
        if (screen_ == Screen::SkipTour)
            confirm_card("Skip the tour?", "You can replay it any time from Settings", "Keep going",
                Screen::Main, "Skip tour", PanelClick{PanelCommand::TourSkip}, Screen::Main, false, true);
    } else if (screen_ == Screen::Main) {
        main_screen(state);
    } else if (screen_ == Screen::Settings) {
        settings_screen(state);
    } else if (screen_ == Screen::Reset) {
        settings_screen(state);
        confirm_card("Reset KnockThru?",
            "This deletes your settings, blocked games, and calibration. The tour starts again afterward.",
            "Cancel", Screen::Settings, "Reset", PanelClick{PanelCommand::FactoryReset}, Screen::Main, true);
    } else if (screen_ == Screen::Report) {
        settings_screen(state);
        report_card(state);
    } else if (screen_ == Screen::Pick) {
        pick_screen(state);
    } else if (screen_ == Screen::Actions) {
        actions_screen(state);
    } else if (screen_ == Screen::AdvancedWarning) {
        settings_screen(state);
        advanced_warning();
    } else if (screen_ == Screen::Debug) {
        debug_screen(state);
    } else if (screen_ == Screen::WhatsNew) {
        whats_new_screen(state);
    } else if (screen_ == Screen::RestoreDefaults) {
        debug_screen(state);
        confirm_card("Restore defaults?",
            "Every knock number goes back to the default. Calibrate again afterward.", "Cancel",
            Screen::Debug, "Restore", PanelClick{PanelCommand::RestoreDefaults}, Screen::Debug, true);
    } else {
        learn_screen(state);
    }
    if (state.update_notice && !calibration && !tour_on_screen) update_card(state);

    // A new view rises into place. The first render has nothing to move from.
    // A card appears over its screen in place, so only the other views rise.
    const auto screen_below = [](std::optional<Screen> v) -> std::optional<Screen> {
        if (v == Screen::Reset || v == Screen::Report || v == Screen::AdvancedWarning)
            return Screen::Settings;
        if (v == Screen::RestoreDefaults) return Screen::Debug;
        return std::nullopt;
    };
    const bool overlay = view_ && (screen_below(view) == *view_ || screen_below(*view_) == view);
    if ((view_ && *view_ != view && !overlay) || (tour_step && tour_step_ && *tour_step_ != *tour_step))
        view_t_ = now;
    view_ = view;
    tour_step_ = tour_step;
    const float entering = static_cast<float>((now - view_t_) / 0.3);
    if (entering < 1 && now > 0) {
        scratch_ = canvas_;
        canvas_ = background_;
        canvas_.draw(scratch_, 0, static_cast<int>(std::lround(20 * (1 - anim::ease_out(entering)))),
            anim::ease_out(entering));
    }
    transitioning_ = entering < 1;
    animating_ = view == Screen::Pick || view == Screen::Learn || view == Screen::Record
        || view == Screen::Try || view == Screen::Tour || view == Screen::Debug
        || view == Screen::RestoreDefaults || entering < 1;
    return canvas_;
}

std::optional<PanelClick> Panel::click(float x, float y) {
    for (const auto& target : targets_) {
        if (!target.rect.contains(x, y)) continue;
        if (target.go_to) {
            // Leaving the calibration screens hides the Saved banner.
            if (screen_ == Screen::Pick && *target.go_to == Screen::Settings) saved_dismissed_ = true;
            screen_ = *target.go_to;
            if (screen_ == Screen::AdvancedWarning) dont_remind_ = false;
            if (screen_ == Screen::WhatsNew) whats_new_page_ = 0;
            if (target.learn) learn_kind_ = *target.learn;
        }
        if (target.tab) debug_tab_ = *target.tab;
        if (target.all_settings) debug_all_ = *target.all_settings;
        if (target.dont_remind) dont_remind_ = *target.dont_remind;
        if (target.page) whats_new_page_ = *target.page;
        return target.click ? target.click : PanelClick{PanelCommand::Navigate};
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
            for (int i = 0; i < ring_sizes[r]; ++i, point += 2)
                ring.push_back({(point[0] - shift) * k, (point[1] - shift) * k});
        }
        // The white bar icon takes Steam's tint: grey when idle, and white on hover. Its accent
        // parts are half transparent, so they stay apart from the controllers like the blue does.
        const bool accent = shape.fill == logo::Fill::Accent;
        Color color = accent ? kAccent : kText;
        if (style == IconStyle::BarWhite)
            color = {255, 255, 255, static_cast<std::uint8_t>(accent ? 128 : 255)};
        canvas.polygon(rings, color);
    }
}

} // namespace knockthru
