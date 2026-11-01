#pragma once

#include "knock_actions.hpp"
#include "knock_detector.hpp"

#include <array>
#include <deque>
#include <string>
#include <vector>

namespace knockthru {

// One line of the feed, in the words the debug view shows.
struct FeedEntry {
    double t; // detector clock, seconds
    std::string title; // "Right controller", "Surface knock twice"
    std::string verdict; // "Too light", "Passthrough"
    std::string detail; // "52 m/s² (min 60)", or empty
    bool good; // an action ran
};

// What the app did with a finished sequence that the detector called an Action.
enum class SetOutcome { Ran, Paused, Blocked, NoAction };

// Everything the debug view shows, fed once per tick. It holds no OpenVR
// types, so the host tests cover it.
class DebugModel {
public:
    static constexpr double kKeepSeconds = 6; // pose history, the graph's 5 s with slack
    static constexpr double kBucketSeconds = 0.01; // graph resolution
    static constexpr std::size_t kFeedSize = 4; // the last knock and the three rows under it

    // Samples whose velocity equals the device's last one are repeats and are dropped. So are samples
    // with a time or velocity that is not a number.
    void push(const PoseSample& sample);
    // Adds the near misses to the feed.
    void add_candidate(const Candidate& candidate);
    void add_set(const Sequence& sequence, SetOutcome outcome, KnockAction action);

    // The largest value in each 10 ms bucket over the last `seconds`, oldest bucket first. The bucket
    // `now` falls in is left out, because it is still filling. Acceleration is the detector's: velocity
    // change over the time between changed samples.
    std::vector<float> accel(Device device, double now, double seconds) const;
    double pose_rate(Device device, double now) const; // changed samples in the last second

    const std::deque<FeedEntry>& feed() const { return feed_; } // newest last

private:
    struct Entry {
        Device device;
        double t;
        float accel; // m/s^2 since the device's previous entry
    };

    void add_feed(FeedEntry entry);

    std::deque<Entry> ring_; // oldest first
    std::array<double, 3> last_t_{{-1, -1, -1}};
    std::array<Vec3, 3> last_velocity_{};
    std::deque<FeedEntry> feed_;
};

// `value` with a fixed number of decimals, for the debug view's text.
std::string number(double value, int decimals);

} // namespace knockthru
