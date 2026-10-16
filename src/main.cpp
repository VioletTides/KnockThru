#include "app.hpp"
#include "calibrator.hpp"
#include "config.hpp"
#include "diagnostics.hpp"
#include "haptics.hpp"
#include "install.hpp"
#include "knock_detector.hpp"
#include "poses.hpp"
#include "room_view.hpp"
#include "steamvr.hpp"
#include "style.hpp"

#include <openvr.h>

#include <csignal>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include <unistd.h>

using namespace knockthru;
namespace fs = std::filesystem;

namespace {

void stop(int) { stopping = 1; }

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
    KnockDetector detector(load_config(install_directory()).detector);
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
            std::printf("%8.3f  knock    %-8s peak %5.0f m/s^2\n", knock.t, to_string(knock.kind), knock.peak);
        if (event)
            std::printf("%8.3f  ACTION   %-8s x%d  (reported %.0f ms after the last knock)\n", event->t,
                to_string(event->kind), event->count, (t - event->t) * 1000);
        std::fflush(stdout);
        clock.tick();
    }
    return 0;
}

// Asks for knocks of one kind over the terminal and saves a new threshold.
int calibrate(const std::string& kind_name) {
    const auto kind = parse_knock_kind(kind_name);
    if (!kind) throw std::runtime_error("--calibrate takes together, surface, or headset");
    VrSession session(vr::VRApplication_Background);
    Clock clock;
    Calibrator calibrator(*kind, load_config(install_directory()).detector, clock.now());
    KnockDetector detector(calibrator.detector_config());
    PoseSampler sampler;
    Haptics haptics;

    std::cout << "Knock " << Calibrator::kWanted << " times, kind \"" << kind_name
              << "\", about 1 s apart. Each knock heard gives one buzz. You have "
              << static_cast<int>(Calibrator::kSeconds) << " s." << std::endl;
    while (!stopping && !calibrator.done() && calibrator.seconds_left(clock.now()) > 0) {
        const double t = clock.now();
        sampler.sample([&](Device device, const vr::TrackedDevicePose_t& pose) {
            PoseSampler::feed(detector, t, device, pose);
        });
        detector.poll(t);
        for (const auto& knock : detector.take_knocks()) calibrator.hear(knock);
        while (const auto result = calibrator.next(t)) {
            const auto& knock = result->knock;
            if (result->heard == Calibrator::Heard::OtherKind)
                std::cout << "  heard a " << to_string(knock.kind) << " knock, ignored" << std::endl;
            if (result->heard != Calibrator::Heard::Counted) continue;
            std::cout << "  knock " << calibrator.count() << " of " << Calibrator::kWanted << ": "
                      << static_cast<int>(knock.peak) << " m/s^2" << std::endl;
            haptics.buzz(1, t);
        }
        haptics.update(t);
        clock.tick();
    }
    if (!calibrator.done())
        throw std::runtime_error("Calibration stopped after " + std::to_string(calibrator.count()) + " knocks. Nothing saved.");
    std::cout << "Thresholds for " << kind_name << " set to " << thresholds_text(calibrator.save()) << " in "
              << (user_directory() / "calibration.json").string()
              << ". A running KnockThru picks it up within 2 s.\n";
    return 0;
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
        room_view = RoomView(port).active() ? "on" : "off";
    } catch (const std::exception& error) {
        room_view = error.what();
    }
    const auto path = write_report(session != nullptr, room_view);
    std::cout << "Wrote " << path.string() << ". Attach it to an issue at https://github.com/VioletTides/KnockThru/issues\n";
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
    const bool on = RoomView(port).active();
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

        if (command == "--run") return run_app();
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

        const auto config = load_config(install_directory());
        if (command == "--status") return status(config.debug_port);
        if (command == "--report") return report(config.debug_port);
        if (command == "--toggle") {
            std::cout << "Room View " << (RoomView(config.debug_port).toggle() ? "on" : "off") << "\n";
            return 0;
        }
        VrSession session(vr::VRApplication_Background);
        if (command == "--style") {
            std::cout << "Room View style " << to_string(toggle_style()) << "\n";
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
