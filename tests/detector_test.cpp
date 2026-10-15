// Detector tests: synthetic sequences, then replays of the Steam Frame
// recordings in tests/data/ (see docs/knock-signal.md).
#include "calibrator.hpp"
#include "config.hpp"
#include "knock_detector.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

using namespace knockthru;

namespace {

int failures = 0;

void check(bool ok, const std::string& what) {
    std::cout << (ok ? "  ok    " : "  FAIL  ") << what << "\n";
    if (!ok) ++failures;
}

std::vector<KnockEvent> drain(KnockDetector& detector, double now, std::vector<KnockEvent> events = {}) {
    if (auto event = detector.poll(now)) events.push_back(*event);
    return events;
}

// Simulates one controller resting at `position`, with knocks at the given
// times. Each knock is a fast approach (0.8 m/s) that stops dead.
std::vector<KnockEvent> synthetic(const std::vector<double>& knock_times, DetectorConfig config = {},
                                  Device device = Device::Right, Vec3 position = {0.2, 0.8, -0.3},
                                  double duration = 3.0) {
    KnockDetector detector(config);
    std::vector<KnockEvent> events;
    const double dt = 1.0 / 233;
    for (double t = 0; t < duration; t += dt) {
        double speed = 0;
        for (const double k : knock_times)
            if (t > k - 0.06 && t <= k) speed = 0.8;
        // Tiny jitter, so every sample counts as a new measurement.
        const double jitter = 0.001 * ((static_cast<int>(t / dt) % 2) ? 1 : -1);
        detector.push(device, t, position, {jitter, -speed, 0});
        detector.push(Device::Hmd, t, {0, 1.6, 0}, {0, jitter, 0});
        events = drain(detector, t, events);
    }
    return events;
}

void synthetic_tests() {
    std::cout << "synthetic\n";
    check(synthetic({1.0}).empty(), "a single knock does nothing");

    auto events = synthetic({1.0, 1.25});
    check(events.size() == 1 && events[0].count == 2 && events[0].kind == KnockKind::Surface,
          "two knocks 250 ms apart are a surface double");

    events = synthetic({1.0, 1.25, 1.5});
    check(events.size() == 1 && events[0].count == 3, "three knocks are a triple");

    check(synthetic({0.5, 0.75, 1.0, 1.25}).empty(), "four knocks do nothing");
    check(synthetic({1.0, 1.7}).empty(), "knocks 700 ms apart do nothing");

    events = synthetic({0.5, 0.75, 1.4, 1.65});
    check(events.size() == 1, "a double 650 ms after another double does nothing");

    check(synthetic({0.75, 1.3, 1.55}).empty(), "a double 550 ms after another knock does nothing");
    check(synthetic({1.0, 1.4, 1.8, 2.25}).size() == 0, "four knocks with even gaps do nothing");
    check(synthetic({0.5, 0.75, 1.75, 2.0}).size() == 2, "two doubles 1 s apart both count");
    check(synthetic({0.2, 1.3, 1.55}).size() == 1, "a double 1.1 s after another knock counts");

    DetectorConfig no_surface;
    no_surface.enabled[static_cast<int>(KnockKind::Surface)] = false;
    check(synthetic({1.0, 1.25}, no_surface).empty(), "a turned-off kind does nothing");
}

struct Row {
    double t;
    Device device;
    Vec3 position, velocity;
};

std::vector<Row> load(const std::string& path) {
    std::vector<Row> rows;
    FILE* pipe = popen(("gzip -dc '" + path + "'").c_str(), "r");
    if (!pipe) return rows;
    char buffer[512];
    bool header = true;
    while (std::fgets(buffer, sizeof buffer, pipe)) {
        if (header) { header = false; continue; }
        std::stringstream line(buffer);
        std::string field;
        std::vector<std::string> f;
        while (std::getline(line, field, ',')) f.push_back(field);
        if (f.size() < 9 || f[2] != "1") continue;
        const Device device = f[1] == "hmd" ? Device::Hmd : f[1] == "left" ? Device::Left : Device::Right;
        rows.push_back({std::stod(f[0]), device, {std::stod(f[3]), std::stod(f[4]), std::stod(f[5])},
                        {std::stod(f[6]), std::stod(f[7]), std::stod(f[8])}});
    }
    pclose(pipe);
    return rows;
}

// KNOCKTHRU_TEST_SET="controller_peak=60,max_speed_after=0.4" overrides
// detector settings, for tuning sweeps.
DetectorConfig test_config() {
    DetectorConfig config;
    const char* set = std::getenv("KNOCKTHRU_TEST_SET");
    if (!set) return config;
    const std::map<std::string, double*> fields{
        {"controller_peak", &config.controller_peak}, {"confirmed_peak", &config.confirmed_peak},
        {"partner_peak", &config.partner_peak}, {"headset_peak", &config.headset_peak}, {"tap_peak", &config.tap_peak},
        {"min_approach_together", &config.min_approach[0]}, {"min_approach_surface", &config.min_approach[1]},
        {"min_approach_headset", &config.min_approach[2]},
        {"max_speed_after", &config.max_speed_after}, {"surface_speed_after", &config.surface_speed_after},
        {"max_speed_before", &config.max_speed_before}, {"same_spot", &config.same_spot},
        {"min_gap_s", &config.min_gap_s}, {"fast_gap_s", &config.fast_gap_s}, {"max_gap_s", &config.max_gap_s},
        {"quiet_before_s", &config.quiet_before_s}};
    std::stringstream list(set);
    std::string item;
    while (std::getline(list, item, ',')) {
        const auto eq = item.find('=');
        const auto field = fields.find(item.substr(0, eq));
        if (eq == std::string::npos || field == fields.end()) {
            std::cerr << "Unknown setting in KNOCKTHRU_TEST_SET: " << item << "\n";
            std::exit(2);
        }
        *field->second = std::stod(item.substr(eq + 1));
    }
    return config;
}

std::vector<KnockEvent> replay_events(const std::string& name, const DetectorConfig& config) {
    const auto rows = load(std::string(KNOCKTHRU_TEST_DATA) + "/" + name + ".csv.gz");
    KnockDetector detector(config);
    std::vector<KnockEvent> events;
    for (const auto& row : rows) {
        detector.push(row.device, row.t, row.position, row.velocity);
        if (auto event = detector.poll(row.t)) events.push_back(*event);
    }
    if (!rows.empty())
        if (auto event = detector.poll(rows.back().t + 1)) events.push_back(*event);
    return events;
}

std::map<std::string, int> replay(const std::string& name) {
    const auto rows = load(std::string(KNOCKTHRU_TEST_DATA) + "/" + name + ".csv.gz");
    KnockDetector detector(test_config());
    std::map<std::string, int> counts;
    for (const auto& row : rows) {
        detector.push(row.device, row.t, row.position, row.velocity);
        auto event = detector.poll(row.t);
        if (std::getenv("KNOCKTHRU_TEST_VERBOSE"))
            for (const auto& knock : detector.take_knocks())
                std::cout << "    knock " << knock.t << " " << to_string(knock.kind) << " source " << knock.source
                          << " peak " << knock.peak << "\n";
        if (event) {
            if (std::getenv("KNOCKTHRU_TEST_VERBOSE"))
                std::cout << "    " << event->t << " " << to_string(event->kind) << "x" << event->count << "\n";
            ++counts[std::string(to_string(event->kind)) + "x" + std::to_string(event->count)];
        }
    }
    if (!rows.empty())
        if (auto event = detector.poll(rows.back().t + 1))
            ++counts[std::string(to_string(event->kind)) + "x" + std::to_string(event->count)];
    std::cout << "  " << name << " (" << rows.size() << " samples):";
    for (const auto& [key, value] : counts) std::cout << " " << key << "=" << value;
    std::cout << "\n";
    return counts;
}

int total(const std::map<std::string, int>& counts) {
    int sum = 0;
    for (const auto& entry : counts) sum += entry.second;
    return sum;
}

// Live session with a known routine: 4 each of together, surface, and headset
// doubles, then 4 each of the same triples, about 1.5 to 2 s apart. Each entry
// is when a sequence starts. An action must end within 1 s of that start.
// Returns how many actions were right and how many wrong.
std::pair<int, int> mixed_score(const DetectorConfig& config, bool verbose) {
    struct Expected { double start; KnockKind kind; int count; };
    std::vector<Expected> expected;
    const auto add = [&](KnockKind kind, int count, std::initializer_list<double> starts) {
        for (const double start : starts) expected.push_back({start, kind, count});
    };
    add(KnockKind::Together, 2, {6.5, 9.2, 11.1, 12.75});
    add(KnockKind::Surface, 2, {14.8, 17.1, 18.8, 20.5});
    add(KnockKind::Headset, 2, {22.1, 24.1, 26.1, 27.75});
    add(KnockKind::Together, 3, {29.7, 31.3, 33.2, 34.85});
    add(KnockKind::Surface, 3, {36.9, 39.5, 41.4, 43.5});
    add(KnockKind::Headset, 3, {45.45, 47.9, 49.9, 51.65});

    int right = 0, wrong = 0;
    for (const auto& event : replay_events("mixed", config)) {
        const Expected* match = nullptr;
        for (const auto& e : expected)
            if (event.t >= e.start && event.t <= e.start + 1.0) match = &e;
        const bool ok = match && match->kind == event.kind && match->count == event.count;
        ok ? ++right : ++wrong;
        if (verbose && (!ok || std::getenv("KNOCKTHRU_TEST_VERBOSE")))
            std::cout << "    " << (ok ? "right" : "WRONG") << " " << event.t << " " << to_string(event.kind)
                      << "x" << event.count << "\n";
    }
    return {right, wrong};
}

void mixed_test() {
    const auto [right, wrong] = mixed_score(test_config(), true);
    std::cout << "  mixed: " << right << " of 24 right, " << wrong << " wrong\n";
    check(wrong == 0, "mixed: no wrong actions");
    check(right >= 20, "mixed: at least 20 of 24 right");
}

// The wearer knocked in pairs for all 45 s of each knock recording, about 20
// pairs per file. The bars sit just under what the Python prototype found.
void replay_tests() {
    std::cout << "recordings\n";
    // Fast together doubles, 89 to 168 ms apart, about 20 pairs in 30 s.
    auto fast = replay("fast");
    check(fast["togetherx2"] >= 12 && total(fast) == fast["togetherx2"], "fast: at least 12 together doubles, nothing else");
    auto together = replay("together");
    check(together["togetherx2"] >= 18, "together: at least 18 together doubles");
    auto surface = replay("surface");
    check(surface["surfacex2"] >= 16, "surface: at least 16 surface doubles");
    auto headset = replay("headset");
    check(headset["headsetx2"] >= 15, "headset: at least 15 headset doubles");
    check(total(replay("play")) == 0, "play: no actions");
    check(total(replay("noise")) == 0, "noise: no actions");
    mixed_test();
}

// Runs a calibration of `kind` on a recording, as the app would.
Calibrator calibrate(const std::string& name, KnockKind kind) {
    const auto rows = load(std::string(KNOCKTHRU_TEST_DATA) + "/" + name + ".csv.gz");
    Calibrator calibrator(kind, DetectorConfig{}, 0);
    KnockDetector detector(calibrator.detector_config());
    for (const auto& row : rows) {
        detector.push(row.device, row.t, row.position, row.velocity);
        detector.poll(row.t);
        for (const auto& knock : detector.take_knocks()) calibrator.hear(knock);
        while (calibrator.next(row.t)) {}
    }
    return calibrator;
}

// Counts the knocks a calibration of `kind` would take from a recording.
int calibration_count(const std::string& name, KnockKind kind) {
    const int counted = calibrate(name, kind).count();
    std::cout << "  calibrating " << to_string(kind) << " on " << name << ": " << counted << " counted\n";
    return counted;
}

// Knocks heard in a surface calibration on the Frame, from application.log: t,
// peak, approach speed, and speed before. At 42.44 s and 48.28 s the controller
// spiked as it started a hard swing, 0.09 to 0.14 s before the knock.
void logged_calibration_test() {
    const double heard[][4] = {{38.180, 52, .65, .12}, {38.320, 39, .80, .28}, {38.982, 31, .20, .59},
        {39.261, 40, 1.64, .35}, {39.520, 33, .32, .67}, {39.621, 34, .41, .66}, {39.981, 268, .99, .21},
        {41.084, 381, 1.09, .07}, {42.441, 37, .08, .02}, {42.584, 146, .96, .02}, {44.021, 67, .71, .02},
        {45.442, 90, .52, .03}, {46.985, 331, 1.82, .19}, {48.279, 37, .29, .03}, {48.364, 461, 1.26, .07},
        {49.758, 173, 1.65, .14}, {51.098, 76, .66, .05}};
    Calibrator calibrator(KnockKind::Surface, DetectorConfig{}, 38);
    for (const auto& h : heard) {
        Knock knock{h[0], KnockKind::Surface, 2, {0, 0, 0}, h[1], h[3], h[2]};
        while (calibrator.next(knock.t)) {}
        calibrator.hear(knock);
    }
    while (calibrator.next(60)) {}
    const auto& peaks = calibrator.peaks();
    const auto has = [&](double peak) { return std::find(peaks.begin(), peaks.end(), peak) != peaks.end(); };
    check(has(146) && has(461) && !has(37), "logged: hard knocks count, not the swing starts before them");
    check(!has(31), "logged: a slow hand movement does not count");
}

// Calibrates each kind on its recording, applies the result at each slider
// notch, and replays every recording with it. Calibration must not add false
// actions, and the middle notch must keep about as many real doubles as the
// defaults. The least sensitive notch may lose a few: that is its job.
void calibrated_replay_tests() {
    std::cout << "calibrated detection\n";
    for (const auto kind : {KnockKind::Together, KnockKind::Surface, KnockKind::Headset}) {
        const auto calibrator = calibrate(to_string(kind), kind);
        const auto name = std::string(to_string(kind));
        for (const double sensitivity : {0.0, 0.5, 1.0}) {
            Calibration calibration;
            calibration[kind] = calibrator.result(sensitivity);
            DetectorConfig config;
            apply_calibration(calibration, config);
            const auto label = name + " at " + std::to_string(sensitivity).substr(0, 3);
            int doubles = 0, other = 0;
            for (const auto& event : replay_events(name, config))
                event.kind == kind && event.count == 2 ? ++doubles : ++other;
            const auto false_actions = replay_events("play", config).size() + replay_events("noise", config).size();
            const auto [right, wrong] = mixed_score(config, false);
            std::cout << "  " << label << ": spike " << calibration[kind]->peak << ", approach "
                      << calibration[kind]->approach << ", " << doubles << " doubles, mixed " << right << " right "
                      << wrong << " wrong\n";
            check(false_actions == 0, label + ": no actions in play and noise");
            check(wrong == 0, label + ": no wrong actions in mixed");
            const int least = sensitivity == 0 ? 15 : kind == KnockKind::Surface ? 17 : 19;
            check(doubles >= least, label + ": at least " + std::to_string(least) + " doubles");
        }
    }
}

// Calibration lowers the thresholds, so waving the controllers must not count.
// The knock recordings are doubles, so one knock of each pair counts.
void calibration_tests() {
    std::cout << "calibration\n";
    logged_calibration_test();
    int noise = 0;
    for (const auto* name : {"play", "noise"})
        for (int k = 0; k < 3; ++k) noise += calibration_count(name, static_cast<KnockKind>(k));
    check(noise == 0, "play and noise: no knocks counted in 90 s");
    check(calibration_count("together", KnockKind::Together) >= 10, "together: 10 knocks counted");
    check(calibration_count("surface", KnockKind::Surface) >= 10, "surface: 10 knocks counted");
    check(calibration_count("headset", KnockKind::Headset) >= 10, "headset: 10 knocks counted");
}

} // namespace

int main() {
    synthetic_tests();
    replay_tests();
    calibration_tests();
    calibrated_replay_tests();
    std::cout << (failures ? std::to_string(failures) + " failed\n" : "all passed\n");
    return failures ? 1 : 0;
}
