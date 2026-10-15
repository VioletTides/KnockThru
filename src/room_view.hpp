#pragma once

#include <string>

namespace knockthru {

// Drives the Room View button on the SteamVR dashboard bar through the Steam
// client's browser debug port (CEF Remote Debugging in Developer settings).
// Approach from toorux/steam-frame-passthrough-toggle (MIT).
class RoomView {
public:
    explicit RoomView(int port = 8080) : port_(port) {}

    // Whether Room View is on. Throws if the port or the button is missing.
    bool active() const;

    // Clicks the Room View button and returns the new state.
    bool toggle() const;

    // Adds a style to the bar page that multiplies the thumbnail of `overlay_key` by the
    // text color of its bar button. A white icon then turns grey when idle and white on
    // hover, like Steam's own icons.
    // The style stays until the page reloads. Adding it again replaces it. With `tint`
    // false, removes the style.
    void tint_icon(const std::string& overlay_key, bool tint = true) const;

private:
    int port_;
    std::string evaluate(const std::string& expression) const;
};

} // namespace knockthru
