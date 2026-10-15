#include "calibrator.hpp"
#include "config.hpp"
#include "knock_detector.hpp"
#include "overlay_texture.hpp"
#include "panel.hpp"
#include "room_view.hpp"
#include "style.hpp"

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
#include <functional>
#include <atomic>
#include <future>
#include <iostream>
#include <mutex>
#include <optional>
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
            sampler.sample([&](Device device, const char*, const vr::TrackedDevicePose_t& pose) {
                const auto& m = pose.mDeviceToAbsoluteTracking.m;
                const float* v = pose.vVelocity.v;
                std::lock_guard<std::mutex> lock(mutex_);
                samples_.push_back({device, t, {m[0][3], m[1][3], m[2][3]}, {v[0], v[1], v[2]}});
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
        sampler.sample([&](Device device, const char*, const vr::TrackedDevicePose_t& pose) {
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
    const auto saved = calibrator.save();
    char thresholds[96];
    std::snprintf(thresholds, sizeof thresholds, "spike %.0f m/s^2, approach %.2f m/s", saved.peak, saved.approach);
    std::cout << "Thresholds for " << kind_name << " set to " << thresholds << " in "
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
    fs::remove(icon_path(), error);
}

int uninstall() {
    VrSession session(vr::VRApplication_Utility);
    stop_running_app();
    unregister();
    std::cout << "KnockThru stopped and removed from SteamVR. Your settings stay in "
              << knockthru::user_directory().string() << ".\n";
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
            case vr::VREvent_MouseMove:
                hover_ = std::make_pair(x, y);
                panel_.move(x, y);
                dirty_ = true;
                break;
            case vr::VREvent_MouseButtonDown:
                if (event.data.mouse.button == vr::VRMouseButton_Left) panel_.press(x, y);
                dirty_ = true;
                break;
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
        const double gap = last_draw_ < 0 ? 0 : (now - last_draw_) * 1000;
        last_draw_ = now;
        dirty_ = false;
        using ms = std::chrono::duration<double, std::milli>;
        const auto start = std::chrono::steady_clock::now();
        const auto& canvas = panel_.render(state, hover_, now);
        const auto rendered = std::chrono::steady_clock::now();
        upload(canvas);
        if (panel_.animating())
            time_frame(gap, ms(rendered - start).count(), ms(std::chrono::steady_clock::now() - rendered).count());
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

    // Logs, every 300 animated frames (about 10 s), the gap between frames and the
    // time spent rendering and uploading: average and longest, in ms.
    void time_frame(double gap, double render, double upload) {
        for (auto [value, stat] : {std::pair{gap, &gap_}, {render, &render_}, {upload, &upload_}}) {
            stat->total += value;
            stat->longest = std::max(stat->longest, value);
        }
        if (++frames_ < 300) return;
        char line[192];
        std::snprintf(line, sizeof line,
            "Panel frames: gap %.1f ms average, %.1f longest. Render %.1f, %.1f. Upload %.1f, %.1f",
            gap_.total / frames_, gap_.longest, render_.total / frames_, render_.longest, upload_.total / frames_,
            upload_.longest);
        log(line);
        frames_ = 0;
        gap_ = render_ = upload_ = {};
    }

    struct Stat {
        double total = 0, longest = 0;
    };
    int frames_ = 0;
    Stat gap_, render_, upload_;
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

std::string thresholds(const knockthru::KindCalibration& calibration) {
    char line[128];
    std::snprintf(line, sizeof line, "spike %.0f m/s^2, approach %.2f m/s, sensitivity %.2f", calibration.peak,
        calibration.approach, calibration.sensitivity);
    return line;
}

// The resident app. SteamVR starts it as a dashboard overlay. A double knock
// turns Room View on or off, a triple knock switches translucent and opaque,
// and the KnockThru dashboard tab has buttons for everything.
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

    Clock clock;
    PoseThread poses(clock);
    knockthru::PanelState state;
    std::future<RoomViewResult> room_view_job;
    bool room_view_job_toggles = false;
    std::future<void> tint_job;
    double next_slow_check = 0;
    double binary_missing_since = -1;
    const auto binary = executable();

    const auto show = [&](const std::string& message, bool error = false) {
        log(message);
        state.message = message;
        state.message_is_error = error;
        state.calibration_saved = false;
        if (panel) panel->invalidate();
    };
    const auto reload = [&] {
        config = knockthru::load_config(install_directory());
        stamp = config_stamp();
        if (!calibrator) detector = knockthru::KnockDetector(config.detector);
    };
    const auto read_room_view = [&] {
        if (room_view_job.valid()) return;
        const knockthru::RoomView room_view(config.debug_port);
        room_view_job_toggles = false;
        room_view_job = std::async(std::launch::async, [room_view] { return RoomViewResult{room_view.active(), ""}; });
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
                try {
                    state.opaque = knockthru::read_style() == knockthru::RoomViewStyle::Opaque;
                } catch (const std::exception&) {
                    state.opaque.reset();
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

        if (panel) {
            bool shown = false;
            for (const auto& click : panel->poll(shown)) {
                using knockthru::PanelCommand;
                const auto kind_name = std::string(knockthru::to_string(click.kind));
                switch (click.command) {
                case PanelCommand::RoomView: toggle_room_view(""); break;
                case PanelCommand::Style: toggle_style(""); break;
                case PanelCommand::Pause:
                    if (save([&] { knockthru::save_paused(!config.paused); }))
                        show(config.paused ? "KnockThru off" : "KnockThru on");
                    break;
                case PanelCommand::BarIcon:
                    if (save([&] { knockthru::save_classic_bar_icon(!config.classic_bar_icon); }))
                        show(config.classic_bar_icon ? "Classic bar icon on" : "Classic bar icon off");
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
                    const double sensitivity = calibration ? calibration->sensitivity : 0.5;
                    calibration.emplace();
                    calibration->kind = click.kind;
                    calibration->wanted = knockthru::Calibrator::kWanted;
                    calibration->sensitivity = sensitivity;
                    show("Calibrating " + kind_name + ": knock " + std::to_string(knockthru::Calibrator::kWanted) +
                        " times, about 1 s apart");
                    break;
                }
                case PanelCommand::CancelCalibration:
                    calibrator.reset();
                    calibration.reset();
                    detector = knockthru::KnockDetector(config.detector);
                    show("Calibration canceled. Nothing saved.");
                    break;
                case PanelCommand::Sensitivity:
                    if (!calibration) break;
                    calibration->sensitivity = click.value;
                    // The slider clicks into its notches at the ends and the middle.
                    if (click.value == 0 || click.value == 0.5 || click.value == 1) haptics.buzz(1, t);
                    break;
                case PanelCommand::TryCalibration:
                    if (!calibrator || !calibration || !calibrator->done()) break;
                    calibration->phase = Phase::Trying;
                    calibration->trial_passed.reset();
                    detector = knockthru::KnockDetector(calibrator->trial_config(config.detector, calibration->sensitivity));
                    log("Trying " + kind_name + " thresholds: " + thresholds(calibrator->result(calibration->sensitivity)));
                    break;
                case PanelCommand::AdjustCalibration:
                    if (!calibration) break;
                    calibration->phase = Phase::Review;
                    detector = knockthru::KnockDetector(config.detector);
                    break;
                case PanelCommand::SaveCalibration:
                    if (!calibrator || !calibration || !calibration->trial_passed.value_or(false)) break;
                    try {
                        const auto saved = calibrator->save(calibration->sensitivity);
                        log("Calibrated " + kind_name + " knocks: " + thresholds(saved));
                        calibrator.reset();
                        calibration.reset();
                        reload();
                        show("Calibration saved");
                        state.calibration_saved = true;
                        haptics.buzz(2, t);
                    } catch (const std::exception& error) {
                        show(std::string("Calibration not saved: ") + error.what(), true);
                    }
                    break;
                }
            }
            if (shown) {
                read_room_view();
                try {
                    state.opaque = knockthru::read_style() == knockthru::RoomViewStyle::Opaque;
                } catch (const std::exception&) {
                    state.opaque.reset();
                }
            }
        }

        for (const auto& sample : poses.take()) detector.push(sample.device, sample.t, sample.position, sample.velocity);
        const auto knock_event = detector.poll(t);
        const auto knocks = detector.take_knocks();

        if (calibrator && calibration && calibration->phase == Phase::Recording) {
            for (const auto& knock : knocks) {
                calibrator->hear(knock);
                char line[160];
                std::snprintf(line, sizeof line,
                    "Calibration heard %s knock at %.3f s: peak %.0f m/s^2, approach %.2f m/s, before %.2f m/s",
                    knockthru::to_string(knock.kind), knock.t, knock.peak, knock.approach_speed, knock.speed_before);
                log(line);
            }
            while (const auto result = calibrator->next(t)) {
                const auto heard = result->heard;
                const auto& knock = result->knock;
                log(std::string("Calibration burst at ") + std::to_string(knock.t) + " s: " +
                    (heard == knockthru::Calibrator::Heard::Counted ? "counted"
                        : heard == knockthru::Calibrator::Heard::OtherKind ? "other kind"
                        : heard == knockthru::Calibrator::Heard::TooLight ? "too light" : "ignored"));
                if (heard == knockthru::Calibrator::Heard::Counted) {
                    haptics.buzz(1, t);
                    calibration->peaks = calibrator->peaks();
                    calibration->speeds = calibrator->speeds();
                    calibration->counted_t = t;
                } else if (heard == knockthru::Calibrator::Heard::OtherKind) {
                    calibration->ignored = knock.kind;
                    calibration->ignored_t = t;
                } else if (heard == knockthru::Calibrator::Heard::TooLight) {
                    calibration->too_light_t = t;
                }
            }
            calibration->seconds_left = static_cast<int>(std::ceil(calibrator->seconds_left(t)));
            if (calibrator->done()) {
                calibration->phase = Phase::Review;
                detector = knockthru::KnockDetector(config.detector);
                haptics.buzz(2, t + 0.3);
                log("Recorded " + std::string(knockthru::to_string(calibrator->kind())) + " knocks for calibration");
            } else if (calibrator->seconds_left(t) <= 0) {
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
                calibration->trial_heard = std::string("a ") + (knock_event->count == 3 ? "triple " : "double ") +
                    knockthru::to_string(knock_event->kind) + " knock";
                calibration->trial_t = t;
                log(std::string("Try step heard ") + calibration->trial_heard + (passed ? ": passed" : ": missed"));
                haptics.buzz(passed ? 1 : 3, t);
            }
        } else if (!calibrator && knock_event) { // the review step ignores knocks
            const std::string what = std::string(" (") + (knock_event->count == 3 ? "triple " : "double ") +
                knockthru::to_string(knock_event->kind) + " knock)";
            if (config.paused) {
                log("Ignored" + what + ": paused");
            } else if (config.blocked_apps.count(state.app_key)) {
                log("Ignored" + what + ": knocks are off in " + state.app_key);
            } else if (knock_event->count == 3) {
                haptics.buzz(toggle_style(what) ? 2 : 3, t);
            } else if (toggle_room_view(what)) {
                // Buzz now, because the switch can take a second. A failure buzzes 3 times later.
                haptics.buzz(1, t);
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
                if (!result.message.empty()) show(result.message);
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

        if (panel) {
            state.busy = room_view_job.valid() && room_view_job_toggles;
            state.paused = config.paused;
            state.classic_bar_icon = config.classic_bar_icon;
            panel->set_bar_icon(config.classic_bar_icon);
            state.enabled = config.detector.enabled;
            state.app_blocked = config.blocked_apps.count(state.app_key) > 0;
            state.calibration = calibration;
            panel->draw(state, t);
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
