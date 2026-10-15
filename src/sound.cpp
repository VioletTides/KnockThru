#include "sound.hpp"

#include <fcntl.h>
#include <spawn.h>
#include <unistd.h>
#include <sys/wait.h>

#include <cstdlib>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

extern char** environ;

namespace knockthru {
namespace {

namespace fs = std::filesystem;

constexpr const char* kClick = "deck_ui_misc_10.wav";

// SteamVR's copy of the click, or Steam's copy of the same file.
fs::path click_file() {
    std::vector<fs::path> candidates = {"/opt/steamvr/resources/webinterface/dashboard/sounds"};
    if (const char* home = std::getenv("HOME"))
        for (const char* steam : {".local/share/Steam", ".steam/steam"})
            candidates.push_back(fs::path(home) / steam / "steamui" / "sounds");
    for (const auto& directory : candidates) {
        std::error_code error;
        if (fs::is_regular_file(directory / kClick, error)) return directory / kClick;
    }
    return {};
}

} // namespace

void play_click() {
    static const std::string path = click_file().string();
    if (path.empty()) return;

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
    char player[] = "pw-play";
    char* argv[] = {player, const_cast<char*>(path.c_str()), nullptr};
    pid_t pid = 0;
    const int error = posix_spawnp(&pid, player, &actions, nullptr, argv, environ);
    posix_spawn_file_actions_destroy(&actions);
    // Reaps the player when it ends, so it does not linger as a zombie.
    if (error == 0) std::thread([pid] { ::waitpid(pid, nullptr, 0); }).detach();
}

} // namespace knockthru
