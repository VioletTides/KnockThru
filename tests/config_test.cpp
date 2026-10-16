// Config tests: the tour flag and factory reset, in a scratch user directory.
#include "config.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include <unistd.h>

using namespace knockthru;
namespace fs = std::filesystem;

namespace {

int failures = 0;

void check(bool ok, const std::string& what) {
    std::cout << (ok ? "  ok    " : "  FAIL  ") << what << "\n";
    if (!ok) ++failures;
}

} // namespace

int main() {
    const auto root = fs::temp_directory_path() / ("knockthru-config-test-" + std::to_string(getpid()));
    fs::remove_all(root);
    fs::create_directories(root);
    setenv("XDG_CONFIG_HOME", root.c_str(), 1);
    const auto user = user_directory();
    const auto install = root / "install"; // has no config.json, so defaults apply

    try {
        reset_user_settings();
        check(true, "reset with no files does not throw");
    } catch (const std::exception& error) {
        check(false, std::string("reset with no files does not throw: ") + error.what());
    }

    check(!load_config(install).tour_done, "tour_done starts false");
    save_tour_done(true);
    check(load_config(install).tour_done, "save_tour_done(true) loads back");
    save_paused(true);
    check(load_config(install).tour_done, "saving another setting keeps tour_done");

    Calibration calibration;
    calibration.surface = KindCalibration{120, 0.5, {}, {}};
    save_calibration(calibration);
    std::ofstream(user / "application.log") << "log line\n";

    reset_user_settings();
    check(!fs::exists(user / "config.json"), "reset deletes config.json");
    check(!fs::exists(user / "calibration.json"), "reset deletes calibration.json");
    check(fs::exists(user / "application.log"), "reset keeps application.log");
    const auto after = load_config(install);
    check(!after.tour_done && !after.paused, "settings are back to defaults after reset");
    check(after.check_updates, "the daily update check is on by default");

    std::ofstream(user / "config.json") << R"({"paused": true, "classic_bar_icon": true, "sounds": false,
        "tour_done": true, "check_updates": false})";
    const auto flags = load_config(install);
    check(flags.paused && flags.classic_bar_icon && !flags.sounds && flags.tour_done && !flags.check_updates,
        "every true or false setting loads from config.json");
    std::ofstream(user / "config.json") << R"({"sounds": "yes"})";
    try {
        load_config(install);
        check(false, "a setting that is not true or false is rejected");
    } catch (const std::exception& error) {
        check(std::string(error.what()).find("sounds must be true or false") != std::string::npos,
            "a setting that is not true or false is rejected");
    }
    fs::remove(user / "config.json");

    {
        AppState state = load_state();
        check(state.steamvr_version.empty() && state.last_update_check == 0, "state defaults without a file");
        check(note_steamvr_version(state, "2.18.2", "2026-10-15").empty() && state.steamvr_since == "2026-10-15",
            "the first SteamVR version is not a change");
        check(note_steamvr_version(state, "2.18.2", "2026-10-16").empty() && state.steamvr_since == "2026-10-15",
            "the same SteamVR version is not a change");
        check(note_steamvr_version(state, "2.19.0", "2026-10-20") == "2.18.2" && state.steamvr_previous == "2.18.2" &&
                  state.steamvr_since == "2026-10-20",
            "a new SteamVR version reports the old one");
        state.last_update_check = 1792000000;
        save_state(state);
        const auto loaded = load_state();
        check(loaded.steamvr_version == "2.19.0" && loaded.steamvr_previous == "2.18.2" &&
                  loaded.last_update_check == 1792000000,
            "state survives a save and load");
        std::ofstream(user / "state.json") << "not json";
        check(load_state().steamvr_version.empty(), "a damaged state file gives defaults");
    }

    fs::remove_all(root);
    std::cout << (failures ? std::to_string(failures) + " failed\n" : "all passed\n");
    return failures ? 1 : 0;
}
