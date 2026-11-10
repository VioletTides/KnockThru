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
    std::ofstream stream(temporary, std::ios::trunc);
    stream << value.dump(2) << "\n";
    stream.close(); // flushes, so a full disk fails here and never reaches the rename
    if (stream.fail()) {
        std::error_code ignored;
        fs::remove(temporary, ignored);
        throw std::runtime_error("Cannot write " + temporary);
    }
    fs::rename(temporary, path);
}

using Member = double DetectorConfig::*;

const std::pair<const char*, Member> kDetectorMembers[] = {
    {"controller_peak", &DetectorConfig::controller_peak},
    {"confirmed_peak", &DetectorConfig::confirmed_peak}, {"partner_peak", &DetectorConfig::partner_peak},
    {"headset_peak", &DetectorConfig::headset_peak},
    {"headset_knock_peak", &DetectorConfig::headset_knock_peak},
    {"max_speed_after", &DetectorConfig::max_speed_after},
    {"surface_speed_after", &DetectorConfig::surface_speed_after},
    {"max_speed_before", &DetectorConfig::max_speed_before},
    {"surface_speed_before", &DetectorConfig::surface_speed_before},
    {"together_distance", &DetectorConfig::together_distance}, {"same_spot", &DetectorConfig::same_spot},
    {"merge_s", &DetectorConfig::merge_s}, {"min_gap_s", &DetectorConfig::min_gap_s},
    {"fast_gap_s", &DetectorConfig::fast_gap_s}, {"max_gap_s", &DetectorConfig::max_gap_s},
    {"quiet_before_s", &DetectorConfig::quiet_before_s}};

} // namespace

double DetectorConfig::* detector_member(std::string_view key) {
    for (const auto& [name, member] : kDetectorMembers)
        if (key == name) return member;
    return nullptr;
}

namespace {

// The kind whose calibration sets a detector key, if any.
std::optional<KnockKind> calibrated_kind(const std::string& key) {
    if (key == "controller_peak") return KnockKind::Surface;
    if (key == "confirmed_peak") return KnockKind::Together;
    if (key == "headset_knock_peak") return KnockKind::Headset;
    return std::nullopt;
}

void merge(AppConfig& config, const json& file, const fs::path& path, Source source) {
    const auto fail = [&](const std::string& what) { throw std::runtime_error(path.string() + ": " + what); };
    const std::map<std::string, bool*> flags{{"paused", &config.paused},
        {"classic_bar_icon", &config.classic_bar_icon}, {"sounds", &config.sounds},
        {"tour_done", &config.tour_done}, {"check_updates", &config.check_updates},
        {"advanced_warning_off", &config.advanced_warning_off}};
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
        } else if (const auto flag = flags.find(key); flag != flags.end()) {
            if (!value.is_boolean()) fail(key + " must be true or false");
            *flag->second = value.get<bool>();
        } else if (key == "knock_actions") {
            // {"surface": {"2": "passthrough", "3": "perf_graph"}}. Missing kinds and counts keep their
            // default.
            const auto bad = [&] {
                fail("knock_actions maps together, surface, or headset to counts 2 and 3, each passthrough, "
                     "style, perf_graph, or none");
            };
            if (!value.is_object()) bad();
            config.knock_actions = {};
            for (const auto& [kind_name, counts] : value.items()) {
                const auto kind = parse_knock_kind(kind_name);
                if (!kind || !counts.is_object()) bad();
                for (const auto& [count_name, action_name] : counts.items()) {
                    const auto action = action_name.is_string()
                        ? parse_knock_action(action_name.get<std::string>())
                        : std::nullopt;
                    const int count = std::atoi(count_name.c_str());
                    const bool known =
                        std::find(kKnockCounts.begin(), kKnockCounts.end(), count) != kKnockCounts.end();
                    if (!action || !known || count_name != std::to_string(count)) bad();
                    config.knock_actions.set(*kind, count, *action);
                }
            }
        } else if (key == "debug_port") {
            if (!value.is_number_integer() || value < 1 || value > 65535)
                fail("debug_port must be a whole number from 1 to 65535");
            config.debug_port = value.get<int>();
        } else if (key == "detector") {
            if (!value.is_object()) fail("detector must be an object");
            for (const auto& [name, number] : value.items()) {
                const auto member = detector_member(name);
                if (!member) fail("unknown detector setting " + name);
                if (!number.is_number()) fail("detector." + name + " must be a number");
                config.detector.*member = number.get<double>();
                config.detector_sources[name] = source;
            }
        } else if (key.rfind("_", 0) != 0) { // keys starting with _ are comments
            fail("unknown setting " + key);
        }
    }
}

} // namespace

