#pragma once

#include <openvr.h>

#include <chrono>
#include <csignal>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <thread>

namespace knockthru {

inline constexpr const char* kAppKey = "local.knockthru";
inline constexpr const char* kPanelKey = "local.knockthru.panel";
inline constexpr const char* kVersion = KNOCKTHRU_VERSION;

// Set by SIGINT, SIGTERM, and SteamVR's quit event. Long waits check it.
extern volatile std::sig_atomic_t stopping;

// Prints a line, and appends it with a timestamp to application.log once open_log() ran.
void log(const std::string& message);
void open_log();

// This program's file, and the folder it runs from.
std::filesystem::path executable();
std::filesystem::path install_directory();

class VrSession {
public:
    // With `wait`, retries every 5 s until SteamVR is up or the process stops.
    explicit VrSession(vr::EVRApplicationType type, bool wait = false) {
        for (unsigned attempt = 0; !stopping; ++attempt) {
            vr::EVRInitError error = vr::VRInitError_None;
            vr::VR_Init(&error, type);
            if (error == vr::VRInitError_None) return;
            const std::string reason = vr::VR_GetVRInitErrorAsEnglishDescription(error);
            if (!wait) throw std::runtime_error("SteamVR unavailable: " + reason);
            if (attempt % 12 == 0) log("Waiting for SteamVR: " + reason);
            for (int i = 0; i < 50 && !stopping; ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        throw std::runtime_error("Stopped while waiting for SteamVR");
    }
    ~VrSession() { vr::VR_Shutdown(); }
    VrSession(const VrSession&) = delete;
};

// Throws with SteamVR's name for `error`, unless it is VRApplicationError_None.
void check_app(vr::EVRApplicationError error, const char* what);

// The app key of the app in front, or "" for none.
std::string scene_app_key();
// The display name of the app with `key`, or "".
std::string app_name(const std::string& key);
std::string steamvr_version();

} // namespace knockthru
