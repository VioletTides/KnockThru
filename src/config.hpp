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
    bool sounds = true; // SteamVR's click on a triple knock and on panel buttons
    bool tour_done = false; // the first-run tour finished or skipped
    bool check_updates = true; // look for a new release on GitHub once a day
};

// What KnockThru remembers between runs that is not a setting, in
// ~/.config/knockthru/state.json. A missing or unreadable file gives defaults.
struct AppState {
    std::string steamvr_version;  // the SteamVR version at the last start
    std::string steamvr_since;    // the date KnockThru first saw that version, YYYY-MM-DD
    std::string steamvr_previous; // the version before it, empty if none
    long long last_update_check = 0; // Unix time of the last automatic update check
    std::string knockthru_version; // the KnockThru version at the last start
};

// One knock kind's calibration.
struct KindCalibration {
    double peak = 0;           // m/s^2, the spike threshold
    double approach = 0;       // m/s, the slowest approach that counts
    std::vector<double> peaks, speeds; // the recorded knocks
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
void save_sounds(bool sounds);
void save_tour_done(bool done);

// Factory reset: deletes the user config.json and calibration.json. Keeps
// application.log and the SteamVR registration files.
void reset_user_settings();

AppState load_state();
void save_state(const AppState& state);

// Records `version` as the running SteamVR version on `today` (YYYY-MM-DD).
// Returns the previous version when it changed, or an empty string.
std::string note_steamvr_version(AppState& state, const std::string& version, const std::string& today);

Calibration load_calibration();
void save_calibration(const Calibration& calibration);

// Applies calibrated peaks to the detector thresholds.
void apply_calibration(const Calibration& calibration, DetectorConfig& detector);

} // namespace knockthru
