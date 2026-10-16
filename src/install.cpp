#include "install.hpp"

#include "canvas.hpp"
#include "config.hpp"
#include "diagnostics.hpp"
#include "panel.hpp"
#include "steamvr.hpp"
#include "update.hpp"

#include <json.hpp>

#include <chrono>
#include <csignal>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <unistd.h>

namespace knockthru {

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

fs::path app_binary() { return install_directory() / "knockthru"; }

fs::path manifest_path() { return user_directory() / "app.vrmanifest"; }

fs::path icon_path() { return user_directory() / "icon.png"; }

void register_app() {
    const auto binary = app_binary().generic_u8string();
    Canvas icon(256, 256);
    Panel::draw_icon(icon);
    fs::create_directories(icon_path().parent_path());
    if (!icon.write_png(icon_path())) throw std::runtime_error("Cannot write " + icon_path().string());
    json app = {
        {"app_key", kAppKey}, {"launch_type", "binary"}, {"is_dashboard_overlay", true},
        {"binary_path_linux", binary},
        // The Frame's native ARM runtime reads this key, not binary_path_linux (from KominoVR).
        {"binary_path_linux_arm", binary},
        {"arguments", "--run"},
        {"image_path", icon_path().generic_u8string()},
        {"action_manifest_path", (install_directory() / "actions.json").generic_u8string()},
        {"strings", {{"en_us", {{"name", "KnockThru"}, {"description", "Double knock to toggle passthrough"}}}}}};
    fs::create_directories(manifest_path().parent_path());
    std::ofstream(manifest_path(), std::ios::trunc) << json{{"source", "builtin"}, {"applications", json::array({app})}}.dump(2);
    check_app(vr::VRApplications()->AddApplicationManifest(manifest_path().u8string().c_str(), false), "Register with SteamVR");
}

// Stops every running resident KnockThru: a process of this user whose program
// is named knockthru and whose arguments include --run. After an update renames
// a new file over the old one, the running process's exe link reads
// "knockthru (deleted)". It reads /proc instead of asking SteamVR, which files
// knockthru-ctl under KnockThru's app key when STEAMVR_APPKEY leaks into it.
void stop_running_app() {
    std::vector<pid_t> stopped;
    std::error_code error;
    for (const auto& entry : fs::directory_iterator("/proc", error)) {
        const auto name = entry.path().filename().string();
        if (name.find_first_not_of("0123456789") != std::string::npos) continue;
        const auto pid = static_cast<pid_t>(std::stol(name));
        if (pid == getpid()) continue;
        std::error_code ignored;
        const auto exe = fs::read_symlink(entry.path() / "exe", ignored);
        if (ignored || (exe.filename() != "knockthru" && exe.filename() != "knockthru (deleted)")) continue;
        std::ifstream file(entry.path() / "cmdline", std::ios::binary);
        std::string args((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        if (args.find(std::string("--run") + '\0') == std::string::npos) continue;
        if (::kill(pid, SIGTERM) == 0) stopped.push_back(pid);
    }
    for (const auto pid : stopped)
        for (int i = 0; i < 50 && fs::exists(fs::path("/proc") / std::to_string(pid)); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

// SteamVR's own per-app file, which holds the autostart flag.
fs::path steamvr_app_config() {
    return user_directory().parent_path() / "openvr" / "config" / "vrappconfig" /
           (std::string(kAppKey) + ".vrappconfig");
}

} // namespace

int install() {
    stop_running_app(); // restart so an update takes effect
    VrSession session(vr::VRApplication_Utility);
    register_app();
    check_app(vr::VRApplications()->SetApplicationAutoLaunch(kAppKey, true), "Turn on SteamVR autostart");
    // SteamVR clears the old app's PID a few ms after the process exits. A launch
    // before that fails with ApplicationAlreadyRunning.
    for (int i = 0; i < 30 && vr::VRApplications()->GetApplicationProcessId(kAppKey) != 0; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    check_app(vr::VRApplications()->LaunchDashboardOverlay(kAppKey), "Start KnockThru");
    std::cout << "KnockThru is running and starts with SteamVR. Double knock to toggle passthrough.\n";
    return 0;
}

void unregister() {
    if (vr::VRApplications()->IsApplicationInstalled(kAppKey)) {
        vr::VRApplications()->SetApplicationAutoLaunch(kAppKey, false);
        check_app(vr::VRApplications()->RemoveApplicationManifest(manifest_path().u8string().c_str()), "Unregister");
    }
    std::error_code error;
    fs::remove(manifest_path(), error);
    fs::remove(icon_path(), error);
}

int uninstall(bool purge) {
    VrSession session(vr::VRApplication_Utility);
    stop_running_app();
    unregister();
    if (!purge) {
        std::cout << "KnockThru stopped and removed from SteamVR. Your settings stay in "
                  << user_directory().string() << ". Add --purge to delete them.\n";
        return 0;
    }
    fs::remove_all(user_directory());
    fs::remove(steamvr_app_config());
    fs::remove(report_path());
    fs::remove(recording_path());
    std::cout << "KnockThru stopped and removed from SteamVR. Deleted " << user_directory().string()
              << ", " << steamvr_app_config().string() << ", and the report files in your home folder.\n";
    return 0;
}

int update() {
    const auto work = user_directory() / "update";
    const auto release = fetch_latest_release(work);
    if (compare_versions(release.version, kVersion) <= 0) {
        std::cout << "KnockThru " << kVersion << " is up to date. The latest release is " << release.version << ".\n";
        return 0;
    }
    std::cout << "Installing KnockThru " << release.version << " from " << release.url << "\n";
    install_release(release, install_directory(), work);
    // The new files are in place. install() restarts the resident app from them.
    return install();
}

} // namespace knockthru
