// SPDX-License-Identifier: MIT
// MultiplayerEvolved: Core/Log.h
//
// Thread safe, allocation-light logging with a file sink and an optional
// debugger sink.
//
// Rationale: the Blam simulation calls into our code from its own simulation
// thread while Steam callbacks run on the thread that pumps SteamAPI. Log
// output is therefore mutex guarded. Formatting uses std::format, which is
// evaluated only when the level passes the filter.
#pragma once

#include <filesystem>
#include <format>
#include <string>
#include <string_view>

namespace mpe::log {

enum class Level : int {
    Trace = 0,
    Debug = 1,
    Info  = 2,
    Warn  = 3,
    Error = 4,
    Off   = 5,
};

[[nodiscard]] std::string_view ToString(Level level) noexcept;

/// Parses "trace" | "debug" | "info" | "warn" | "error" | "off".
/// Unrecognized input yields Level::Info so a typo in the config never silences
/// the log entirely.
[[nodiscard]] Level ParseLevel(std::string_view text) noexcept;

/// Opens the log file, truncating any previous run. Safe to call once per
/// process; subsequent calls are ignored. If the file cannot be opened the
/// debugger sink still receives output.
void Initialize(const std::filesystem::path& file, Level min_level);

/// Flushes and closes the sink. Called from the module unload path.
void Shutdown();

void SetMinLevel(Level level) noexcept;
[[nodiscard]] Level MinLevel() noexcept;

/// Writes one pre-formatted line. Prefer the macros below.
void Write(Level level, std::string_view category, std::string_view message);

/// Called for every record at or above a threshold, after it has been written.
///
/// For anything that needs to react to the fact that something was logged rather than to
/// the log itself. The one use is report sharing, which sends the log when something goes
/// wrong: waiting for a timer to come round means the report describing a failure arrives
/// after whatever the failure caused, which is the wrong way round for reading it.
///
/// Called on whichever thread wrote the record, with the sink's lock released, so a hook may
/// log. It must not block: it runs inline on threads that are counting down countdowns and
/// answering Steam.
using RecordHook = void (*)(Level level, std::string_view category, std::string_view message);

/// Installs the hook, or clears it with nullptr. Records below threshold are not delivered.
void SetRecordHook(RecordHook hook, Level threshold) noexcept;

/// Internal: used by the macros so std::format runs only when enabled.
template <typename... Args>
void WriteFormatted(Level level, std::string_view category,
                    std::format_string<Args...> fmt, Args&&... args) {
    if (level < MinLevel()) {
        return;
    }
    Write(level, category, std::format(fmt, std::forward<Args>(args)...));
}

} // namespace mpe::log

// Each translation unit defines MPE_LOG_CATEGORY before including this header,
// keeping call sites free of a repeated literal.
#ifndef MPE_LOG_CATEGORY
#define MPE_LOG_CATEGORY "MultiplayerEvolved"
#endif

#define MPE_LOG_TRACE(...) \
    ::mpe::log::WriteFormatted(::mpe::log::Level::Trace, MPE_LOG_CATEGORY, __VA_ARGS__)
#define MPE_LOG_DEBUG(...) \
    ::mpe::log::WriteFormatted(::mpe::log::Level::Debug, MPE_LOG_CATEGORY, __VA_ARGS__)
#define MPE_LOG_INFO(...) \
    ::mpe::log::WriteFormatted(::mpe::log::Level::Info,  MPE_LOG_CATEGORY, __VA_ARGS__)
#define MPE_LOG_WARN(...) \
    ::mpe::log::WriteFormatted(::mpe::log::Level::Warn,  MPE_LOG_CATEGORY, __VA_ARGS__)
#define MPE_LOG_ERROR(...) \
    ::mpe::log::WriteFormatted(::mpe::log::Level::Error, MPE_LOG_CATEGORY, __VA_ARGS__)
