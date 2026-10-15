#pragma once

#include <openvr.h>

#include <cstdint>
#include <memory>

namespace knockthru {

// Shows RGBA images on an overlay through Vulkan textures. SetOverlayRaw
// reloads the image asynchronously and the panel flashes on every update.
// Here two images take turns: KnockThru fills one while SteamVR shows the
// other. Loads libvulkan at runtime and needs an active OpenVR session.
class OverlayTexture {
public:
    OverlayTexture(std::uint32_t width, std::uint32_t height); // throws if Vulkan is unavailable
    ~OverlayTexture();
    OverlayTexture(const OverlayTexture&) = delete;

    void show(vr::VROverlayHandle_t overlay, const std::uint8_t* rgba);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace knockthru
