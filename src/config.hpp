#pragma once

#include "knock_detector.hpp"

#include <filesystem>
#include <optional>
#include <set>
#include <string>

namespace knockthru {

struct AppConfig {
    DetectorConfig detector;
    std::set<std::string> blocked_apps; // SteamVR app keys, for example steam.app.620980
    int debug_port = 8080;
};

// Peaks recorded by --calibrate, per knock kind.
struct Calibration {
    std::optional<double> surface, together, headset;
};

// ~/.config/knockthru, or $XDG_CONFIG_HOME/knockthru. Holds config.json,
// calibration.json, and application.log. FrameDrop updates replace the
// install directory, so user files live here.
std::filesystem::path user_directory();

// Defaults, then <install>/config.json, then the user config.json, then
// calibration.json. Missing files are skipped. A bad file throws.
AppConfig load_config(const std::filesystem::path& install_directory);

Calibration load_calibration();
void save_calibration(const Calibration& calibration);

// Applies calibrated peaks to the detector thresholds.
void apply_calibration(const Calibration& calibration, DetectorConfig& detector);

} // namespace knockthru
