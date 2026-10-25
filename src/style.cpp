#include "style.hpp"

#include <openvr.h>

#include <stdexcept>
#include <string>

namespace knockthru {
namespace {

constexpr const char* kSection = "camera";
constexpr const char* kKey = "roomViewStyle";

void check(vr::EVRSettingsError error) {
    if (error != vr::VRSettingsError_None)
        throw std::runtime_error(
            std::string("camera.roomViewStyle: ") + vr::VRSettings()->GetSettingsErrorNameFromEnum(error));
}

} // namespace

RoomViewStyle read_style() {
    vr::EVRSettingsError error = vr::VRSettingsError_None;
    const auto value = vr::VRSettings()->GetInt32(kSection, kKey, &error);
    check(error);
    return value == static_cast<int>(RoomViewStyle::Opaque) ? RoomViewStyle::Opaque
                                                            : RoomViewStyle::Translucent;
}

RoomViewStyle toggle_style() {
    const auto target =
        read_style() == RoomViewStyle::Opaque ? RoomViewStyle::Translucent : RoomViewStyle::Opaque;
    vr::EVRSettingsError error = vr::VRSettingsError_None;
    vr::VRSettings()->SetInt32(kSection, kKey, static_cast<int>(target), &error);
    check(error);
    if (read_style() != target)
        throw std::runtime_error(
            "SteamVR did not keep camera.roomViewStyle. The runtime may have rejected it.");
    return target;
}

const char* to_string(RoomViewStyle style) {
    return style == RoomViewStyle::Opaque ? "opaque" : "translucent";
}

} // namespace knockthru
