#include "panel.hpp"
#include "panel_style.hpp"
#include "logo_shapes.hpp"

#include <algorithm>
#include <cmath>

namespace knockthru {

using namespace panel_style;

namespace panel_style {

void draw_sprite(Canvas& canvas, const sprites::Sprite& sprite, float x, float y, float height, float angle) {
    const float a = angle * anim::kPi / 180, c = std::cos(a), s = std::sin(a);
    const auto place = [&](const std::vector<std::vector<float>>& source) {
        std::vector<std::vector<Point>> rings;
        for (const auto& ring : source) {
            auto& out = rings.emplace_back();
            for (std::size_t i = 0; i + 1 < ring.size(); i += 2) {
                const float px = ring[i] * height, py = ring[i + 1] * height;
                out.push_back({x + px * c + py * s, y - px * s + py * c});
            }
        }
        return rings;
    };
    canvas.polygon(place(sprite.body), kText);
    if (!sprite.accent.empty()) canvas.polygon(place(sprite.accent), kAccent);
}

void burst(Canvas& canvas, float x, float y, std::optional<float> played, float scale) {
    if (!played) return;
    const float spread = anim::ease_out(*played), fade = 1 - anim::ease_in_out(*played);
    for (int k = 0; k < 8; ++k) {
        const float a = k * anim::kPi / 4 + anim::kPi / 8, dx = std::cos(a), dy = std::sin(a);
        const float inner = (20 + 14 * spread) * scale, outer = inner + (8 - 2 * spread) * scale;
        // A short ray with round ends, shaped like the rays in images/logo.svg. A single polygon keeps the
        // faded color from doubling where separate end circles would overlap the bar.
        const float half = 3.2f * scale;
        std::vector<Point> ray;
        for (const float end : {outer, inner}) {
            const float cx = x + dx * end, cy = y + dy * end,
                        turn = end == outer ? -anim::kPi / 2 : anim::kPi / 2;
            for (int i = 0; i <= 8; ++i) {
                const float b = a + turn + i * anim::kPi / 8;
                ray.push_back({cx + std::cos(b) * half, cy + std::sin(b) * half});
            }
        }
        canvas.polygon({ray}, faded(kAccent, fade));
    }
}

} // namespace panel_style

Panel::Panel(const std::filesystem::path& fonts)
    : bold_(fonts / "NotoSans-SemiBold.ttf"), regular_(fonts / "NotoSans-Regular.ttf"),
      canvas_(kWidth, kHeight), scratch_(kWidth, kHeight), background_(kWidth, kHeight) {
    background_.rounded_rect({0, 0, kWidth, kHeight}, 28, kBackground);
}

// Drawing the icon from its outlines is slow, so each size and style is drawn once and kept.
const Canvas& Panel::icon(int size, IconStyle style) {
    const auto key = std::make_pair(size, style);
    auto found = icons_.find(key);
    if (found == icons_.end()) {
        found = icons_.emplace(key, Canvas(size, size)).first;
        draw_icon(found->second, style);
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
        knob = kFaint;
    } else if (on) {
        fill = kAccent;
        knob = kText;
    }
    canvas_.rounded_rect(track, r, fill);
    canvas_.circle(on ? track.x + track.w - r : track.x + r, cy, r - 6 * scale, knob);
}

// A danger button is red when primary, and has a red label otherwise.
void Panel::button(Rect rect, const std::string& label, std::optional<PanelClick> click,
    std::optional<Screen> go_to, bool primary, bool enabled, bool danger) {
    if (!enabled) {
        canvas_.rounded_rect(rect, kRadius, kSurfaceHover);
        canvas_.text(bold_, label, rect.x + rect.w / 2, rect.y + rect.h / 2, 28, kFaint, Align::Center);
        return;
    }
    const bool hover = hovered(rect);
    Color fill = hover ? kRaisedHover : kRaised, ink = danger ? kError : kText;
    if (primary && danger) {
        fill = hover ? kErrorHover : kError;
        ink = kOnError;
    } else if (primary) {
        fill = hover ? kAccentHover : kAccent;
    }
    canvas_.rounded_rect(rect, kRadius, fill);
    canvas_.text(bold_, label, rect.x + rect.w / 2, rect.y + rect.h / 2, 28, ink, Align::Center);
    targets_.push_back({rect, click, go_to});
}

void Panel::header(const std::string& title) {
    canvas_.draw(icon(56, IconStyle::Tile), static_cast<int>(kMargin), 40);
    canvas_.text(bold_, title, kMargin + 76, 68, 38, kText);
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
    canvas_.text(bold_, canvas_.fit(bold_, title, 30, rect.w - 150), rect.x + 24, title_y, 30,
        enabled ? kText : kFaint);
    if (!detail.empty())
        canvas_.text(regular_, canvas_.fit(regular_, detail, 26, rect.w - 150), rect.x + 24,
            rect.y + rect.h / 2 + 21, 26, enabled ? kDim : kFaint);
    switch_control(rect.x + rect.w - 24, rect.y + rect.h / 2, on, enabled, 1);
    if (enabled && click) targets_.push_back({rect, click, std::nullopt});
}

// A settings row with a title, a detail line, and a button at the right.
void Panel::button_row(Rect rect, const std::string& title, const std::string& detail,
    const std::string& label, std::optional<PanelClick> click, std::optional<Screen> go_to, bool danger,
    bool divider) {
    if (divider) canvas_.rounded_rect({rect.x + 24, rect.y, rect.w - 48, 1.5f}, 0, kHairline);
    canvas_.text(bold_, title, rect.x + 24, rect.y + rect.h / 2 - 17, 30, kText);
    canvas_.text(regular_, detail, rect.x + 24, rect.y + rect.h / 2 + 21, 26, kDim);
    button({rect.x + rect.w - 24 - 170, rect.y + rect.h / 2 - 30, 170, 60}, label, click, go_to, false, true,
        danger);
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

// A check mark drawn as two bars on an accent circle. `pop` scales it from 0 to 1.
void Panel::check_mark(float cx, float cy, float radius, float pop) {
    canvas_.circle(cx, cy, radius * pop, kAccent);
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
    if (state.tour && !was_touring_) screen_ = Screen::Main; // just started
    if (!state.tour && was_touring_) screen_ = Screen::Main; // finished or skipped
    was_touring_ = state.tour.has_value();
    // The tour's Calibrate step hands over to the Learn screen, whose Back returns to the tour.
    const bool tour_on_screen = state.tour && screen_ != Screen::Learn;

    View view = View::Learn;
    switch (screen_) {
    case Screen::Main: view = View::Main; break;
    case Screen::Settings: view = View::Settings; break;
    case Screen::Reset: view = View::Reset; break;
    case Screen::Report: view = View::Report; break;
    case Screen::Pick: view = View::Pick; break;
    case Screen::Actions: view = View::Actions; break;
    case Screen::Learn:
    case Screen::SkipTour: break;
    }
    std::optional<TourView::Step> tour_step;
    if (calibration) {
        using Phase = CalibrationView::Phase;
        view = calibration->phase == Phase::Recording ? View::Record : View::Try;
        if (view == View::Record)
            record_screen(state, *calibration);
        else
            try_screen(state, *calibration);
    } else if (tour_on_screen) {
        view = View::Tour;
        tour_step = state.tour->step;
        tour_screen(state, *state.tour);
        if (screen_ == Screen::SkipTour)
            confirm_card("Skip the tour?", "You can replay it any time from Settings.", "Keep going",
                Screen::Main, "Skip", PanelClick{PanelCommand::TourSkip}, false);
    } else if (screen_ == Screen::Main) {
        main_screen(state);
    } else if (screen_ == Screen::Settings) {
        settings_screen(state);
    } else if (screen_ == Screen::Reset) {
        settings_screen(state);
        confirm_card("Reset KnockThru?",
            "This deletes your settings, blocked games, and calibration. The tour starts again afterward.",
            "Cancel", Screen::Settings, "Reset", PanelClick{PanelCommand::FactoryReset}, true);
    } else if (screen_ == Screen::Report) {
        settings_screen(state);
        report_card(state);
    } else if (screen_ == Screen::Pick) {
        pick_screen(state);
    } else if (screen_ == Screen::Actions) {
        actions_screen(state);
    } else {
        learn_screen(state);
    }
    if (state.update_notice && !calibration && !tour_on_screen) update_card(*state.update_notice);

    // A new view rises into place. The first render has nothing to move from.
    // The reset confirm appears over Settings in place, so only the other views rise.
    const auto over_settings = [](std::optional<View> v) { return v == View::Reset || v == View::Report; };
    const bool overlay =
        (over_settings(view) && view_ == View::Settings) || (view == View::Settings && over_settings(view_));
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
    animating_ = view == View::Pick || view == View::Learn || view == View::Record || view == View::Try
        || view == View::Tour || entering < 1;
    return canvas_;
}

std::optional<PanelClick> Panel::click(float x, float y) {
    for (const auto& target : targets_) {
        if (!target.rect.contains(x, y)) continue;
        if (target.go_to) {
            // Leaving the calibration screens hides the Saved banner.
            if (screen_ == Screen::Pick && *target.go_to == Screen::Settings) saved_dismissed_ = true;
            screen_ = *target.go_to;
            if (target.learn) learn_kind_ = *target.learn;
        }
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
