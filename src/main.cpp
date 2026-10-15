#include "config.hpp"
#include "knock_detector.hpp"
#include "room_view.hpp"
#include "style.hpp"

#include <json.hpp>
#include <openvr.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <unistd.h>

namespace {

namespace fs = std::filesystem;
using json = nlohmann::json;
using knockthru::Device;

constexpr const char* kAppKey = "local.knockthru";
constexpr const char* kVersion = KNOCKTHRU_VERSION;

volatile std::sig_atomic_t stopping = 0;
void stop(int) { stopping = 1; }

std::ofstream log_file;

void log(const std::string& message) {
    const auto now = std::time(nullptr);
    char stamp[32];
    std::strftime(stamp, sizeof stamp, "%F %T", std::localtime(&now));
    std::cout << message << std::endl;
    if (log_file.is_open()) log_file << stamp << "  " << message << std::endl;
}

void open_log() {
    const auto path = knockthru::user_directory() / "application.log";
    fs::create_directories(path.parent_path());
    std::error_code error;
    if (fs::exists(path) && fs::file_size(path, error) > 1024 * 1024) fs::rename(path, path.string() + ".old", error);
    log_file.open(path, std::ios::app);
}

// Resolved once, because /proc/self/exe stops resolving after the install
// directory is deleted.
fs::path executable() {
    static const fs::path path = fs::canonical("/proc/self/exe");
    return path;
}
fs::path install_directory() { return executable().parent_path(); }
fs::path app_binary() { return install_directory() / "knockthru"; }

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
            for (int i = 0; i < 50 && !stopping; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        throw std::runtime_error("Stopped while waiting for SteamVR");
    }
    ~VrSession() { vr::VR_Shutdown(); }
    VrSession(const VrSession&) = delete;
};

void check_app(vr::EVRApplicationError error, const char* what) {
    if (error != vr::VRApplicationError_None)
        throw std::runtime_error(std::string(what) + ": " + vr::VRApplications()->GetApplicationsErrorNameFromEnum(error));
}

// Reads headset and controller poses once per call. SteamVR poses are only
// valid in the standing universe on the Frame.
class PoseSampler {
public:
    using Callback = std::function<void(Device, const char* name, const vr::TrackedDevicePose_t&)>;

    void sample(const Callback& callback) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= next_refresh_) {
            auto* system = vr::VRSystem();
            indices_[0] = vr::k_unTrackedDeviceIndex_Hmd;
            indices_[1] = system->GetTrackedDeviceIndexForControllerRole(vr::TrackedControllerRole_LeftHand);
            indices_[2] = system->GetTrackedDeviceIndexForControllerRole(vr::TrackedControllerRole_RightHand);
            next_refresh_ = now + std::chrono::seconds(1);
        }
        vr::VRSystem()->GetDeviceToAbsoluteTrackingPose(
            vr::TrackingUniverseStanding, 0, poses_.data(), static_cast<uint32_t>(poses_.size()));
        static constexpr std::array<Device, 3> devices{Device::Hmd, Device::Left, Device::Right};
        static constexpr std::array<const char*, 3> names{"hmd", "left", "right"};
        for (std::size_t i = 0; i < 3; ++i) {
            if (indices_[i] >= vr::k_unMaxTrackedDeviceCount) continue;
            const auto& pose = poses_[indices_[i]];
            if (pose.bPoseIsValid) callback(devices[i], names[i], pose);
        }
    }

    static void feed(knockthru::KnockDetector& detector, double t, Device device, const vr::TrackedDevicePose_t& pose) {
        const auto& m = pose.mDeviceToAbsoluteTracking.m;
        const float* v = pose.vVelocity.v;
        detector.push(device, t, {m[0][3], m[1][3], m[2][3]}, {v[0], v[1], v[2]});
    }

    static void write_csv(std::ofstream& out, double t, const char* name, const vr::TrackedDevicePose_t& pose) {
        const auto& m = pose.mDeviceToAbsoluteTracking.m;
        const float* v = pose.vVelocity.v;
        const float* w = pose.vAngularVelocity.v;
        char line[256];
        std::snprintf(line, sizeof line, "%.6f,%s,1,%.5f,%.5f,%.5f,%.5f,%.5f,%.5f,%.5f,%.5f,%.5f\n", t, name,
            m[0][3], m[1][3], m[2][3], v[0], v[1], v[2], w[0], w[1], w[2]);
        out << line;
    }

