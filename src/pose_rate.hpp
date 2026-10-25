#pragma once

#include <chrono>

namespace knockthru {

// Picks the wait before the next pose read from what the last read returned.
// Every knock kind needs a controller spike, so with no controller tracked,
// such as in standby or with the controllers off, a slow read is enough. A
// knock also needs the controller to move first, so while both controllers
// stay still, a medium read is enough. The first read that sees a controller
// move switches to the fast rate, and the fast rate holds for `hold_s` after
// the last movement, which covers the rest of a knock sequence.
// The host tests replay the recordings through it, as the pose thread reads.
class PoseRate {
public:
    std::chrono::milliseconds fast{1};
    std::chrono::milliseconds still{10};
    std::chrono::milliseconds idle{50};
    double moving_speed = 0.1; // m/s, a controller faster than this is moving
    double hold_s = 0.5;

    // `controllers` is how many controllers returned a valid pose, and
    // `fastest` is the speed of the fastest one, in m/s.
    std::chrono::milliseconds next(double t, int controllers, double fastest) {
        if (!controllers) return idle;
        if (fastest > moving_speed) moved_t_ = t;
        return t - moved_t_ < hold_s ? fast : still;
    }

private:
    double moved_t_ = -1e9;
};

} // namespace knockthru
