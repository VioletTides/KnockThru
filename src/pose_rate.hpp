#pragma once

#include <chrono>

namespace knockthru {

// Picks the wait before the next pose read from what the last read returned.
// Every knock kind needs a controller spike, so with no controller tracked,
// such as in standby or with the controllers off, a slow read is enough.
// The host tests replay the recordings through it, as the pose thread reads.
class PoseRate {
public:
    static constexpr std::chrono::milliseconds kFast{1};
    static constexpr std::chrono::milliseconds kIdle{50};

    // `controllers` is how many controllers returned a valid pose.
    std::chrono::milliseconds next(int controllers) const { return controllers ? kFast : kIdle; }
};

} // namespace knockthru
