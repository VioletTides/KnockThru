#include "calibrator.hpp"
#include "config.hpp"
#include "knock_detector.hpp"
#include "overlay_texture.hpp"
#include "panel.hpp"
#include "room_view.hpp"
#include "sound.hpp"
#include "style.hpp"
#include "report.hpp"
#include "report_server.hpp"
#include "update.hpp"

#include <json.hpp>
#include <openvr.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <array>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <functional>
#include <atomic>
#include <future>
#include <iostream>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <sys/utsname.h>
#include <unistd.h>

namespace {

namespace fs = std::filesystem;
using json = nlohmann::json;
using knockthru::Device;

constexpr const char* kAppKey = "local.knockthru";
constexpr const char* kPanelKey = "local.knockthru.panel";
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

constexpr std::array<const char*, 3> kDeviceNames{"hmd", "left", "right"}; // indexed by Device

knockthru::Vec3 position_of(const vr::TrackedDevicePose_t& pose) {
    const auto& m = pose.mDeviceToAbsoluteTracking.m;
    return {m[0][3], m[1][3], m[2][3]};
}

knockthru::Vec3 velocity_of(const vr::TrackedDevicePose_t& pose) {
    const float* v = pose.vVelocity.v;
    return {v[0], v[1], v[2]};
}

constexpr const char* kCsvHeader = "t_s,device,valid,px,py,pz,vx,vy,vz,wx,wy,wz\n";

// One row of the tests/data CSV format. Without `angular`, its columns are zero.
void write_csv_row(std::ostream& out, double t, Device device, const knockthru::Vec3& p, const knockthru::Vec3& v,
                   const float* angular = nullptr) {
    char line[256];
    const int n = std::snprintf(line, sizeof line, "%.6f,%s,1,%.5f,%.5f,%.5f,%.5f,%.5f,%.5f,", t,
        kDeviceNames[static_cast<int>(device)], p[0], p[1], p[2], v[0], v[1], v[2]);
    if (angular) std::snprintf(line + n, sizeof line - n, "%.5f,%.5f,%.5f\n", angular[0], angular[1], angular[2]);
    else std::snprintf(line + n, sizeof line - n, "0,0,0\n");
    out << line;
}

// Reads headset and controller poses once per call. SteamVR poses are only
// valid in the standing universe on the Frame.
class PoseSampler {
public:
    using Callback = std::function<void(Device, const vr::TrackedDevicePose_t&)>;

    void sample(const Callback& callback) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= next_refresh_) {
            auto* system = vr::VRSystem();
            indices_[0] = vr::k_unTrackedDeviceIndex_Hmd;
            indices_[1] = system->GetTrackedDeviceIndexForControllerRole(vr::TrackedControllerRole_LeftHand);
            indices_[2] = system->GetTrackedDeviceIndexForControllerRole(vr::TrackedControllerRole_RightHand);
            next_refresh_ = now + std::chrono::seconds(1);
        }
        // Fetches only up to the highest device index in use, not all 64.
        std::uint32_t count = 0;
        for (const auto index : indices_)
            if (index < vr::k_unMaxTrackedDeviceCount) count = std::max(count, index + 1);
        vr::VRSystem()->GetDeviceToAbsoluteTrackingPose(vr::TrackingUniverseStanding, 0, poses_.data(), count);
        for (std::size_t i = 0; i < 3; ++i) {
            if (indices_[i] >= vr::k_unMaxTrackedDeviceCount) continue;
            const auto& pose = poses_[indices_[i]];
            if (pose.bPoseIsValid) callback(static_cast<Device>(i), pose);
        }
    }

    static void feed(knockthru::KnockDetector& detector, double t, Device device, const vr::TrackedDevicePose_t& pose) {
        detector.push(device, t, position_of(pose), velocity_of(pose));
    }

    static void write_csv(std::ostream& out, double t, Device device, const vr::TrackedDevicePose_t& pose) {
        write_csv_row(out, t, device, position_of(pose), velocity_of(pose), pose.vAngularVelocity.v);
    }

private:
    std::array<vr::TrackedDeviceIndex_t, 3> indices_{};
    std::array<vr::TrackedDevicePose_t, vr::k_unMaxTrackedDeviceCount> poses_{};
    std::chrono::steady_clock::time_point next_refresh_{};
};

class Clock {
public:
    double now() const { return std::chrono::duration<double>(std::chrono::steady_clock::now() - start_).count(); }
    // Sleeps until the next tick, `step` after the last one.
    void tick(std::chrono::milliseconds step = std::chrono::milliseconds(1)) {
        next_ += step;
        std::this_thread::sleep_until(next_);
    }

private:
    std::chrono::steady_clock::time_point start_ = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point next_ = start_;
};

// Samples poses every millisecond on its own thread. Knock strength is the
// velocity change between samples over the time between them, so a main loop
// busy drawing the panel would stretch those gaps and garble the strength.
class PoseThread {
public:
    struct Sample {
        Device device;
        double t;
        knockthru::Vec3 position, velocity;
    };

    explicit PoseThread(const Clock& clock) : thread_([this, &clock] { loop(clock); }) {}
    ~PoseThread() {
        stop_ = true;
        thread_.join();
    }
    PoseThread(const PoseThread&) = delete;

    // Returns the samples since the last call, oldest first.
    std::vector<Sample> take() {
        std::vector<Sample> samples;
        std::lock_guard<std::mutex> lock(mutex_);
        samples.swap(samples_);
        return samples;
    }

private:
    void loop(const Clock& clock) {
        PoseSampler sampler;
        Clock ticker;
        while (!stop_) {
            const double t = clock.now();
            sampler.sample([&](Device device, const vr::TrackedDevicePose_t& pose) {
                std::lock_guard<std::mutex> lock(mutex_);
                samples_.push_back({device, t, position_of(pose), velocity_of(pose)});
            });
            ticker.tick();
        }
    }

