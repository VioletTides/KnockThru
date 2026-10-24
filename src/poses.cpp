#include "poses.hpp"

#include <algorithm>
#include <cstdio>

namespace knockthru {

Vec3 position_of(const vr::TrackedDevicePose_t& pose) {
    const auto& m = pose.mDeviceToAbsoluteTracking.m;
    return {m[0][3], m[1][3], m[2][3]};
}

Vec3 velocity_of(const vr::TrackedDevicePose_t& pose) {
    const float* v = pose.vVelocity.v;
    return {v[0], v[1], v[2]};
}

void write_csv_row(
    std::ostream& out, double t, Device device, const Vec3& p, const Vec3& v, const float* angular) {
    char line[256];
    const int n = std::snprintf(line, sizeof line, "%.6f,%s,1,%.5f,%.5f,%.5f,%.5f,%.5f,%.5f,", t,
        kDeviceNames[static_cast<int>(device)], p[0], p[1], p[2], v[0], v[1], v[2]);
    if (angular)
        std::snprintf(line + n, sizeof line - n, "%.5f,%.5f,%.5f\n", angular[0], angular[1], angular[2]);
    else
        std::snprintf(line + n, sizeof line - n, "0,0,0\n");
    out << line;
}

} // namespace knockthru