private:
    std::array<vr::TrackedDeviceIndex_t, 3> indices_{};
    std::array<vr::TrackedDevicePose_t, vr::k_unMaxTrackedDeviceCount> poses_{};
    std::chrono::steady_clock::time_point next_refresh_{};
};

constexpr const char* kCsvHeader = "t_s,device,valid,px,py,pz,vx,vy,vz,wx,wy,wz\n";

class Clock {
public:
    double now() const { return std::chrono::duration<double>(std::chrono::steady_clock::now() - start_).count(); }
    // Sleeps until the next 1 ms tick.
    void tick() {
        next_ += std::chrono::milliseconds(1);
        std::this_thread::sleep_until(next_);
    }

private:
    std::chrono::steady_clock::time_point start_ = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point next_ = start_;
};

// Short buzzes on both controllers. Uses SteamVR input actions when the action
// manifest is loaded, and the older direct pulse call otherwise.
class Haptics {
public:
    void use_actions(vr::VRActionHandle_t left, vr::VRActionHandle_t right) { left_ = left; right_ = right; }

    // Queues `count` pulses, 150 ms apart.
    void buzz(int count, double now) {
        for (int i = 0; i < count; ++i) pending_.push_back(now + 0.15 * i);
    }

    void update(double now) {
        for (auto it = pending_.begin(); it != pending_.end();) {
            if (*it > now) { ++it; continue; }
            pulse();
            it = pending_.erase(it);
        }
    }

private:
    vr::VRActionHandle_t left_ = vr::k_ulInvalidActionHandle;
    vr::VRActionHandle_t right_ = vr::k_ulInvalidActionHandle;
    std::vector<double> pending_;

