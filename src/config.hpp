#pragma once

#include "knock_actions.hpp"
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
    std::set<std::string> blocked_apps; // SteamVR app keys, like steam.app.620980
    int debug_port = 8080;
    bool paused = false;
    bool classic_bar_icon = false; // logo colors on the dashboard bar, untinted
    bool sounds = true;
    bool tour_done = false;
    bool check_updates = true;
    KnockActions knock_actions;
};

// state.json, for what is not a setting. A missing or bad file gives defaults.
struct AppState {
    std::string steamvr_version; // at the last start
    std::string steamvr_since; // YYYY-MM-DD, when KnockThru first saw steamvr_version
    std::string steamvr_previous;
    long long last_update_check = 0; // Unix time, automatic checks only
    std::string knockthru_version;
};

struct KindCalibration {
    double peak = 0; // m/s^2
    double approach = 0; // m/s, the slowest approach that counts
    std::vector<double> peaks, speeds; // the recorded knocks
};

// Older files hold a bare spike threshold per kind, which loads as `peak` alone.
struct Calibration {
    std::optional<KindCalibration> surface, together, headset;
    std::optional<KindCalibration>& operator[](KnockKind kind) {
        switch (kind) {
        case KnockKind::Surface: return surface;
        case KnockKind::Together: return together;
        case KnockKind::Headset: break;
        }
        return headset;
    }
};

// $XDG_CONFIG_HOME/knockthru or ~/.config/knockthru. FrameDrop updates replace
// the install folder, so user files live here.
std::filesystem::path user_directory();

// Defaults, then <install>/config.json, then the user config.json, then
// calibration.json. Missing files are skipped. A bad file throws.
AppConfig load_config(const std::filesystem::path& install_directory);

// Each changes one key in the user config.json and keeps the rest.
void save_paused(bool paused);
void save_knock_types(const std::array<bool, 3>& enabled);
void save_blocked_apps(const std::set<std::string>& apps);
void save_sounds(bool sounds);
// Writes the table, or removes the key when every action is the default.
void save_knock_actions(const KnockActions& actions);
void save_tour_done(bool done);

// Deletes only the user config.json and calibration.json.
void reset_user_settings();

AppState load_state();
void save_state(const AppState& state);

// Returns the previous version when SteamVR changed, or "". `today` is YYYY-MM-DD.
std::string note_steamvr_version(AppState& state, const std::string& version, const std::string& today);

Calibration load_calibration();
void save_calibration(const Calibration& calibration);

void apply_calibration(const Calibration& calibration, DetectorConfig& detector);

} // namespace knockthru
