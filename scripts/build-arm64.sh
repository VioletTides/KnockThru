#!/usr/bin/env bash
# Cross-compiles knockthru for the Steam Frame (Linux ARM64) with Zig.
# glibc stays dynamic for the SteamVR loader. Zig links libc++ statically.
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
build="${1:-$root/build-arm64}"
version="${KNOCKTHRU_VERSION:-0.1.0}"
mkdir -p "$build/bin"
export ZIG_GLOBAL_CACHE_DIR="$build/zig-global-cache"
export ZIG_LOCAL_CACHE_DIR="$build/zig-local-cache"

sources=(
    src/main.cpp
    src/knock_detector.cpp
    src/room_view.cpp
    src/style.cpp
    src/sound.cpp
    src/config.cpp
    src/calibrator.cpp
    src/update.cpp
    src/report.cpp
    third_party/qrcodegen/qrcodegen.cpp
    src/canvas.cpp
    src/panel.cpp
    src/overlay_texture.cpp
    third_party/openvr/src/openvr_api_public.cpp
    third_party/openvr/src/jsoncpp.cpp
    third_party/openvr/src/vrcore/dirtools_public.cpp
    third_party/openvr/src/vrcore/envvartools_public.cpp
    third_party/openvr/src/vrcore/pathtools_public.cpp
    third_party/openvr/src/vrcore/sharedlibtools_public.cpp
    third_party/openvr/src/vrcore/hmderrors_public.cpp
    third_party/openvr/src/vrcore/vrpathregistry_public.cpp
    third_party/openvr/src/vrcore/strtools_public.cpp
)
includes=(src third_party/json third_party/stb third_party/qrcodegen third_party/vulkan/include third_party/openvr/headers third_party/openvr/src third_party/openvr/src/vrcore)

cd "$root"
zig c++ -target aarch64-linux-gnu.2.31 -std=c++17 -O2 -s -fvisibility=hidden -Wno-deprecated-declarations \
    -DLINUX -DPOSIX -DLINUXARM64 -DVR_API_PUBLIC -DVRCORE_NO_PLATFORM -DOPENVR_BUILD_STATIC \
    -DKNOCKTHRU_VERSION="\"$version\"" \
    "${includes[@]/#/-I}" "${sources[@]}" -ldl -lpthread -o "$build/bin/knockthru"

cp "$build/bin/knockthru" "$build/bin/knockthru-ctl"
cp -r assets/. "$build/bin/"
echo "ARM64 build ready: $build/bin/knockthru"