fs::path home_directory() {
    const auto home = std::getenv("HOME");
    return home && *home ? fs::path(home) : fs::path();
}

fs::path user_directory() {
    if (const auto xdg = std::getenv("XDG_CONFIG_HOME"); xdg && *xdg && fs::path(xdg).is_absolute())
        return fs::path(xdg) / "knockthru";
    const auto home = home_directory();
    if (home.empty()) throw std::runtime_error("HOME is not set");
    return home / ".config" / "knockthru";
}

namespace {

AppConfig default_config() {
    AppConfig config;
    for (const auto& [name, member] : kDetectorMembers) config.detector_sources[name] = Source::Default;
    return config;
}

} // namespace

AppConfig load_config_or_defaults(const fs::path& install_directory, std::string& error) {
    auto config = default_config();
    // Each file loads into a copy, so a file that fails halfway leaves nothing behind.
    const auto load = [&](const auto& apply) {
        auto next = config;
        try {
            apply(next);
            config = std::move(next);
        } catch (const std::exception& failure) {
            if (error.empty()) error = failure.what();
        }
    };
    load([&](AppConfig& next) {
        merge(next, read_json(install_directory / "config.json"), install_directory / "config.json",
            Source::Install);
    });
    load([](AppConfig& next) {
        merge(next, read_json(user_directory() / "config.json"), user_directory() / "config.json",
            Source::User);
    });
    load([](AppConfig& next) {
        apply_calibration(load_calibration(), next.detector, &next.detector_sources);
    });
    return config;
}

AppConfig load_config(const fs::path& install_directory) {
    std::string error;
    auto config = load_config_or_defaults(install_directory, error);
    if (!error.empty()) throw std::runtime_error(error);
    return config;
}

namespace {

// A null `value` removes the key.
void update_user_config(const std::string& key, const json& value) {
    const auto path = user_directory() / "config.json";
    auto file = read_json(path);
    if (value.is_null())
        file.erase(key);
    else
        file[key] = value;
    AppConfig check;
    merge(check, file, path, Source::User); // never write a file that load_config would reject
    write_json(path, file);
}

} // namespace

void save_paused(bool paused) { update_user_config("paused", paused); }

void save_knock_types(const std::array<bool, 3>& enabled) {
    json kinds = json::array();
    for (int k = 0; k < 3; ++k)
        if (enabled[k]) kinds.push_back(to_string(static_cast<KnockKind>(k)));
    update_user_config("knock_types", kinds);
}

void save_blocked_apps(const std::set<std::string>& apps) { update_user_config("blocked_apps", apps); }

void save_sounds(bool sounds) { update_user_config("sounds", sounds); }

void save_knock_actions(const KnockActions& actions) {
    if (actions.is_default()) {
        update_user_config("knock_actions", nullptr);
        return;
    }
    json table = json::object();
    for (int k = 0; k < 3; ++k) {
        const auto kind = static_cast<KnockKind>(k);
        for (const int count : kKnockCounts)
            table[to_string(kind)][std::to_string(count)] = to_string(actions.get(kind, count));
    }
    update_user_config("knock_actions", table);
}

void save_tour_done(bool done) { update_user_config("tour_done", done); }

void save_advanced_warning_off(bool off) { update_user_config("advanced_warning_off", off); }

