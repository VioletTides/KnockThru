#include "sound.hpp"

#include <dlfcn.h>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

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

struct Clip {
    uint32_t rate = 0;
    uint8_t channels = 0;
    std::vector<char> samples; // signed 16-bit little endian
};

uint32_t read_u32(const char* p) { uint32_t v; std::memcpy(&v, p, 4); return v; }
uint16_t read_u16(const char* p) { uint16_t v; std::memcpy(&v, p, 2); return v; }

// Reads a 16-bit PCM WAV file. Returns nothing for any other format.
std::optional<Clip> load_clip(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    const std::vector<char> file((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (file.size() < 12 || std::memcmp(file.data(), "RIFF", 4) || std::memcmp(file.data() + 8, "WAVE", 4)) return {};
    Clip clip;
    bool pcm16 = false;
    for (size_t at = 12; at + 8 <= file.size();) {
        const char* chunk = file.data() + at;
        const size_t size = read_u32(chunk + 4);
        if (at + 8 + size > file.size()) break;
        if (!std::memcmp(chunk, "fmt ", 4) && size >= 16) {
            pcm16 = read_u16(chunk + 8) == 1 && read_u16(chunk + 22) == 16;
            clip.channels = static_cast<uint8_t>(read_u16(chunk + 10));
            clip.rate = read_u32(chunk + 12);
        } else if (!std::memcmp(chunk, "data", 4)) {
            clip.samples.assign(chunk + 8, chunk + 8 + size);
        }
        at += 8 + size + (size & 1);
    }
    if (!pcm16 || clip.samples.empty()) return {};
    return clip;
}

// The parts of libpulse-simple the player uses, loaded at run time so the
// build needs no PulseAudio headers.
struct PulseSimple {
    struct SampleSpec { int format; uint32_t rate; uint8_t channels; };
    struct BufferAttr { uint32_t maxlength, tlength, prebuf, minreq, fragsize; };
    static constexpr int kS16LE = 3, kPlayback = 1;

    void* (*open)(const char*, const char*, int, const char*, const char*, const SampleSpec*, const void*,
                  const BufferAttr*, int*) = nullptr;
    int (*write)(void*, const void*, size_t, int*) = nullptr;
    void (*close)(void*) = nullptr;

    bool load() {
        void* library = ::dlopen("libpulse-simple.so.0", RTLD_NOW);
        if (!library) return false;
        open = reinterpret_cast<decltype(open)>(::dlsym(library, "pa_simple_new"));
        write = reinterpret_cast<decltype(write)>(::dlsym(library, "pa_simple_write"));
        close = reinterpret_cast<decltype(close)>(::dlsym(library, "pa_simple_free"));
        return open && write && close;
    }
};

class Player {
public:
    Player() { std::thread([this] { run(); }).detach(); }

    void prepare() {
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            wanted_ = true;
        }
        wake_.notify_one();
    }

    void play() {
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            wanted_ = true;
            if (pending_ < 2) ++pending_; // a burst of presses plays at most twice more
        }
        wake_.notify_one();
    }

private:
    static constexpr auto kIdleClose = std::chrono::seconds(10); // the delay Steam's UI uses

    void run() {
        PulseSimple pulse;
        const auto path = click_file();
        const auto clip = path.empty() ? std::nullopt : load_clip(path);
        if (!clip || !pulse.load()) return;
        const PulseSimple::SampleSpec spec{PulseSimple::kS16LE, clip->rate, clip->channels};
        // A 20 ms target buffer keeps the click close to the press.
        const uint32_t frame = 2u * clip->channels;
        const PulseSimple::BufferAttr attr{~0u, clip->rate / 50 * frame, ~0u, ~0u, ~0u};
        void* stream = nullptr;
        std::unique_lock<std::mutex> lock(mutex_);
        for (;;) {
            if (!stream) {
                wake_.wait(lock, [this] { return wanted_; });
                lock.unlock();
                stream = pulse.open(nullptr, "KnockThru", PulseSimple::kPlayback, nullptr, "Click", &spec, nullptr,
                                    &attr, nullptr);
                lock.lock();
                wanted_ = false;
                if (!stream) pending_ = 0; // no sound server. The next click tries again.
                continue;
            }
            if (!wake_.wait_for(lock, kIdleClose, [this] { return pending_ > 0 || wanted_; })) {
                pulse.close(stream);
                stream = nullptr;
                continue;
            }
            wanted_ = false;
            if (pending_ == 0) continue;
            --pending_;
            lock.unlock();
            const bool written = pulse.write(stream, clip->samples.data(), clip->samples.size(), nullptr) >= 0;
            if (!written) {
                pulse.close(stream); // the server restarted. Reopen for the next click.
                stream = nullptr;
            }
            lock.lock();
        }
    }

    std::mutex mutex_;
    std::condition_variable wake_;
    bool wanted_ = false; // a caller asked for an open stream
    int pending_ = 0;
};

// Never destroyed, because its thread runs until the process exits.
Player& player() {
    static Player* const instance = new Player;
    return *instance;
}

} // namespace

void prepare_click() { player().prepare(); }
void play_click() { player().play(); }

} // namespace knockthru
