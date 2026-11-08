#include "panel.hpp"
#include "panel_style.hpp"
#include "calibrator.hpp"
#include "sprite_shapes.hpp"

#include <algorithm>
#include <cmath>

namespace knockthru {

using namespace panel_style;

namespace {

constexpr Color kTable{120, 92, 70};
constexpr const char* kKindNames[3] = {"a together knock", "a surface knock", "a headset knock"};

float head_radius(float height) { return 0.288f * height; } // measured on the controller sprites

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

} // namespace

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
        draw_sprite(canvas_, sprites::kControllerRight, X(cx) + shake,
            Y(table_y - head_radius(h) * 0.95f - 45 * gap), h * s, 135 - 12 * gap);
        canvas_.rounded_rect({X(cx - 130), Y(table_y), 260 * s, 16 * s}, 3 * s, kTable);
    } else {
        const float h = 130, cx = 170, cy = 120, hw = 160, hx = cx + 70, edge = hx - hw / 2;
        burst(canvas_, X(edge), Y(cy), played, s);
        draw_sprite(canvas_, sprites::kHeadset, X(hx), Y(cy), hw / sprites::kHeadset.aspect * s, 0);
        draw_sprite(canvas_, sprites::kControllerLeft, X(edge - head_radius(h) * 0.95f - 30 * gap) + shake,
            Y(cy), h * s, -30 - 8 * gap);
    }
}

// A card with the knock kind's scene above its name.
void Panel::knock_card(KnockKind kind, Rect box, float t, int knocks) {
    canvas_.rounded_rect(box, kRadius, kSurface);
    knock_scene(kind, {box.x + 20, box.y + 40, box.w - 40, 340}, t, knocks);
    canvas_.text(
        bold_, kKindTitles[static_cast<int>(kind)], box.x + box.w / 2, box.y + 440, 28, kText, Align::Center);
}

// The status card while a knock is awaited: a pulsing dot and "Listening".
void Panel::listening(Rect status) {
    canvas_.rounded_rect(status, kRadius, kSurface);
    const float pulse = 0.5f + 0.5f * std::sin(static_cast<float>(now_) * 2 * anim::kPi / 1.4f);
    const float cy = status.y + status.h / 2;
    canvas_.circle(status.x + 56, cy, 14 + 6 * pulse, faded(kAccent, 0.5f + 0.5f * pulse));
    canvas_.text(bold_, "Listening", status.x + 96, cy, 34, kText);
}

// Step 1: three cards, each knocking twice on a loop, as knocks work outside calibration.
void Panel::pick_screen(const PanelState& state) {
    back_header("Calibrate", Screen::Settings);
    steps(1, 4);
    const float t = static_cast<float>(std::fmod(now_, 2.6));
    // The cards fill the panel, and make room when the Saved banner shows.
    const bool banner = state.calibration_saved && !saved_dismissed_;
    // A saved kind that is turned off, or KnockThru paused, would leave knocks doing nothing.
    const int saved = state.calibration_saved ? static_cast<int>(*state.calibration_saved) : 0;
    const bool off = banner && !state.enabled[saved];
    const std::string saved_note = !banner ? ""
        : state.paused                     ? "KnockThru is off. Turn it on from the main screen to use knocks"
        : !state.enabled[saved]            ? std::string(kKindTitles[saved]) + " is off"
                                           : "KnockThru now listens for knocks like the ones you tried";
    const float card_h = banner ? 420 : 560;
    for (int k = 0; k < 3; ++k) {
        const Rect card{kMargin + k * 384.0f, 128, 352, card_h};
        canvas_.rounded_rect(card, kRadius, hovered(card) ? kSurfaceHover : kSurface);
        knock_scene(static_cast<KnockKind>(k), {card.x + 16, card.y + 16, card.w - 32, card_h - 100}, t, 2);
        canvas_.text(
            bold_, kKindTitles[k], card.x + card.w / 2, card.y + card_h - 44, 28, kText, Align::Center);
        targets_.push_back({card, std::nullopt, Screen::Learn, static_cast<KnockKind>(k)});
    }
    if (banner) {
        const Rect done{kMargin, 572, kWidth - 2 * kMargin, 120};
        canvas_.rounded_rect(done, kRadius, kActive);
        canvas_.text(bold_, "Saved", done.x + 32, done.y + done.h / 2 - 20, 38, kText);
        const Rect done_button{done.x + done.w - 32 - 170, done.y + 30, 170, 60};
        const float buttons_w = off ? 2 * 170 + 16 : 170;
        canvas_.text(regular_, canvas_.fit(regular_, saved_note, 26, done.w - 64 - buttons_w - 24),
            done.x + 32, done.y + done.h / 2 + 24, 26, kText);
        // A kind that is off would leave the new calibration unused.
        if (off)
            button({done_button.x - 16 - 170, done_button.y, 170, 60}, "Turn on",
                PanelClick{PanelCommand::TurnOnKind, static_cast<KnockKind>(saved)}, std::nullopt);
        button(done_button, "Done", std::nullopt, Screen::Settings, true);
    } else if (state.message_is_error) {
        message(state, 730);
    }
}