void save_detector(const DetectorConfig& tuned, const AppConfig& loaded) {
    auto calibration = load_calibration();
    bool calibration_changed = false;
    json detector = read_json(user_directory() / "config.json").value("detector", json::object());
    bool detector_changed = false;
    for (const auto& [name, member] : kDetectorMembers) {
        const double value = tuned.*member;
        if (value == loaded.detector.*member) continue;
        const auto kind = calibrated_kind(name);
        if (kind && loaded.detector_sources.at(name) == Source::Calibration && calibration[*kind]) {
            calibration[*kind]->peak = value;
            calibration_changed = true;
        } else {
            detector[name] = value;
            detector_changed = true;
        }
    }
    if (calibration_changed) save_calibration(calibration);
    if (detector_changed) update_user_config("detector", detector);
}

namespace {

void remove_user_file(const char* name) {
    const auto path = user_directory() / name;
    std::error_code error;
    fs::remove(path, error); // a missing file is not an error
    if (error) throw std::runtime_error("Cannot delete " + path.string() + ": " + error.message());
}

} // namespace

void restore_detector_defaults() {
    update_user_config("detector", nullptr);
    remove_user_file("calibration.json");
}

void reset_user_settings() {
    remove_user_file("config.json");
    remove_user_file("calibration.json");
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
        state.running = file.value("running", false);
    } catch (const std::exception&) {
        // A damaged state file only costs the remembered values.
    }
    return state;
}

void save_state(const AppState& state) {
    write_json(user_directory() / "state.json",
        {{"steamvr_version", state.steamvr_version}, {"steamvr_since", state.steamvr_since},
            {"steamvr_previous", state.steamvr_previous}, {"last_update_check", state.last_update_check},
            {"knockthru_version", state.knockthru_version}, {"running", state.running}});
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
    for (const auto kind : {KnockKind::Together, KnockKind::Surface, KnockKind::Headset}) {
        const char* key = to_string(kind);
        if (!file.contains(key)) continue;
        const auto fail = [&](const std::string& what) {
            throw std::runtime_error(path.string() + ": " + key + " " + what);
        };
        const auto& entry = file[key];
        if (!entry.is_object()) fail("must be an object");
        KindCalibration out;
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
        calibration[kind] = out;
    }
    return calibration;
}

void save_calibration(const Calibration& calibration) {
    json file = json::object();
    auto copy = calibration;
    for (const auto kind : {KnockKind::Together, KnockKind::Surface, KnockKind::Headset}) {
        const auto& entry = copy[kind];
        if (!entry) continue;
        file[to_string(kind)] = {{"peak", entry->peak}, {"approach", entry->approach},
            {"peaks", entry->peaks}, {"speeds", entry->speeds}};
    }
    write_json(user_directory() / "calibration.json", file);
}

void apply_calibration(
    const Calibration& calibration, DetectorConfig& detector, std::map<std::string, Source>* sources) {
    const auto mark = [&](const char* key) {
        if (sources) (*sources)[key] = Source::Calibration;
    };
    auto copy = calibration;
    // A file saved under an older, lower floor gets the current one.
    for (const auto kind : {KnockKind::Together, KnockKind::Surface, KnockKind::Headset})
        if (auto& entry = copy[kind]) entry->peak = std::max(entry->peak, Calibrator::floor(kind));
    const auto approach = [&](KnockKind kind, const KindCalibration& entry) {
        detector.min_approach[static_cast<int>(kind)] = entry.approach;
    };
    if (const auto& surface = copy[KnockKind::Surface]) {
        detector.controller_peak = surface->peak;
        mark("controller_peak");
        approach(KnockKind::Surface, *surface);
    }
    if (const auto& together = copy[KnockKind::Together]) {
        detector.confirmed_peak = together->peak;
        mark("confirmed_peak");
        // A soft together knock also gives a soft partner spike.
        const double partner = std::min(detector.partner_peak, together->peak * 2 / 3);
        if (partner < detector.partner_peak) mark("partner_peak");
        detector.partner_peak = partner;
        approach(KnockKind::Together, *together);
    }
    if (const auto& headset = copy[KnockKind::Headset]) {
        detector.headset_knock_peak = headset->peak;
        mark("headset_knock_peak");
        approach(KnockKind::Headset, *headset);
    }
}

} // namespace knockthru
