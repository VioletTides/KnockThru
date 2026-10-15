// Detector tests: synthetic sequences, then replays of the Steam Frame
// recordings in tests/data/ (see docs/knock-signal.md).
#include "knock_detector.hpp"

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
        {"partner_peak", &config.partner_peak}, {"headset_peak", &config.headset_peak},
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

std::vector<KnockEvent> replay_events(const std::string& name) {
    const auto rows = load(std::string(KNOCKTHRU_TEST_DATA) + "/" + name + ".csv.gz");
    KnockDetector detector(test_config());
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
void mixed_test() {
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
    for (const auto& event : replay_events("mixed")) {
        const Expected* match = nullptr;
        for (const auto& e : expected)
            if (event.t >= e.start && event.t <= e.start + 1.0) match = &e;
        const bool ok = match && match->kind == event.kind && match->count == event.count;
        ok ? ++right : ++wrong;
        if (!ok || std::getenv("KNOCKTHRU_TEST_VERBOSE"))
            std::cout << "    " << (ok ? "right" : "WRONG") << " " << event.t << " " << to_string(event.kind)
                      << "x" << event.count << "\n";
    }
    std::cout << "  mixed: " << right << " of " << expected.size() << " right, " << wrong << " wrong\n";
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

} // namespace

int main() {
    synthetic_tests();
    replay_tests();
    std::cout << (failures ? std::to_string(failures) + " failed\n" : "all passed\n");
    return failures ? 1 : 0;
}