// Step 2: how to knock for calibration, one knock at a time.
void Panel::learn_screen(const PanelState& state) {
    const int k = static_cast<int>(learn_kind_);
    back_header(kKindTitles[k], Screen::Pick);
    steps(2, 4);
    const Rect box{kMargin, 128, 540, 560};
    canvas_.rounded_rect(box, kRadius, kSurface);
    knock_scene(
        learn_kind_, {box.x + 30, box.y + 60, box.w - 60, 440}, static_cast<float>(std::fmod(now_, 2.0)), 1);

    const float x = box.x + box.w + 48, w = kWidth - kMargin - x;
    canvas_.text(bold_, "Wait for the ring, then knock once", x, 190, 36, kText);
    float y = 250;
    for (const std::string& line : {std::string(kKindOnce[k]), std::string("Hold still while the ring fills"),
             std::string("Do it 10 times, one buzz per knock"),
             std::string("Knock as hard as you would to switch passthrough")}) {
        y = paragraph(regular_, line, x, y, 28, 40, w, 2, kDim);
        y += 14;
    }
    button({kWidth - kMargin - 240, 616, 240, 72}, "Start", PanelClick{PanelCommand::Calibrate, learn_kind_},
        std::nullopt, true);
    if (state.message_is_error) message(state, 730);
}

// Step 3: counts single knocks, with each one's strength.
void Panel::record_screen(const PanelState& state, const CalibrationView& view) {
    button({kMargin, 36, 160, 64}, "Cancel", PanelClick{PanelCommand::CancelCalibration, view.kind},
        std::nullopt);
    canvas_.text(bold_, "Recording", kMargin + 192, 68, 38, kText);
    steps(3, 4);
    const Rect box{kMargin, 128, 420, 560};
    knock_card(view.kind, box, static_cast<float>(std::fmod(now_, 2.0)), 1);
    canvas_.text(regular_, "Wait for the ring, then knock once", box.x + box.w / 2, box.y + 486, 26, kDim,
        Align::Center);

    const float x = box.x + box.w + 48;
    const int count = static_cast<int>(view.peaks.size());
    canvas_.text(bold_, std::to_string(count) + " of " + std::to_string(view.wanted), x, 190, 80, kText);
    canvas_.text(regular_, std::to_string(view.seconds_left) + " s left", kWidth - kMargin, 190, 28, kDim,
        Align::Right);

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
    const float cy = 610,
                ready = getting_ready
        ? anim::clamp(static_cast<float>((now_ - view.started_t) / Calibrator::kGetReady))
        : anim::clamp(static_cast<float>((now_ - view.knocked_t) / Calibrator::kPace));
    canvas_.arc(x + 26, cy, 20, 28, 0, 7, kRaised);
    if (ready < 1)
        canvas_.arc(x + 26, cy, 20, 28, anim::kPi / 2 - ready * 2 * anim::kPi, anim::kPi / 2, kAccent);
    else
        canvas_.circle(x + 26, cy, 28, kAccent);
    canvas_.text(bold_, ready >= 1 ? "Knock" : getting_ready ? "Get ready" : "Wait", x + 74, cy, 32, kText);

    // A knock that did not count gets a hint for a few seconds: too soon, too
    // light, or another kind. The newest one shows.
    const double latest = std::max({view.too_soon_t, view.too_light_t, view.ignored ? view.ignored_t : -1.0});
    const float hint = static_cast<float>(now_ - latest);
    if (latest >= 0 && hint < 3) {
        const float shown =
            std::min(anim::ease_out(hint / 0.25f), 1 - anim::ease_in_out((hint - 2.5f) / 0.5f));
        const auto text = latest == view.too_soon_t ? std::string("Too soon: wait for the ring")
            : latest == view.too_light_t
            ? std::string("Too light: knock a little harder")
            : std::string("That looked like ") + kKindNames[static_cast<int>(*view.ignored)];
        canvas_.text(regular_, text, x, 670 + 8 * (1 - shown), 26, faded(kError, shown));
    } else if (state.message_is_error) {
        message(state, 670);
    }
}