    void pulse() {
        if (left_ != vr::k_ulInvalidActionHandle) {
            for (const auto action : {left_, right_})
                vr::VRInput()->TriggerHapticVibrationAction(action, 0, 0.07f, 160, 1.0f, vr::k_ulInvalidInputValueHandle);
            return;
        }
        auto* system = vr::VRSystem();
        for (int i = 0; i < 14; ++i) { // 14 x 5 ms
            for (const auto role : {vr::TrackedControllerRole_LeftHand, vr::TrackedControllerRole_RightHand})
                system->TriggerHapticPulse(system->GetTrackedDeviceIndexForControllerRole(role), 0, 3999);
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }
};

std::string scene_app_key() {
    const auto pid = vr::VRCompositor() ? vr::VRCompositor()->GetCurrentSceneFocusProcess() : 0;
    if (!pid) return "";
    std::array<char, vr::k_unMaxApplicationKeyLength> key{};
    if (vr::VRApplications()->GetApplicationKeyByProcessId(pid, key.data(), static_cast<uint32_t>(key.size())) !=
        vr::VRApplicationError_None) return "";
    return key.data();
}

// Logs raw headset and controller poses to CSV, so knock signals can be
// inspected and replayed in detector tests.
int record(const std::string& path, double seconds) {
    VrSession session(vr::VRApplication_Background);
    std::ofstream out(path, std::ios::trunc);
    if (!out) throw std::runtime_error("Cannot write " + path);
    out << kCsvHeader;
    PoseSampler sampler;
    Clock clock;
    std::cerr << "Recording to " << path << ". Press Ctrl+C to stop.\n";
    while (!stopping && (seconds <= 0 || clock.now() < seconds)) {
        const double t = clock.now();
        sampler.sample([&](Device, const char* name, const vr::TrackedDevicePose_t& pose) {
            PoseSampler::write_csv(out, t, name, pose);
        });
        clock.tick();
    }
    std::cerr << "Recorded " << clock.now() << " s\n";
    return 0;
}

// Runs the detector live and prints every knock and action, without acting.
int detect(double seconds, const std::string& save_path) {
    VrSession session(vr::VRApplication_Background);
    knockthru::KnockDetector detector(knockthru::load_config(install_directory()).detector);
    std::ofstream out;
    if (!save_path.empty()) {
        out.open(save_path, std::ios::trunc);
        if (!out) throw std::runtime_error("Cannot write " + save_path);
        out << kCsvHeader;
    }
    PoseSampler sampler;
    Clock clock;
    std::cerr << "Detecting knocks. Press Ctrl+C to stop.\n";
    while (!stopping && (seconds <= 0 || clock.now() < seconds)) {
        const double t = clock.now();
        sampler.sample([&](Device device, const char* name, const vr::TrackedDevicePose_t& pose) {
            if (out.is_open()) PoseSampler::write_csv(out, t, name, pose);
            PoseSampler::feed(detector, t, device, pose);
        });
        const auto event = detector.poll(t);
        for (const auto& knock : detector.take_knocks())
            std::printf("%8.3f  knock    %-8s peak %5.0f m/s^2\n", knock.t, knockthru::to_string(knock.kind), knock.peak);
        if (event)
            std::printf("%8.3f  ACTION   %-8s x%d  (reported %.0f ms after the last knock)\n", event->t,
                knockthru::to_string(event->kind), event->count, (t - event->t) * 1000);
        std::fflush(stdout);
        clock.tick();
    }
    return 0;
}

// Asks for 5 double knocks of one kind and sets that kind's threshold to 60
// percent of the weakest knock. Floors come from replaying tests/data: surface
// below 50 m/s^2 splits real doubles, and together or headset below 20 lets
// rebounds pass the fast_gap_s check and turn doubles into triples.
int calibrate(const std::string& kind_name) {
    const auto kind = knockthru::parse_knock_kind(kind_name);
    if (!kind) throw std::runtime_error("--calibrate takes together, surface, or headset");
    VrSession session(vr::VRApplication_Background);
    auto config = knockthru::load_config(install_directory()).detector;
    config.controller_peak = std::min(config.controller_peak, 30.0);
    config.confirmed_peak = std::min(config.confirmed_peak, 12.0);
    config.partner_peak = std::min(config.partner_peak, 8.0);
    knockthru::KnockDetector detector(config);
    PoseSampler sampler;
    Haptics haptics;
    Clock clock;

    constexpr std::size_t kWanted = 10;
    std::vector<double> peaks;
    std::cout << "Do 5 double knocks of kind \"" << kind_name << "\", about 2 s apart. "
              << "Each knock heard gives one buzz. You have 90 s." << std::endl;
    while (!stopping && peaks.size() < kWanted && clock.now() < 90) {
        const double t = clock.now();
        sampler.sample([&](Device device, const char*, const vr::TrackedDevicePose_t& pose) {
            PoseSampler::feed(detector, t, device, pose);
        });
        detector.poll(t);
        for (const auto& knock : detector.take_knocks()) {
            if (knock.kind != *kind) {
                std::cout << "  heard a " << knockthru::to_string(knock.kind) << " knock, ignored" << std::endl;
                continue;
            }
            peaks.push_back(knock.peak);
            std::cout << "  knock " << peaks.size() << " of " << kWanted << ": "
                      << static_cast<int>(knock.peak) << " m/s^2" << std::endl;
            haptics.buzz(1, t);
        }
        haptics.update(t);
        clock.tick();
    }
    if (peaks.size() < kWanted)
        throw std::runtime_error("Calibration stopped after " + std::to_string(peaks.size()) + " knocks. Nothing saved.");

    const double weakest = *std::min_element(peaks.begin(), peaks.end());
    const double floor = *kind == knockthru::KnockKind::Surface ? 50.0 : 20.0;
    const double threshold = std::clamp(0.6 * weakest, floor, 150.0);
    auto calibration = knockthru::load_calibration();
    auto& slot = *kind == knockthru::KnockKind::Surface ? calibration.surface
        : *kind == knockthru::KnockKind::Together ? calibration.together : calibration.headset;
    slot = threshold;
    knockthru::save_calibration(calibration);
    std::cout << "Weakest knock " << static_cast<int>(weakest) << " m/s^2. Threshold for " << kind_name << " set to "
              << static_cast<int>(threshold) << " m/s^2 in " << (knockthru::user_directory() / "calibration.json").string()
              << ". A running KnockThru picks it up within 2 s.\n";
    return 0;
}

fs::path manifest_path() { return knockthru::user_directory() / "app.vrmanifest"; }

void register_app() {
    const auto binary = app_binary().generic_u8string();
    json app = {
        {"app_key", kAppKey}, {"launch_type", "binary"}, {"is_dashboard_overlay", true},
        {"binary_path_linux", binary},
        // The Frame's native ARM runtime reads this key, not binary_path_linux (from KominoVR).
        {"binary_path_linux_arm", binary},
        {"arguments", "--run"},
        {"action_manifest_path", (install_directory() / "actions.json").generic_u8string()},
        {"strings", {{"en_us", {{"name", "KnockThru"}, {"description", "Double knock to toggle passthrough"}}}}}};
    fs::create_directories(manifest_path().parent_path());
    std::ofstream(manifest_path(), std::ios::trunc) << json{{"source", "builtin"}, {"applications", json::array({app})}}.dump(2);
    check_app(vr::VRApplications()->AddApplicationManifest(manifest_path().u8string().c_str(), false), "Register with SteamVR");
}

// Stops a running KnockThru, after checking that the process is this program.
void stop_running_app() {
    const auto pid = vr::VRApplications()->GetApplicationProcessId(kAppKey);
    if (!pid || pid == static_cast<uint32_t>(getpid())) return;
    std::error_code error;
    const auto exe = fs::read_symlink(fs::path("/proc") / std::to_string(pid) / "exe", error);
    if (error || exe.filename() != "knockthru") return;
    ::kill(static_cast<pid_t>(pid), SIGTERM);
    for (int i = 0; i < 50 && fs::exists(fs::path("/proc") / std::to_string(pid)); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

int install() {
    VrSession session(vr::VRApplication_Utility);
    register_app();
    check_app(vr::VRApplications()->SetApplicationAutoLaunch(kAppKey, true), "Turn on SteamVR autostart");
    stop_running_app(); // restart so an update takes effect
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
}

int uninstall() {
    VrSession session(vr::VRApplication_Utility);
    stop_running_app();
    unregister();
    std::cout << "KnockThru stopped and removed from SteamVR. Your settings stay in "
              << knockthru::user_directory().string() << ".\n";
    return 0;
}

// The resident app. SteamVR starts it as a dashboard overlay with no visible
// surface. A double knock turns Room View on or off, and a triple knock
// switches translucent and opaque.
int run() {
    open_log();
    log(std::string("KnockThru ") + kVersion + " starting");
    VrSession session(vr::VRApplication_Overlay, true);

    const auto pid = static_cast<uint32_t>(getpid());
    std::array<char, vr::k_unMaxApplicationKeyLength> key{};
    vr::VRApplications()->GetApplicationKeyByProcessId(pid, key.data(), static_cast<uint32_t>(key.size()));
    if (std::string(key.data()) != kAppKey && vr::VRApplications()->IsApplicationInstalled(kAppKey))
        vr::VRApplications()->IdentifyApplication(pid, kAppKey);

    Haptics haptics;
    vr::VRActionSetHandle_t action_set = vr::k_ulInvalidActionSetHandle;
    vr::VRActionHandle_t left = vr::k_ulInvalidActionHandle, right = vr::k_ulInvalidActionHandle;
    const auto actions = (install_directory() / "actions.json").u8string();
    if (vr::VRInput()->SetActionManifestPath(actions.c_str()) == vr::VRInputError_None &&
        vr::VRInput()->GetActionSetHandle("/actions/knockthru", &action_set) == vr::VRInputError_None &&
        vr::VRInput()->GetActionHandle("/actions/knockthru/out/haptic_left", &left) == vr::VRInputError_None &&
        vr::VRInput()->GetActionHandle("/actions/knockthru/out/haptic_right", &right) == vr::VRInputError_None) {
        haptics.use_actions(left, right);
    } else {
        action_set = vr::k_ulInvalidActionSetHandle;
        log("SteamVR input actions unavailable. Using direct haptic pulses.");
    }
    vr::VRActiveActionSet_t active{};
    active.ulActionSet = action_set;

    const std::array<fs::path, 3> config_files{install_directory() / "config.json",
        knockthru::user_directory() / "config.json", knockthru::user_directory() / "calibration.json"};
    const auto config_stamp = [&] {
        std::string stamp;
        for (const auto& path : config_files) {
            std::error_code error;
            stamp += std::to_string(static_cast<long long>(fs::last_write_time(path, error).time_since_epoch().count())) + ",";
        }
        return stamp;
    };
    auto config = knockthru::load_config(install_directory());
    auto stamp = config_stamp();
    knockthru::KnockDetector detector(config.detector);

    PoseSampler sampler;
    Clock clock;
    std::future<std::string> room_view_job;
    std::string scene_app;
    double next_slow_check = 0;
    double binary_missing_since = -1;
    const auto binary = executable();
    log("Ready. Double knock: Room View on or off. Triple knock: translucent or opaque.");

    while (!stopping) {
        const double t = clock.now();
        vr::VREvent_t event{};
        while (vr::VRSystem()->PollNextEvent(&event, sizeof event)) {
            if (event.eventType == vr::VREvent_Quit) {
                vr::VRSystem()->AcknowledgeQuit_Exiting();
                stopping = 1;
            }
        }
        if (action_set != vr::k_ulInvalidActionSetHandle)
            vr::VRInput()->UpdateActionState(&active, sizeof active, 1);

        if (t >= next_slow_check) {
            next_slow_check = t + 2;
            scene_app = scene_app_key();
            if (const auto now_stamp = config_stamp(); now_stamp != stamp) {
                stamp = now_stamp;
                try {
                    config = knockthru::load_config(install_directory());
                    detector = knockthru::KnockDetector(config.detector);
                    log("Settings reloaded");
                } catch (const std::exception& error) {
                    log(std::string("Settings not reloaded: ") + error.what());
                }
            }
            // FrameDrop updates delete the install directory, then copy the new
            // one. A binary gone for 15 s means the app was removed.
            if (fs::exists(binary)) {
                binary_missing_since = -1;
            } else if (binary_missing_since < 0) {
                binary_missing_since = t;
            } else if (t - binary_missing_since > 15) {
                log("Install directory removed. Unregistering and exiting.");
                unregister();
                break;
            }
        }

        sampler.sample([&](Device device, const char*, const vr::TrackedDevicePose_t& pose) {
            PoseSampler::feed(detector, t, device, pose);
        });
        const auto knock_event = detector.poll(t);
        detector.take_knocks();

        if (knock_event) {
            const std::string what = std::string(knockthru::to_string(knock_event->kind)) + " x" +
                std::to_string(knock_event->count);
            if (config.blocked_apps.count(scene_app)) {
                log(what + " ignored: blocked app " + scene_app);
            } else if (knock_event->count == 3) {
                try {
                    log(what + ": style " + knockthru::to_string(knockthru::toggle_style()));
                    haptics.buzz(2, t);
                } catch (const std::exception& error) {
                    log(what + " failed: " + error.what());
                    haptics.buzz(3, t);
                }
            } else if (room_view_job.valid()) {
                log(what + " ignored: Room View change still in progress");
            } else {
                // Buzz now, because the switch can take a second. A failure buzzes 3 times later.
                haptics.buzz(1, t);
                const knockthru::RoomView room_view(config.debug_port);
                room_view_job = std::async(std::launch::async, [room_view, what] {
                    return what + ": Room View " + (room_view.toggle() ? "on" : "off");
                });
            }
        }
        if (room_view_job.valid() && room_view_job.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            try {
                log(room_view_job.get());
            } catch (const std::exception& error) {
                log(std::string("Room View change failed: ") + error.what());
                haptics.buzz(3, t);
            }
        }
        haptics.update(t);
        clock.tick();
    }
    log("KnockThru stopped");
    return 0;
}

int status(int port) {
    VrSession session(vr::VRApplication_Utility);
    auto* apps = vr::VRApplications();
    const bool installed = apps->IsApplicationInstalled(kAppKey);
    std::cout << "Registered with SteamVR: " << (installed ? "yes" : "no") << "\n";
    if (installed) {
        std::cout << "Starts with SteamVR: " << (apps->GetApplicationAutoLaunch(kAppKey) ? "yes" : "no") << "\n";
        const auto pid = apps->GetApplicationProcessId(kAppKey);
        std::cout << "Running: " << (pid ? "yes, pid " + std::to_string(pid) : std::string("no")) << "\n";
    }
    const bool on = knockthru::RoomView(port).active();
    std::cout << "Room View: " << (on ? "on" : "off") << "\n";
    return 0;
}

void usage() {
    std::cout << "KnockThru " << kVersion << ": double knock to toggle Steam Frame passthrough.\n\n"
        "Run with no arguments to register with SteamVR and start (the same as --install).\n\n"
        "  --install            Register, turn on SteamVR autostart, and start\n"
        "  --uninstall          Stop and remove from SteamVR. Keeps ~/.config/knockthru\n"
        "  --status             Show registration and Room View state\n"
        "  --toggle             Turn Room View on or off once\n"
        "  --style              Switch Room View translucent or opaque once\n"
        "  --calibrate KIND     Set the threshold for together, surface, or headset knocks\n"
        "  --current-app        Print the running app's key, for blocked_apps\n"
        "  --detect [--save F]  Print knocks live without acting. --save also writes raw poses\n"
        "  --record FILE        Write raw poses to CSV\n"
        "  --seconds N          Stop --detect or --record after N seconds\n"
        "  --run                The resident app. SteamVR starts this\n";
}

} // namespace

int main(int argc, char** argv) {
    std::signal(SIGINT, stop);
    std::signal(SIGTERM, stop);
    try {
        std::string command = "--install", path, kind;
        double seconds = 0;
        for (int i = 1; i < argc; ++i) {
            const std::string arg = argv[i];
            const auto value = [&]() -> std::string {
                if (i + 1 >= argc) throw std::runtime_error(arg + " needs a value");
                return argv[++i];
            };
            if (arg == "--help" || arg == "-h") { usage(); return 0; }
            if (arg == "--seconds") seconds = std::stod(value());
            else if (arg == "--save") path = value();
            else if (arg == "--record") { command = arg; path = value(); }
            else if (arg == "--calibrate") { command = arg; kind = value(); }
            else if (arg == "--install" || arg == "--uninstall" || arg == "--status" || arg == "--toggle" ||
                     arg == "--style" || arg == "--current-app" || arg == "--detect" || arg == "--run")
                command = arg;
            else throw std::runtime_error("Unknown argument: " + arg + ". See --help.");
        }

        // SteamVR identifies clients by executable path, so a command run from
        // the same file takes over the resident app's identity. Commands run
        // through knockthru-ctl, a byte-identical copy (from KominoVR).
        if (command != "--run" && executable().filename() == "knockthru") {
            const auto ctl = install_directory() / "knockthru-ctl";
            if (fs::exists(ctl)) {
                std::vector<char*> args{const_cast<char*>(ctl.c_str())};
                for (int i = 1; i < argc; ++i) args.push_back(argv[i]);
                args.push_back(nullptr);
                ::execv(ctl.c_str(), args.data());
                throw std::runtime_error("Cannot start " + ctl.string());
            }
        }

        if (command == "--run") return run();
        if (command == "--install") return install();
        if (command == "--uninstall") return uninstall();
        if (command == "--record") return record(path, seconds);
        if (command == "--detect") return detect(seconds, path);
        if (command == "--calibrate") return calibrate(kind);

        const auto config = knockthru::load_config(install_directory());
        if (command == "--status") return status(config.debug_port);
        if (command == "--toggle") {
            std::cout << "Room View " << (knockthru::RoomView(config.debug_port).toggle() ? "on" : "off") << "\n";
            return 0;
        }
        VrSession session(vr::VRApplication_Background);
        if (command == "--style") {
            std::cout << "Room View style " << knockthru::to_string(knockthru::toggle_style()) << "\n";
        } else if (command == "--current-app") {
            const auto key = scene_app_key();
            std::cout << (key.empty() ? "No app is running" : key) << "\n";
        }
        return 0;
    } catch (const std::exception& error) {
        if (stopping) return 0;
        log(std::string("knockthru: ") + error.what());
        return 1;
    }
}
