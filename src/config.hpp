#pragma once

#include "knock_detector.hpp"

#include <array>
#include <filesystem>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace knockthru {

struct AppConfig {
    DetectorConfig detector;
    std::set<std::string> blocked_apps; // SteamVR app keys, for example steam.app.620980
    int debug_port = 8080;
    bool paused = false; // knocks do nothing while paused
    bool classic_bar_icon = false; // the dashboard bar icon in the logo colors, without Steam's tint
};

// One knock kind's calibration.
struct KindCalibration {
    double peak = 0;           // m/s^2, the spike threshold
    double approach = 0;       // m/s, the slowest approach that counts
    double sensitivity = 0.5;  // the slider position, from 0 (least) to 1 (most)
    std::vector<double> peaks, speeds; // the recorded knocks, for moving the slider later
};

// calibration.json: one entry per calibrated knock kind. Older files hold a
// bare spike threshold per kind, which loads as `peak` alone.
struct Calibration {
    std::optional<KindCalibration> surface, together, headset;
    std::optional<KindCalibration>& operator[](KnockKind kind) {
        return kind == KnockKind::Surface ? surface : kind == KnockKind::Together ? together : headset;
    }
};

// ~/.config/knockthru, or $XDG_CONFIG_HOME/knockthru. Holds config.json,
// calibration.json, and application.log. FrameDrop updates replace the
// install directory, so user files live here.
std::filesystem::path user_directory();

// Defaults, then <install>/config.json, then the user config.json, then
// calibration.json. Missing files are skipped. A bad file throws.
AppConfig load_config(const std::filesystem::path& install_directory);

// Changes settings in the user config.json and keeps every other key. The
// dashboard panel uses these.
void save_paused(bool paused);
void save_classic_bar_icon(bool classic);
void save_knock_types(const std::array<bool, 3>& enabled);
void save_blocked_apps(const std::set<std::string>& apps);

Calibration load_calibration();
void save_calibration(const Calibration& calibration);

// Applies calibrated peaks to the detector thresholds.
void apply_calibration(const Calibration& calibration, DetectorConfig& detector);

} // namespace knockthru
