#include "panel.hpp"
#include "panel_style.hpp"

#include <algorithm>
#include <cmath>

namespace knockthru {

using namespace panel_style;

void Panel::passthrough_tile(Rect rect, const PanelState& state) {
    const bool on = state.room_view.value_or(false);
    const bool broken = !state.room_view_error.empty();
    const bool hover = hovered(rect);
    canvas_.rounded_rect(
        rect, kRadius, on ? (hover ? kAccentHover : kAccent) : (hover ? kRaisedHover : kRaised));
    const float x = rect.x + 36, w = rect.w - 72;
    canvas_.text(bold_, "Passthrough", x, rect.y + 52, 30, kText);
    std::string value = on ? "On" : "Off";
    if (state.busy)
        value = "Switching";
    else if (broken)
        value = "Unavailable";
    else if (!state.room_view)
        value = "Checking";
    canvas_.text(bold_, value, x, rect.y + 160, broken ? 64 : 96, kText);
    if (broken) {
        paragraph(regular_, state.room_view_error, x, rect.y + rect.h - 92, 26, 36, w, 2, kText);
    }
    targets_.push_back({rect, PanelClick{PanelCommand::RoomView}, std::nullopt});
}

void Panel::style_control(Rect rect, const PanelState& state) {
    canvas_.text(bold_, "Style", rect.x, rect.y + 16, 28, kText);
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
    std::string what;
    if (state.opaque) what = *state.opaque ? "You see only the room." : "The room shows through your game.";
    canvas_.text(regular_, what, rect.x, track.y + track.h + 34, 26, kDim);
}

void Panel::knocks_row(Rect rect, const PanelState& state) {
    const bool any_kind = state.enabled[0] || state.enabled[1] || state.enabled[2];
    const bool active = !state.paused && any_kind;
    const bool hover = hovered(rect);
    canvas_.rounded_rect(
        rect, kRadius, active ? (hover ? kActiveHover : kActive) : (hover ? kRaisedHover : kRaised));
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
    // A dot on Settings while an update waits for its install or its restart, ringed in the background color
    // so it reads on any fill.
    if (state.update.status == UpdateView::Status::Available
        || state.update.status == UpdateView::Status::Installed) {
        canvas_.circle(settings.x + settings.w - 6, settings.y + 6, 13, kBackground);
        canvas_.circle(settings.x + settings.w - 6, settings.y + 6, 9, kUpdate);
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
        switch_row({x, y, w, 120}, PanelClick{PanelCommand::Kind, static_cast<KnockKind>(k)},
            state.enabled[k], kKindTitles[k], "", !state.paused, false);
    y += 8;

    if (state.app_key.empty()) {
        switch_row({x, y, w, 128}, std::nullopt, false, "This game", "No game running", false, true);
    } else {
        const auto name = state.app_name.empty() ? state.app_key : state.app_name;
        switch_row({x, y, w, 128}, PanelClick{PanelCommand::BlockApp}, !state.app_blocked,
            "Knocks in " + name, state.app_blocked ? "Off for this game only" : "On", true, true);
    }

    // The tiles already show what worked, so the main screen only shows errors.
    if (state.message_is_error) message(state, 718);
}

void Panel::settings_screen(const PanelState& state) {
    button({kMargin, 36, 140, 64}, "Back", std::nullopt, Screen::Main);
    canvas_.text(bold_, "Settings", kMargin + 172, 68, 38, kText);
    update_control(state);
    const float top = 128, w = kWidth - 2 * kMargin, row = 92;
    canvas_.rounded_rect({kMargin, top, w, 6 * row}, kRadius, kSurface);
    button_row({kMargin, top, w, row}, "Knocks getting missed?", "KnockThru learns how hard you knock",
        "Calibrate", std::nullopt, Screen::Pick, false, false);
    button_row({kMargin, top + row, w, row}, "Knock actions", "What knocking twice or thrice does", "Change",
        std::nullopt, Screen::Actions, false);
    switch_row({kMargin, top + 2 * row, w, row}, PanelClick{PanelCommand::Sounds}, state.sounds, "Sounds",
        "Click on knocks and on buttons", true, true);
    button_row({kMargin, top + 3 * row, w, row}, "Guided tour", "Learn the knocks again", "Replay",
        PanelClick{PanelCommand::StartTour}, std::nullopt, false);
    button_row({kMargin, top + 4 * row, w, row}, "Something wrong?", "Tell the developer on GitHub", "Report",
        PanelClick{PanelCommand::ShowReport}, Screen::Report, false);
    button_row({kMargin, top + 5 * row, w, row}, "Factory reset", "Delete settings and calibration", "Reset",
        std::nullopt, Screen::Reset, true);
    if (state.message_is_error) message(state, top + 6 * row + 40);
}

// Knock twice and Knock thrice side by side, each a list of actions where one press picks.
void Panel::actions_screen(const PanelState& state) {
    button({kMargin, 36, 140, 64}, "Back", std::nullopt, Screen::Settings);
    canvas_.text(bold_, "Knock actions", kMargin + 172, 68, 38, kText);
    const float gap = 20, w = (kWidth - 2 * kMargin - gap) / kKnockCounts.size();
    bool differ = false;
    for (std::size_t i = 0; i < kKnockCounts.size(); ++i) {
        action_list({kMargin + i * (w + gap), 128, w, 0}, state, kKnockCounts[i]);
        differ = differ || !state.knock_actions.shared(kKnockCounts[i]);
    }
    // config.json can give each kind its own actions. The lists then pick nothing, and a press sets every
    // kind.
    const std::string note = differ
        ? "Your config.json gives knock kinds different actions. A choice here sets them all."
        : "Every kind of knock gets these actions. To set kinds apart, edit config.json.";
    if (state.message_is_error)
        message(state, 600);
    else
        canvas_.text(regular_, note, kMargin, 620, 26, kDim);
}

namespace {

// The names the action lists show.
const char* action_name(KnockAction action) {
    switch (action) {
    case KnockAction::Passthrough: return "Show or hide the room";
    case KnockAction::Style: return "Translucent or opaque";
    case KnockAction::PerfGraph: return "Performance graph";
    case KnockAction::None: return "Do nothing";
    }
    return "";
}

} // namespace

// A card titled "Knock twice" or "Knock thrice" with one row per action. The
// current action is filled, with a check mark, and the default says so. `rect`
// gives the position and width.
void Panel::action_list(Rect rect, const PanelState& state, int count) {
    const float title_h = 66, row = 84;
    canvas_.rounded_rect(
        {rect.x, rect.y, rect.w, title_h + kKnockActionList.size() * row + 8}, kRadius, kSurface);
    canvas_.text(bold_, std::string("Knock ") + count_word(count), rect.x + 24, rect.y + 38, 30, kText);
    const auto current = state.knock_actions.shared(count);
    for (std::size_t i = 0; i < kKnockActionList.size(); ++i) {
        const auto action = kKnockActionList[i];
        const Rect option{rect.x + 8, rect.y + title_h + i * row, rect.w - 16, row - 8};
        const bool selected = current == action;
        if (selected)
            canvas_.rounded_rect(option, kRadius, kActive);
        else if (hovered(option))
            canvas_.rounded_rect(option, kRadius, kSurfaceHover);
        const float cy = option.y + option.h / 2;
        const auto name = action_name(action);
        canvas_.text(bold_, name, option.x + 18, cy, 27, kText);
        if (action == KnockActions::default_for(count))
            canvas_.text(regular_, "(default)", option.x + 18 + canvas_.measure(bold_, name, 27) + 10, cy, 22,
                selected ? kText : kDim);
        if (selected)
            check_mark(option.x + option.w - 18 - 20, cy, 20, 1);
        else {
            PanelClick click{PanelCommand::SetKnockAction};
            click.count = count;
            click.action = action;
            targets_.push_back({option, click});
        }
    }
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
    case Status::Checking:
        label = "Checking";
        enabled = false;
        break;
    case Status::Current: status = "Version " + state.version + " is the latest"; break;
    case Status::Available:
        status = "You have version " + state.version;
        label = "Install " + update.latest;
        click = PanelClick{PanelCommand::InstallUpdate};
        break;
    case Status::Installing:
        label = "Installing " + std::to_string(std::lround(state.update_progress * 100)) + "%";
        break;
    case Status::Installed:
        status = "Version " + update.latest + " is installed";
        label = "Restart";
        primary = true; // the filled bar turns into the blue Restart button
        click = PanelClick{PanelCommand::RestartForUpdate};
        break;
    case Status::Restarting:
        status = "Version " + update.latest + " is installed";
        label = "Restarting";
        enabled = false;
        break;
    case Status::Updated: status = "Updated to version " + state.version; break;
    case Status::Failed:
        status = update.latest.empty() ? "Check failed" : "Install failed";
        label = "Try again";
        break;
    }
    canvas_.text(regular_, status, rect.x - 24, y + h / 2, 26,
        update.status == Status::Failed ? kError : kDim, Align::Right);
    // The alert mark ties the yellow dot on the Settings button to this row. It is as tall as the text's
    // capitals.
    if (update.status == Status::Available || update.status == Status::Installed)
        alert_mark(rect.x - 24 - canvas_.measure(regular_, status, 26) - 14, y + h / 2, 1.05f, kUpdate);
    if (update.status == Status::Installing)
        progress_button(rect, label, state.update_progress);
    else
        button(rect, label, click, std::nullopt, primary, enabled);
}

} // namespace knockthru
