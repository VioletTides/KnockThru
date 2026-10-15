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

add_library(knockthru_core STATIC src/knock_detector.cpp src/config.cpp src/calibrator.cpp)
target_include_directories(knockthru_core PUBLIC src third_party/json)
target_compile_options(knockthru_core PRIVATE -Wall -Wextra -Wpedantic)

add_library(knockthru_ui STATIC src/canvas.cpp src/panel.cpp)
target_include_directories(knockthru_ui PUBLIC src)
target_include_directories(knockthru_ui SYSTEM PRIVATE third_party/stb)
target_link_libraries(knockthru_ui PUBLIC knockthru_core)
target_compile_options(knockthru_ui PRIVATE -Wall -Wextra -Wpedantic)

add_executable(knockthru src/main.cpp src/room_view.cpp src/style.cpp src/overlay_texture.cpp)
target_include_directories(knockthru SYSTEM PRIVATE third_party/vulkan/include)
target_link_libraries(knockthru PRIVATE knockthru_core knockthru_ui openvr_loader Threads::Threads ${CMAKE_DL_LIBS})
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

# Renders the dashboard panel in sample states to PNG files, for review.
add_executable(panel_preview tests/panel_preview.cpp)
target_link_libraries(panel_preview PRIVATE knockthru_ui)
target_compile_definitions(panel_preview PRIVATE KNOCKTHRU_FONTS="${CMAKE_CURRENT_SOURCE_DIR}/assets/fonts")
