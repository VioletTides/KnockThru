// Config tests: the tour flag and factory reset, in a scratch user directory.
#include "check.hpp"
#include "calibrator.hpp"
#include "config.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include <unistd.h>

using namespace knockthru;
namespace fs = std::filesystem;

int main() {
    const auto root = fs::temp_directory_path() / ("knockthru-config-test-" + std::to_string(getpid()));
    fs::remove_all(root);
    fs::create_directories(root);
    setenv("XDG_CONFIG_HOME", root.c_str(), 1);
    const auto user = user_directory();
    const auto install = root / "install"; // has no config.json, so defaults apply

    check(!expect_throw([] { reset_user_settings(); }), "reset with no files does not throw");

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
    check(expect_throw([&] { load_config(install); }, "sounds must be true or false"),
        "a setting that is not true or false is rejected");
    fs::remove(user / "config.json");

    // Knock actions: defaults, a partial table from the file, a bad action, and saving.
    const auto defaults = load_config(install).knock_actions;
    check(defaults.is_default() && defaults.get(KnockKind::Headset, 2) == KnockAction::Passthrough
            && defaults.get(KnockKind::Headset, 3) == KnockAction::Style,
        "twice switches passthrough and thrice switches the style by default");
    std::ofstream(user / "config.json")
        << R"({"knock_actions": {"headset": {"3": "perf_graph"}, "surface": {"2": "none"}}})";
    const auto loaded = load_config(install).knock_actions;
    check(loaded.get(KnockKind::Headset, 3) == KnockAction::PerfGraph
            && loaded.get(KnockKind::Surface, 2) == KnockAction::None
            && loaded.get(KnockKind::Together, 3) == KnockAction::Style && !loaded.shared(3),
        "knock_actions loads, and kinds and counts it leaves out keep their default");
    for (const auto* bad : {R"({"knock_actions": {"headset": {"3": "fly"}}})",
             R"({"knock_actions": {"headset": {"4": "style"}}})",
             R"({"knock_actions": {"elbow": {"2": "style"}}})"}) {
        std::ofstream(user / "config.json", std::ios::trunc) << bad;
        check(expect_throw([&] { load_config(install); }, "knock_actions maps"),
            std::string("knock_actions rejects ") + bad);
    }
    fs::remove(user / "config.json");
    auto changed = KnockActions{};
    changed.set_all(3, KnockAction::PerfGraph);
    save_sounds(false);
    save_knock_actions(changed);
    check(load_config(install).knock_actions == changed && changed.shared(3) == KnockAction::PerfGraph,
        "save_knock_actions loads back");
    save_knock_actions(KnockActions{});
    check(load_config(install).knock_actions.is_default() && !load_config(install).sounds
            && std::ifstream(user / "config.json")
            && read(user / "config.json").find("knock_actions") == std::string::npos,
        "saving the defaults removes knock_actions and keeps other settings");
    fs::remove(user / "config.json");

    {
        AppState state = load_state();
        check(state.steamvr_version.empty() && state.last_update_check == 0, "state defaults without a file");
        check(note_steamvr_version(state, "2.18.2", "2026-10-15").empty()
                && state.steamvr_since == "2026-10-15",
            "the first SteamVR version is not a change");
        check(note_steamvr_version(state, "2.18.2", "2026-10-16").empty()
                && state.steamvr_since == "2026-10-15",
            "the same SteamVR version is not a change");
        check(note_steamvr_version(state, "2.19.0", "2026-10-20") == "2.18.2"
                && state.steamvr_previous == "2.18.2" && state.steamvr_since == "2026-10-20",
            "a new SteamVR version reports the old one");
        state.last_update_check = 1792000000;
        save_state(state);
        const auto loaded = load_state();
        check(loaded.steamvr_version == "2.19.0" && loaded.steamvr_previous == "2.18.2"
                && loaded.last_update_check == 1792000000,
            "state survives a save and load");
        std::ofstream(user / "state.json") << "not json";
        check(load_state().steamvr_version.empty(), "a damaged state file gives defaults");
    }

    // The Advanced warning flag, Restore defaults, and detector sources.
    check(!load_config(install).advanced_warning_off, "the Advanced warning starts on");
    save_advanced_warning_off(true);
    check(load_config(install).advanced_warning_off, "Don't remind me saves");
    std::ofstream(user / "config.json") << R"({"sounds": false, "detector": {"merge_s": 0.06}})";
    std::ofstream(user / "calibration.json") << R"({"surface": {"peak": 30, "approach": 0.4}})";
    restore_detector_defaults();
    const auto restored = load_config(install);
    check(restored.detector.merge_s == DetectorConfig{}.merge_s && !restored.sounds
            && !fs::exists(user / "calibration.json"),
        "Restore defaults drops tuned and calibrated numbers and keeps other settings");
    fs::remove(user / "config.json");

    fs::create_directories(install);
    std::ofstream(install / "config.json")
        << R"({"detector": {"max_gap_s": 0.3, "merge_s": 0.07, "partner_peak": 40}})";
    std::ofstream(user / "config.json") << R"({"detector": {"merge_s": 0.06, "same_spot": 0.05}})";
    calibration = {};
    calibration.surface = KindCalibration{90, 0.5, {}, {}};
    calibration.together = KindCalibration{45, 0.5, {}, {}};
    save_calibration(calibration);
    {
        const auto loaded = load_config(install);
        const auto& sources = loaded.detector_sources;
        check(sources.size() == 16 && sources.at("headset_peak") == Source::Default,
            "every detector key has a source, and untouched keys are Default");
        check(sources.at("max_gap_s") == Source::Install, "an install value has the Install source");
        check(sources.at("merge_s") == Source::User && loaded.detector.merge_s == 0.06,
            "a user value overrides the install value and has the User source");
        check(sources.at("same_spot") == Source::User, "a user value has the User source");
        check(sources.at("controller_peak") == Source::Calibration && loaded.detector.controller_peak == 90
                && sources.at("confirmed_peak") == Source::Calibration
                && sources.at("headset_knock_peak") == Source::Default,
            "calibrated kinds have the Calibration source");
        check(sources.at("partner_peak") == Source::Calibration && loaded.detector.partner_peak == 30,
            "partner_peak is Calibration when calibration lowers it");
    }
    calibration.together->peak = 90;
    save_calibration(calibration);
    check(load_config(install).detector_sources.at("partner_peak") == Source::Install,
        "partner_peak keeps its source when calibration leaves it");
    calibration.together->peak = 45;
    save_calibration(calibration);

    // Saving tuned detector numbers.
    std::ofstream(user / "config.json") << R"({"sounds": false, "detector": {"same_spot": 0.05}})";
    {
        const auto loaded = load_config(install);
        auto tuned = loaded.detector;
        save_detector(tuned, loaded);
        check(read(user / "config.json").find("merge_s") == std::string::npos
                && read(user / "calibration.json").find("\"peak\": 90") != std::string::npos,
            "unchanged values are not written");
        tuned.max_gap_s = 0.33;
        tuned.controller_peak = 100;
        save_detector(tuned, loaded);
        const auto saved = load_config(install);
        check(saved.detector.max_gap_s == 0.33 && saved.detector_sources.at("max_gap_s") == Source::User,
            "a plain value goes to config.json");
        check(!saved.sounds && saved.detector.same_spot == 0.05, "saving a detector keeps the other keys");
        check(read(user / "config.json").find("controller_peak") == std::string::npos,
            "a calibrated peak stays out of config.json");
        check(saved.detector.controller_peak == 100
                && saved.detector_sources.at("controller_peak") == Source::Calibration,
            "a calibrated peak goes to calibration.json and loads back");
        check(load_calibration().surface->approach == 0.5, "saving a peak keeps the rest of the calibration");
        tuned = saved.detector;
        tuned.controller_peak = 10;
        save_detector(tuned, saved);
        check(load_config(install).detector.controller_peak == Calibrator::floor(KnockKind::Surface),
            "a calibrated peak under the floor loads back at the floor");
    }

    if (fs::exists("/dev/full")) {
        // A temp file that is a symlink to /dev/full fails every flush, like a full disk.
        save_paused(false);
        const auto before = read(user / "config.json");
        fs::create_symlink("/dev/full", user / "config.json.tmp");
        check(expect_throw([] { save_paused(true); }), "a failed write throws");
        check(read(user / "config.json") == before, "a failed write keeps the old config.json");
        check(!fs::is_symlink(user / "config.json.tmp") && !fs::exists(user / "config.json.tmp"),
            "a failed write removes the temp file");
    }

    fs::remove_all(root);
    return finish();
}
