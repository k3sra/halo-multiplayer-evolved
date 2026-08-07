// SPDX-License-Identifier: MIT
// MultiplayerEvolved: Engine/IEngineControl.cpp
//
// Enum plumbing and settings validation. Kept out of the header so the
// interface stays readable.
#include "Engine/IEngineControl.h"

#include <format>

namespace mpe::engine {

std::string_view ToString(GameMode mode) noexcept {
    switch (mode) {
        case GameMode::Slayer:         return "slayer";
        case GameMode::TeamSlayer:     return "team_slayer";
        case GameMode::CaptureTheFlag: return "capture_the_flag";
        case GameMode::Oddball:        return "oddball";
        case GameMode::KingOfTheHill:  return "king_of_the_hill";
        case GameMode::Territories:    return "territories";
        case GameMode::Juggernaut:     return "juggernaut";
        case GameMode::Infection:      return "infection";
    }
    return "unknown";
}

std::string_view ToString(SessionClass value) noexcept {
    switch (value) {
        case SessionClass::None:            return "none";
        case SessionClass::Offline:         return "offline";
        case SessionClass::SystemLink:      return "system_link";
        case SessionClass::PlatformService: return "platform_service";
    }
    return "unknown";
}

std::string_view ToString(SessionPrivacy value) noexcept {
    switch (value) {
        case SessionPrivacy::Open:           return "open";
        case SessionPrivacy::FriendsOnly:    return "friends_only";
        case SessionPrivacy::InvitationOnly: return "invitation_only";
    }
    return "unknown";
}

std::string_view ToString(EngineLifecycle value) noexcept {
    switch (value) {
        case EngineLifecycle::Uninitialized: return "uninitialized";
        case EngineLifecycle::Idle:          return "idle";
        case EngineLifecycle::Loading:       return "loading";
        case EngineLifecycle::InMatch:       return "in_match";
        case EngineLifecycle::PostMatch:     return "post_match";
        case EngineLifecycle::Faulted:       return "faulted";
    }
    return "unknown";
}

bool ParseGameMode(std::string_view text, GameMode& out_mode) noexcept {
    if (text == "slayer")           { out_mode = GameMode::Slayer;         return true; }
    if (text == "team_slayer")      { out_mode = GameMode::TeamSlayer;     return true; }
    if (text == "capture_the_flag") { out_mode = GameMode::CaptureTheFlag; return true; }
    if (text == "oddball")          { out_mode = GameMode::Oddball;        return true; }
    if (text == "king_of_the_hill") { out_mode = GameMode::KingOfTheHill;  return true; }
    if (text == "territories")      { out_mode = GameMode::Territories;    return true; }
    if (text == "juggernaut")       { out_mode = GameMode::Juggernaut;     return true; }
    if (text == "infection")        { out_mode = GameMode::Infection;      return true; }
    return false;
}

namespace {

/// True for modes whose rules require at least two opposing teams.
[[nodiscard]] bool RequiresTeams(GameMode mode) noexcept {
    switch (mode) {
        case GameMode::TeamSlayer:
        case GameMode::CaptureTheFlag:
        case GameMode::Territories:
        case GameMode::Infection:
            return true;
        default:
            return false;
    }
}

} // namespace

Result MatchSettings::Validate() const {
    if (scenario.empty()) {
        return Result::Fail(ErrorCode::ValidationFailed, "scenario is empty");
    }
    if (team_count == 0 || team_count > 8) {
        return Result::Fail(ErrorCode::ValidationFailed,
                            std::format("team_count {} is outside 1..8", team_count));
    }
    if (RequiresTeams(mode) && team_count < 2) {
        return Result::Fail(
            ErrorCode::ValidationFailed,
            std::format("{} requires at least 2 teams, got {}", ToString(mode), team_count));
    }
    // A match with neither a score limit nor a time limit can never end.
    if (score_to_win == 0 && time_limit_seconds == 0) {
        return Result::Fail(ErrorCode::ValidationFailed,
                            "score_to_win and time_limit_seconds are both zero, "
                            "so the match would have no end condition");
    }
    if (time_limit_seconds > 7200) {
        return Result::Fail(ErrorCode::ValidationFailed,
                            std::format("time_limit_seconds {} exceeds the 2 hour ceiling",
                                        time_limit_seconds));
    }
    if (respawn_enabled && respawn_delay_seconds > 60) {
        return Result::Fail(ErrorCode::ValidationFailed,
                            std::format("respawn_delay_seconds {} exceeds 60", respawn_delay_seconds));
    }
    return Result::Success();
}

std::string EngineCapabilities::Describe() const {
    return std::format(
        "begin_scenario={} commands={} session={} map_variant={} sandbox={} "
        "load_progress={} (host={}, join={})",
        can_begin_scenario, can_execute_commands, can_configure_session,
        can_load_map_variant, can_place_sandbox_objects, can_query_load_progress,
        SufficientToHost(), SufficientToJoin());
}

} // namespace mpe::engine
