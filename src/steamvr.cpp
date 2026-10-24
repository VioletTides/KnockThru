#include "steamvr.hpp"

#include "config.hpp"

#include <array>
#include <cstdint>
#include <ctime>
#include <fstream>
#include <iostream>

namespace knockthru {

namespace fs = std::filesystem;

volatile std::sig_atomic_t stopping = 0;

namespace {

std::ofstream log_file;

} // namespace

void log(const std::string& message) {
    const auto now = std::time(nullptr);
    char stamp[32];
    std::strftime(stamp, sizeof stamp, "%F %T", std::localtime(&now));
    std::cout << message << std::endl;
    if (log_file.is_open()) log_file << stamp << "  " << message << std::endl;
}

void open_log() {
    const auto path = user_directory() / "application.log";
    fs::create_directories(path.parent_path());
    std::error_code error;
    if (fs::exists(path) && fs::file_size(path, error) > 1024 * 1024)
        fs::rename(path, path.string() + ".old", error);
    log_file.open(path, std::ios::app);
}

// Resolved once, because /proc/self/exe stops resolving after the install
// directory is deleted.
fs::path executable() {
    static const fs::path path = fs::canonical("/proc/self/exe");
    return path;
}

fs::path install_directory() { return executable().parent_path(); }

void check_app(vr::EVRApplicationError error, const char* what) {
    if (error != vr::VRApplicationError_None)
        throw std::runtime_error(
            std::string(what) + ": " + vr::VRApplications()->GetApplicationsErrorNameFromEnum(error));
}

std::string scene_app_key() {
    const auto pid = vr::VRCompositor() ? vr::VRCompositor()->GetCurrentSceneFocusProcess() : 0;
    if (!pid) return "";
    std::array<char, vr::k_unMaxApplicationKeyLength> key{};
    if (vr::VRApplications()->GetApplicationKeyByProcessId(pid, key.data(), static_cast<uint32_t>(key.size()))
        != vr::VRApplicationError_None)
        return "";
    return key.data();
}

std::string app_name(const std::string& key) {
    if (key.empty()) return "";
    std::array<char, 256> name{};
    vr::EVRApplicationError error = vr::VRApplicationError_None;
    vr::VRApplications()->GetApplicationPropertyString(key.c_str(), vr::VRApplicationProperty_Name_String,
        name.data(), static_cast<uint32_t>(name.size()), &error);
    return error == vr::VRApplicationError_None ? name.data() : "";
}

std::string steamvr_version() {
    const char* version = vr::VRSystem() ? vr::VRSystem()->GetRuntimeVersion() : nullptr;
    return version && *version ? version : "unknown";
}

} // namespace knockthru
