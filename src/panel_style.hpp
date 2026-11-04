#pragma once

// Colors, sizes, and drawing helpers that the panel_*.cpp files share.
// Only the panel's own files include this.

#include "animation.hpp"
#include "canvas.hpp"

#include <cmath>
#include <cstdint>

namespace knockthru::panel_style {

inline constexpr Color kBackground{18, 20, 24, 248};
inline constexpr Color kSurface{28, 31, 37};
inline constexpr Color kSurfaceHover{36, 40, 47};
inline constexpr Color kRaised{44, 48, 57};
inline constexpr Color kRaisedHover{54, 59, 69};
inline constexpr Color kHairline{46, 50, 58};
inline constexpr Color kText{236, 238, 241};
inline constexpr Color kDim{150, 156, 168};
inline constexpr Color kFaint{96, 102, 114};
inline constexpr Color kAccent{58, 145, 232};
inline constexpr Color kAccentHover{80, 160, 240};
// Steam's alert yellow, from the mark on its Settings icon when a system update is ready.
inline constexpr Color kUpdate{255, 200, 44};
inline constexpr Color kKnobOff{196, 200, 208};
inline constexpr Color kError{240, 110, 100};
inline constexpr Color kErrorHover{248, 132, 122};
inline constexpr Color kOnError{40, 14, 12};
// The knocks row is a tint of the accent while knocks act, so the passthrough tile keeps the full accent.
inline constexpr Color kActive{34, 62, 94};
inline constexpr Color kActiveHover{40, 72, 108};
// The headset trace on the debug graph, apart from the blue left trace, the yellow right trace, and errors.
inline constexpr Color kHeadsetTrace{185, 140, 255};

// Containers use a 16 px radius. Switches and the style control are pills.
inline constexpr float kRadius = 16;
inline constexpr float kMargin = 40;

inline constexpr const char* kKindTitles[3] = {
    "Controllers together", "Controller on a surface", "Knock on the headset"};
inline constexpr const char* kKindOnce[3] = {"Knock the controllers together",
    "Knock one controller on a surface, like your leg", "Knock on the side of the headset with a controller"};

inline Color faded(Color color, float alpha) {
    return {color.r, color.g, color.b, static_cast<std::uint8_t>(std::lround(color.a * anim::clamp(alpha)))};
}

} // namespace knockthru::panel_style
