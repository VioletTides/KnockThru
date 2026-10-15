#include "config.hpp"

#include <json.hpp>

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <map>
#include <stdexcept>

namespace knockthru {
namespace {

namespace fs = std::filesystem;
using json = nlohmann::json;

json read_json(const fs::path& path) {
    std::ifstream stream(path);
    if (!stream) return json::object();
    try {
        json value;
        stream >> value;
        if (!value.is_object()) throw std::runtime_error("top level must be an object");
        return value;
    } catch (const std::exception& error) {
        throw std::runtime_error("Cannot read " + path.string() + ": " + error.what());
    }
}

void write_json(const fs::path& path, const json& value) {
    fs::create_directories(path.parent_path());
    const auto temporary = path.string() + ".tmp";
    {
        std::ofstream stream(temporary, std::ios::trunc);
        if (!(stream << value.dump(2) << "\n")) throw std::runtime_error("Cannot write " + temporary);
    }
    fs::rename(temporary, path);
}

void merge(AppConfig& config, const json& file, const fs::path& path) {
    const auto fail = [&](const std::string& what) {
        throw std::runtime_error(path.string() + ": " + what);
    };
    for (const auto& [key, value] : file.items()) {
        if (key == "knock_types") {
            if (!value.is_array()) fail("knock_types must be a list");
            config.detector.enabled = {false, false, false};
            for (const auto& name : value) {
                const auto kind = name.is_string() ? parse_knock_kind(name.get<std::string>()) : std::nullopt;
                if (!kind) fail("knock_types accepts together, surface, and headset");
                config.detector.enabled[static_cast<int>(*kind)] = true;
            }
        } else if (key == "blocked_apps") {
            if (!value.is_array()) fail("blocked_apps must be a list of app keys");
            config.blocked_apps.clear();
            for (const auto& app : value) {
                if (!app.is_string()) fail("blocked_apps must be a list of app keys");
                config.blocked_apps.insert(app.get<std::string>());
            }
        } else if (key == "debug_port") {
            if (!value.is_number_integer()) fail("debug_port must be a whole number");
            config.debug_port = value.get<int>();
        } else if (key == "detector") {
            auto& d = config.detector;
            const std::map<std::string, double*> fields{
                {"controller_peak", &d.controller_peak}, {"confirmed_peak", &d.confirmed_peak},
                {"partner_peak", &d.partner_peak}, {"headset_peak", &d.headset_peak},
                {"max_speed_after", &d.max_speed_after}, {"surface_speed_after", &d.surface_speed_after},
                {"max_speed_before", &d.max_speed_before}, {"together_distance", &d.together_distance},
                {"same_spot", &d.same_spot}, {"merge_s", &d.merge_s}, {"min_gap_s", &d.min_gap_s}, {"fast_gap_s", &d.fast_gap_s},
                {"max_gap_s", &d.max_gap_s}, {"quiet_before_s", &d.quiet_before_s}};
            if (!value.is_object()) fail("detector must be an object");
            for (const auto& [name, number] : value.items()) {
                const auto field = fields.find(name);
                if (field == fields.end()) fail("unknown detector setting " + name);
                if (!number.is_number()) fail("detector." + name + " must be a number");
                *field->second = number.get<double>();
            }
        } else if (key.rfind("_", 0) != 0) { // keys starting with _ are comments
            fail("unknown setting " + key);
        }
    }
}

} // namespace

fs::path user_directory() {
    if (const auto xdg = std::getenv("XDG_CONFIG_HOME"); xdg && *xdg && fs::path(xdg).is_absolute())
        return fs::path(xdg) / "knockthru";
    const auto home = std::getenv("HOME");
    if (!home || !*home) throw std::runtime_error("HOME is not set");
    return fs::path(home) / ".config" / "knockthru";
}

AppConfig load_config(const fs::path& install_directory) {
    AppConfig config;
    for (const auto& path : {install_directory / "config.json", user_directory() / "config.json"})
        merge(config, read_json(path), path);
    apply_calibration(load_calibration(), config.detector);
    return config;
}

Calibration load_calibration() {
    const auto path = user_directory() / "calibration.json";
    const auto file = read_json(path);
    Calibration calibration;
    const auto read = [&](const char* key, std::optional<double>& out) {
        if (!file.contains(key)) return;
        if (!file[key].is_number()) throw std::runtime_error(path.string() + ": " + key + " must be a number");
        out = file[key].get<double>();
    };
    read("surface", calibration.surface);
    read("together", calibration.together);
    read("headset", calibration.headset);
    return calibration;
}

void save_calibration(const Calibration& calibration) {
    json file = json::object();
    if (calibration.surface) file["surface"] = *calibration.surface;
    if (calibration.together) file["together"] = *calibration.together;
    if (calibration.headset) file["headset"] = *calibration.headset;
    write_json(user_directory() / "calibration.json", file);
}

void apply_calibration(const Calibration& calibration, DetectorConfig& detector) {
    if (calibration.surface) detector.controller_peak = *calibration.surface;
    // Together and headset knocks share one threshold, so the softer kind wins.
    if (calibration.together && calibration.headset)
        detector.confirmed_peak = std::min(*calibration.together, *calibration.headset);
    else if (calibration.together || calibration.headset)
        detector.confirmed_peak = calibration.together ? *calibration.together : *calibration.headset;
    // A soft together knock also gives a soft partner spike.
    if (calibration.together || calibration.headset)
        detector.partner_peak = std::min(detector.partner_peak, detector.confirmed_peak * 2 / 3);
}

} // namespace knockthru