// Step 4: a double knock at the new thresholds, without switching passthrough.
void Panel::try_screen(const PanelState& state, const CalibrationView& view) {
    button({kMargin, 36, 160, 64}, "Cancel", PanelClick{PanelCommand::CancelCalibration, view.kind},
        std::nullopt);
    canvas_.text(bold_, "Try it", kMargin + 192, 68, 38, kText);
    steps(4, 4);
    const Rect box{kMargin, 128, 420, 500};
    knock_card(view.kind, box, static_cast<float>(std::fmod(now_, 2.6)), 2);

    const float x = box.x + box.w + 48, w = kWidth - kMargin - x;
    canvas_.text(bold_, "Knock twice", x, 190, 36, kText);
    canvas_.text(regular_, "All " + std::to_string(view.peaks.size()) + " knocks recorded", x, 240, 28, kDim);
    canvas_.text(regular_, "Knock the way you would to switch passthrough", x, 280, 28, kDim);
    canvas_.text(regular_, "Passthrough stays as it is", x, 320, 28, kDim);

    const Rect status{x, 368, w, 240};
    const float since = static_cast<float>(now_ - view.trial_t);
    if (!view.trial_passed) {
        listening(status);
    } else if (*view.trial_passed) {
        canvas_.rounded_rect(status, kRadius, kActive);
        const float cx = status.x + 64, cy = status.y + status.h / 2;
        check_mark(cx, cy, 34, anim::ease_out_back(since / 0.3f));
        canvas_.text(bold_, "That worked", cx + 64, cy - 22, 34, kText);
        canvas_.text(regular_, "Save to keep this calibration", cx + 64, cy + 24, 26, kText);
    } else {
        canvas_.rounded_rect(status, kRadius, kSurface);
        const float shake = anim::buzz(since, 0, 2) * 2;
        canvas_.text(bold_, "Not quite", status.x + 40 + shake, status.y + 70, 34, kError);
        float y = status.y + 124;
        y = paragraph(
            regular_, "Heard " + view.trial_heard, status.x + 40, y, 26, 36, status.w - 80, 2, kDim);
        paragraph(regular_, "Try again or record again", status.x + 40, y, 26, 36, status.w - 80, 2, kDim);
    }

    button({kMargin, 656, 260, 72}, "Record again", PanelClick{PanelCommand::Calibrate, view.kind},
        std::nullopt);
    const bool passed = view.trial_passed.value_or(false);
    const Rect save{kWidth - kMargin - 260, 656, 260, 72};
    button(save, "Save calibration", PanelClick{PanelCommand::SaveCalibration, view.kind}, std::nullopt, true,
        passed);
    // Save stays off until a knock twice passes.
    if (!passed)
        canvas_.text(
            regular_, "Knock twice to save", save.x - 24, save.y + save.h / 2, 24, kDim, Align::Right);
    if (state.message_is_error) message(state, 610);
}

} // namespace knockthru
