#include "sound.hpp"

#include <fcntl.h>
#include <spawn.h>
#include <unistd.h>
#include <sys/wait.h>

#include <cstdlib>
#include <filesystem>
#include <string>
#include <thread>

extern char** environ;

namespace knockthru {
namespace {

namespace fs = std::filesystem;

const char* file_name(Sound sound) {
    switch (sound) {
    case Sound::SwitchOn: return "deck_ui_switch_toggle_on.wav";
    case Sound::SwitchOff: return "deck_ui_switch_toggle_off.wav";
    case Sound::Button: return "deck_ui_default_activation.wav";
    }
    return "";
}

// The Steam UI sounds folder, or an empty path if Steam is not where SteamOS puts it.
fs::path sounds_directory() {
    const char* home = std::getenv("HOME");
    if (!home) return {};
    for (const char* steam : {".local/share/Steam", ".steam/steam"}) {
        const auto directory = fs::path(home) / steam / "steamui" / "sounds";
        std::error_code error;
        if (fs::is_directory(directory, error)) return directory;
    }
    return {};
}

} // namespace

void play(Sound sound) {
    static const fs::path directory = sounds_directory();
    if (directory.empty()) return;
    const std::string path = (directory / file_name(sound)).string();

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
