# Valve's portable OpenVR loader, built from source. Prebuilt SDK binaries are
# x86-only in some SDK releases and cannot run on the Steam Frame ARM64 CPU.
set(openvr_dir "${CMAKE_CURRENT_SOURCE_DIR}/third_party/openvr")
set(openvr_sources
    src/openvr_api_public.cpp
    src/jsoncpp.cpp
    src/vrcore/dirtools_public.cpp
    src/vrcore/envvartools_public.cpp
    src/vrcore/pathtools_public.cpp
    src/vrcore/sharedlibtools_public.cpp
    src/vrcore/hmderrors_public.cpp
    src/vrcore/vrpathregistry_public.cpp
    src/vrcore/strtools_public.cpp)
list(TRANSFORM openvr_sources PREPEND "${openvr_dir}/")
add_library(openvr_loader STATIC ${openvr_sources})
target_include_directories(openvr_loader SYSTEM PUBLIC "${openvr_dir}/headers")
target_include_directories(openvr_loader PRIVATE "${openvr_dir}/src" "${openvr_dir}/src/vrcore")
target_compile_definitions(openvr_loader PRIVATE VR_API_PUBLIC VRCORE_NO_PLATFORM)
target_compile_definitions(openvr_loader PUBLIC OPENVR_BUILD_STATIC)
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64)$")
    target_compile_definitions(openvr_loader PRIVATE LINUX POSIX LINUXARM64)
else()
    target_compile_definitions(openvr_loader PRIVATE LINUX POSIX LINUX64)
endif()
target_link_libraries(openvr_loader PRIVATE ${CMAKE_DL_LIBS})
