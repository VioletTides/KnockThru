#include "perf_overlay.hpp"

#include "steamvr.hpp"

#include <openvr.h>

#include <stdexcept>
#include <string>

namespace knockthru {
namespace {

void check(vr::EVRSettingsError error) {
    if (error != vr::VRSettingsError_None)
        throw std::runtime_error(std::string("perfcheck.perfGraphInHMD: ")
            + vr::VRSettings()->GetSettingsErrorNameFromEnum(error));
}

} // namespace

bool read_perf_overlay() {
    vr::EVRSettingsError error = vr::VRSettingsError_None;
    const bool shown =
        vr::VRSettings()->GetBool(vr::k_pch_Perf_Section, vr::k_pch_Perf_PerfGraphInHMD_Bool, &error);
    check(error);
    return shown;
}

bool toggle_perf_overlay() {
    const bool target = !read_perf_overlay();
    set_setting(
        "perfcheck.perfGraphInHMD", target, read_perf_overlay, [](bool shown, vr::EVRSettingsError* error) {
            vr::VRSettings()->SetBool(
                vr::k_pch_Perf_Section, vr::k_pch_Perf_PerfGraphInHMD_Bool, shown, error);
        });
    return target;
}

} // namespace knockthru
