#include "panel.hpp"
#include "panel_style.hpp"

#include <qrcodegen.hpp>

#include <cmath>

namespace knockthru {

using namespace panel_style;

namespace {

// Draws `text` as a QR code on white with a 4-module quiet zone, in whole pixels per module.
void draw_qr(Canvas& canvas, Rect white, const std::string& text) {
    canvas.rounded_rect(white, kRadius, {255, 255, 255});
    if (text.empty()) return;
    const auto qr = qrcodegen::QrCode::encodeText(text.c_str(), qrcodegen::QrCode::Ecc::MEDIUM);
    const int size = qr.getSize(), module = static_cast<int>(white.w) / (size + 8);
    const float origin_x = std::floor(white.x + (white.w - module * size) / 2);
    const float origin_y = std::floor(white.y + (white.h - module * size) / 2);
    // Each run of dark modules in a row is one rectangle, so no seams show between them.
    // Radius 0.5 fills solid. Radius 0 gives every pixel half coverage in rounded_rect.
    for (int my = 0; my < size; ++my) {
        for (int mx = 0; mx < size;) {
            if (!qr.getModule(mx, my)) {
                ++mx;
                continue;
            }
            int end = mx;
            while (end < size && qr.getModule(end, my)) ++end;
            canvas.rounded_rect({origin_x + mx * module, origin_y + my * module,
                                    static_cast<float>((end - mx) * module), static_cast<float>(module)},
                0.5f, {0, 0, 0});
            mx = end;
        }
    }
}

} // namespace

// Clears the targets and dims the screen below a card, so only the card responds.
void Panel::scrim() {
    targets_.clear();
    canvas_.rounded_rect({0, 0, kWidth, kHeight}, 28, {0, 0, 0, 215});
}

// The report card over Settings. A phone held to a lens scans the QR code and
// opens a page the headset serves, which copies the full report and opens the
// GitHub form. Without a network, the code opens the form directly, and Save to
// headset writes the report to a file for the person to attach.
void Panel::report_card(const PanelState& state) {
    scrim();
    const Rect card{80, 60, kWidth - 160, 650};
    canvas_.rounded_rect(card, kRadius, kSurface);
    const float box = 380, x = card.x + 48, text_w = card.w - 96 - box - 40;
    canvas_.text(bold_, "Report a problem", x, card.y + 72, 38, kText);

    const auto steps = state.report_full
        ? std::array<const char*, 3>{"Scan this code with your phone", "Press Copy report and open GitHub",
              "Paste, say what went wrong, and press Create"}
        : std::array<const char*, 3>{"Scan this code with your phone",
              "Sign in to GitHub and say what went wrong", "Attach the saved report, then press Create"};
    float y = card.y + 150;
    for (int i = 0; i < 3; ++i) {
        canvas_.circle(x + 22, y, 22, kAccent);
        canvas_.text(bold_, std::to_string(i + 1), x + 22, y, 26, kText, Align::Center);
        y = paragraph(regular_, steps[i], x + 64, y, 28, 40, text_w - 64, 2, kText) + 36;
    }
    const std::string hint = state.report_full ? "Hold the phone up to a lens, on the Frame's Wi-Fi"
                                               : "The Frame is offline: save the report below";
    paragraph(regular_, hint, x, y, 26, 36, text_w, 3, kDim);

    // Knock problems need the motion itself, so the report can carry a recording.
    const Rect record{x, card.y + 420, 240, 64};
    const bool recording = state.recording_left >= 0;
    std::string record_label = state.recorded ? "Record again" : "Record knocks";
    if (recording) record_label = "Recording " + std::to_string(state.recording_left) + " s";
    button(record, record_label, PanelClick{PanelCommand::RecordKnocks}, std::nullopt, false, !recording);
    std::string record_note = "Knocks missed or wrong? Record 20 s";
    if (recording)
        record_note =
            "Knock the way that goes wrong. Heard " + std::to_string(state.recording_knocks) + " so far";
    else if (state.recorded)
        record_note = state.report_full ? "Recording added to the report file your phone gets"
                                        : "Recording saved as knockthru-knocks.csv.gz";
    const float record_x = record.x + record.w + 24;
    paragraph(regular_, record_note, record_x, record.y + record.h / 2 - 16, 24, 32, text_w - record.w - 24,
        2, kDim);

    draw_qr(canvas_, {card.x + card.w - 48 - box, card.y + 48, box, box}, state.report_url);

    // Without a phone, the file is the way to get the full report into an issue.
    const float by = card.y + card.h - 48 - 72;
    const bool saved = !state.report_saved.empty();
    // Offline, the hint above already points here.
    std::string note = state.report_full ? "No phone? Save the report and attach it to an issue" : "";
    if (saved) note = "Saved as knockthru-report.txt in your home folder";
    if (!state.report_error.empty()) note = "Not saved: " + state.report_error;
    // The note sits between the two buttons, next to the one it explains.
    const float note_x = x + 300 + 28, note_w = card.x + card.w - 48 - 240 - 28 - note_x;
    const auto note_lines =
        note.empty() ? std::vector<std::string>{} : canvas_.wrap(regular_, note, 24, note_w, 2);
    float ny = by + 36 - 16 * (static_cast<float>(note_lines.size()) - 1);
    for (const auto& line : note_lines) {
        canvas_.text(regular_, line, note_x, ny, 24, state.report_error.empty() ? kDim : kError);
        ny += 32;
    }
    const Rect save{x, by, 300, 72};
    if (saved) {
        // A disabled button that says why: the check mark and its label are drawn here, centered together.
        canvas_.rounded_rect(save, kRadius, kSurfaceHover);
        const float r = 16, gap = 14, label_w = canvas_.measure(bold_, "Saved", 28);
        const float left = save.x + (save.w - (2 * r + gap + label_w)) / 2, cy = save.y + save.h / 2;
        check_mark(left + r, cy, r, 1, kDim);
        canvas_.text(bold_, "Saved", left + 2 * r + gap, cy, 28, kDim);
    } else {
        button(save, "Save to headset", PanelClick{PanelCommand::SaveReport}, std::nullopt);
    }
    button({card.x + card.w - 48 - 240, by, 240, 72}, "Done", PanelClick{PanelCommand::CloseReport},
        Screen::Settings, true);
}

// A confirm card over the screen below it, centred at y 385. Only its two buttons respond. Returns the card,
// so a caller can add to it.
Rect Panel::confirm_card(const std::string& title, const std::string& text, const std::string& cancel,
    Screen cancel_to, const std::string& confirm, std::optional<PanelClick> confirm_click, Screen confirm_to,
    bool danger, bool cancel_primary, float height, int max_lines) {
    scrim();
    const Rect card{220, 385 - height / 2, kWidth - 440, height};
    canvas_.rounded_rect(card, kRadius, kSurface);
    canvas_.text(bold_, title, card.x + 48, card.y + 72, 38, kText);
    paragraph(regular_, text, card.x + 48, card.y + 140, 28, 40, card.w - 96, max_lines, kText);
    const float by = card.y + card.h - 48 - 72;
    button({card.x + 48, by, 240, 72}, cancel, std::nullopt, cancel_to, cancel_primary);
    button({card.x + card.w - 48 - 240, by, 240, 72}, confirm, confirm_click, confirm_to, !cancel_primary,
        true, danger);
    return card;
}

// The warning before Advanced opens. Its switch stops it from showing again once Open is pressed.
void Panel::advanced_warning() {
    const auto open = dont_remind_ ? std::optional<PanelClick>(PanelClick{PanelCommand::HideAdvancedWarning})
                                   : std::nullopt;
    const Rect card = confirm_card("Advanced settings",
        "These change how KnockThru hears knocks. Wrong values can stop knocks from working. Restore "
        "defaults in Tuning puts them back. Use them at your own risk.",
        "Cancel", Screen::Settings, "Open", open, Screen::Debug, false, true, 500, 4);
    const Rect remind{card.x + 24, card.y + 270, card.w - 48, 72};
    switch_row(remind, std::nullopt, dont_remind_, "Don't remind me", "", true, false);
    targets_.push_back({remind});
    targets_.back().dont_remind = !dont_remind_;
}

namespace {

// Notes on the update card and the What's new screen.
constexpr float kNoteSize = 28, kNoteLeading = 40, kNoteIndent = 36;
// A version on the What's new screen: its number, its notes, then a gap.
constexpr float kVersionHead = 56, kVersionGap = 16;
// The What's new list card, and the width of its notes.
constexpr float kListTop = 128, kListHeight = Panel::kHeight - kListTop - kMargin, kListPad = 8;
constexpr float kListNoteWidth = Panel::kWidth - 2 * kMargin - 48 - kNoteIndent;

} // namespace

std::vector<std::string> Panel::note_lines(const std::string& note, float width) {
    return canvas_.wrap(regular_, note, kNoteSize, width, 99);
}

float Panel::notes_height(const VersionNotes& version, float width) {
    float lines = 0;
    for (const auto& note : version.notes) lines += note_lines(note, width).size();
    return lines * kNoteLeading;
}

// A dot and up to two lines for each note, from the first line's middle at `y`. Returns the y after them.
float Panel::bullets(const std::vector<std::string>& notes, float x, float y, float width) {
    for (const auto& note : notes) {
        canvas_.circle(x + 8, y, 4, kText);
        y = paragraph(regular_, note, x + kNoteIndent, y, kNoteSize, kNoteLeading, width, 2, kText);
    }
    return y;
}

std::vector<std::size_t> Panel::whats_new_pages(const std::vector<VersionNotes>& versions) {
    std::vector<std::size_t> starts;
    float used = kListHeight;
    for (std::size_t i = 0; i < versions.size(); ++i) {
        const float h = kVersionHead + notes_height(versions[i], kListNoteWidth) + kVersionGap;
        if (used + h > kListHeight - 2 * kListPad) {
            starts.push_back(i);
            used = 0;
        }
        used += h;
    }
    return starts;
}

std::string Panel::whats_new_overflow(const std::vector<VersionNotes>& versions) {
    for (const auto& version : versions) {
        for (const auto& note : version.notes) {
            const auto lines = note_lines(note, kListNoteWidth);
            const bool wide = std::any_of(lines.begin(), lines.end(), [&](const auto& line) {
                return canvas_.measure(regular_, line, kNoteSize) > kListNoteWidth;
            });
            if (lines.size() > 2 || wide)
                return version.version + " has a note longer than two lines: " + note;
        }
        if (kVersionHead + notes_height(version, kListNoteWidth) + kVersionGap > kListHeight - 2 * kListPad)
            return version.version + " does not fit on one page";
    }
    return "";
}

// The update result over the screen below it. After an update, one version whose notes fit shows them
// here, and anything more gets a What's new button.
void Panel::update_card(const PanelState& state) {
    const auto& notice = *state.update_notice;
    scrim();
    const float w = kWidth - 440, note_width = w - 96 - kNoteIndent;
    const auto lines = canvas_.wrap(regular_, notice.text, 28, w - 96, 3);
    const auto& versions = notice.failed ? std::vector<VersionNotes>{} : state.whats_new;
    bool inline_notes = versions.size() == 1;
    float note_count = 0;
    if (inline_notes) {
        for (const auto& note : versions[0].notes) {
            const auto n = note_lines(note, note_width).size();
            inline_notes = inline_notes && n <= 2;
            note_count += n;
        }
        inline_notes = inline_notes && note_count <= 5;
    }
    // The card grows with the text, from one to three lines, and with the notes.
    const float notes_h = inline_notes ? 16 + kNoteLeading * note_count : 0;
    const float h = 290 + 40 * static_cast<float>(lines.size()) + notes_h;
    const Rect card{220, (kHeight - h) / 2, w, h};
    canvas_.rounded_rect(card, kRadius, kSurface);
    const float cx = card.x + 48 + 30, cy = card.y + 78;
    if (notice.failed) {
        canvas_.circle(cx, cy, 30, kError);
        alert_mark(cx, cy, 1.6f, kOnError);
    } else {
        check_mark(cx, cy, 30, 1);
    }
    canvas_.text(bold_, notice.failed ? "Update failed" : "KnockThru updated", cx + 52, cy, 38, kText);
    float y = card.y + 156;
    for (const auto& line : lines) {
        canvas_.text(regular_, line, card.x + 48, y, 28, kText);
        y += 40;
    }
    if (inline_notes) bullets(versions[0].notes, card.x + 48, y + 16, note_width);
    const float by = card.y + card.h - 48 - 72;
    const PanelClick dismiss{PanelCommand::DismissUpdateNotice};
    button({card.x + card.w - 48 - 200, by, 200, 72}, "OK", dismiss, std::nullopt, true);
    if (!versions.empty() && !inline_notes)
        button({card.x + card.w - 48 - 200 - 20 - 240, by, 240, 72}, "What's new", dismiss, Screen::WhatsNew);
}

// The notes of every version since the one before the update, a page at a time, newest first.
void Panel::whats_new_screen(const PanelState& state) {
    back_header("What's new", Screen::Main);
    const auto& versions = state.whats_new;
    const auto starts = whats_new_pages(versions);
    const int pages = static_cast<int>(starts.size());
    whats_new_page_ = std::clamp(whats_new_page_, 0, std::max(pages - 1, 0));
    if (pages > 1) {
        const float right = kWidth - kMargin;
        const Rect older{right - 200, 36, 200, 64}, newer{older.x - 20 - 190, 36, 190, 64};
        const bool first = whats_new_page_ == 0, last = whats_new_page_ == pages - 1;
        button(newer, "Newer", std::nullopt, std::nullopt, false, !first);
        if (!first) targets_.back().page = whats_new_page_ - 1;
        button(older, "Older", std::nullopt, std::nullopt, false, !last);
        if (!last) targets_.back().page = whats_new_page_ + 1;
        canvas_.text(regular_, std::to_string(whats_new_page_ + 1) + " of " + std::to_string(pages),
            newer.x - 24, 68, 26, kDim, Align::Right);
    }
    if (versions.empty()) return;
    const std::size_t begin = starts[whats_new_page_];
    const std::size_t end = whats_new_page_ + 1 < pages ? starts[whats_new_page_ + 1] : versions.size();
    const float x = kMargin + 24, w = kWidth - 2 * kMargin;
    float bottom = kListTop + kListPad;
    for (std::size_t i = begin; i < end; ++i)
        bottom += kVersionHead + notes_height(versions[i], kListNoteWidth) + kVersionGap;
    canvas_.rounded_rect({kMargin, kListTop, w, bottom + kListPad - kListTop}, kRadius, kSurface);
    float y = kListTop + kListPad;
    for (std::size_t i = begin; i < end; ++i) {
        if (i != begin) canvas_.rounded_rect({x, y, w - 48, 1.5f}, 0, kHairline);
        canvas_.text(bold_, versions[i].version, x, y + 34, 28, kDim);
        y = bullets(versions[i].notes, x, y + kVersionHead + kNoteLeading / 2, kListNoteWidth)
            - kNoteLeading / 2 + kVersionGap;
    }
}

} // namespace knockthru
