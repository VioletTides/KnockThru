#include "panel.hpp"

#include "animation.hpp"
#include "calibrator.hpp"
#include "logo_shapes.hpp"
#include "sprite_shapes.hpp"

#include <qrcodegen.hpp>

#include <algorithm>
#include <array>
#include <cmath>

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
constexpr Color kErrorHover{248, 132, 122};
constexpr Color kOnError{40, 14, 12};
// The knocks row is a tint of the accent while knocks act, so the passthrough tile keeps the full accent.
constexpr Color kActive{34, 62, 94};
constexpr Color kActiveHover{40, 72, 108};
constexpr Color kTable{120, 92, 70};

// Containers use a 16 px radius. Switches and the style control are pills.
constexpr float kRadius = 16;
constexpr float kMargin = 40;

constexpr const char* kKindTitles[3] = {"Controllers together", "Controller on a surface", "Knock on the headset"};
constexpr const char* kKindOnce[3] = {"Knock the controllers together.", "Knock one controller on a table or wall.",
                                      "Knock on the side of the headset with a controller."};
constexpr const char* kKindNames[3] = {"a together knock", "a surface knock", "a headset knock"};

Color faded(Color color, float alpha) {
    return {color.r, color.g, color.b, static_cast<std::uint8_t>(std::lround(color.a * anim::clamp(alpha)))};
}

// Draws a sprite with its anchor at (x, y), `height` pixels tall, turned
// counterclockwise by `angle` degrees.
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

// Impact lines that fly outward from (x, y) and fade. `played` runs from 0 to 1.
void burst(Canvas& canvas, float x, float y, std::optional<float> played, float scale) {
    if (!played) return;
    const float spread = anim::ease_out(*played), fade = 1 - anim::ease_in_out(*played);
    for (int k = 0; k < 8; ++k) {
        const float a = k * anim::kPi / 4 + anim::kPi / 8, dx = std::cos(a), dy = std::sin(a);
        const float inner = (16 + 18 * spread) * scale, outer = inner + (18 - 8 * spread) * scale;
        const float half = 2 * scale, nx = -dy * half, ny = dx * half;
        canvas.polygon({{{x + dx * inner + nx, y + dy * inner + ny}, {x + dx * outer + nx, y + dy * outer + ny},
                         {x + dx * outer - nx, y + dy * outer - ny}, {x + dx * inner - nx, y + dy * inner - ny}}},
            faded(kAccent, fade));
    }
}

float head_radius(float height) { return 0.288f * height; } // measured on the controller sprites

} // namespace

Panel::Panel(const std::filesystem::path& fonts)
    : bold_(fonts / "NotoSans-SemiBold.ttf"), regular_(fonts / "NotoSans-Regular.ttf"), canvas_(kWidth, kHeight),
      scratch_(kWidth, kHeight), background_(kWidth, kHeight) {
    background_.rounded_rect({0, 0, kWidth, kHeight}, 28, kBackground);
}

bool Panel::hovered(Rect rect) const { return hover_ && rect.contains(hover_->first, hover_->second); }

void Panel::switch_control(float right, float cy, bool on, bool enabled, float scale) {
    const float h = 48 * scale, w = 80 * scale, r = h / 2;
    const Rect track{right - w, cy - r, w, h};
    canvas_.rounded_rect(track, r, !enabled ? kSurfaceHover : on ? kAccent : kRaisedHover);
    canvas_.circle(on ? track.x + track.w - r : track.x + r, cy, r - 6 * scale, !enabled ? kFaint : on ? kText : kKnobOff);
}

