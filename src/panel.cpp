#include "panel.hpp"

#include "animation.hpp"
#include "calibrator.hpp"
#include "logo_shapes.hpp"
#include "sprite_shapes.hpp"

#include <algorithm>
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
// The knocks row is a tint of the accent while knocks act, so the passthrough tile keeps the full accent.
constexpr Color kActive{34, 62, 94};
constexpr Color kActiveHover{40, 72, 108};
constexpr Color kTable{120, 92, 70};

// Containers use a 16 px radius. Switches and the style control are pills.
constexpr float kRadius = 16;
constexpr float kMargin = 40;

constexpr const char* kKindTitles[3] = {"Controllers together", "Controller on a surface", "Tap on the headset"};
constexpr const char* kKindOnce[3] = {"Tap the controllers together.", "Knock one controller on a table or wall.",
                                      "Tap the side of the headset with a controller."};
constexpr const char* kKindNames[3] = {"a together knock", "a surface knock", "a headset tap"};

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

void Panel::button(Rect rect, const std::string& label, std::optional<PanelClick> click, std::optional<Screen> go_to,
                   bool primary, bool enabled) {
    if (!enabled) {
        canvas_.rounded_rect(rect, kRadius, kSurfaceHover);
        canvas_.text(bold_, label, rect.x + rect.w / 2, rect.y + rect.h / 2, 28, kFaint, Align::Center);
        return;
    }
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
    button({x + w - 24 - 170, y + 28, 170, 56}, "Calibrate", std::nullopt, Screen::Pick);

    // The tiles already show what worked, so the main screen only shows errors.
    if (state.message_is_error) message(state, 718);
}

// Four bars at the top right. Steps up to `current` (from 1) are filled.
void Panel::steps(int current) {
    const float w = 44, gap = 10, right = kWidth - kMargin, y = 64;
    const float left = right - 4 * w - 3 * gap;
    canvas_.text(regular_, "Step " + std::to_string(current) + " of 4", left - 20, y + 4, 24, kDim, Align::Right);
    for (int i = 0; i < 4; ++i)
        canvas_.rounded_rect({left + i * (w + gap), y, w, 8}, 4, i < current ? kAccent : kRaised);
}

// One knock kind on the shared knock timeline, fit into `box`. The layout is on
// a 340 by 240 grid, the same as the README's knock kind cards.
void Panel::knock_scene(KnockKind kind, Rect box, float t, int taps) {
    const float s = std::min(box.w / 340, box.h / 240);
    const float ox = box.x + (box.w - 340 * s) / 2, oy = box.y + (box.h - 240 * s) / 2;
    const auto X = [&](float x) { return ox + x * s; };
    const auto Y = [&](float y) { return oy + y * s; };
    const float gap = anim::knock_motion(t, taps);
    const auto played = anim::impact(t, taps);
    // A counted calibration knock buzzes at once. An action buzzes after the 0.4 s wait.
    const float action = anim::knock_time(taps - 1) + (taps == 1 ? 0.05f : 0.4f);
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
    button({kMargin, 36, 140, 64}, "Back", std::nullopt, Screen::Main);
    canvas_.text(bold_, "Calibrate", kMargin + 172, 68, 38, kText);
    steps(1);
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
        button({done.x + done.w - 32 - 170, done.y + 30, 170, 60}, "Done", std::nullopt, Screen::Main, true);
    } else if (state.message_is_error) {
        message(state, 730);
    }
}

// Step 2: how to knock for calibration, one knock at a time.
void Panel::learn_screen(const PanelState& state) {
    const int k = static_cast<int>(learn_kind_);
    button({kMargin, 36, 140, 64}, "Back", std::nullopt, Screen::Pick);
    canvas_.text(bold_, kKindTitles[k], kMargin + 172, 68, 38, kText);
    steps(2);
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
    steps(3);
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
    steps(4);
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
        const float pop = anim::ease_out_back(since / 0.3f);
        const float cx = status.x + 64, cy = status.y + status.h / 2;
        canvas_.circle(cx, cy, 34 * pop, kAccent);
        // A check mark drawn as two bars.
        const float s = pop;
        canvas_.polygon({{{cx - 16 * s, cy}, {cx - 6 * s, cy + 11 * s}, {cx - 6 * s, cy + 20 * s}, {cx - 21 * s, cy + 5 * s}}}, kText);
        canvas_.polygon({{{cx - 6 * s, cy + 11 * s}, {cx + 16 * s, cy - 13 * s}, {cx + 21 * s, cy - 8 * s}, {cx - 6 * s, cy + 20 * s}}}, kText);
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
    const float top = 150, w = kWidth - 2 * kMargin;
    canvas_.rounded_rect({kMargin, top, w, 128}, kRadius, kSurface);
    switch_row({kMargin, top, w, 128}, PanelClick{PanelCommand::BarIcon}, state.classic_bar_icon, "Classic bar icon",
        "Show the logo colors in the dashboard bar", true, false);
    Canvas icon(64, 64);
    draw_icon(icon, state.classic_bar_icon ? IconStyle::BarColor : IconStyle::BarWhite);
    canvas_.draw(icon, static_cast<int>(kMargin + w - 24 - 80 - 40 - 64), static_cast<int>(top + 32));
    if (state.message_is_error) message(state, top + 128 + 50);
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

    View view = screen_ == Screen::Main ? View::Main : screen_ == Screen::Settings ? View::Settings
        : screen_ == Screen::Pick ? View::Pick : View::Learn;
    if (calibration) {
        using Phase = CalibrationView::Phase;
        view = calibration->phase == Phase::Recording ? View::Record : View::Try;
        if (view == View::Record) record_screen(state, *calibration);
        else try_screen(state, *calibration);
    } else if (screen_ == Screen::Main) {
        main_screen(state);
    } else if (screen_ == Screen::Settings) {
        settings_screen(state);
    } else if (screen_ == Screen::Pick) {
        pick_screen(state);
    } else {
        learn_screen(state);
    }

    // A new view rises into place. The first render has nothing to move from.
    if (view_ && *view_ != view) view_t_ = now;
    view_ = view;
    const float entering = static_cast<float>((now - view_t_) / 0.3);
    if (entering < 1 && now > 0) {
        scratch_ = canvas_;
        canvas_ = background_;
        canvas_.draw(scratch_, 0, static_cast<int>(std::lround(20 * (1 - anim::ease_out(entering)))), anim::ease_out(entering));
    }
    animating_ = view == View::Pick || view == View::Learn || view == View::Record || view == View::Try || entering < 1;
    return canvas_;
}

std::optional<PanelClick> Panel::click(float x, float y) {
    for (const auto& target : targets_) {
        if (!target.rect.contains(x, y)) continue;
        if (target.go_to) {
            // Leaving the calibration screens hides the Saved banner.
            if (screen_ == Screen::Pick && *target.go_to == Screen::Main) saved_dismissed_ = true;
            screen_ = *target.go_to;
            if (target.learn) learn_kind_ = *target.learn;
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
