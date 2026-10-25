#pragma once

#include <stdexcept>
#include <string>

namespace knockthru {

// The dashboard bar loaded, but the Room View button is not where KnockThru
// looks for it. A Steam update that changes the dashboard causes this, so a
// newer KnockThru may be needed. Other failures throw std::runtime_error.
struct DashboardChanged : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// Drives the Room View button on the SteamVR dashboard bar through the Steam
// client's browser debug port (CEF Remote Debugging in Developer settings).
// Approach from toorux/steam-frame-passthrough-toggle (MIT).
class RoomView {
public:
    explicit RoomView(int port = 8080) : port_(port) {}

    // Whether Room View is on. Throws if the port or the button is missing,
    // DashboardChanged for the button.
    bool active() const;

    // Clicks the Room View button and returns the new state.
    bool toggle() const;

    // Tints the white bar icon of `overlay_key` with its button's text color, grey when idle
    // and white on hover, like Steam's own icons. Lasts until the page reloads. `tint` false
    // removes it.
    void tint_icon(const std::string& overlay_key, bool tint = true) const;

private:
    int port_;
    std::string evaluate(const std::string& expression) const;
};

} // namespace knockthru