// A danger button is red when primary, and has a red label otherwise.
void Panel::button(Rect rect, const std::string& label, std::optional<PanelClick> click, std::optional<Screen> go_to,
                   bool primary, bool enabled, bool danger) {
    if (!enabled) {
        canvas_.rounded_rect(rect, kRadius, kSurfaceHover);
        canvas_.text(bold_, label, rect.x + rect.w / 2, rect.y + rect.h / 2, 28, kFaint, Align::Center);
        return;
    }
    const bool hover = hovered(rect);
    const Color fill = primary && danger ? (hover ? kErrorHover : kError)
        : primary ? (hover ? kAccentHover : kAccent) : (hover ? kRaisedHover : kRaised);
    canvas_.rounded_rect(rect, kRadius, fill);
    canvas_.text(bold_, label, rect.x + rect.w / 2, rect.y + rect.h / 2, 28,
        primary && danger ? kOnError : danger ? kError : kText, Align::Center);
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

// A settings row with a title, a detail line, and a button at the right.
void Panel::button_row(Rect rect, const std::string& title, const std::string& detail, const std::string& label,
                       std::optional<PanelClick> click, std::optional<Screen> go_to, bool danger, bool divider) {
    if (divider) canvas_.rounded_rect({rect.x + 24, rect.y, rect.w - 48, 1.5f}, 0, kHairline);
    canvas_.text(bold_, title, rect.x + 24, rect.y + rect.h / 2 - 17, 30, kText);
    canvas_.text(regular_, detail, rect.x + 24, rect.y + rect.h / 2 + 21, 26, kDim);
    button({rect.x + rect.w - 24 - 170, rect.y + rect.h / 2 - 30, 170, 60}, label, click, go_to, false, true, danger);
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
    const Rect settings{kMargin + 500 - 160, 36, 160, 64};
    button(settings, "Settings", std::nullopt, Screen::Settings);
    // A dot on Settings when an update is ready, ringed in the background color so it reads on any fill.
    if (state.update.status == UpdateView::Status::Available) {
        canvas_.circle(settings.x + settings.w - 6, settings.y + 6, 13, kBackground);
        canvas_.circle(settings.x + settings.w - 6, settings.y + 6, 9, kAccent);
    }
    const float left_w = 500;
    passthrough_tile({kMargin, 128, left_w, 340}, state);
    style_control({kMargin, 500, left_w, 170}, state);

    const float x = kMargin + left_w + 48, w = kWidth - kMargin - x;
    canvas_.rounded_rect({x, 40, w, 648}, kRadius, kSurface);
    float y = 40;

    knocks_row({x + 8, y + 8, w - 16, 104}, state);
    y += 112;

    y += 24;
    for (int k = 0; k < 3; ++k, y += 120)
        switch_row({x, y, w, 120}, PanelClick{PanelCommand::Kind, static_cast<KnockKind>(k)}, state.enabled[k],
            kKindTitles[k], "", !state.paused, false);
    y += 8;

    if (state.app_key.empty()) {
        switch_row({x, y, w, 128}, std::nullopt, false, "This game", "No game running", false, true);
    } else {
        const auto name = state.app_name.empty() ? state.app_key : state.app_name;
        switch_row({x, y, w, 128}, PanelClick{PanelCommand::BlockApp}, !state.app_blocked, "Knocks in " + name,
            state.app_blocked ? "Off for this game only" : "On", true, true);
    }

    // The tiles already show what worked, so the main screen only shows errors.
    if (state.message_is_error) message(state, 718);
}

// One bar per step at the top right. Steps up to `current` (from 1) are filled.
void Panel::steps(int current, int total) {
    const float w = 44, gap = 10, right = kWidth - kMargin, y = 64;
    const float left = right - total * w - (total - 1) * gap;
    canvas_.text(regular_, "Step " + std::to_string(current) + " of " + std::to_string(total), left - 20, y + 4, 24, kDim,
        Align::Right);
    for (int i = 0; i < total; ++i)
        canvas_.rounded_rect({left + i * (w + gap), y, w, 8}, 4, i < current ? kAccent : kRaised);
}

// One knock kind on the shared knock timeline, fit into `box`. The layout is on
// a 340 by 240 grid, the same as the README's knock kind cards.
void Panel::knock_scene(KnockKind kind, Rect box, float t, int knocks) {
    const float s = std::min(box.w / 340, box.h / 240);
    const float ox = box.x + (box.w - 340 * s) / 2, oy = box.y + (box.h - 240 * s) / 2;
    const auto X = [&](float x) { return ox + x * s; };
    const auto Y = [&](float y) { return oy + y * s; };
    const float gap = anim::knock_motion(t, knocks);
    const auto played = anim::impact(t, knocks);
    // A counted calibration knock buzzes at once. An action buzzes after the 0.4 s wait.
    const float action = anim::knock_time(knocks - 1) + (knocks == 1 ? 0.05f : 0.4f);
    const float shake = anim::buzz(t, action, 1) * s;
    if (kind == KnockKind::Together) {
        const float h = 150, cx = 170, cy = 105;
        const float offset = head_radius(h) * 0.95f + 30 * gap, tilt = 30 + 10 * gap;
        burst(canvas_, X(cx), Y(cy), played, s);
        draw_sprite(canvas_, sprites::kControllerLeft, X(cx - offset) + shake, Y(cy), h * s, -tilt);
        draw_sprite(canvas_, sprites::kControllerRight, X(cx + offset) - shake, Y(cy), h * s, tilt);
    } else if (kind == KnockKind::Surface) {
        const float h = 150, cx = 170, table_y = 200;
        burst(canvas_, X(cx), Y(table_y), played, s);
        draw_sprite(canvas_, sprites::kControllerRight, X(cx) + shake, Y(table_y - head_radius(h) * 0.95f - 45 * gap),
            h * s, 135 - 12 * gap);
        canvas_.rounded_rect({X(cx - 130), Y(table_y), 260 * s, 16 * s}, 3 * s, kTable);
    } else {
        const float h = 130, cx = 170, cy = 120, hw = 160, hx = cx + 70, edge = hx - hw / 2;
        burst(canvas_, X(edge), Y(cy), played, s);
        draw_sprite(canvas_, sprites::kHeadset, X(hx), Y(cy), hw / sprites::kHeadset.aspect * s, 0);
        draw_sprite(canvas_, sprites::kControllerLeft, X(edge - head_radius(h) * 0.95f - 30 * gap) + shake, Y(cy),
            h * s, -30 - 8 * gap);
    }
}

// Step 1: three cards, each knocking twice on a loop, as knocks work outside calibration.
void Panel::pick_screen(const PanelState& state) {
    button({kMargin, 36, 140, 64}, "Back", std::nullopt, Screen::Settings);
    canvas_.text(bold_, "Calibrate", kMargin + 172, 68, 38, kText);
    steps(1, 4);
    const float t = static_cast<float>(std::fmod(now_, 2.6));
    // The cards fill the panel, and make room when the Saved banner shows.
    const bool banner = state.calibration_saved && !saved_dismissed_;
    // A saved kind that is turned off, or KnockThru paused, would leave knocks doing nothing.
    const int saved = state.calibration_saved ? static_cast<int>(*state.calibration_saved) : 0;
    const std::string saved_note = !banner ? ""
        : state.paused ? "KnockThru is off. Turn it on from the main screen to use knocks."
        : !state.enabled[saved] ? std::string(kKindTitles[saved]) + " is off. Turn it on from the main screen."
        : "KnockThru now listens for knocks like the ones you tried.";
    const float card_h = banner ? 420 : 560;
    for (int k = 0; k < 3; ++k) {
        const Rect card{kMargin + k * 384.0f, 128, 352, card_h};
        canvas_.rounded_rect(card, kRadius, hovered(card) ? kSurfaceHover : kSurface);
        knock_scene(static_cast<KnockKind>(k), {card.x + 16, card.y + 16, card.w - 32, card_h - 100}, t, 2);
        canvas_.text(bold_, kKindTitles[k], card.x + card.w / 2, card.y + card_h - 44, 28, kText, Align::Center);
        targets_.push_back({card, std::nullopt, Screen::Learn, static_cast<KnockKind>(k)});
    }
    if (banner) {
        const Rect done{kMargin, 572, kWidth - 2 * kMargin, 120};
        canvas_.rounded_rect(done, kRadius, kActive);
        canvas_.text(bold_, "Saved", done.x + 32, done.y + done.h / 2 - 20, 38, kText);
        canvas_.text(regular_, canvas_.fit(regular_, saved_note, 26, done.w - 260), done.x + 32, done.y + done.h / 2 + 24,
            26, kText);
        button({done.x + done.w - 32 - 170, done.y + 30, 170, 60}, "Done", std::nullopt, Screen::Settings, true);
    } else if (state.message_is_error) {
        message(state, 730);
    }
}

// A check mark drawn as two bars on an accent circle. `pop` scales it from 0 to 1.
void Panel::check_mark(float cx, float cy, float radius, float pop) {
    canvas_.circle(cx, cy, radius * pop, kAccent);
    const float s = pop * radius / 34;
    canvas_.polygon({{{cx - 16 * s, cy}, {cx - 6 * s, cy + 11 * s}, {cx - 6 * s, cy + 20 * s}, {cx - 21 * s, cy + 5 * s}}}, kText);
    canvas_.polygon({{{cx - 6 * s, cy + 11 * s}, {cx + 16 * s, cy - 13 * s}, {cx + 21 * s, cy - 8 * s}, {cx - 6 * s, cy + 20 * s}}}, kText);
}

// Step 2: how to knock for calibration, one knock at a time.
void Panel::learn_screen(const PanelState& state) {
    const int k = static_cast<int>(learn_kind_);
    button({kMargin, 36, 140, 64}, "Back", std::nullopt, Screen::Pick);
    canvas_.text(bold_, kKindTitles[k], kMargin + 172, 68, 38, kText);
    steps(2, 4);
    const Rect box{kMargin, 128, 540, 560};
    canvas_.rounded_rect(box, kRadius, kSurface);
    knock_scene(learn_kind_, {box.x + 30, box.y + 60, box.w - 60, 440}, static_cast<float>(std::fmod(now_, 2.0)), 1);

    const float x = box.x + box.w + 48, w = kWidth - kMargin - x;
    canvas_.text(bold_, "Wait for the ring, then knock once.", x, 190, 36, kText);
    float y = 250;
    for (const std::string& line : {std::string(kKindOnce[k]),
                                   std::string("Hold still while the ring fills."),
                                   std::string("Do it 10 times. One buzz means it counted."),
                                   std::string("Knock as hard as you would to switch passthrough.")}) {
        for (const auto& part : canvas_.wrap(regular_, line, 28, w, 2)) {
            canvas_.text(regular_, part, x, y, 28, kDim);
            y += 40;
        }
        y += 14;
    }
    button({kWidth - kMargin - 240, 616, 240, 72}, "Start", PanelClick{PanelCommand::Calibrate, learn_kind_}, std::nullopt,
        true);
    if (state.message_is_error) message(state, 730);
}

// Step 3: counts single knocks, with each one's strength.
void Panel::record_screen(const PanelState& state, const CalibrationView& view) {
    const int k = static_cast<int>(view.kind);
    button({kMargin, 36, 160, 64}, "Cancel", PanelClick{PanelCommand::CancelCalibration, view.kind}, std::nullopt);
    canvas_.text(bold_, "Recording", kMargin + 192, 68, 38, kText);
    steps(3, 4);
    const Rect box{kMargin, 128, 420, 560};
    canvas_.rounded_rect(box, kRadius, kSurface);
    knock_scene(view.kind, {box.x + 20, box.y + 40, box.w - 40, 340}, static_cast<float>(std::fmod(now_, 2.0)), 1);
    canvas_.text(bold_, kKindTitles[k], box.x + box.w / 2, box.y + 440, 28, kText, Align::Center);
    canvas_.text(regular_, "Wait for the ring, then knock once", box.x + box.w / 2, box.y + 486, 26, kDim, Align::Center);

    const float x = box.x + box.w + 48;
    const int count = static_cast<int>(view.peaks.size());
    canvas_.text(bold_, std::to_string(count) + " of " + std::to_string(view.wanted), x, 190, 80, kText);
    canvas_.text(regular_, std::to_string(view.seconds_left) + " s left", kWidth - kMargin, 190, 28, kDim, Align::Right);

    // One bar per knock above its dot, by how fast the controller moved into it.
    // A full bar is 2 m/s, a little faster than the hardest recorded knocks.
    const float since = static_cast<float>(now_ - view.counted_t);
    const float base = 470, tallest = 180, spacing = 64, r = 18;
    for (int i = 0; i < view.wanted; ++i) {
        const float cx = x + r + i * spacing;
        const bool latest = i == count - 1;
        if (i < count) {
            const float grow = latest ? anim::ease_out(since / 0.35f) : 1;
            const double speed = i < static_cast<int>(view.speeds.size()) ? view.speeds[i] : 0;
            const float h = std::max(8.0f, static_cast<float>(std::min(1.0, speed / 2.0)) * tallest * grow);
            canvas_.rounded_rect({cx - 14, base - h, 28, h}, 8, kAccent);
            const float pop = latest ? anim::ease_out_back(since / 0.25f) : 1;
            canvas_.circle(cx, base + 44, r * pop, kAccent);
            if (latest && since < 0.5f) {
                const float p = since / 0.5f;
                const float ring = r + 22 * anim::ease_out(p);
                canvas_.arc(cx, base + 44, ring - 3, ring, 0, 7, faded(kAccent, 1 - p));
            }
        } else {
            canvas_.rounded_rect({cx - 14, base - 8, 28, 8}, 4, kRaised);
            canvas_.arc(cx, base + 44, r - 3, r, 0, 7, kRaisedHover);
        }
    }

    // Pacing: a ring fills while knocks cannot count yet. First for the get-ready
    // time after Start, then for a second after each counted knock.
    const bool getting_ready = count == 0;
    const float cy = 610, ready = getting_ready
        ? anim::clamp(static_cast<float>((now_ - view.started_t) / Calibrator::kGetReady))
        : anim::clamp(static_cast<float>((now_ - view.knocked_t) / Calibrator::kPace));
    canvas_.arc(x + 26, cy, 20, 28, 0, 7, kRaised);
    if (ready < 1) canvas_.arc(x + 26, cy, 20, 28, anim::kPi / 2 - ready * 2 * anim::kPi, anim::kPi / 2, kAccent);
    else canvas_.circle(x + 26, cy, 28, kAccent);
    canvas_.text(bold_, ready >= 1 ? "Knock" : getting_ready ? "Get ready" : "Wait", x + 74, cy, 32, kText);

    // A knock that did not count gets a hint for a few seconds: too soon, too
    // light, or another kind. The newest one shows.
    const double latest = std::max({view.too_soon_t, view.too_light_t, view.ignored ? view.ignored_t : -1.0});
    const float hint = static_cast<float>(now_ - latest);
    if (latest >= 0 && hint < 3) {
        const float shown = std::min(anim::ease_out(hint / 0.25f), 1 - anim::ease_in_out((hint - 2.5f) / 0.5f));
        const auto text = latest == view.too_soon_t ? std::string("Too soon. Wait for the ring.")
            : latest == view.too_light_t ? std::string("Too light. Knock a little harder.")
            : std::string("That looked like ") + kKindNames[static_cast<int>(*view.ignored)] + ".";
        canvas_.text(regular_, text, x, 670 + 8 * (1 - shown), 26, faded(kError, shown));
    } else if (state.message_is_error) {
        message(state, 670);
    }
}

// Step 4: a double knock at the new thresholds, without switching passthrough.
void Panel::try_screen(const PanelState& state, const CalibrationView& view) {
    const int k = static_cast<int>(view.kind);
    button({kMargin, 36, 160, 64}, "Cancel", PanelClick{PanelCommand::CancelCalibration, view.kind}, std::nullopt);
    canvas_.text(bold_, "Try it", kMargin + 192, 68, 38, kText);
    steps(4, 4);
    const Rect box{kMargin, 128, 420, 500};
    canvas_.rounded_rect(box, kRadius, kSurface);
    knock_scene(view.kind, {box.x + 20, box.y + 40, box.w - 40, 340}, static_cast<float>(std::fmod(now_, 2.6)), 2);
    canvas_.text(bold_, kKindTitles[k], box.x + box.w / 2, box.y + 440, 28, kText, Align::Center);

    const float x = box.x + box.w + 48, w = kWidth - kMargin - x;
    canvas_.text(bold_, "Knock twice", x, 190, 36, kText);
    canvas_.text(regular_, "All " + std::to_string(view.peaks.size()) + " knocks recorded.", x, 240, 28, kDim);
    canvas_.text(regular_, "Knock the way you would to switch passthrough.", x, 280, 28, kDim);
    canvas_.text(regular_, "Passthrough stays as it is.", x, 320, 28, kDim);

    const Rect status{x, 368, w, 240};
    const float since = static_cast<float>(now_ - view.trial_t);
    if (!view.trial_passed) {
        canvas_.rounded_rect(status, kRadius, kSurface);
        const float pulse = 0.5f + 0.5f * std::sin(static_cast<float>(now_) * 2 * anim::kPi / 1.4f);
        canvas_.circle(status.x + 56, status.y + status.h / 2, 14 + 6 * pulse, faded(kAccent, 0.5f + 0.5f * pulse));
        canvas_.text(bold_, "Listening", status.x + 96, status.y + status.h / 2, 34, kText);
    } else if (*view.trial_passed) {
        canvas_.rounded_rect(status, kRadius, kActive);
        const float cx = status.x + 64, cy = status.y + status.h / 2;
        check_mark(cx, cy, 34, anim::ease_out_back(since / 0.3f));
        canvas_.text(bold_, "That worked", cx + 64, cy - 22, 34, kText);
        canvas_.text(regular_, "Save to keep this calibration.", cx + 64, cy + 24, 26, kText);
    } else {
        canvas_.rounded_rect(status, kRadius, kSurface);
        const float shake = anim::buzz(since, 0, 2) * 2;
        canvas_.text(bold_, "Not quite", status.x + 40 + shake, status.y + 70, 34, kError);
        float y = status.y + 124;
        for (const auto& line : canvas_.wrap(regular_, "Heard " + view.trial_heard + ". Try again, or record your knocks again.",
                 26, status.w - 80, 3)) {
            canvas_.text(regular_, line, status.x + 40, y, 26, kDim);
            y += 36;
        }
    }

    button({kMargin, 656, 260, 72}, "Record again", PanelClick{PanelCommand::Calibrate, view.kind}, std::nullopt);
    const bool passed = view.trial_passed.value_or(false);
    button({kWidth - kMargin - 260, 656, 260, 72}, "Save", PanelClick{PanelCommand::SaveCalibration, view.kind},
        std::nullopt, true, passed);
    if (state.message_is_error) message(state, 610);
}

void Panel::settings_screen(const PanelState& state) {
    button({kMargin, 36, 140, 64}, "Back", std::nullopt, Screen::Main);
    canvas_.text(bold_, "Settings", kMargin + 172, 68, 38, kText);
    update_control(state);
    const float top = 128, w = kWidth - 2 * kMargin, row = 92;
    canvas_.rounded_rect({kMargin, top, w, 6 * row}, kRadius, kSurface);
    button_row({kMargin, top, w, row}, "Knocks getting missed?", "KnockThru learns how hard you knock", "Calibrate",
        std::nullopt, Screen::Pick, false, false);
    switch_row({kMargin, top + row, w, row}, PanelClick{PanelCommand::Sounds}, state.sounds, "Sounds",
        "Click on a triple knock and on buttons", true, true);
    switch_row({kMargin, top + 2 * row, w, row}, PanelClick{PanelCommand::BarIcon}, state.classic_bar_icon,
        "Classic bar icon", "Show the logo colors in the dashboard bar", true, true);
    Canvas icon(64, 64);
    draw_icon(icon, state.classic_bar_icon ? IconStyle::BarColor : IconStyle::BarWhite);
    canvas_.draw(icon, static_cast<int>(kMargin + w - 24 - 80 - 40 - 64), static_cast<int>(top + 2 * row + (row - 64) / 2));
    button_row({kMargin, top + 3 * row, w, row}, "Guided tour", "Learn the knocks again", "Replay",
        PanelClick{PanelCommand::StartTour}, std::nullopt, false);
    button_row({kMargin, top + 4 * row, w, row}, "Something wrong?", "Tell the developer on GitHub", "Report",
        PanelClick{PanelCommand::ShowReport}, Screen::Report, false);
    button_row({kMargin, top + 5 * row, w, row}, "Factory reset", "Delete settings and calibration", "Reset", std::nullopt,
        Screen::Reset, true);
    if (state.message_is_error) message(state, top + 6 * row + 40);
}

// The version and the update button, at the top right of Settings.
void Panel::update_control(const PanelState& state) {
    using Status = UpdateView::Status;
    const auto& update = state.update;
    const float right = kWidth - kMargin, w = 250, h = 64, y = 36;
    const Rect rect{right - w, y, w, h};
    std::string status = "Version " + state.version, label = "Check for updates";
    bool primary = false, enabled = true;
    std::optional<PanelClick> click = PanelClick{PanelCommand::CheckUpdate};
    switch (update.status) {
    case Status::Idle: break;
    case Status::Checking: label = "Checking"; enabled = false; break;
    case Status::Current: status = "Version " + state.version + " is the latest"; break;
    case Status::Available:
        status = "You have version " + state.version;
        label = "Install " + update.latest;
        primary = true;
        click = PanelClick{PanelCommand::InstallUpdate};
        break;
    case Status::Installing: label = "Installing"; enabled = false; break;
    case Status::Failed: status = update.latest.empty() ? "Check failed" : "Install failed"; label = "Try again"; break;
    }
    canvas_.text(regular_, status, rect.x - 24, y + h / 2, 26, update.status == Status::Failed ? kError : kDim, Align::Right);
    button(rect, label, click, std::nullopt, primary, enabled);
}

// The report card over Settings. A phone held to a lens scans the QR code and
// opens a page the headset serves, which copies the full report and opens the
// GitHub form. Without a network, the code opens the form directly, and Save to
// headset writes the report to a file for the person to attach.
void Panel::report_card(const PanelState& state) {
    targets_.clear();
    canvas_.rounded_rect({0, 0, kWidth, kHeight}, 28, {0, 0, 0, 150});
    const Rect card{80, 60, kWidth - 160, 650};
    canvas_.rounded_rect(card, kRadius, kSurface);
    const float box = 380, x = card.x + 48, text_w = card.w - 96 - box - 40;
    canvas_.text(bold_, "Report a problem", x, card.y + 72, 38, kText);

    const auto steps = state.report_full
        ? std::array<const char*, 3>{"Scan this code with your phone.", "Tap Copy report and open GitHub.",
                                     "Paste, say what went wrong, and tap Create."}
        : std::array<const char*, 3>{"Scan this code with your phone.",
                                     "Sign in to GitHub and say what went wrong.",
                                     "Attach the saved report, then tap Create."};
    float y = card.y + 150;
    for (int i = 0; i < 3; ++i) {
        canvas_.circle(x + 22, y, 22, kAccent);
        canvas_.text(bold_, std::to_string(i + 1), x + 22, y, 26, kText, Align::Center);
        const auto lines = canvas_.wrap(regular_, steps[i], 28, text_w - 64, 2);
        for (std::size_t k = 0; k < lines.size(); ++k) canvas_.text(regular_, lines[k], x + 64, y + k * 40, 28, kText);
        y += 40 * lines.size() + 36;
    }
    const std::string hint = state.report_full ? "Hold the phone up to a lens. Use the Frame's Wi-Fi."
                                               : "The Frame is offline. Save the report below to attach it.";
    for (const auto& line : canvas_.wrap(regular_, hint, 26, text_w, 3)) {
        canvas_.text(regular_, line, x, y, 26, kDim);
        y += 36;
    }

    // Knock problems need the motion itself, so the report can carry a recording.
    const Rect record{x, card.y + 420, 240, 64};
    const bool recording = state.recording_left >= 0;
    const std::string record_label = recording ? "Recording " + std::to_string(state.recording_left) + " s"
        : state.recorded ? "Record again" : "Record knocks";
    button(record, record_label, PanelClick{PanelCommand::RecordKnocks}, std::nullopt, false, !recording);
    std::string record_note = "Knocks missed or wrong? Record 20 s of them.";
    if (recording) record_note = "Knock the way that goes wrong. Heard " + std::to_string(state.recording_knocks) + " so far.";
    else if (state.recorded) record_note = state.report_full ? "Recording added. Your phone offers it as a download."
                                                             : "Recording saved as knockthru-knocks.csv.gz.";
    const float record_x = record.x + record.w + 24;
    float ry = record.y + record.h / 2 - 16;
    for (const auto& line : canvas_.wrap(regular_, record_note, 24, text_w - record.w - 24, 2)) {
        canvas_.text(regular_, line, record_x, ry, 24, kDim);
        ry += 32;
    }

    // The code on white with a 4-module quiet zone, in whole pixels per module.
    const Rect white{card.x + card.w - 48 - box, card.y + 48, box, box};
    canvas_.rounded_rect(white, 12, {255, 255, 255});
    if (!state.report_url.empty()) {
        const auto qr = qrcodegen::QrCode::encodeText(state.report_url.c_str(), qrcodegen::QrCode::Ecc::MEDIUM);
        const int size = qr.getSize(), module = static_cast<int>(box) / (size + 8);
        const float origin_x = std::floor(white.x + (box - module * size) / 2);
        const float origin_y = std::floor(white.y + (box - module * size) / 2);
        // Each run of dark modules in a row is one rectangle, so no seams show between them.
        // Radius 0.5 fills solid. Radius 0 gives every pixel half coverage in rounded_rect.
        for (int my = 0; my < size; ++my) {
            for (int mx = 0; mx < size;) {
                if (!qr.getModule(mx, my)) { ++mx; continue; }
                int end = mx;
                while (end < size && qr.getModule(end, my)) ++end;
                canvas_.rounded_rect({origin_x + mx * module, origin_y + my * module,
                    static_cast<float>((end - mx) * module), static_cast<float>(module)}, 0.5f, {0, 0, 0});
                mx = end;
            }
        }
    }

    // Without a phone, the file is the way to get the full report into an issue.
    const float by = card.y + card.h - 48 - 72;
    const bool saved = !state.report_saved.empty();
    // Offline, the hint above already points here.
    std::string note = state.report_full ? "No phone? Save the report, then attach it to an issue." : "";
    if (saved) note = "Saved as knockthru-report.txt in your home folder.";
    if (!state.report_error.empty()) note = "Not saved: " + state.report_error;
    // The note sits between the two buttons, next to the one it explains.
    const float note_x = x + 300 + 28, note_w = card.x + card.w - 48 - 240 - 28 - note_x;
    const auto note_lines = note.empty() ? std::vector<std::string>{} : canvas_.wrap(regular_, note, 24, note_w, 2);
    float ny = by + 36 - 16 * (static_cast<float>(note_lines.size()) - 1);
    for (const auto& line : note_lines) {
        canvas_.text(regular_, line, note_x, ny, 24, state.report_error.empty() ? kDim : kError);
        ny += 32;
    }
    button({x, by, 300, 72}, saved ? "Saved" : "Save to headset", PanelClick{PanelCommand::SaveReport}, std::nullopt,
        false, !saved);
    button({card.x + card.w - 48 - 240, by, 240, 72}, "Done", PanelClick{PanelCommand::CloseReport}, Screen::Settings, true);
}

// A confirm card over the screen below it. Only its two buttons respond.
void Panel::confirm_card(const std::string& title, const std::string& text, const std::string& cancel, Screen cancel_to,
                         const std::string& confirm, PanelClick confirm_click, bool danger) {
    targets_.clear();
    canvas_.rounded_rect({0, 0, kWidth, kHeight}, 28, {0, 0, 0, 150});
    const Rect card{220, 190, kWidth - 440, 390};
    canvas_.rounded_rect(card, kRadius, kSurface);
    canvas_.text(bold_, title, card.x + 48, card.y + 72, 38, kText);
    float y = card.y + 140;
    for (const auto& line : canvas_.wrap(regular_, text, 28, card.w - 96, 3)) {
        canvas_.text(regular_, line, card.x + 48, y, 28, kText);
        y += 40;
    }
    const float by = card.y + card.h - 48 - 72;
    button({card.x + 48, by, 240, 72}, cancel, std::nullopt, cancel_to);
    button({card.x + card.w - 48 - 240, by, 240, 72}, confirm, confirm_click, Screen::Main, true, true, danger);
}

// Back and Next along the bottom, with Skip. The first step has Skip and Start, the last Back and Done.
// Next stays off until the step is complete. Skip always works, after a confirm card.
void Panel::tour_buttons(bool first, bool last, bool complete) {
    const float y = 656, h = 72;
    const Rect go{kWidth - kMargin - 200, y, 200, h};
    if (first) {
        button({kMargin, y, 180, h}, "Skip", std::nullopt, Screen::SkipTour);
        button(go, "Start", PanelClick{PanelCommand::TourNext}, std::nullopt, true);
        return;
    }
    button({kMargin, y, 180, h}, "Back", PanelClick{PanelCommand::TourBack}, std::nullopt);
    if (last) {
        button(go, "Done", PanelClick{PanelCommand::TourSkip}, std::nullopt, true);
        return;
    }
    button({go.x - 24 - 160, y, 160, h}, "Skip", std::nullopt, Screen::SkipTour);
    button(go, "Next", PanelClick{PanelCommand::TourNext}, std::nullopt, true, complete);
}

// The double and triple knock steps: the knock on a loop at the left, and at the
// right what to do and whether it worked. Knocks here really switch.
void Panel::tour_knock_step(const PanelState& state, const TourView& tour) {
    const bool triple = tour.step == TourView::Step::Triple;
    canvas_.text(bold_, triple ? "Knock three times" : "Knock twice", kMargin, 68, 38, kText);
    const Rect box{kMargin, 128, 420, 500};
    canvas_.rounded_rect(box, kRadius, kSurface);
    // The scene shows each knock kind in turn, one per loop.
    const double loop = 2.6;
    const auto kind = static_cast<KnockKind>(static_cast<int>(now_ / loop) % 3);
    knock_scene(kind, {box.x + 20, box.y + 40, box.w - 40, 340}, static_cast<float>(std::fmod(now_, loop)), triple ? 3 : 2);
    canvas_.text(bold_, kKindTitles[static_cast<int>(kind)], box.x + box.w / 2, box.y + 440, 28, kText, Align::Center);

    const float x = box.x + box.w + 48, w = kWidth - kMargin - x;
    float y = 190;
    for (const auto& line : canvas_.wrap(bold_,
             triple ? "Knock three times to switch between translucent and opaque" : "Knock twice to switch passthrough", 36, w, 2)) {
        canvas_.text(bold_, line, x, y, 36, kText);
        y += 48;
    }
    y += 4;
    // Lists only the kinds left on, since the others do nothing.
    static constexpr const char* kWays[3] = {"controllers together", "a controller on a table", "a controller on the headset"};
    std::vector<std::string> ways;
    for (int k = 0; k < 3; ++k)
        if (state.enabled[k]) ways.push_back(kWays[k]);
    std::string list;
    for (std::size_t i = 0; i < ways.size(); ++i)
        list += (i == 0 ? "" : i + 1 == ways.size() ? (ways.size() == 2 ? " or " : ", or ") : ", ") + ways[i];
    const std::string how = ways.size() > 1 ? "Any of these work: " + list + "." : "Use " + list + ".";
    for (const auto& line : canvas_.wrap(regular_, how, 28, w, 3)) {
        canvas_.text(regular_, line, x, y, 28, kDim);
        y += 40;
    }

    const Rect status{x, 408, w, 220};
    const double heard_t = triple ? tour.triple_t : tour.double_t;
    const float cy = status.y + status.h / 2;
    if (!triple && !state.room_view_error.empty()) {
        canvas_.rounded_rect(status, kRadius, kSurface);
        canvas_.text(bold_, "Passthrough unavailable", status.x + 40, status.y + 64, 34, kError);
        float line_y = status.y + 118;
        for (const auto& line : canvas_.wrap(regular_, state.room_view_error, 26, status.w - 80, 2)) {
            canvas_.text(regular_, line, status.x + 40, line_y, 26, kDim);
            line_y += 36;
        }
    } else if (heard_t < 0) {
        canvas_.rounded_rect(status, kRadius, kSurface);
        const float pulse = 0.5f + 0.5f * std::sin(static_cast<float>(now_) * 2 * anim::kPi / 1.4f);
        canvas_.circle(status.x + 56, cy, 14 + 6 * pulse, faded(kAccent, 0.5f + 0.5f * pulse));
        canvas_.text(bold_, "Listening", status.x + 96, cy, 34, kText);
    } else {
        canvas_.rounded_rect(status, kRadius, kActive);
        const float cx = status.x + 64;
        check_mark(cx, cy, 34, anim::ease_out_back(static_cast<float>(now_ - heard_t) / 0.3f));
        std::string title, detail;
        if (triple) {
            title = !state.opaque ? "Style switched" : *state.opaque ? "Room View is opaque" : "Room View is translucent";
            detail = "Knock three times again to switch back.";
        } else {
            const bool on = state.room_view.value_or(false);
            title = state.busy ? "Switching" : on ? "Passthrough is on" : "Passthrough is off";
            detail = std::string("Knock twice again to turn it ") + (on ? "off." : "on.");
        }
        canvas_.text(bold_, canvas_.fit(bold_, title, 34, status.w - 150), cx + 64, cy - 22, 34, kText);
        canvas_.text(regular_, canvas_.fit(regular_, detail, 26, status.w - 150), cx + 64, cy + 24, 26, kText);
    }
}

// The first-run tour: welcome, the knock kinds, calibration, the double knock, the triple knock, and done.
void Panel::tour_screen(const PanelState& state, const TourView& tour) {
    using Step = TourView::Step;
    steps(static_cast<int>(tour.step) + 1, 6);
    const float cx = kWidth / 2.0f;
    switch (tour.step) {
    case Step::Welcome: {
        Canvas icon(152, 152);
        draw_icon(icon);
        canvas_.draw(icon, (kWidth - 152) / 2, 150);
        canvas_.text(bold_, "Welcome to KnockThru", cx, 370, 48, kText, Align::Center);
        canvas_.text(regular_, "Knock to switch passthrough, without the side button.", cx, 440, 30, kDim, Align::Center);
        canvas_.text(regular_, "This tour takes a few minutes.", cx, 484, 30, kDim, Align::Center);
        tour_buttons(true, false, true);
        break;
    }
    case Step::Double:
    case Step::Triple:
        tour_knock_step(state, tour);
        tour_buttons(false, false, (tour.step == Step::Double ? tour.double_t : tour.triple_t) >= 0);
        break;
    case Step::Kinds: {
        canvas_.text(bold_, "Pick your knocks", kMargin, 68, 38, kText);
        const float w = kWidth - 2 * kMargin;
        canvas_.rounded_rect({kMargin, 128, w, 352}, kRadius, kSurface);
        for (int k = 0; k < 3; ++k)
            switch_row({kMargin, 136 + k * 112.0f, w, 112}, PanelClick{PanelCommand::Kind, static_cast<KnockKind>(k)},
                state.enabled[k], kKindTitles[k], kKindOnce[k], true, k > 0);
        if (!state.enabled[0] && !state.enabled[1] && !state.enabled[2])
            canvas_.text(regular_, "Turn on at least one knock to continue.", kMargin, 530, 28, kError);
        tour_buttons(false, false, state.enabled[0] || state.enabled[1] || state.enabled[2]);
        break;
    }
    case Step::Calibrate: {
        canvas_.text(bold_, "Calibrate", kMargin, 68, 38, kText);
        const float t = static_cast<float>(std::fmod(now_, 2.6));
        const float card_h = 500;
        bool complete = true; // every kind that is on has a calibration
        for (int k = 0; k < 3; ++k) {
            const Rect card{kMargin + k * 384.0f, 128, 352, card_h};
            // A kind turned off on the previous step has nothing to calibrate.
            const bool on = state.enabled[k];
            canvas_.rounded_rect(card, kRadius, on && hovered(card) ? kSurfaceHover : kSurface);
            knock_scene(static_cast<KnockKind>(k), {card.x + 16, card.y + 16, card.w - 32, card_h - 100}, t, 2);
            if (!on) canvas_.rounded_rect(card, kRadius, faded(kBackground, 0.7f));
            canvas_.text(bold_, kKindTitles[k], card.x + card.w / 2, card.y + card_h - 60, 28, on ? kText : kFaint,
                Align::Center);
            if (on && !tour.calibrated[k]) complete = false;
            const std::string note = !on ? "Off" : tour.calibrated[k] ? "Calibrated" : "Select to calibrate";
            canvas_.text(regular_, note, card.x + card.w / 2, card.y + card_h - 24, 24, tour.calibrated[k] ? kAccent : kFaint,
                Align::Center);
            if (tour.calibrated[k]) check_mark(card.x + card.w - 40, card.y + 40, 22, 1);
            if (on) targets_.push_back({card, std::nullopt, Screen::Learn, static_cast<KnockKind>(k)});
        }
        tour_buttons(false, false, complete);
        break;
    }
    case Step::Done:
        check_mark(cx, 230, 56, 1);
        canvas_.text(bold_, "You're set", cx, 340, 48, kText, Align::Center);
        canvas_.text(regular_, "Replay this tour any time from Settings.", cx, 410, 30, kDim, Align::Center);
        tour_buttons(false, true, true);
        break;
    }
    // Errors go in the strip under the buttons, which every step leaves clear.
    if (state.message_is_error)
        canvas_.text(regular_, canvas_.fit(regular_, state.message, 24, kWidth - 2 * kMargin), kMargin, 748, 24, kError);
}

const Canvas& Panel::render(const PanelState& state, std::optional<std::pair<float, float>> hover, double now) {
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

    View view = screen_ == Screen::Main ? View::Main : screen_ == Screen::Settings ? View::Settings
        : screen_ == Screen::Reset ? View::Reset : screen_ == Screen::Report ? View::Report : screen_ == Screen::Pick ? View::Pick : View::Learn;
    std::optional<TourView::Step> tour_step;
    if (calibration) {
        using Phase = CalibrationView::Phase;
        view = calibration->phase == Phase::Recording ? View::Record : View::Try;
        if (view == View::Record) record_screen(state, *calibration);
        else try_screen(state, *calibration);
    } else if (tour_on_screen) {
        view = View::Tour;
        tour_step = state.tour->step;
        tour_screen(state, *state.tour);
        if (screen_ == Screen::SkipTour)
            confirm_card("Skip the tour?", "You can replay it any time from Settings.", "Keep going", Screen::Main,
                "Skip", PanelClick{PanelCommand::TourSkip}, false);
    } else if (screen_ == Screen::Main) {
        main_screen(state);
    } else if (screen_ == Screen::Settings) {
        settings_screen(state);
    } else if (screen_ == Screen::Reset) {
        settings_screen(state);
        confirm_card("Reset KnockThru?",
            "This deletes your settings, blocked games, and calibration. The tour starts again afterward.", "Cancel",
            Screen::Settings, "Reset", PanelClick{PanelCommand::FactoryReset}, true);
    } else if (screen_ == Screen::Report) {
        settings_screen(state);
        report_card(state);
    } else if (screen_ == Screen::Pick) {
        pick_screen(state);
    } else {
        learn_screen(state);
    }

    // A new view rises into place. The first render has nothing to move from.
    // The reset confirm appears over Settings in place, so only the other views rise.
    const auto over_settings = [](std::optional<View> v) { return v == View::Reset || v == View::Report; };
    const bool overlay = (over_settings(view) && view_ == View::Settings) || (view == View::Settings && over_settings(view_));
    if ((view_ && *view_ != view && !overlay) || (tour_step && tour_step_ && *tour_step_ != *tour_step)) view_t_ = now;
    view_ = view;
    tour_step_ = tour_step;
    const float entering = static_cast<float>((now - view_t_) / 0.3);
    if (entering < 1 && now > 0) {
        scratch_ = canvas_;
        canvas_ = background_;
        canvas_.draw(scratch_, 0, static_cast<int>(std::lround(20 * (1 - anim::ease_out(entering)))), anim::ease_out(entering));
    }
    animating_ = view == View::Pick || view == View::Learn || view == View::Record || view == View::Try ||
        view == View::Tour || entering < 1;
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
