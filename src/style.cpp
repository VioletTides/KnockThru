#include "style.hpp"

#include "steamvr.hpp"

#include <openvr.h>

namespace knockthru {
namespace {

constexpr const char* kSection = "camera";
constexpr const char* kKey = "roomViewStyle";
constexpr const char* kName = "camera.roomViewStyle";

} // namespace

RoomViewStyle read_style() {
    vr::EVRSettingsError error = vr::VRSettingsError_None;
    const auto value = vr::VRSettings()->GetInt32(kSection, kKey, &error);
    check_setting(error, kName);
    return value == static_cast<int>(RoomViewStyle::Opaque) ? RoomViewStyle::Opaque
                                                            : RoomViewStyle::Translucent;
}

RoomViewStyle toggle_style() {
    const auto target =
        read_style() == RoomViewStyle::Opaque ? RoomViewStyle::Translucent : RoomViewStyle::Opaque;
    set_setting(kName, target, read_style, [](RoomViewStyle style, vr::EVRSettingsError* error) {
        vr::VRSettings()->SetInt32(kSection, kKey, static_cast<int>(style), error);
    });
    return target;
}

const char* to_string(RoomViewStyle style) {
    return style == RoomViewStyle::Opaque ? "opaque" : "translucent";
}

} // namespace knockthru
