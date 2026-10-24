#include "panel.hpp"
#include "panel_style.hpp"

#include <qrcodegen.hpp>

#include <algorithm>
#include <cmath>

namespace knockthru {

using namespace panel_style;

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
              "Sign in to GitHub and say what went wrong.", "Attach the saved report, then tap Create."};
    float y = card.y + 150;
    for (int i = 0; i < 3; ++i) {
        canvas_.circle(x + 22, y, 22, kAccent);
        canvas_.text(bold_, std::to_string(i + 1), x + 22, y, 26, kText, Align::Center);
        const auto lines = canvas_.wrap(regular_, steps[i], 28, text_w - 64, 2);
        for (std::size_t k = 0; k < lines.size(); ++k)
            canvas_.text(regular_, lines[k], x + 64, y + k * 40, 28, kText);
        y += 40 * lines.size() + 36;
    }
    const std::string hint = state.report_full ? "Hold the phone up to a lens. Use the Frame's Wi-Fi."
                                               : "The Frame is offline. Save the report below to attach it.";
    paragraph(regular_, hint, x, y, 26, 36, text_w, 3, kDim);

    // Knock problems need the motion itself, so the report can carry a recording.
    const Rect record{x, card.y + 420, 240, 64};
    const bool recording = state.recording_left >= 0;
    const std::string record_label = recording ? "Recording " + std::to_string(state.recording_left) + " s"
        : state.recorded                       ? "Record again"
                                               : "Record knocks";
    button(record, record_label, PanelClick{PanelCommand::RecordKnocks}, std::nullopt, false, !recording);
    std::string record_note = "Knocks missed or wrong? Record 20 s of them.";
    if (recording)
        record_note =
            "Knock the way that goes wrong. Heard " + std::to_string(state.recording_knocks) + " so far.";
    else if (state.recorded)
        record_note = state.report_full ? "Recording added. Your phone gets it in one report file."
                                        : "Recording saved as knockthru-knocks.csv.gz.";
    const float record_x = record.x + record.w + 24;
    paragraph(regular_, record_note, record_x, record.y + record.h / 2 - 16, 24, 32, text_w - record.w - 24,
        2, kDim);

    // The code on white with a 4-module quiet zone, in whole pixels per module.
    const Rect white{card.x + card.w - 48 - box, card.y + 48, box, box};
    canvas_.rounded_rect(white, kRadius, {255, 255, 255});
    if (!state.report_url.empty()) {
        const auto qr =
            qrcodegen::QrCode::encodeText(state.report_url.c_str(), qrcodegen::QrCode::Ecc::MEDIUM);
        const int size = qr.getSize(), module = static_cast<int>(box) / (size + 8);
        const float origin_x = std::floor(white.x + (box - module * size) / 2);
        const float origin_y = std::floor(white.y + (box - module * size) / 2);
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
                canvas_.rounded_rect({origin_x + mx * module, origin_y + my * module,
                                         static_cast<float>((end - mx) * module), static_cast<float>(module)},
                    0.5f, {0, 0, 0});
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
    const auto note_lines =
        note.empty() ? std::vector<std::string>{} : canvas_.wrap(regular_, note, 24, note_w, 2);
    float ny = by + 36 - 16 * (static_cast<float>(note_lines.size()) - 1);
    for (const auto& line : note_lines) {
        canvas_.text(regular_, line, note_x, ny, 24, state.report_error.empty() ? kDim : kError);
        ny += 32;
    }
    button({x, by, 300, 72}, saved ? "Saved" : "Save to headset", PanelClick{PanelCommand::SaveReport},
        std::nullopt, false, !saved);
    button({card.x + card.w - 48 - 240, by, 240, 72}, "Done", PanelClick{PanelCommand::CloseReport},
        Screen::Settings, true);
}

// A confirm card over the screen below it. Only its two buttons respond.
void Panel::confirm_card(const std::string& title, const std::string& text, const std::string& cancel,
    Screen cancel_to, const std::string& confirm, PanelClick confirm_click, bool danger) {
    targets_.clear();
    canvas_.rounded_rect({0, 0, kWidth, kHeight}, 28, {0, 0, 0, 150});
    const Rect card{220, 190, kWidth - 440, 390};
    canvas_.rounded_rect(card, kRadius, kSurface);
    canvas_.text(bold_, title, card.x + 48, card.y + 72, 38, kText);
    paragraph(regular_, text, card.x + 48, card.y + 140, 28, 40, card.w - 96, 3, kText);
    const float by = card.y + card.h - 48 - 72;
    button({card.x + 48, by, 240, 72}, cancel, std::nullopt, cancel_to);
    button(
        {card.x + card.w - 48 - 240, by, 240, 72}, confirm, confirm_click, Screen::Main, true, true, danger);
}

// The update result over the screen below it. Only OK responds.
void Panel::update_card(const UpdateNotice& notice) {
    targets_.clear();
    canvas_.rounded_rect({0, 0, kWidth, kHeight}, 28, {0, 0, 0, 150});
    // The card grows with the text, from one to three lines.
    const float w = kWidth - 440;
    const auto lines = canvas_.wrap(regular_, notice.text, 28, w - 96, 3);
    const float h = 290 + 40 * static_cast<float>(lines.size());
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
    const float by = card.y + card.h - 48 - 72;
    button({card.x + card.w - 48 - 200, by, 200, 72}, "OK", PanelClick{PanelCommand::DismissUpdateNotice},
        std::nullopt, true);
}

} // namespace knockthru
