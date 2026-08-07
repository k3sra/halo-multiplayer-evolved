// SPDX-License-Identifier: MIT
// MultiplayerEvolved: Core/Log.cpp
#include "Core/Log.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <share.h> // _SH_DENYWR

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

namespace mpe::log {
namespace {

std::mutex g_mutex;

/// Notified for every record at or above its threshold. See Log.h for why.
RecordHook g_record_hook{nullptr};
Level      g_record_hook_threshold{Level::Error};
std::FILE*        g_file = nullptr;
std::atomic<Level> g_min_level{Level::Info};
std::atomic<bool>  g_initialized{false};

/// Local wall clock with millisecond precision. Matches the timestamp format
/// used by the engine log so the two can be interleaved during triage.
std::string Timestamp() {
    using namespace std::chrono;
    const auto now  = system_clock::now();
    const auto ms   = duration_cast<milliseconds>(now.time_since_epoch()) % 1000;
    const auto time = system_clock::to_time_t(now);

    std::tm tm{};
    // localtime_s is the thread safe variant on MSVC.
    if (localtime_s(&tm, &time) != 0) {
        return "0000-00-00 00:00:00.000";
    }

    char buffer[32];
    const int written = std::snprintf(
        buffer, sizeof(buffer), "%04d-%02d-%02d %02d:%02d:%02d.%03d",
        tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
        tm.tm_hour, tm.tm_min, tm.tm_sec, static_cast<int>(ms.count()));

    return written > 0 ? std::string(buffer, static_cast<size_t>(written))
                       : std::string("0000-00-00 00:00:00.000");
}

} // namespace

std::string_view ToString(Level level) noexcept {
    switch (level) {
        case Level::Trace: return "TRACE";
        case Level::Debug: return "DEBUG";
        case Level::Info:  return "INFO ";
        case Level::Warn:  return "WARN ";
        case Level::Error: return "ERROR";
        case Level::Off:   return "OFF  ";
    }
    return "?????";
}

Level ParseLevel(std::string_view text) noexcept {
    if (text == "trace") return Level::Trace;
    if (text == "debug") return Level::Debug;
    if (text == "info")  return Level::Info;
    if (text == "warn")  return Level::Warn;
    if (text == "error") return Level::Error;
    if (text == "off")   return Level::Off;
    return Level::Info;
}

void Initialize(const std::filesystem::path& file, Level min_level) {
    std::lock_guard lock(g_mutex);
    if (g_initialized.load(std::memory_order_acquire)) {
        return;
    }

    g_min_level.store(min_level, std::memory_order_release);

    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    // A failure to create the directory is non fatal: the debugger sink remains.

    // Binary mode, not "w, ccs=UTF-8".
    //
    // A ccs= mode puts the stream into wide orientation. Writing narrow bytes into
    // a wide oriented stream with fwrite is undefined, and in practice the CRT
    // invokes its invalid parameter handler, which calls __fastfail and terminates
    // the process with STATUS_STACK_BUFFER_OVERRUN. Inside the game that is an
    // instant crash on the third log line, with no diagnostic.
    //
    // Everything written here is already UTF-8, so the correct mode is binary: no
    // orientation, no code page conversion, no CRLF translation.
    //
    // _wfsopen with _SH_DENYWR rather than _wfopen_s, so the file stays readable
    // while the game holds it open. fopen denies all sharing on Windows, which made
    // the log unreadable until the game exited: exactly when a user trying to
    // diagnose a hang or a failed join most needs to see it.
    // The previous run is kept before this one overwrites it.
    //
    // Opening "wb" truncates, so every launch destroyed the log of the launch before it.
    // That is exactly backwards for the problems worth reporting: a player who has just
    // seen a join fail restarts the game to try again, and the restart is what deletes the
    // record of the failure. One generation back is enough, because the run being asked
    // about is almost always the one immediately before the restart.
    std::error_code rotate_error;
    std::filesystem::path previous = file;
    previous.replace_extension();
    previous += ".previous.log";
    if (std::filesystem::exists(file, rotate_error)) {
        std::filesystem::remove(previous, rotate_error);
        std::filesystem::rename(file, previous, rotate_error);
        // A failure here is not worth refusing to log over: the worst case is the old
        // behaviour, which is that the previous run is lost.
    }

    g_file = _wfsopen(file.c_str(), L"wb", _SH_DENYWR);

    if (g_file != nullptr) {
        // UTF-8 byte order mark, so editors and PowerShell detect the encoding
        // rather than guessing at the system code page.
        static constexpr unsigned char kUtf8Bom[] = {0xEF, 0xBB, 0xBF};
        std::fwrite(kUtf8Bom, 1, sizeof(kUtf8Bom), g_file);
    }

    g_initialized.store(true, std::memory_order_release);
}

void Shutdown() {
    std::lock_guard lock(g_mutex);
    if (g_file != nullptr) {
        std::fflush(g_file);
        std::fclose(g_file);
        g_file = nullptr;
    }
    g_initialized.store(false, std::memory_order_release);
}

void SetMinLevel(Level level) noexcept {
    g_min_level.store(level, std::memory_order_release);
}

Level MinLevel() noexcept {
    return g_min_level.load(std::memory_order_acquire);
}

void SetRecordHook(RecordHook hook, Level threshold) noexcept {
    std::lock_guard lock(g_mutex);
    g_record_hook           = hook;
    g_record_hook_threshold = threshold;
}

void Write(Level level, std::string_view category, std::string_view message) {
    if (level < MinLevel()) {
        return;
    }

    const std::string line = std::format("[{}] [{}] [{}] {}\n", Timestamp(),
                                         ToString(level), category, message);

    RecordHook hook = nullptr;
    {
        std::lock_guard lock(g_mutex);
        if (g_file != nullptr) {
            std::fwrite(line.data(), 1, line.size(), g_file);
            // Flushed per line: a crash during engine bring up must not lose the
            // last entry, which is usually the one that explains the crash.
            std::fflush(g_file);
        }
        if (::IsDebuggerPresent() != FALSE) {
            ::OutputDebugStringA(line.c_str());
        }
        if (g_record_hook != nullptr && level >= g_record_hook_threshold) {
            hook = g_record_hook;
        }
    }

    // Called with the lock released, so a hook is free to log without deadlocking against
    // the write that invoked it.
    if (hook != nullptr) {
        hook(level, category, message);
    }
}

} // namespace mpe::log
