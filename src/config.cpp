#include "config.hpp"

#include "calibrator.hpp"

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
        } else if (key == "paused") {
            if (!value.is_boolean()) fail("paused must be true or false");
            config.paused = value.get<bool>();
        } else if (key == "classic_bar_icon") {
            if (!value.is_boolean()) fail("classic_bar_icon must be true or false");
            config.classic_bar_icon = value.get<bool>();
        } else if (key == "sounds") {
            if (!value.is_boolean()) fail("sounds must be true or false");
            config.sounds = value.get<bool>();
        } else if (key == "tour_done") {
            if (!value.is_boolean()) fail("tour_done must be true or false");
            config.tour_done = value.get<bool>();
        } else if (key == "check_updates") {
            if (!value.is_boolean()) fail("check_updates must be true or false");
            config.check_updates = value.get<bool>();
        } else if (key == "debug_port") {
            if (!value.is_number_integer()) fail("debug_port must be a whole number");
            config.debug_port = value.get<int>();
        } else if (key == "detector") {
            auto& d = config.detector;
            const std::map<std::string, double*> fields{
                {"controller_peak", &d.controller_peak}, {"confirmed_peak", &d.confirmed_peak},
                {"partner_peak", &d.partner_peak}, {"headset_peak", &d.headset_peak}, {"headset_knock_peak", &d.headset_knock_peak},
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

namespace {

void update_user_config(const std::string& key, const json& value) {
    const auto path = user_directory() / "config.json";
    auto file = read_json(path);
    file[key] = value;
    AppConfig check;
    merge(check, file, path); // never write a file that load_config would reject
    write_json(path, file);
}

} // namespace

void save_paused(bool paused) { update_user_config("paused", paused); }

void save_classic_bar_icon(bool classic) { update_user_config("classic_bar_icon", classic); }

void save_knock_types(const std::array<bool, 3>& enabled) {
    json kinds = json::array();
    for (int k = 0; k < 3; ++k)
        if (enabled[k]) kinds.push_back(to_string(static_cast<KnockKind>(k)));
    update_user_config("knock_types", kinds);
}

void save_blocked_apps(const std::set<std::string>& apps) { update_user_config("blocked_apps", apps); }

void save_sounds(bool sounds) { update_user_config("sounds", sounds); }

void save_tour_done(bool done) { update_user_config("tour_done", done); }

void reset_user_settings() {
    for (const auto* name : {"config.json", "calibration.json"}) {
        const auto path = user_directory() / name;
        std::error_code error;
        fs::remove(path, error); // a missing file is not an error
        if (error) throw std::runtime_error("Cannot delete " + path.string() + ": " + error.message());
    }
}

AppState load_state() {
    AppState state;
    try {
        const auto file = read_json(user_directory() / "state.json");
        state.steamvr_version = file.value("steamvr_version", "");
        state.steamvr_since = file.value("steamvr_since", "");
        state.steamvr_previous = file.value("steamvr_previous", "");
        state.last_update_check = file.value("last_update_check", 0LL);
        state.knockthru_version = file.value("knockthru_version", "");
    } catch (const std::exception&) {
        // A damaged state file only costs the remembered values.
    }
    return state;
}

void save_state(const AppState& state) {
    write_json(user_directory() / "state.json",
        {{"steamvr_version", state.steamvr_version}, {"steamvr_since", state.steamvr_since},
         {"steamvr_previous", state.steamvr_previous}, {"last_update_check", state.last_update_check},
         {"knockthru_version", state.knockthru_version}});
}

std::string note_steamvr_version(AppState& state, const std::string& version, const std::string& today) {
    if (version.empty() || version == state.steamvr_version) return "";
    const auto previous = state.steamvr_version;
    if (!previous.empty()) state.steamvr_previous = previous;
    state.steamvr_version = version;
    state.steamvr_since = today;
    return previous;
}

Calibration load_calibration() {
    const auto path = user_directory() / "calibration.json";
    const auto file = read_json(path);
    Calibration calibration;
    bool legacy = false;
    for (const auto kind : {KnockKind::Together, KnockKind::Surface, KnockKind::Headset}) {
        const char* key = to_string(kind);
        if (!file.contains(key)) continue;
        const auto fail = [&](const std::string& what) {
            throw std::runtime_error(path.string() + ": " + key + " " + what);
        };
        const auto& entry = file[key];
        KindCalibration out;
        if (entry.is_number()) {
            out.peak = entry.get<double>();
            legacy = true;
        } else if (entry.is_object()) {
            const auto number = [&](const char* field, double& value) {
                if (!entry.contains(field)) return;
                if (!entry[field].is_number()) fail(std::string(field) + " must be a number");
                value = entry[field].get<double>();
            };
            const auto numbers = [&](const char* field, std::vector<double>& values) {
                if (!entry.contains(field)) return;
                if (!entry[field].is_array()) fail(std::string(field) + " must be a list of numbers");
                for (const auto& value : entry[field]) {
                    if (!value.is_number()) fail(std::string(field) + " must be a list of numbers");
                    values.push_back(value.get<double>());
                }
            };
            if (!entry.contains("peak")) fail("needs a peak");
            number("peak", out.peak);
            number("approach", out.approach);
            numbers("peaks", out.peaks);
            numbers("speeds", out.speeds);
        } else {
            fail("must be a number or an object");
        }
        calibration[kind] = out;
    }
    // Older files shared one threshold between together and headset knocks.
    const auto peak_only = [](double peak) {
        KindCalibration out;
        out.peak = peak;
        return out;
    };
    if (legacy && !calibration.headset && calibration.together) calibration.headset = peak_only(calibration.together->peak);
    if (legacy && !calibration.together && calibration.headset) calibration.together = peak_only(calibration.headset->peak);
    return calibration;
}

void save_calibration(const Calibration& calibration) {
    json file = json::object();
    auto copy = calibration;
    for (const auto kind : {KnockKind::Together, KnockKind::Surface, KnockKind::Headset}) {
        const auto& entry = copy[kind];
        if (!entry) continue;
        file[to_string(kind)] = {{"peak", entry->peak}, {"approach", entry->approach}, {"peaks", entry->peaks},
            {"speeds", entry->speeds}};
    }
    write_json(user_directory() / "calibration.json", file);
}

void apply_calibration(const Calibration& calibration, DetectorConfig& detector) {
    auto copy = calibration;
    // A file saved under an older, lower floor gets the current one.
    for (const auto kind : {KnockKind::Together, KnockKind::Surface, KnockKind::Headset})
        if (auto& entry = copy[kind]) entry->peak = std::max(entry->peak, Calibrator::floor(kind));
    const auto approach = [&](KnockKind kind, const KindCalibration& entry) {
        detector.min_approach[static_cast<int>(kind)] = entry.approach;
    };
    if (const auto& surface = copy[KnockKind::Surface]) {
        detector.controller_peak = surface->peak;
        approach(KnockKind::Surface, *surface);
    }
    if (const auto& together = copy[KnockKind::Together]) {
        detector.confirmed_peak = together->peak;
        // A soft together knock also gives a soft partner spike.
        detector.partner_peak = std::min(detector.partner_peak, together->peak * 2 / 3);
        approach(KnockKind::Together, *together);
    }
    if (const auto& headset = copy[KnockKind::Headset]) {
        detector.headset_knock_peak = headset->peak;
        approach(KnockKind::Headset, *headset);
    }
}

} // namespace knockthru
