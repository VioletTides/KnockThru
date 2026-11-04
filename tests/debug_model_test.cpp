// Debug model tests: graph series, the feed rules, CPU use, and battery power.
#include "check.hpp"
#include "debug_model.hpp"
#include "usage.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include <unistd.h>

using namespace knockthru;

namespace {

Knock knock(KnockKind kind, int source, double t, double peak = 80) {
    Knock k{};
    k.t = t;
    k.kind = kind;
    k.source = source;
    k.peak = peak;
    return k;
}

Candidate spike(
    Verdict verdict, double peak = 80, int source = static_cast<int>(Device::Right), double t = 5) {
    Candidate c{knock(KnockKind::Surface, source, t, peak), verdict, 60};
    c.speed_30ms_before = 2.4;
    c.speed_30ms_after = 0.834;
    c.before_limit = 1.8;
    c.after_limit = 0.55;
    return c;
}

Sequence set(SequenceVerdict verdict, int count, KnockKind kind = KnockKind::Surface) {
    Sequence s;
    for (int i = 0; i < count; ++i) s.knocks.push_back(knock(kind, 2, 5 + 0.25 * i));
    s.verdict = verdict;
    s.fastest_approach = 0.21;
    s.needed_approach = 0.3;
    return s;
}

// Feeds changing velocities, so every sample counts. Speed is `speed` m/s along x.
void feed(DebugModel& model, Device device, double t, double speed) {
    model.push({device, t, {0, 0, 0}, {speed, 0, 0}});
}

void graph_tests() {
    std::cout << "graph\n";
    DebugModel model;
    // Still at 1 m/s from 0 to 1 s, one 2 ms spike to 5 m/s at 0.5 s, 1 ms samples.
    for (int i = 0; i <= 1000; ++i) {
        const double t = i * 0.001;
        const double v = (i == 500 || i == 501) ? 5 : (i % 2 ? 1.0 : 1.001);
        feed(model, Device::Right, t, v);
    }
    const auto accel = model.accel(Device::Right, 1.0, 1.0);
    check(accel.size() == 100, "one second is 100 buckets");
    check(*std::max_element(accel.begin(), accel.end()) > 3000, "acceleration is velocity change over time");
    check(model.accel(Device::Left, 1.0, 1.0) == std::vector<float>(100, 0.0f),
        "a device with no samples is flat");

    DebugModel repeats;
    for (int i = 0; i < 50; ++i) feed(repeats, Device::Left, i * 0.001, 1.0);
    check(repeats.pose_rate(Device::Left, 0.05) == 1, "pose rate counts changed samples");

    DebugModel rate;
    for (int i = 0; i < 300; ++i) feed(rate, Device::Right, i * 0.01, 1 + i);
    check(rate.pose_rate(Device::Right, 3.0) == 99 || rate.pose_rate(Device::Right, 3.0) == 100,
        "pose rate is the samples in the last second");
}

// The bucket `now` falls in is still filling. Drawn, it would pull the graph's right edge down.
void filling_bucket_tests() {
    std::cout << "filling bucket\n";
    DebugModel model;
    // 2 m/s^2 for 1 s, 1 ms samples, then one sample at 1.003 s that changes velocity 1000 m/s in 1 ms.
    for (int i = 0; i <= 1002; ++i) feed(model, Device::Right, i * 0.001, 0.002 * i);
    feed(model, Device::Right, 1.003, 1000);
    const auto accel = model.accel(Device::Right, 1.0035, 1.0);
    check(accel.size() == 100, "one second is 100 buckets");
    check(accel.back() > 1 && accel.back() < 3, "the newest bucket is the last full one");
    check(*std::max_element(accel.begin(), accel.end()) < 3, "a sample in the filling bucket stays out");
    check(std::all_of(accel.begin(), accel.end(), [](float value) { return value > 1; }),
        "full buckets with samples are not empty");
}

// A time or velocity that is not a number never reaches a bucket, and the next real sample still counts.
void non_finite_tests() {
    std::cout << "non-finite samples\n";
    const double nan = std::nan(""), inf = HUGE_VAL;
    DebugModel model;
    feed(model, Device::Right, 0.0, 1);
    feed(model, Device::Right, 0.1, 2);
    feed(model, Device::Right, nan, 3);
    feed(model, Device::Right, 0.2, nan);
    feed(model, Device::Right, 0.3, inf);
    feed(model, Device::Right, inf, 4);
    feed(model, Device::Right, 0.4, 5);
    check(model.pose_rate(Device::Right, 0.5) == 3, "samples that are not numbers are dropped");
    const auto accel = model.accel(Device::Right, 0.5, 0.5);
    check(accel.size() == 50
            && std::all_of(accel.begin(), accel.end(), [](float v) { return std::isfinite(v); }),
        "every bucket is a number");
    check(accel[40] > 9 && accel[40] < 11, "the sample after them measures against the last real one");
}

void ring_tests() {
    std::cout << "ring\n";
    DebugModel model;
    for (int i = 0; i < 30000; ++i) {
        feed(model, Device::Left, i * 0.001, 1 + i);
        feed(model, Device::Right, i * 0.001, 1 + i);
    }
    // 30 s in, the ring holds the graph's 5 s with slack, so the 5 s window is full and older motion is gone.
    const auto recent = model.accel(Device::Left, 30, 5);
    check(std::all_of(recent.begin(), recent.end(), [](float v) { return v > 0; }),
        "the last 5 s are all there");
    const auto old = model.accel(Device::Left, 30, DebugModel::kKeepSeconds + 3);
    check(std::all_of(old.begin(), old.begin() + 100, [](float v) { return v == 0; }),
        "the ring drops samples older than its span");
}

void feed_tests() {
    std::cout << "feed\n";
    DebugModel model;
    model.add_candidate(spike(Verdict::BelowPeak, 44));
    check(model.feed().empty(), "a spike under 75% of its threshold is skipped");
    model.add_candidate(spike(Verdict::BelowPeak, 45));
    check(model.feed().size() == 1, "a spike at 75% of its threshold is a near miss");
    check(model.feed().back().title == "Right controller", "a right spike is titled Right controller");
    check(model.feed().back().verdict == "Too light", "BelowPeak reads Too light");
    check(model.feed().back().detail == "45 m/s² (min 60)", "BelowPeak detail has the peak and the minimum");
    check(!model.feed().back().good, "a near miss is not good");

    model.add_candidate(spike(Verdict::FastBefore, 80, static_cast<int>(Device::Left)));
    check(model.feed().back().title == "Left controller" && model.feed().back().verdict == "Came in too fast",
        "FastBefore on the left controller");
    check(model.feed().back().detail == "2.40 m/s before (max 1.80)", "FastBefore detail");
    model.add_candidate(spike(Verdict::MovingAfter, 80, -1));
    check(model.feed().back().title == "Both controllers" && model.feed().back().verdict == "Kept moving",
        "MovingAfter on both controllers");
    check(model.feed().back().detail == "0.83 m/s after (max 0.55)", "MovingAfter detail");
    model.add_candidate(spike(Verdict::Untracked));
    check(model.feed().back().verdict == "Not tracked" && model.feed().back().detail.empty(),
        "Untracked has no detail");

    for (const auto verdict : {Verdict::Counted, Verdict::Bounce, Verdict::Rebound})
        model.add_candidate(spike(verdict, 80, static_cast<int>(Device::Right), 99));
    check(model.feed().back().t != 99, "Counted, Bounce, and Rebound spikes stay out of the feed");

    model.add_set(set(SequenceVerdict::Action, 2), SetOutcome::Ran, KnockAction::Passthrough);
    check(model.feed().back().title == "Surface knock twice" && model.feed().back().verdict == "Passthrough"
            && model.feed().back().good,
        "an action set that ran is good");
    check(model.feed().back().t == 5.25, "a set is stamped with its last knock");
    model.add_set(set(SequenceVerdict::Action, 3, KnockKind::Together), SetOutcome::Ran, KnockAction::Style);
    check(model.feed().back().title == "Together knock thrice" && model.feed().back().verdict == "Style",
        "a together triple that switched the style");
    model.add_set(
        set(SequenceVerdict::Action, 2, KnockKind::Headset), SetOutcome::Ran, KnockAction::PerfGraph);
    check(model.feed().back().title == "Headset knock twice"
            && model.feed().back().verdict == "Performance graph",
        "a headset double that opened the perf graph");

    model.add_set(set(SequenceVerdict::Action, 2), SetOutcome::Failed, KnockAction::Passthrough);
    check(model.feed().back().verdict == "Failed" && !model.feed().back().good, "an action that did not run");
    model.add_set(set(SequenceVerdict::Action, 2), SetOutcome::Paused, KnockAction::Passthrough);
    check(model.feed().back().verdict == "Paused" && !model.feed().back().good, "a paused action set");
    model.add_set(set(SequenceVerdict::Action, 2), SetOutcome::Blocked, KnockAction::Passthrough);
    check(model.feed().back().verdict == "Off in this game", "a blocked action set");
    model.add_set(set(SequenceVerdict::Action, 2), SetOutcome::NoAction, KnockAction::None);
    check(model.feed().back().verdict == "No action", "an action set with no action");

    model.add_set(set(SequenceVerdict::WrongCount, 1), SetOutcome::NoAction, KnockAction::None);
    check(model.feed().back().title == "Surface knock" && model.feed().back().verdict == "Wrong count",
        "one knock has no count word");
    model.add_set(set(SequenceVerdict::WrongCount, 4), SetOutcome::NoAction, KnockAction::None);
    check(model.feed().back().title == "Surface knock, 4 times", "four knocks are counted in digits");

    const std::pair<SequenceVerdict, const char*> words[] = {{SequenceVerdict::NotQuiet, "Too soon"},
        {SequenceVerdict::MixedKinds, "Mixed kinds"}, {SequenceVerdict::MixedSources, "Mixed hands"},
        {SequenceVerdict::MixedSpots, "Moved between knocks"}, {SequenceVerdict::TooSlow, "Too slow"},
        {SequenceVerdict::KindOff, "Kind is off"}};
    for (const auto& [verdict, word] : words) {
        model.add_set(set(verdict, 2), SetOutcome::NoAction, KnockAction::None);
        check(model.feed().back().verdict == word, std::string("sequence verdict reads ") + word);
    }
    check(model.feed().back().detail.empty(), "KindOff has no detail");
    model.add_set(set(SequenceVerdict::TooSlow, 2), SetOutcome::NoAction, KnockAction::None);
    check(model.feed().back().detail == "0.21 m/s approach (min 0.30)", "TooSlow detail");
    model.add_set(set(SequenceVerdict::NotQuiet, 2), SetOutcome::NoAction, KnockAction::None);
    check(model.feed().back().detail.empty(), "NotQuiet has no detail");

    DebugModel full;
    for (int i = 0; i < 10; ++i)
        full.add_candidate(spike(Verdict::Untracked, 80, static_cast<int>(Device::Right), i));
    check(full.feed().size() == 4, "the feed holds the last knock and three rows");
    check(full.feed().front().t == 6 && full.feed().back().t == 9, "the feed drops the oldest first");
}

void usage_tests() {
    std::cout << "usage\n";
    CpuUsage usage;
    check(usage.sample() == 0, "the first call returns 0");
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(300);
    volatile double sink = 0;
    while (std::chrono::steady_clock::now() < until) sink = sink + std::sqrt(2.0);
    const double percent = usage.sample();
    check(percent > 10 && percent <= 110, "a busy loop shows as CPU use, got " + std::to_string(percent));

    // A fake gauge: -500 mA at 3.8 V is 1900 mW.
    namespace fs = std::filesystem;
    const auto supplies = fs::temp_directory_path() / ("knockthru-usage-test-" + std::to_string(getpid()));
    const auto battery = supplies / "max1720x_bat_7-36", charger = supplies / "charger";
    fs::create_directories(battery);
    fs::create_directories(charger);
    const auto write = [](const fs::path& path, const char* text) { std::ofstream(path) << text << "\n"; };
    write(battery / "status", "Discharging");
    write(battery / "current_now", "-500000");
    write(battery / "voltage_now", "3800000");
    write(charger / "online", "0");
    const auto power = battery_mw(supplies);
    check(power && std::abs(*power - 1900) < 1e-6, "battery power in mW from the gauge");
    write(charger / "online", "1");
    check(!battery_mw(supplies), "no battery power while a charger is online");
    write(charger / "online", "0");
    write(battery / "status", "Charging");
    check(!battery_mw(supplies), "no battery power while charging");
    check(!battery_mw(supplies / "missing"), "no battery power without a gauge");
    fs::remove_all(supplies);
}

} // namespace

int main() {
    graph_tests();
    filling_bucket_tests();
    non_finite_tests();
    ring_tests();
    feed_tests();
    usage_tests();
    return finish();
}
