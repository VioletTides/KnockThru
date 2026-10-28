#include "panel.hpp"
#include "panel_style.hpp"

#include <algorithm>
#include <cmath>

namespace knockthru {

using namespace panel_style;

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
    // The scene shows each knock kind left on in turn, one per loop.
    std::vector<KnockKind> shown;
    for (int k = 0; k < 3; ++k)
        if (state.enabled[k]) shown.push_back(static_cast<KnockKind>(k));
    if (shown.empty()) shown = {KnockKind::Together, KnockKind::Surface, KnockKind::Headset};
    const double loop = 2.6;
    const auto kind = shown[static_cast<std::size_t>(now_ / loop) % shown.size()];
    knock_scene(kind, {box.x + 20, box.y + 40, box.w - 40, 340}, static_cast<float>(std::fmod(now_, loop)),
        triple ? 3 : 2);
    canvas_.text(
        bold_, kKindTitles[static_cast<int>(kind)], box.x + box.w / 2, box.y + 440, 28, kText, Align::Center);

    const float x = box.x + box.w + 48, w = kWidth - kMargin - x;
    float y = 190;
    const auto title = triple ? "Knock three times to switch between translucent and opaque"
                              : "Knock twice to switch passthrough";
    y = paragraph(bold_, title, x, y, 36, 48, w, 2, kText);
    y += 4;
    // Lists only the kinds left on, since the others do nothing.
    static constexpr const char* kWays[3] = {
        "controllers together", "a controller on a surface like your leg", "a controller on the headset"};
    std::vector<std::string> ways;
    for (int k = 0; k < 3; ++k)
        if (state.enabled[k]) ways.push_back(kWays[k]);
    // "a", "a or b", "a, b, or c"
    std::string list;
    for (std::size_t i = 0; i < ways.size(); ++i) {
        const bool last = i + 1 == ways.size();
        if (i > 0 && !last) list += ", ";
        if (i > 0 && last) list += ways.size() == 2 ? " or " : ", or ";
        list += ways[i];
    }
    const std::string how = ways.size() > 1 ? "Any of these work: " + list : "Use " + list;
    paragraph(regular_, how, x, y, 28, 40, w, 3, kDim);

    const Rect status{x, 408, w, 220};
    const double heard_t = triple ? tour.triple_t : tour.double_t;
    const float cy = status.y + status.h / 2;
    if (!triple && !state.room_view_error.empty()) {
        canvas_.rounded_rect(status, kRadius, kSurface);
        canvas_.text(bold_, "Passthrough unavailable", status.x + 40, status.y + 64, 34, kError);
        paragraph(
            regular_, state.room_view_error, status.x + 40, status.y + 118, 26, 36, status.w - 80, 2, kDim);
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
            title = "Style switched";
            if (state.opaque) title = *state.opaque ? "Style is opaque" : "Style is translucent";
            // A triple knock while passthrough is off sets the style it opens in.
            detail = state.room_view == false ? "Passthrough is off. It opens in this style."
                                              : "Knock three times again to switch back";
        } else {
            const bool on = state.room_view.value_or(false);
            title = on ? "Passthrough is on" : "Passthrough is off";
            if (state.busy) title = "Switching";
            detail = std::string("Knock twice again to turn it ") + (on ? "off" : "on");
        }
        canvas_.text(bold_, canvas_.fit(bold_, title, 34, status.w - 150), cx + 64, cy - 22, 34, kText);
        canvas_.text(
            regular_, canvas_.fit(regular_, detail, 26, status.w - 150), cx + 64, cy + 24, 26, kText);
    }
}

// The first-run tour: welcome, the knock kinds, calibration, the double knock, the triple knock, and done.
void Panel::tour_screen(const PanelState& state, const TourView& tour) {
    using Step = TourView::Step;
    steps(static_cast<int>(tour.step) + 1, 6);
    const float cx = kWidth / 2.0f;
    switch (tour.step) {
    case Step::Welcome: {
        canvas_.draw(icon(152, IconStyle::Tile), (kWidth - 152) / 2, 150);
        canvas_.text(bold_, "Welcome to KnockThru", cx, 370, 48, kText, Align::Center);
        canvas_.text(regular_, "Knock to switch passthrough, without the side button", cx, 440, 30, kDim,
            Align::Center);
        canvas_.text(regular_, "This tour takes a few minutes", cx, 484, 30, kDim, Align::Center);
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
            switch_row({kMargin, 136 + k * 112.0f, w, 112},
                PanelClick{PanelCommand::Kind, static_cast<KnockKind>(k)}, state.enabled[k], kKindTitles[k],
                kKindOnce[k], true, k > 0);
        if (!state.enabled[0] && !state.enabled[1] && !state.enabled[2])
            canvas_.text(regular_, "Turn on at least one knock to continue", kMargin, 530, 28, kError);
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
            knock_scene(
                static_cast<KnockKind>(k), {card.x + 16, card.y + 16, card.w - 32, card_h - 100}, t, 2);
            if (!on) canvas_.rounded_rect(card, kRadius, faded(kBackground, 0.7f));
            canvas_.text(bold_, kKindTitles[k], card.x + card.w / 2, card.y + card_h - 60, 28,
                on ? kText : kFaint, Align::Center);
            if (on && !tour.calibrated[k]) complete = false;
            const std::string note = !on ? "Off" : tour.calibrated[k] ? "Calibrated" : "Select to calibrate";
            canvas_.text(regular_, note, card.x + card.w / 2, card.y + card_h - 24, 24,
                tour.calibrated[k] ? kAccent : kFaint, Align::Center);
            if (tour.calibrated[k]) check_mark(card.x + card.w - 40, card.y + 40, 22, 1);
            if (on) targets_.push_back({card, std::nullopt, Screen::Learn, static_cast<KnockKind>(k)});
        }
        tour_buttons(false, false, complete);
        break;
    }
    case Step::Done:
        check_mark(cx, 230, 56, 1);
        canvas_.text(bold_, "You're set", cx, 340, 48, kText, Align::Center);
        canvas_.text(regular_, "Replay this tour any time from Settings", cx, 410, 30, kDim, Align::Center);
        tour_buttons(false, true, true);
        break;
    }
    // Errors go in the strip under the buttons, which every step leaves clear.
    if (state.message_is_error)
        canvas_.text(regular_, canvas_.fit(regular_, state.message, 24, kWidth - 2 * kMargin), kMargin, 748,
            24, kError);
}

} // namespace knockthru
