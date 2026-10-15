cmake_minimum_required(VERSION 3.16)
project(KnockThru VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "Build on 64-bit Linux. The Steam Frame build uses scripts/build-arm64.sh.")
endif()

include(cmake/OpenVRLoader.cmake)
find_package(Threads REQUIRED)

add_library(knockthru_core STATIC src/knock_detector.cpp src/config.cpp)
target_include_directories(knockthru_core PUBLIC src third_party/json)
target_compile_options(knockthru_core PRIVATE -Wall -Wextra -Wpedantic)

add_executable(knockthru src/main.cpp src/room_view.cpp src/style.cpp)
target_link_libraries(knockthru PRIVATE knockthru_core openvr_loader Threads::Threads)
target_compile_definitions(knockthru PRIVATE KNOCKTHRU_VERSION="${PROJECT_VERSION}")
target_compile_options(knockthru PRIVATE -Wall -Wextra -Wpedantic)
add_custom_command(TARGET knockthru POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/assets" "$<TARGET_FILE_DIR:knockthru>"
    COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:knockthru>" "$<TARGET_FILE_DIR:knockthru>/knockthru-ctl"
    COMMENT "Copying config, SteamVR action files, and the knockthru-ctl copy")

enable_testing()
add_executable(detector_test tests/detector_test.cpp)
target_link_libraries(detector_test PRIVATE knockthru_core)
target_compile_definitions(detector_test PRIVATE KNOCKTHRU_TEST_DATA="${CMAKE_CURRENT_SOURCE_DIR}/tests/data")
target_compile_options(detector_test PRIVATE -Wall -Wextra -Wpedantic)
add_test(NAME detector COMMAND detector_test)
