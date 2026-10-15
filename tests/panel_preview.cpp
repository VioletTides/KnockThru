// Writes the dashboard panel in sample states to PNG files.
// Usage: panel_preview OUTPUT_DIRECTORY
#include "panel.hpp"

#include <iostream>

using namespace knockthru;

int main(int argc, char** argv) {
    const std::filesystem::path out = argc > 1 ? argv[1] : ".";
    Panel panel(KNOCKTHRU_FONTS);

    PanelState idle;
    idle.room_view = false;
    idle.opaque = false;
    idle.app_key = "steam.app.620980";
    idle.app_name = "Beat Saber";
    idle.message = "Passthrough off (double together knock)";
    panel.render(idle, std::make_pair(900.0f, 300.0f)).write_png(out / "panel-idle.png");

    PanelState busy = idle;
    busy.room_view = true;
    busy.opaque = true;
    busy.paused = true;
    busy.app_blocked = true;
    busy.enabled = {true, false, true};
    panel.render(busy).write_png(out / "panel-paused.png");
    busy.calibrating = KnockKind::Surface;
    busy.calibration_count = 3;
    busy.calibration_seconds_left = 74;
    busy.message = "Steam debug port 8080 is closed. Turn on CEF Remote Debugging in the Frame's Developer settings.";
    busy.message_is_error = true;
    Panel calibrating_panel(KNOCKTHRU_FONTS);
    calibrating_panel.render(busy).write_png(out / "panel-calibrating.png");

    PanelState broken;
    broken.room_view_error = "Turn on CEF Remote Debugging in the Frame's Developer settings.";
    broken.opaque = false;
    panel.render(broken).write_png(out / "panel-broken.png");

    // Opens the calibration screen through its button, as a click would.
    panel.click(1050, 620);
    PanelState done = idle;
    done.message = "Saved. Controller knocks now match how hard you knock.";
    panel.render(done, std::make_pair(600.0f, 420.0f)).write_png(out / "panel-calibrate.png");

    Canvas icon(256, 256);
    Panel::draw_icon(icon);
    icon.write_png(out / "icon.png");
    std::cout << "Wrote panel previews to " << out << "\n";
}