    std::mutex mutex_;
    std::vector<Sample> samples_;
    std::atomic<bool> stop_{false};
    std::thread thread_; // last, so it starts after the members it uses
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
        sampler.sample([&](Device device, const vr::TrackedDevicePose_t& pose) {
            PoseSampler::write_csv(out, t, device, pose);
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
        sampler.sample([&](Device device, const vr::TrackedDevicePose_t& pose) {
            if (out.is_open()) PoseSampler::write_csv(out, t, device, pose);
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

std::string thresholds(const knockthru::KindCalibration& calibration) {
    char line[128];
    std::snprintf(line, sizeof line, "spike %.0f m/s^2, approach %.2f m/s", calibration.peak, calibration.approach);
    return line;
}

// "double" or "triple".
const char* sequence_name(const knockthru::KnockEvent& event) { return event.count == 3 ? "triple" : "double"; }

// Asks for knocks of one kind over the terminal and saves a new threshold.
int calibrate(const std::string& kind_name) {
    const auto kind = knockthru::parse_knock_kind(kind_name);
    if (!kind) throw std::runtime_error("--calibrate takes together, surface, or headset");
    VrSession session(vr::VRApplication_Background);
    Clock clock;
    knockthru::Calibrator calibrator(*kind, knockthru::load_config(install_directory()).detector, clock.now());
    knockthru::KnockDetector detector(calibrator.detector_config());
    PoseSampler sampler;
    Haptics haptics;

    std::cout << "Knock " << knockthru::Calibrator::kWanted << " times, kind \"" << kind_name
              << "\", about 1 s apart. Each knock heard gives one buzz. You have "
              << static_cast<int>(knockthru::Calibrator::kSeconds) << " s." << std::endl;
    while (!stopping && !calibrator.done() && calibrator.seconds_left(clock.now()) > 0) {
        const double t = clock.now();
        sampler.sample([&](Device device, const vr::TrackedDevicePose_t& pose) {
            PoseSampler::feed(detector, t, device, pose);
        });
        detector.poll(t);
        for (const auto& knock : detector.take_knocks()) calibrator.hear(knock);
        while (const auto result = calibrator.next(t)) {
            const auto& knock = result->knock;
            if (result->heard == knockthru::Calibrator::Heard::OtherKind)
                std::cout << "  heard a " << knockthru::to_string(knock.kind) << " knock, ignored" << std::endl;
            if (result->heard != knockthru::Calibrator::Heard::Counted) continue;
            std::cout << "  knock " << calibrator.count() << " of " << knockthru::Calibrator::kWanted << ": "
                      << static_cast<int>(knock.peak) << " m/s^2" << std::endl;
            haptics.buzz(1, t);
        }
        haptics.update(t);
        clock.tick();
    }
    if (!calibrator.done())
        throw std::runtime_error("Calibration stopped after " + std::to_string(calibrator.count()) + " knocks. Nothing saved.");
    std::cout << "Thresholds for " << kind_name << " set to " << thresholds(calibrator.save()) << " in "
              << (knockthru::user_directory() / "calibration.json").string()
              << ". A running KnockThru picks it up within 2 s.\n";
    return 0;
}

fs::path manifest_path() { return knockthru::user_directory() / "app.vrmanifest"; }

fs::path icon_path() { return knockthru::user_directory() / "icon.png"; }

void register_app() {
    const auto binary = app_binary().generic_u8string();
    knockthru::Canvas icon(256, 256);
    knockthru::Panel::draw_icon(icon);
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

// The bug report files go in the home folder, outside ~/.config, so a person can find them to attach.
fs::path home_directory() {
    const auto home = std::getenv("HOME");
    return home && *home ? home : "/tmp";
}

fs::path report_path() { return home_directory() / "knockthru-report.txt"; }

fs::path recording_path() { return home_directory() / "knockthru-knocks.csv.gz"; }

// SteamVR's own per-app file, which holds the autostart flag.
fs::path steamvr_app_config() {
    return knockthru::user_directory().parent_path() / "openvr" / "config" / "vrappconfig" /
           (std::string(kAppKey) + ".vrappconfig");
}

// With `purge`, also deletes ~/.config/knockthru, SteamVR's file for the app, and the
// report files in the home folder. The install folder stays, because FrameDrop or the
// person who unpacked it owns it.
int uninstall(bool purge) {
    VrSession session(vr::VRApplication_Utility);
    stop_running_app();
    unregister();
    if (!purge) {
        std::cout << "KnockThru stopped and removed from SteamVR. Your settings stay in "
                  << knockthru::user_directory().string() << ". Add --purge to delete them.\n";
        return 0;
    }
    fs::remove_all(knockthru::user_directory());
    fs::remove(steamvr_app_config());
    fs::remove(report_path());
    fs::remove(recording_path());
    std::cout << "KnockThru stopped and removed from SteamVR. Deleted " << knockthru::user_directory().string()
              << ", " << steamvr_app_config().string() << ", and the report files in your home folder.\n";
    return 0;
}

std::string today() {
    const auto now = std::time(nullptr);
    char date[16];
    std::strftime(date, sizeof date, "%F", std::localtime(&now));
    return date;
}

std::string steamvr_version() {
    const char* version = vr::VRSystem() ? vr::VRSystem()->GetRuntimeVersion() : nullptr;
    return version && *version ? version : "unknown";
}

// The last `count` lines of a text file, or a note if it is missing.
std::string tail(const fs::path& path, std::size_t count) {
    std::ifstream stream(path);
    if (!stream) return "(missing)\n";
    std::vector<std::string> lines;
    for (std::string line; std::getline(stream, line);) {
        lines.push_back(line);
        if (lines.size() > count) lines.erase(lines.begin());
    }
    std::string text;
    for (const auto& line : lines) text += line + "\n";
    return text.empty() ? "(empty)\n" : text;
}

std::string os_name() {
    std::ifstream stream("/etc/os-release");
    std::string name = "unknown";
    for (std::string line; std::getline(stream, line);)
        if (line.rfind("PRETTY_NAME=", 0) == 0) name = line.substr(12);
    name.erase(std::remove(name.begin(), name.end(), '"'), name.end());
    utsname system{};
    if (::uname(&system) == 0) name += std::string(", kernel ") + system.release;
    return name;
}

// 20 s of headset and controller motion, in the tests/data CSV format, for a knock bug report.
struct KnockRecording {
    static constexpr double kSeconds = 20;
    double start = 0;
    std::vector<PoseThread::Sample> samples;
    std::vector<std::string> heard; // one line per knock and action, for the report
};

// Writes the recording as ~/knockthru-knocks.csv.gz and its summary as knocks.txt.
// The angular velocity columns are zero, because the detector does not read them.
void write_recording(const KnockRecording& recording) {
    const auto csv = knockthru::user_directory() / "knocks.csv";
    {
        std::ofstream out(csv, std::ios::trunc);
        out << kCsvHeader;
        for (const auto& s : recording.samples) write_csv_row(out, s.t - recording.start, s.device, s.position, s.velocity);
        if (!out) throw std::runtime_error("Cannot write " + csv.string());
    }
    if (knockthru::run_program({"gzip", "-f", csv.string()}) != 0) throw std::runtime_error("gzip could not compress the recording");
    fs::copy_file(csv.string() + ".gz", recording_path(), fs::copy_options::overwrite_existing);
    fs::remove(csv.string() + ".gz");
    std::ofstream summary(knockthru::user_directory() / "knocks.txt", std::ios::trunc);
    summary << "Recorded " << recording.samples.size() << " samples over " << KnockRecording::kSeconds << " s. Heard "
            << recording.heard.size() << " knocks and actions:\n";
    for (const auto& line : recording.heard) summary << line << "\n";
}

// A bug report for a GitHub issue: versions, SteamVR state, settings, and the
// end of the logs. `vr` is false when SteamVR is not running, and `room_view`
// describes what the caller read from Room View.
std::string report_text(bool vr, const std::string& room_view) {
    std::ostringstream out;
    const auto now = std::time(nullptr);
    char stamp[32];
    std::strftime(stamp, sizeof stamp, "%F %T", std::localtime(&now));
    out << "KnockThru bug report, " << stamp << "\n\n";
    out << "KnockThru: " << kVersion << "\n";
    out << "Program: " << executable().string() << "\n";
    out << "System: " << os_name() << "\n";
    if (vr) {
        auto* apps = vr::VRApplications();
        out << "SteamVR: " << steamvr_version() << "\n";
        const auto remembered = knockthru::load_state();
        if (!remembered.steamvr_since.empty())
            out << "SteamVR seen since: " << remembered.steamvr_since
                << (remembered.steamvr_previous.empty() ? "" : ", before that " + remembered.steamvr_previous) << "\n";
        out << "Registered with SteamVR: " << (apps->IsApplicationInstalled(kAppKey) ? "yes" : "no") << "\n";
        out << "Starts with SteamVR: " << (apps->GetApplicationAutoLaunch(kAppKey) ? "yes" : "no") << "\n";
        const auto pid = apps->GetApplicationProcessId(kAppKey);
        out << "Running: " << (pid ? "yes, pid " + std::to_string(pid) : std::string("no")) << "\n";
        try {
            out << "Style: " << knockthru::to_string(knockthru::read_style()) << "\n";
        } catch (const std::exception& error) {
            out << "Style: " << error.what() << "\n";
        }
        const auto key = scene_app_key();
        out << "Current app: " << (key.empty() ? "none" : key) << "\n";
    } else {
        out << "SteamVR: not running\n";
    }
    out << "Room View: " << room_view << "\n";
    const auto user = knockthru::user_directory();
    // The summary of the last knock recording, if it is under an hour old.
    std::error_code age_error;
    const auto knocks = user / "knocks.txt";
    if (fs::exists(knocks, age_error) &&
        fs::file_time_type::clock::now() - fs::last_write_time(knocks, age_error) < std::chrono::hours(1))
        out << "\n== Knock recording, knockthru-knocks.csv.gz ==\n" << tail(knocks, 200);
    for (const auto* name : {"config.json", "calibration.json"})
        out << "\n== " << (user / name).string() << " ==\n" << tail(user / name, 400);
    out << "\n== " << (user / "application.log").string() << ", last 300 lines ==\n" << tail(user / "application.log", 300);
    const auto home = std::getenv("HOME");
    if (home && *home) {
        const auto steam_log = fs::path(home) / ".local/share/Steam/logs/vrclient_knockthru.txt";
        out << "\n== " << steam_log.string() << ", last 100 lines ==\n" << tail(steam_log, 100);
    }
    return out.str();
}

fs::path write_report(bool vr, const std::string& room_view) {
    const auto path = report_path();
    std::ofstream file(path, std::ios::trunc);
    if (!(file << report_text(vr, room_view))) throw std::runtime_error("Cannot write " + path.string());
    return path;
}

// Installs the latest release over this one, then restarts KnockThru.
int update() {
    const auto work = knockthru::user_directory() / "update";
    const auto release = knockthru::fetch_latest_release(work);
    if (knockthru::compare_versions(release.version, kVersion) <= 0) {
        std::cout << "KnockThru " << kVersion << " is up to date. The latest release is " << release.version << ".\n";
        return 0;
    }
    std::cout << "Installing KnockThru " << release.version << " from " << release.url << "\n";
    knockthru::install_release(release, install_directory(), work);
    // The new files are in place. install() restarts the resident app from them.
    return install();
}

int report(int port) {
    // Background, because a report must not start SteamVR when it is not running.
    std::unique_ptr<VrSession> session;
    try {
        session = std::make_unique<VrSession>(vr::VRApplication_Background);
    } catch (const std::exception&) {
    }
    std::string room_view;
    try {
        room_view = knockthru::RoomView(port).active() ? "on" : "off";
    } catch (const std::exception& error) {
        room_view = error.what();
    }
    const auto path = write_report(session != nullptr, room_view);
    std::cout << "Wrote " << path.string() << ". Attach it to an issue at https://github.com/VioletTides/KnockThru/issues\n";
    return 0;
}

// The KnockThru tab on the SteamVR dashboard: an overlay showing the panel
// image, with the laser pointer as a mouse.
class DashboardPanel {
public:
    explicit DashboardPanel(const fs::path& font) : panel_(font) {
        auto* overlay = vr::VROverlay();
        const auto error = overlay->CreateDashboardOverlay(kPanelKey, "KnockThru", &main_, &thumbnail_);
        if (error != vr::VROverlayError_None)
            throw std::runtime_error(std::string("Cannot add the dashboard panel: ") +
                overlay->GetOverlayErrorNameFromEnum(error));
        overlay->SetOverlayWidthInMeters(main_, 1.6f);
        overlay->SetOverlayInputMethod(main_, vr::VROverlayInputMethod_Mouse);
        const vr::HmdVector2_t scale{{knockthru::Panel::kWidth, knockthru::Panel::kHeight}};
        overlay->SetOverlayMouseScale(main_, &scale);
        try {
            texture_ = std::make_unique<knockthru::OverlayTexture>(knockthru::Panel::kWidth, knockthru::Panel::kHeight);
        } catch (const std::exception& error) {
            log(std::string("Panel uses slower image uploads: ") + error.what());
        }
    }
    ~DashboardPanel() { vr::VROverlay()->DestroyOverlay(main_); }
    DashboardPanel(const DashboardPanel&) = delete;

    bool visible() const { return visible_; }

    // Handles overlay events. Returns the clicked buttons and sets `shown` when
    // the panel just opened.
    std::vector<knockthru::PanelClick> poll(bool& shown) {
        std::vector<knockthru::PanelClick> clicks;
        vr::VREvent_t event{};
        while (vr::VROverlay()->PollNextOverlayEvent(main_, &event, sizeof event)) {
            // OpenVR puts the mouse origin at the bottom left.
            const float x = event.data.mouse.x, y = knockthru::Panel::kHeight - event.data.mouse.y;
            switch (event.eventType) {
            case vr::VREvent_OverlayShown: visible_ = shown = dirty_ = true; break;
            case vr::VREvent_OverlayHidden: visible_ = false; hover_.reset(); break;
            case vr::VREvent_MouseMove: hover_ = std::make_pair(x, y); dirty_ = true; break;
            case vr::VREvent_MouseButtonUp:
                if (event.data.mouse.button == vr::VRMouseButton_Left)
                    if (const auto click = panel_.click(x, y)) clicks.push_back(*click);
                dirty_ = true;
                break;
            default: break;
            }
        }
        return clicks;
    }

    void invalidate() { dirty_ = true; }

    // Sets the dashboard bar icon: the mark in the logo colors, or in white for Steam's tint.
    void set_bar_icon(bool classic) {
        if (bar_icon_classic_ == classic) return;
        bar_icon_classic_ = classic;
        knockthru::Canvas icon(256, 256);
        knockthru::Panel::draw_icon(icon, classic ? knockthru::IconStyle::BarColor : knockthru::IconStyle::BarWhite);
        vr::VROverlay()->SetOverlayRaw(thumbnail_, const_cast<std::uint8_t*>(icon.pixels().data()), 256, 256, 4);
    }

    // Renders 30 times a second while something moves, at most 20 times a second
    // after a change, and twice a second otherwise, for countdowns and changes
    // made outside KnockThru. Uploads only changed images.
    void draw(const knockthru::PanelState& state, double now) {
        const double interval = panel_.animating() ? 1.0 / 30 : dirty_ ? 0.05 : 0.5;
        if (!visible_ || now - last_draw_ < interval) return;
        last_draw_ = now;
        dirty_ = false;
        upload(panel_.render(state, hover_, now));
    }

private:
    void upload(const knockthru::Canvas& canvas) {
        if (canvas.pixels() == uploaded_) return;
        uploaded_ = canvas.pixels();
        if (texture_) {
            try {
                texture_->show(main_, uploaded_.data());
                return;
            } catch (const std::exception& error) {
                log(std::string("Panel uses slower image uploads: ") + error.what());
                texture_.reset();
            }
        }
        vr::VROverlay()->SetOverlayRaw(main_, uploaded_.data(),
            static_cast<uint32_t>(canvas.width()), static_cast<uint32_t>(canvas.height()), 4);
    }

    knockthru::Panel panel_;
    vr::VROverlayHandle_t main_ = vr::k_ulOverlayHandleInvalid, thumbnail_ = vr::k_ulOverlayHandleInvalid;
    bool visible_ = false, dirty_ = true;
    double last_draw_ = -1;
    std::optional<std::pair<float, float>> hover_;
    std::optional<bool> bar_icon_classic_;
    std::vector<std::uint8_t> uploaded_;
    std::unique_ptr<knockthru::OverlayTexture> texture_;
};

std::string app_name(const std::string& key) {
    if (key.empty()) return "";
    std::array<char, 256> name{};
    vr::EVRApplicationError error = vr::VRApplicationError_None;
    vr::VRApplications()->GetApplicationPropertyString(key.c_str(), vr::VRApplicationProperty_Name_String,
        name.data(), static_cast<uint32_t>(name.size()), &error);
    return error == vr::VRApplicationError_None ? name.data() : "";
}

// A Room View read or change running on another thread, because the debug
// port can take a second.
struct RoomViewResult {
    bool on;
    std::string message; // empty for a plain read
};

// One line for the log: how each knock heard while recording ended.
std::string heard_summary(const knockthru::Calibrator& calibrator) {
    using Heard = knockthru::Calibrator::Heard;
    char line[192];
    std::snprintf(line, sizeof line, "Recorded %s knocks: %d counted, %d too soon, %d too light, %d other kind, %d ignored",
        knockthru::to_string(calibrator.kind()), calibrator.heard(Heard::Counted), calibrator.heard(Heard::TooSoon),
        calibrator.heard(Heard::TooLight), calibrator.heard(Heard::OtherKind), calibrator.heard(Heard::Ignored));
    return line;
}

// The resident app. SteamVR starts it as a dashboard overlay. A double knock
// turns Room View on or off, a triple knock switches translucent and opaque,
// and the KnockThru dashboard tab has buttons for everything.
int run() {
    open_log();
    log(std::string("KnockThru ") + kVersion + " starting");
    VrSession session(vr::VRApplication_Overlay, true);
    log("SteamVR " + steamvr_version());
    // A SteamVR update is the usual cause when Room View control breaks, so the log marks each one.
    auto app_state = knockthru::load_state();
    const auto save_app_state = [&] {
        try {
            knockthru::save_state(app_state);
        } catch (const std::exception& error) {
            log(std::string("State not saved: ") + error.what());
        }
    };
    // A state file without knockthru_version comes from an older KnockThru.
    const bool ran_before = !app_state.steamvr_version.empty();
    {
        const auto before = app_state.steamvr_version;
        const auto previous = knockthru::note_steamvr_version(app_state, steamvr_version(), today());
        if (!previous.empty()) log("SteamVR changed from " + previous + " to " + app_state.steamvr_version);
        if (app_state.steamvr_version != before) save_app_state();
    }
    // A newer version than at the last start means an update ran, from Settings or FrameDrop. The panel says so.
    // `updated_from` is empty when the last version is unknown.
    std::optional<std::string> updated_from;
    if (app_state.knockthru_version != kVersion) {
        if (app_state.knockthru_version.empty() ? ran_before
                : knockthru::compare_versions(kVersion, app_state.knockthru_version) > 0) {
            updated_from = app_state.knockthru_version;
            log("Updated from " + (updated_from->empty() ? "an older version" : *updated_from));
        }
        app_state.knockthru_version = kVersion;
        save_app_state();
    }

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

    std::unique_ptr<DashboardPanel> panel;
    try {
        panel = std::make_unique<DashboardPanel>(install_directory() / "fonts");
    } catch (const std::exception& error) {
        log(error.what());
    }

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
    std::optional<knockthru::Calibrator> calibrator;
    std::optional<knockthru::CalibrationView> calibration; // the walkthrough the panel shows
    using Phase = knockthru::CalibrationView::Phase;
    std::optional<knockthru::TourView> tour; // the first-run tour, while the panel shows it
    using Step = knockthru::TourView::Step;
    // Someone with settings or a calibration already knows KnockThru, so the tour
    // only shows itself to new users. Settings can replay it.
    if (!config.tour_done && (fs::exists(knockthru::user_directory() / "config.json") ||
                              fs::exists(knockthru::user_directory() / "calibration.json"))) {
        try {
            knockthru::save_tour_done(true);
            config = knockthru::load_config(install_directory());
            stamp = config_stamp();
        } catch (const std::exception& error) {
            log(std::string("Tour not marked done: ") + error.what());
        }
    }

    Clock clock;
    PoseThread poses(clock);
    knockthru::PanelState state;
    std::future<RoomViewResult> room_view_job;
    bool room_view_job_toggles = false;
    std::future<void> tint_job;
    // Downloads stop when run() returns, so quitting SteamVR never waits on one. The
    // futures below wait for their jobs when destroyed, and the guard after them runs
    // first, because C++ destroys locals in reverse order.
    std::atomic<bool> cancel_downloads{false};
    std::atomic<float> install_progress{0}; // the install job writes it, from 0 to 1
    std::future<knockthru::Release> check_job;
    bool check_is_automatic = false; // the daily check, which never shows errors
    std::future<void> install_job;
    struct CancelOnExit {
        std::atomic<bool>& flag;
        ~CancelOnExit() { flag = true; }
    } cancel_on_exit{cancel_downloads};
    std::optional<knockthru::Release> latest; // the newer release a check found
    const auto update_work = knockthru::user_directory() / "update";
    state.version = kVersion;
    if (updated_from) {
        state.update = {knockthru::UpdateView::Status::Updated, *updated_from, ""};
        const auto had = updated_from->empty() ? "" : " You had version " + *updated_from + ".";
        state.update_notice = knockthru::UpdateNotice{false, std::string("You now have version ") + kVersion + "." + had};
    }
    const auto start_check = [&] {
        check_job = std::async(std::launch::async, [update_work, cancel = &cancel_downloads] {
            return knockthru::fetch_latest_release(update_work, cancel);
        });
    };
    double restart_started = -1; // when this process asked knockthru-ctl to restart into the new version
    // The new files stay in place when a restart fails, so Settings keeps its Restart button.
    const auto restart_failed = [&](const std::string& version) {
        state.update = {knockthru::UpdateView::Status::Installed, version, ""};
        state.update_notice = knockthru::UpdateNotice{true, "Version " + version +
            " is installed, but KnockThru did not restart. Press Restart in Settings to try again, or restart SteamVR."};
    };
    double next_slow_check = 0;
    double binary_missing_since = -1;
    const auto binary = executable();

    std::string last_error; // for the report link
    std::optional<KnockRecording> recording; // while the report card records knocks
    std::future<void> recording_job;         // writes a finished recording
    std::unique_ptr<knockthru::ReportServer> report_server; // while the report card shows
    const auto room_view_text = [&]() -> std::string {
        return !state.room_view_error.empty() ? state.room_view_error
            : !state.room_view ? "not read yet" : *state.room_view ? "on" : "off";
    };
    const auto show = [&](const std::string& message, bool error = false) {
        log(message);
        if (error) last_error = message;
        state.message = message;
        state.message_is_error = error;
        state.calibration_saved.reset();
        if (panel) panel->invalidate();
    };
    const auto reload = [&] {
        config = knockthru::load_config(install_directory());
        stamp = config_stamp();
        if (!calibrator) detector = knockthru::KnockDetector(config.detector);
    };
    // SteamVR's dashboard click. Steam already clicks when the Room View button
    // is pressed, so a double knock and the panel's Room View button stay silent.
    const auto play_click = [&] {
        if (config.sounds) knockthru::play_click();
    };
    // A new tour counts kinds calibrated earlier as done, so a replay only asks for the rest.
    const auto start_tour = [&] {
        tour.emplace();
        try {
            auto calibrated = knockthru::load_calibration();
            for (const auto kind : {knockthru::KnockKind::Together, knockthru::KnockKind::Surface, knockthru::KnockKind::Headset})
                tour->calibrated[static_cast<int>(kind)] = calibrated[kind].has_value();
        } catch (const std::exception& error) {
            log(error.what());
        }
    };
    const auto read_room_view = [&] {
        if (room_view_job.valid()) return;
        const knockthru::RoomView room_view(config.debug_port);
        room_view_job_toggles = false;
        room_view_job = std::async(std::launch::async, [room_view] { return RoomViewResult{room_view.active(), ""}; });
    };
    const auto read_opaque = [&] {
        try {
            state.opaque = knockthru::read_style() == knockthru::RoomViewStyle::Opaque;
        } catch (const std::exception&) {
            state.opaque.reset();
        }
    };
    const auto toggle_room_view = [&](const std::string& why) {
        if (room_view_job.valid() && room_view_job_toggles) {
            log("Ignored" + why + ": passthrough is still switching");
            return false;
        }
        if (room_view_job.valid()) room_view_job.wait(); // a plain read, at most a few ms
        const knockthru::RoomView room_view(config.debug_port);
        room_view_job_toggles = true;
        room_view_job = std::async(std::launch::async, [room_view, why] {
            const bool on = room_view.toggle();
            return RoomViewResult{on, std::string("Passthrough ") + (on ? "on" : "off") + why};
        });
        return true;
    };
    const auto toggle_style = [&](const std::string& why) {
        try {
            const auto style = knockthru::toggle_style();
            state.opaque = style == knockthru::RoomViewStyle::Opaque;
            show(std::string("Style ") + knockthru::to_string(style) + why);
            return true;
        } catch (const std::exception& error) {
            show(std::string("Style not changed: ") + error.what(), true);
            return false;
        }
    };
    const auto save = [&](const std::function<void()>& write) {
        try {
            write();
            reload();
            return true;
        } catch (const std::exception& error) {
            show(std::string("Settings not saved: ") + error.what(), true);
            return false;
        }
    };

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
            // Keeps the click stream open while the panel shows, so a press clicks at once.
            if (config.sounds && panel && panel->visible()) knockthru::prepare_click();
            // Restyles the bar icon while the dashboard is open, in case the bar page
            // reloaded. If this fails, the icon stays undimmed. The classic icon has no tint.
            if (vr::VROverlay()->IsDashboardVisible() &&
                (!tint_job.valid() || tint_job.wait_for(std::chrono::seconds(0)) == std::future_status::ready)) {
                if (tint_job.valid()) try { tint_job.get(); } catch (const std::exception&) {}
                const knockthru::RoomView bar(config.debug_port);
                const bool tint = !config.classic_bar_icon;
                tint_job = std::async(std::launch::async, [bar, tint] { bar.tint_icon(kPanelKey, tint); });
            }
            state.app_key = scene_app_key();
            state.app_name = app_name(state.app_key);
            // The daily check waits a minute after start, for the Wi-Fi. It fails quietly.
            const auto now_epoch = static_cast<long long>(std::time(nullptr));
            if (config.check_updates && t > 60 && !check_job.valid() && !install_job.valid() &&
                (state.update.status == knockthru::UpdateView::Status::Idle ||
                 state.update.status == knockthru::UpdateView::Status::Updated) &&
                now_epoch - app_state.last_update_check >= 24 * 3600) {
                app_state.last_update_check = now_epoch;
                save_app_state();
                check_is_automatic = true;
                log("Daily update check");
                start_check();
            }
            if (const auto now_stamp = config_stamp(); now_stamp != stamp) {
                stamp = now_stamp;
                try {
                    reload();
                    log("Settings reloaded");
                } catch (const std::exception& error) {
                    show(std::string("Settings not reloaded: ") + error.what(), true);
                }
            }
            if (panel && panel->visible()) {
                read_room_view(); // Room View can also change from the bar or the side button
                read_opaque();
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

        if (panel) {
            bool shown = false;
            for (const auto& click : panel->poll(shown)) {
                using knockthru::PanelCommand;
                if (click.command != PanelCommand::RoomView) play_click();
                const auto kind_name = std::string(knockthru::to_string(click.kind));
                switch (click.command) {
                case PanelCommand::RoomView: toggle_room_view(""); break;
                case PanelCommand::Style: toggle_style(""); break;
                case PanelCommand::Navigate: break;
                case PanelCommand::Pause:
                    if (save([&] { knockthru::save_paused(!config.paused); }))
                        show(config.paused ? "KnockThru off" : "KnockThru on");
                    break;
                case PanelCommand::Sounds:
                    if (save([&] { knockthru::save_sounds(!config.sounds); }))
                        show(config.sounds ? "Sounds on" : "Sounds off");
                    break;
                case PanelCommand::StartTour: start_tour(); break;
                case PanelCommand::TourNext:
                    if (tour && tour->step != Step::Done) tour->step = static_cast<Step>(static_cast<int>(tour->step) + 1);
                    break;
                case PanelCommand::TourBack:
                    if (tour && tour->step != Step::Welcome) tour->step = static_cast<Step>(static_cast<int>(tour->step) - 1);
                    break;
                case PanelCommand::TourSkip:
                    // The tour closes even if the flag cannot be saved, so it never traps anyone.
                    if (!config.tour_done) save([&] { knockthru::save_tour_done(true); });
                    if (tour) log(tour->step == Step::Done ? "Tour finished" : "Tour skipped");
                    tour.reset();
                    break;
                case PanelCommand::FactoryReset:
                    calibrator.reset();
                    calibration.reset();
                    try {
                        knockthru::reset_user_settings();
                        reload();
                        log("Factory reset: settings and calibration deleted");
                        state.message.clear();
                        state.message_is_error = false;
                        state.calibration_saved.reset();
                        start_tour();
                    } catch (const std::exception& error) {
                        detector = knockthru::KnockDetector(config.detector);
                        show(std::string("Factory reset failed: ") + error.what(), true);
                    }
                    break;
                case PanelCommand::CheckUpdate:
                    // A daily check already running answers this press too.
                    if (check_job.valid() && check_is_automatic) {
                        check_is_automatic = false;
                        state.update = {knockthru::UpdateView::Status::Checking, "", ""};
                    }
                    if (check_job.valid() || install_job.valid()) break;
                    check_is_automatic = false;
                    state.update = {knockthru::UpdateView::Status::Checking, "", ""};
                    start_check();
                    break;
                case PanelCommand::RestartForUpdate:
                    if (state.update.status != knockthru::UpdateView::Status::Installed) break;
                    log("Restarting into KnockThru " + state.update.latest);
                    try {
                        // knockthru-ctl --install stops this process and starts the new one.
                        knockthru::start_detached({(install_directory() / "knockthru-ctl").string(), "--install"});
                        state.update.status = knockthru::UpdateView::Status::Restarting;
                        restart_started = t;
                    } catch (const std::exception& error) {
                        log(std::string("Restart failed: ") + error.what());
                        restart_failed(state.update.latest);
                    }
                    break;
                case PanelCommand::DismissUpdateNotice:
                    state.update_notice.reset();
                    break;
                case PanelCommand::InstallUpdate:
                    if (!latest || check_job.valid() || install_job.valid()) break;
                    log("Installing KnockThru " + latest->version + " from " + latest->url);
                    state.update.status = knockthru::UpdateView::Status::Installing;
                    install_progress = 0;
                    install_job = std::async(std::launch::async,
                        [release = *latest, update_work, cancel = &cancel_downloads, progress = &install_progress] {
                            knockthru::install_release(release, install_directory(), update_work, cancel, progress);
                        });
                    break;
                case PanelCommand::ShowReport: {
                    // The QR code opens a page this headset serves, with the full report. Without a
                    // network, it opens the GitHub form directly, which only has room for the versions.
                    const auto issue = knockthru::issue_url({kVersion, steamvr_version(), room_view_text(), last_error});
                    report_server.reset();
                    try {
                        report_server = std::make_unique<knockthru::ReportServer>(report_text(true, room_view_text()), issue);
                        state.report_url = report_server->url();
                        state.report_full = true;
                        // A recording made before the card closed belongs to this report too.
                        std::error_code missing;
                        if (state.recorded && fs::exists(recording_path(), missing))
                            report_server->set_recording(recording_path());
                        log("Serving the bug report at " + state.report_url);
                    } catch (const std::exception& error) {
                        state.report_url = issue;
                        state.report_full = false;
                        log(std::string("Bug report page unavailable: ") + error.what());
                    }
                    state.report_saved.clear();
                    state.report_error.clear();
                    break;
                }
                case PanelCommand::RecordKnocks:
                    if (recording || recording_job.valid()) break;
                    recording.emplace();
                    recording->start = t;
                    recording->samples.reserve(70000);
                    state.recording_knocks = 0;
                    state.recorded = false;
                    log("Recording knocks for a bug report");
                    break;
                case PanelCommand::CloseReport:
                    report_server.reset();
                    break;
                case PanelCommand::SaveReport:
                    try {
                        state.report_saved = write_report(true, room_view_text()).string();
                        state.report_error.clear();
                        log("Bug report saved to " + state.report_saved);
                    } catch (const std::exception& error) {
                        state.report_error = error.what();
                        log(std::string("Bug report not saved: ") + error.what());
                    }
                    break;
                case PanelCommand::BarIcon:
                    if (save([&] { knockthru::save_classic_bar_icon(!config.classic_bar_icon); })) {
                        show(config.classic_bar_icon ? "Classic bar icon on" : "Classic bar icon off");
                        // The icon image changes this frame. Restyle the bar now, not at the
                        // next 2 s check, so the old tint never shows over the new icon.
                        next_slow_check = 0;
                    }
                    break;
                case PanelCommand::Kind: {
                    auto enabled = config.detector.enabled;
                    enabled[static_cast<int>(click.kind)] = !enabled[static_cast<int>(click.kind)];
                    if (save([&] { knockthru::save_knock_types(enabled); }))
                        show(kind_name + " knocks " + (config.detector.enabled[static_cast<int>(click.kind)] ? "on" : "off"));
                    break;
                }
                case PanelCommand::BlockApp: {
                    if (state.app_key.empty()) break;
                    auto blocked = config.blocked_apps;
                    if (!blocked.erase(state.app_key)) blocked.insert(state.app_key);
                    const auto name = state.app_name.empty() ? state.app_key : state.app_name;
                    if (save([&] { knockthru::save_blocked_apps(blocked); }))
                        show(std::string("Knocks ") + (config.blocked_apps.count(state.app_key) ? "off" : "on") + " in " + name);
                    break;
                }
                case PanelCommand::Calibrate: {
                    calibrator.emplace(click.kind, config.detector, t);
                    detector = knockthru::KnockDetector(calibrator->detector_config());
                    calibration.emplace();
                    calibration->kind = click.kind;
                    calibration->wanted = knockthru::Calibrator::kWanted;
                    calibration->started_t = t;
                    show("Calibrating " + kind_name + ": knock " + std::to_string(knockthru::Calibrator::kWanted) +
                        " times, about 1 s apart");
                    break;
                }
                case PanelCommand::CancelCalibration:
                    if (calibrator && calibration && calibration->phase == Phase::Recording)
                        log(heard_summary(*calibrator));
                    calibrator.reset();
                    calibration.reset();
                    detector = knockthru::KnockDetector(config.detector);
                    show("Calibration canceled. Nothing saved.");
                    break;
                case PanelCommand::SaveCalibration:
                    if (!calibrator || !calibration || !calibration->trial_passed.value_or(false)) break;
                    try {
                        const auto saved = calibrator->save();
                        log("Calibrated " + kind_name + " knocks: " + thresholds(saved));
                        calibrator.reset();
                        calibration.reset();
                        reload();
                        show("Calibration saved");
                        // The tour's Calibrate step marks its card, so the Saved banner is only for Settings.
                        if (tour) tour->calibrated[static_cast<int>(click.kind)] = true;
                        else state.calibration_saved = click.kind;
                        haptics.buzz(2, t);
                    } catch (const std::exception& error) {
                        show(std::string("Calibration not saved: ") + error.what(), true);
                    }
                    break;
                }
            }
            if (shown && !config.tour_done && !tour && !calibrator) {
                log("Starting the first-run tour");
                start_tour();
            }
            if (shown) {
                if (config.sounds) knockthru::prepare_click();
                read_room_view();
                read_opaque();
            }
        }

        for (const auto& sample : poses.take()) {
            detector.push(sample.device, sample.t, sample.position, sample.velocity);
            if (recording) recording->samples.push_back(sample);
        }
        const auto knock_event = detector.poll(t);
        const auto knocks = detector.take_knocks();
        if (recording) {
            char line[160];
            for (const auto& knock : knocks) {
                std::snprintf(line, sizeof line, "%6.2f s  %s knock, peak %.0f m/s^2", knock.t - recording->start,
                    knockthru::to_string(knock.kind), knock.peak);
                recording->heard.push_back(line);
            }
            if (knock_event) {
                std::snprintf(line, sizeof line, "%6.2f s  %s %s knock", knock_event->t - recording->start,
                    sequence_name(*knock_event), knockthru::to_string(knock_event->kind));
                recording->heard.push_back(line);
            }
            state.recording_knocks = static_cast<int>(knocks.size()) + state.recording_knocks;
            const double left = KnockRecording::kSeconds - (t - recording->start);
            state.recording_left = static_cast<int>(std::ceil(std::max(left, 0.0)));
            if (left <= 0) {
                state.recording_left = -1;
                log("Recorded " + std::to_string(recording->samples.size()) + " samples and " +
                    std::to_string(recording->heard.size()) + " knocks and actions");
                recording_job = std::async(std::launch::async, [finished = std::move(*recording)] { write_recording(finished); });
                recording.reset();
            }
            if (panel) panel->invalidate();
        }
        if (recording_job.valid() && recording_job.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            try {
                recording_job.get();
                state.recorded = true;
                log("Knock recording saved to " + recording_path().string());
                if (report_server) {
                    report_server->set_report(report_text(true, room_view_text()));
                    report_server->set_recording(recording_path());
                }
            } catch (const std::exception& error) {
                state.report_error = std::string("recording not saved: ") + error.what();
                log(std::string("Knock recording not saved: ") + error.what());
            }
            if (panel) panel->invalidate();
        }
        // Opens the click stream on the first knock, so it is ready if a triple knock follows.
        if (config.sounds && !config.paused && !knocks.empty()) knockthru::prepare_click();

        if (calibrator && calibration && calibration->phase == Phase::Recording) {
            for (const auto& knock : knocks) calibrator->hear(knock);
            while (const auto result = calibrator->next(t)) {
                const auto heard = result->heard;
                const auto& knock = result->knock;
                if (heard == knockthru::Calibrator::Heard::Counted) {
                    haptics.buzz(1, t);
                    calibration->peaks = calibrator->peaks();
                    calibration->speeds = calibrator->speeds();
                    calibration->counted_t = t;
                    calibration->knocked_t = knock.t;
                } else if (heard == knockthru::Calibrator::Heard::OtherKind) {
                    calibration->ignored = knock.kind;
                    calibration->ignored_t = t;
                } else if (heard == knockthru::Calibrator::Heard::TooLight) {
                    calibration->too_light_t = t;
                } else if (heard == knockthru::Calibrator::Heard::TooSoon) {
                    calibration->too_soon_t = t;
                }
            }
            calibration->seconds_left = static_cast<int>(std::ceil(calibrator->seconds_left(t)));
            if (calibrator->done()) {
                // Straight on to the try step, which judges knocks at the new thresholds.
                calibration->phase = Phase::Trying;
                detector = knockthru::KnockDetector(calibrator->trial_config(config.detector));
                haptics.buzz(2, t + 0.3);
                log(heard_summary(*calibrator) + ". Trying thresholds: " + thresholds(calibrator->result()));
            } else if (calibrator->seconds_left(t) <= 0) {
                log(heard_summary(*calibrator));
                show("Time ran out after " + std::to_string(calibrator->count()) + " of " +
                    std::to_string(knockthru::Calibrator::kWanted) + " knocks. Nothing saved.", true);
                calibrator.reset();
                calibration.reset();
                detector = knockthru::KnockDetector(config.detector);
            }
        } else if (calibrator && calibration && calibration->phase == Phase::Trying) {
            // The try step judges knocks at the new threshold and never switches passthrough.
            if (knock_event) {
                const bool passed = knock_event->kind == calibrator->kind() && knock_event->count == 2;
                calibration->trial_passed = passed;
                calibration->trial_heard = std::string("a ") + sequence_name(*knock_event) + " " +
                    knockthru::to_string(knock_event->kind) + " knock";
                calibration->trial_t = t;
                log(std::string("Try step heard ") + calibration->trial_heard + (passed ? ": passed" : ": missed"));
                haptics.buzz(passed ? 1 : 3, t);
            }
        } else if (!calibrator && knock_event) {
            const std::string what = std::string(" (") + sequence_name(*knock_event) + " " +
                knockthru::to_string(knock_event->kind) + " knock)";
            // The tour's knock steps act even when paused or off in this game, so the user can practice.
            // Only while the panel shows them, so a tour left open never overrides the settings in a game.
            const bool practicing = tour && (tour->step == Step::Double || tour->step == Step::Triple) && panel &&
                panel->visible();
            if (config.paused && !practicing) {
                log("Ignored" + what + ": paused");
            } else if (config.blocked_apps.count(state.app_key) && !practicing) {
                log("Ignored" + what + ": knocks are off in " + state.app_key);
            } else if (knock_event->count == 3) {
                const bool changed = toggle_style(what);
                if (changed) play_click();
                haptics.buzz(changed ? 2 : 3, t);
                if (changed && tour && tour->step == Step::Triple) tour->triple_t = t;
            } else if (toggle_room_view(what)) {
                // Buzz now, because the switch can take a second. A failure buzzes 3 times later.
                haptics.buzz(1, t);
                if (tour && tour->step == Step::Double) tour->double_t = t;
            }
        }

        if (room_view_job.valid() && room_view_job.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            const bool toggled = room_view_job_toggles;
            room_view_job_toggles = false;
            try {
                const auto result = room_view_job.get();
                if (state.room_view != result.on && panel) panel->invalidate();
                state.room_view = result.on;
                state.room_view_error.clear();
                state.dashboard_changed = false;
                if (!result.message.empty()) show(result.message);
            } catch (const knockthru::DashboardChanged& error) {
                // Logged once per change, with the SteamVR version, for bug reports.
                if (!state.dashboard_changed)
                    log(std::string(error.what()) + ". SteamVR " + steamvr_version() +
                        ". A Steam update may have changed the dashboard.");
                state.dashboard_changed = true;
                state.room_view.reset();
                state.room_view_error = "A Steam update moved the Room View button. Update KnockThru in Settings.";
                if (toggled) {
                    show("Passthrough not changed: Room View button not found", true);
                    haptics.buzz(3, t);
                }
            } catch (const std::exception& error) {
                state.room_view.reset();
                const std::string reason = error.what();
                state.room_view_error = reason.find("debug port") != std::string::npos
                    ? "Turn on CEF Remote Debugging in the Frame's Developer settings."
                    : reason;
                if (toggled) {
                    show(std::string("Passthrough not changed: ") + error.what(), true);
                    haptics.buzz(3, t);
                } else if (state.message != error.what()) {
                    show(error.what(), true);
                }
            }
        }

        using UpdateStatus = knockthru::UpdateView::Status;
        if (check_job.valid() && check_job.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            try {
                const auto release = check_job.get();
                const bool newer = knockthru::compare_versions(release.version, kVersion) > 0;
                if (newer) latest = release;
                else latest.reset();
                // An automatic check that finds nothing leaves the header as it was.
                if (newer || !check_is_automatic)
                    state.update = {newer ? UpdateStatus::Available : UpdateStatus::Current, release.version, ""};
                log("Update check: " + release.version + (newer ? " is available" : " is the latest"));
            } catch (const std::exception& error) {
                if (check_is_automatic) {
                    // Try again in an hour, not a day, since the Wi-Fi may only have been down.
                    app_state.last_update_check = static_cast<long long>(std::time(nullptr)) - 23 * 3600;
                    save_app_state();
                    log(std::string("Daily update check failed: ") + error.what());
                } else {
                    state.update = {UpdateStatus::Failed, "", error.what()};
                    show(std::string("Update check failed: ") + error.what(), true);
                }
            }
            if (panel) panel->invalidate();
        }
        // The Installing button fills as the download runs. Whole percents are enough to redraw for.
        if (install_job.valid() && std::abs(install_progress - state.update_progress) >= 0.01f) {
            state.update_progress = install_progress;
            if (panel) panel->invalidate();
        }
        if (install_job.valid() && install_job.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            const auto version = latest ? latest->version : "";
            try {
                install_job.get();
                // The Restart button in Settings starts the new version now. Otherwise SteamVR starts it at its next launch.
                log("Installed KnockThru " + version);
                state.update = {UpdateStatus::Installed, version, ""};
            } catch (const std::exception& error) {
                std::string reason = error.what();
                if (!reason.empty() && reason.back() != '.') reason += '.';
                state.update = {UpdateStatus::Failed, version, error.what()};
                state.update_notice = knockthru::UpdateNotice{true, "Version " + version + " was not installed. " +
                    reason + " You still have version " + kVersion + "."};
                log("Update not installed: " + reason);
            }
            if (panel) panel->invalidate();
        }
        // knockthru-ctl stops this process within seconds, so one still running after 30 s missed its restart.
        if (restart_started >= 0 && t - restart_started > 30) {
            restart_started = -1;
            log("Restart after the update did not happen");
            restart_failed(latest ? latest->version : "");
            if (panel) panel->invalidate();
        }

        if (panel) {
            state.busy = room_view_job.valid() && room_view_job_toggles;
            state.paused = config.paused;
            state.classic_bar_icon = config.classic_bar_icon;
            state.sounds = config.sounds;
            state.tour = tour;
            panel->set_bar_icon(config.classic_bar_icon);
            state.enabled = config.detector.enabled;
            state.app_blocked = config.blocked_apps.count(state.app_key) > 0;
            state.calibration = calibration;
            panel->draw(state, t);
        }
        haptics.update(t);
        // Knocks are judged 50 ms late anyway, so a hidden panel lets the loop wake every 5 ms.
        // The pose thread still samples every millisecond.
        clock.tick(std::chrono::milliseconds(panel && panel->visible() ? 1 : 5));
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
        "  --purge              With --uninstall, also delete ~/.config/knockthru\n"
        "  --status             Show registration and Room View state\n"
        "  --report             Write ~/knockthru-report.txt for a bug report\n"
        "  --update             Install the latest release from GitHub and restart\n"
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
        bool purge = false;
        for (int i = 1; i < argc; ++i) {
            const std::string arg = argv[i];
            const auto value = [&]() -> std::string {
                if (i + 1 >= argc) throw std::runtime_error(arg + " needs a value");
                return argv[++i];
            };
            if (arg == "--help" || arg == "-h") { usage(); return 0; }
            if (arg == "--seconds") seconds = std::stod(value());
            else if (arg == "--save") path = value();
            else if (arg == "--purge") purge = true;
            else if (arg == "--record") { command = arg; path = value(); }
            else if (arg == "--calibrate") { command = arg; kind = value(); }
            else if (arg == "--install" || arg == "--uninstall" || arg == "--status" || arg == "--toggle" ||
                     arg == "--style" || arg == "--current-app" || arg == "--detect" || arg == "--run" ||
                     arg == "--report" || arg == "--update")
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
        // SteamVR starts the resident app with STEAMVR_APPKEY set to KnockThru's key.
        // A command the app starts, like Restart in Settings, inherits it, and SteamVR
        // would then take the command for KnockThru itself.
        ::unsetenv("STEAMVR_APPKEY");
        // Restart in Settings runs --install with no terminal, so its errors go to the log.
        if (command == "--install" || command == "--update") open_log();
        if (command == "--install") return install();
        if (purge && command != "--uninstall") throw std::runtime_error("--purge only works with --uninstall");
        if (command == "--uninstall") return uninstall(purge);
        if (command == "--record") return record(path, seconds);
        if (command == "--detect") return detect(seconds, path);
        if (command == "--calibrate") return calibrate(kind);
        if (command == "--update") return update();

        const auto config = knockthru::load_config(install_directory());
        if (command == "--status") return status(config.debug_port);
        if (command == "--report") return report(config.debug_port);
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
