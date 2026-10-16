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

    fs::remove_all(root);
    std::cout << (failures ? std::to_string(failures) + " failed\n" : "all passed\n");
    return failures ? 1 : 0;
}
