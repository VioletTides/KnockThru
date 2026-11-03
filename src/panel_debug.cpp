#include "panel.hpp"
#include "panel_style.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>

namespace knockthru {

using namespace panel_style;

namespace {

constexpr float kRowHeight = 80;
constexpr float kCardPad = 8;
constexpr float kLabel = 24, kDetail = 22, kButton = 22;
constexpr float kMaxAccel = 100; // m/s², the graph's full scale
constexpr float kGraphPad = 8; // above and below the full scale, inside the graph
constexpr int kMaxRows = 6; // rows a tab shows

// The i-th row of a card at `card`.
Rect row_at(Rect card, int i, float height = kRowHeight) {
    return {card.x, card.y + kCardPad + i * height, card.w, height};
}

float card_height(int rows, float row_height = kRowHeight) { return 2 * kCardPad + rows * row_height; }

Color verdict_color(const FeedEntry& entry) { return entry.good ? kAccent : kError; }

} // namespace

// The debug view: a header like Settings', three tabs, and the tab's content.
void Panel::debug_screen(const DebugView& view) {
    back_header("Advanced", Screen::Settings);
    const auto tabs = segments({kMargin, 124, kWidth - 2 * kMargin, 72}, {"Live", "Tuning", "Status"},
        static_cast<int>(debug_tab_), kButton, kRaised);
    for (std::size_t i = 0; i < tabs.size(); ++i)
        targets_.push_back({tabs[i], std::nullopt, std::nullopt, std::nullopt, static_cast<DebugTab>(i)});
    const Rect area{kMargin, 220, kWidth - 2 * kMargin, kHeight - 36 - 220};
    switch (debug_tab_) {
    case DebugTab::Live: debug_live(view, area); break;
    case DebugTab::Tuning: debug_tuning(view, area); break;
    case DebugTab::Status: debug_status(view, area); break;
    }
}

void Panel::list_card(Rect rect, int rows, float row_height) {
    canvas_.rounded_rect({rect.x, rect.y, rect.w, card_height(rows, row_height)}, kRadius, kSurface);
    for (int i = 1; i < rows; ++i)
        canvas_.rounded_rect(
            {rect.x + 28, row_at(rect, i, row_height).y, rect.w - 56, 1.5f}, 0.5f, kHairline);
}

// A row's label and its dim line. `reserve` is the width the right side needs.
void Panel::list_row(
    Rect rect, const std::string& label, const std::string& detail, float reserve, bool hover_fill) {
    if (hover_fill) canvas_.rounded_rect({rect.x + 8, rect.y, rect.w - 16, rect.h}, kRadius, kSurfaceHover);
    const float width = rect.w - 56 - reserve, cy = rect.y + rect.h / 2;
    canvas_.text(bold_, canvas_.fit(bold_, label, kLabel, width), rect.x + 28, detail.empty() ? cy : cy - 16,
        kLabel, kText);
    if (!detail.empty())
        canvas_.text(
            regular_, canvas_.fit(regular_, detail, kDetail, width), rect.x + 28, cy + 17, kDetail, kDim);
}

// A list row with a value at its right end.
void Panel::value_row(
    Rect rect, const std::string& label, const std::string& detail, const std::string& value, Color color) {
    list_row(rect, label, detail, canvas_.measure(bold_, value, kLabel) + 16, false);
    canvas_.text(bold_, value, rect.x + rect.w - 28, rect.y + rect.h / 2, kLabel, color, Align::Right);
}

// Fills a card under an "Earlier" heading with one row per entry: title, detail, and the verdict.
void Panel::feed_rows(Rect rect, const std::vector<FeedEntry>& entries) {
    const int rows = std::min<int>(entries.size(), kMaxRows);
    if (rows == 0) return;
    constexpr float kHeading = 32;
    canvas_.rounded_rect({rect.x, rect.y, rect.w, kHeading + card_height(rows)}, kRadius, kSurface);
    const Rect body{rect.x, rect.y + kHeading, rect.w, 0};
    list_card(body, rows);
    canvas_.text(bold_, "Earlier", rect.x + 28, rect.y + 38, kLabel, kText);
    for (int i = 0; i < rows; ++i) {
        const auto& entry = entries[i];
        const Rect r = row_at(body, i);
        value_row(r, entry.title, entry.detail, entry.verdict, verdict_color(entry));
    }
}

// Three traces over the last `points` buckets, newest at the right, and the threshold on top. A value
// above the full scale leaves the graph: its line is cut at the top edge and a triangle marks the spot.
void Panel::debug_graph(Rect rect, const DebugView& view, int points) {
    const float top = rect.y + kGraphPad, bottom = rect.y + rect.h - kGraphPad;
    // Not a number reads as off the scale too, because `!(value <= full scale)` holds for it.
    const auto off_scale = [](float accel) { return !(accel <= kMaxAccel); };
    const auto y_of = [&](float accel) {
        return off_scale(accel) ? top : bottom - accel / kMaxAccel * (bottom - top);
    };
    const auto x_of = [&](int i) { return rect.x + i * rect.w / (points - 1); };
    // Headset first, so the controllers draw over it.
    const std::array<std::pair<Device, Color>, 3> traces{
        {{Device::Hmd, kHeadsetTrace}, {Device::Right, kUpdate}, {Device::Left, kAccent}}};
    for (const auto& [device, color] : traces) {
        const auto& values = view.accel[static_cast<int>(device)];
        const int count = std::min<int>(values.size(), points), first = points - count;
        const std::size_t skip = values.size() - count;
        for (int i = 1; i < count; ++i) {
            const float a = values[skip + i - 1], b = values[skip + i];
            // A segment with an end off the scale runs toward a point above the graph, and the part above
            // the top edge is cut. Two off-scale ends leave nothing to draw.
            const float reach = 2 * kGraphPad;
            Point from{x_of(first + i - 1), y_of(a)}, to{x_of(first + i), y_of(b)};
            if (off_scale(a) && off_scale(b)) continue;
            if (off_scale(a)) {
                const float cut = (to.y - top) / (to.y - (top - reach));
                from = {to.x + (from.x - to.x) * cut, top};
            } else if (off_scale(b)) {
                const float cut = (from.y - top) / (from.y - (top - reach));
                to = {from.x + (to.x - from.x) * cut, top};
            }
            canvas_.line(from, to, 3, color);
        }
        // One triangle per run of off-scale buckets.
        for (int i = 0; i < count; ++i) {
            if (!off_scale(values[skip + i])) continue;
            int end = i;
            while (end + 1 < count && off_scale(values[skip + end + 1])) ++end;
            const float cx = (x_of(first + i) + x_of(first + end)) / 2;
            canvas_.polygon({{{cx - 6, top + 1}, {cx + 6, top + 1}, {cx, rect.y - 4}}}, color);
            i = end;
        }
    }
    // The line sits on a whole pixel row so its two pixels stay crisp.
    const float ty = std::round(y_of(static_cast<float>(view.threshold)));
    canvas_.rounded_rect({rect.x, ty - 1, rect.w, 2}, 1, kDim);
    char label[32];
    std::snprintf(label, sizeof label, "%.0f m/s²", view.threshold);
    const float pill_w = canvas_.measure(bold_, label, kDetail) + 24;
    canvas_.rounded_rect({rect.x + rect.w - pill_w, ty - 17, pill_w, 34}, 17, kBackground);
    canvas_.text(bold_, label, rect.x + rect.w - pill_w + 12, ty + 1, kDetail, kDim);
}

// Three dots in a row, the first `filled` of them solid.
void Panel::set_dots(float x, float cy, float radius, int filled) {
    for (int i = 0; i < 3; ++i) {
        const float cx = x + radius + i * (2 * radius + 14);
        if (i < filled)
            canvas_.circle(cx, cy, radius, kAccent);
        else
            canvas_.arc(cx, cy, radius - 3, radius, 0, 7, kDim);
    }
}

void Panel::debug_live(const DebugView& view, Rect area) {
    const float side_w = 520, gap = 24;
    const Rect tile{area.x, area.y, area.w - side_w - gap, area.h};
    canvas_.rounded_rect(tile, kRadius, kRaised);
    const float x = tile.x + 30, w = tile.w - 60, top = tile.y + 24, bottom = tile.y + tile.h - 24;

    // The key to the three traces.
    float key_x = x;
    const std::array<std::pair<const char*, Color>, 3> keys{
        {{"Left", kAccent}, {"Right", kUpdate}, {"Headset", kHeadsetTrace}}};
    for (const auto& [name, color] : keys) {
        canvas_.rounded_rect({key_x, top + 12, 22, 6}, 3, color);
        canvas_.text(regular_, name, key_x + 32, top + 15, kDetail, kDim);
        key_x += 32 + canvas_.measure(regular_, name, kDetail) + 28;
    }
    const float set_cy = bottom - 17;
    debug_graph({x, top + 44, w, set_cy - 17 - 14 - (top + 44)}, view, 500);
    canvas_.text(regular_, "Knocks", x, set_cy, kLabel, kText);
    set_dots(x + canvas_.measure(regular_, "Knocks", kLabel) + 14, set_cy, 11, view.set_knocks);

    // The last knock and the rows under it.
    const float sx = tile.x + tile.w + gap;
    const Rect last{sx, area.y, side_w, 190};
    canvas_.rounded_rect(last, kRadius, kRaised);
    canvas_.text(bold_, "Last knock", sx + 30, last.y + 38, kLabel, kText);
    const float text_w = side_w - 60;
    if (view.last) {
        canvas_.text(bold_, canvas_.fit(bold_, view.last->verdict, 56, text_w), sx + 30, last.y + 94, 56,
            verdict_color(*view.last));
        canvas_.text(
            bold_, canvas_.fit(bold_, view.last->detail, 30, text_w), sx + 30, last.y + 150, 30, kDim);
    } else {
        canvas_.text(bold_, "No knocks yet", sx + 30, last.y + 94, 56, kDim);
    }
    feed_rows({sx, last.y + last.h + gap, side_w, 0}, view.recent);
}

// The value between two round buttons.
void Panel::debug_stepper(Rect rect, const Tunable& tunable, int index) {
    canvas_.rounded_rect(rect, rect.h / 2, kBackground);
    const float r = rect.h / 2 - 4, bar = r * 0.5f;
    for (int side = 0; side < 2; ++side) {
        const float cx = side == 0 ? rect.x + 4 + r : rect.x + rect.w - 4 - r, cy = rect.y + rect.h / 2;
        const Rect hit{cx - r, cy - r, 2 * r, 2 * r};
        canvas_.circle(cx, cy, r, hovered(hit) ? kRaisedHover : kRaised);
        canvas_.rounded_rect({cx - bar / 2 * 1.4f, cy - 1.5f, bar * 1.4f, 3}, 0.5f, kText);
        if (side == 1) canvas_.rounded_rect({cx - 1.5f, cy - bar / 2 * 1.4f, 3, bar * 1.4f}, 0.5f, kText);
        PanelClick click{side == 0 ? PanelCommand::TuneDown : PanelCommand::TuneUp};
        click.tunable = index;
        targets_.push_back({hit, click});
    }
    canvas_.text(bold_, number(tunable.value, tunable.decimals), rect.x + rect.w / 2, rect.y + rect.h / 2,
        kLabel, tunable.changed() ? kUpdate : kText, Align::Center);
}

void Panel::debug_footer(const DebugView& view, Rect rect) {
    canvas_.rounded_rect(rect, kRadius, kSurface);
    const bool dirty =
        std::any_of(view.tunables.begin(), view.tunables.end(), [](const Tunable& t) { return t.changed(); });
    const float h = 64, y = rect.y + 14, right = rect.x + rect.w - 14;
    // Unsaved changes get Undo and Keep. Saved settings get Restore defaults, for tuning that broke knocks.
    float limit = right;
    if (dirty) {
        const float keep_w = pill_width("Keep after restart", 36), undo_w = pill_width("Undo changes", 36);
        const Rect keep{right - keep_w, y, keep_w, h}, undo{keep.x - 16 - undo_w, y, undo_w, h};
        button(undo, "Undo changes", PanelClick{PanelCommand::UndoTuning}, std::nullopt, false, true, false,
            kButton);
        button(keep, "Keep after restart", PanelClick{PanelCommand::KeepTuning}, std::nullopt, true, true,
            false, kButton);
        limit = undo.x - 16;
    } else {
        const float restore_w = pill_width("Restore defaults", 36);
        button({right - restore_w, y, restore_w, h}, "Restore defaults", std::nullopt,
            Screen::RestoreDefaults, false, true, false, kButton);
    }

    float x = rect.x + 28;
    if (debug_all_) {
        const float fewer_w = pill_width("Fewer settings", 28);
        button({x - 14, y, fewer_w, h}, "Fewer settings", std::nullopt, std::nullopt, false, true, false,
            kButton);
        targets_.back().all_settings = false;
        x += fewer_w + 6;
    }
    const float cy = rect.y + rect.h / 2;
    if (!dirty) {
        canvas_.text(bold_, "Using saved settings", x, cy, kLabel, kText);
        return;
    }
    canvas_.text(bold_, canvas_.fit(bold_, "Changes work now, until KnockThru restarts", kLabel, limit - x),
        x, cy - 17, kLabel, kText);
    canvas_.text(regular_, "Yellow", x, cy + 17, kDetail, kUpdate);
    canvas_.text(regular_, " values are not saved yet", x + canvas_.measure(regular_, "Yellow", kDetail),
        cy + 17, kDetail, kDim);
}

void Panel::debug_tuning(const DebugView& view, Rect area) {
    const float gap = 24, col_w = (area.w - gap) / 2;
    const Rect left{area.x, area.y, col_w, 0}, right{area.x + col_w + gap, area.y, col_w, 0};
    const int count = static_cast<int>(view.tunables.size());
    // With every setting, a row is as high as the stepper needs, so eight fit above the footer.
    const bool all = debug_all_;
    const float stepper_w = all ? 160 : 184;
    const float row_h = all ? 48 : kRowHeight, step_h = all ? 44 : 60;
    const int per_column = all ? (count + 1) / 2 : 3;
    const int rows = per_column;
    list_card(left, rows, row_h);
    list_card(right, rows, row_h);
    for (int i = 0; i < count; ++i) {
        if (!all && i >= 5) break;
        const int column = all ? i / per_column : (i < 3 ? 0 : 1);
        const int index = all ? i % per_column : (i < 3 ? i : i - 3);
        const Rect r = row_at(column == 0 ? left : right, index, row_h);
        const auto& tunable = view.tunables[i];
        list_row(r, tunable.label, all ? "" : tunable.detail, stepper_w + 16, false);
        debug_stepper(
            {r.x + r.w - 28 - stepper_w, r.y + (row_h - step_h) / 2, stepper_w, step_h}, tunable, i);
    }
    if (!all) {
        const Rect r = row_at(right, 2);
        list_row(r, "All settings", "", 0, hovered(r));
        const float cx = r.x + r.w - 38, cy = r.y + r.h / 2;
        canvas_.line({cx - 5, cy - 10}, {cx + 5, cy}, 3, kDim);
        canvas_.line({cx + 5, cy}, {cx - 5, cy + 10}, 3, kDim);
        targets_.push_back({r, std::nullopt, std::nullopt, std::nullopt, std::nullopt, true});
    }
    debug_footer(view, {area.x, area.y + card_height(rows, row_h) + gap, area.w, 92});
}

void Panel::debug_status(const DebugView& view, Rect area) {
    const float gap = 24, col_w = (area.w - gap) / 2;
    const auto rows_of = [&](Rect rect, const std::vector<StatusRow>& rows) {
        for (std::size_t i = 0; i < rows.size(); ++i) {
            const Rect r = row_at(rect, i);
            value_row(r, rows[i].label, rows[i].detail, rows[i].value, rows[i].bad ? kError : kDim);
        }
    };
    const Rect left{area.x, area.y, col_w, 0}, right{area.x + col_w + gap, area.y, col_w, 0};
    const int left_rows = std::min<int>(view.status_left.size(), kMaxRows);
    const int right_rows = std::min<int>(view.status_right.size(), kMaxRows - 2);
    list_card(left, left_rows);
    rows_of(left, {view.status_left.begin(), view.status_left.begin() + left_rows});
    list_card(right, right_rows + 2);
    rows_of(right, {view.status_right.begin(), view.status_right.begin() + right_rows});

    // Two rows of test buttons under the readings.
    const std::array<std::array<std::pair<const char*, PanelCommand>, 2>, 2> tests{
        {{{{"Buzz", PanelCommand::TestBuzz}, {"Click", PanelCommand::TestClick}}},
            {{{"Passthrough", PanelCommand::RoomView}, {"Style", PanelCommand::Style}}}}};
    const std::array<const char*, 2> names{"Test feedback", "Test actions"};
    for (int i = 0; i < 2; ++i) {
        const Rect r = row_at(right, right_rows + i);
        canvas_.text(bold_, names[i], r.x + 28, r.y + r.h / 2, kLabel, kText);
        float x = r.x + r.w - 28;
        for (int b = 1; b >= 0; --b) {
            const auto& [label, command] = tests[i][b];
            const float w = pill_width(label, 26);
            x -= w;
            button({x, r.y + (r.h - 56) / 2, w, 56}, label, PanelClick{command}, std::nullopt, false, true,
                false, kButton);
            x -= 10;
        }
    }
}

} // namespace knockthru
