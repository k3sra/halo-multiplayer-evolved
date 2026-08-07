// SPDX-License-Identifier: MIT
// MultiplayerEvolved: Engine/IEngineControl.h
//
// The single seam between MultiplayerEvolved's own logic and the host game.
//
// Nothing above this interface knows that the game is Blam, that it is hosted
// inside a UE5 shell, or that commands travel through a debug console. The
// lobby, the map pipeline and the networking code depend only on IEngineControl.
// Three payoffs:
//
//   1. Dedicated servers. A headless build supplies a different implementation
//      and the lobby code is untouched.
//   2. Patch resilience. A game update changes only the implementation.
//   3. Testability. Unit tests run the full lobby state machine against a
//      recording fake with no game process at all.
//
// Every method reports failure rather than asserting. The engine is a live
// process we do not own, so "the command did not take" is a normal runtime
// condition to be surfaced to the user, not a programming error.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "Core/Result.h"

namespace mpe::engine {

/// Mirrors the engine's own session classes, verified present in the shipping
/// module as network_session_class_none / _offline / _system_link / _xbox_live.
///
/// SystemLink is the class MultiplayerEvolved drives. It is the engine's direct peer
/// to peer path with no platform account service in the loop, which is exactly
/// what we want once SteamSocketsTransport is carrying the datagrams.
enum class SessionClass : std::uint8_t {
    None = 0,
    Offline,
    SystemLink,
    PlatformService,
};

/// Mirrors network_session_privacy_open / _friends_only / _invitation_only.
enum class SessionPrivacy : std::uint8_t {
    Open = 0,
    FriendsOnly,
    InvitationOnly,
};

/// Classic gametypes. These map onto game engine variants that already exist in
/// the shipped simulation; MultiplayerEvolved selects and configures them rather than
/// reimplementing their rules.
enum class GameMode : std::uint8_t {
    Slayer = 0,
    TeamSlayer,
    CaptureTheFlag,
    Oddball,
    KingOfTheHill,
    Territories,
    Juggernaut,
    Infection,
};

[[nodiscard]] std::string_view ToString(GameMode mode) noexcept;
[[nodiscard]] std::string_view ToString(SessionClass value) noexcept;
[[nodiscard]] std::string_view ToString(SessionPrivacy value) noexcept;

/// Parses the canonical lowercase identifier, for example "capture_the_flag".
/// Returns false and leaves out_mode untouched on unknown input.
[[nodiscard]] bool ParseGameMode(std::string_view text, GameMode& out_mode) noexcept;

/// Rule set for one match. Held in lobby state, replicated to every peer, then
/// applied to the engine immediately before the scenario loads.
struct MatchSettings {
    GameMode      mode{GameMode::Slayer};
    std::string   scenario;             ///< Base scenario the variant builds on.
    std::string   variant_name;         ///< Display name of the map variant.
    std::uint16_t score_to_win{25};     ///< Kills, captures or points by mode.
    std::uint16_t time_limit_seconds{600}; ///< Zero means no limit.
    std::uint8_t  team_count{2};        ///< One means free for all.
    bool          friendly_fire{false};
    bool          respawn_enabled{true};
    std::uint16_t respawn_delay_seconds{5};

    /// Seeded from the host and replicated, so weapon respawn jitter and any
    /// other stochastic engine behaviour stays identical on every peer.
    std::uint32_t random_seed{0};

    /// Rejects internally inconsistent settings before they reach the engine.
    /// A CTF match with one team, or a zero score limit with no time limit,
    /// would otherwise produce a match that can never end.
    [[nodiscard]] Result Validate() const;
};

/// Where the engine currently is. Polled by the lobby to drive its own state
/// machine and to decide when a synchronized launch has completed.
enum class EngineLifecycle : std::uint8_t {
    Uninitialized = 0, ///< Simulation module present but shell not created.
    Idle,              ///< Front end, no match.
    Loading,           ///< Scenario or variant loading.
    InMatch,
    PostMatch,
    Faulted,           ///< Engine reported an unrecoverable state.
};

[[nodiscard]] std::string_view ToString(EngineLifecycle value) noexcept;

/// Opaque handle for one object placed into the sandbox by the map injector.
/// Zero is never valid.
enum class SandboxObjectHandle : std::uint64_t { Invalid = 0 };

/// One object placement request. Coordinates are in engine world units and
/// rotation is a unit quaternion, matching what the parser produces after it
/// has normalized whatever the authoring tool wrote.
struct SandboxPlacement {
    std::string palette_key;     ///< Stable content key, for example "weapon.rocket_launcher".
    float       position[3]{0.0f, 0.0f, 0.0f};
    float       rotation[4]{0.0f, 0.0f, 0.0f, 1.0f}; ///< x, y, z, w.
    float       scale{1.0f};

    std::uint8_t  team{0xFFu};       ///< 0xFF means neutral / no team.
    std::uint16_t spawn_time_seconds{0};
    bool          spawn_at_start{true};
    bool          physics_fixed{false};
    bool          physics_phased{false};
    std::int32_t  respawn_count{-1}; ///< Negative means infinite.
    std::string   label;             ///< Gametype label, for example "ctf_flag_return".
    std::uint32_t user_data{0};
};

/// What the current implementation can actually do. Reported once at startup so
/// the mod can refuse to host with a clear message instead of failing halfway
/// through a match launch.
struct EngineCapabilities {
    bool can_execute_commands{false};
    bool can_configure_session{false};
    bool can_load_map_variant{false};
    bool can_place_sandbox_objects{false};
    bool can_query_load_progress{false};

    /// The mod can put this machine into a named scenario.
    ///
    /// This is the one thing a match cannot happen without, and it is deliberately
    /// separate from can_execute_commands. Those describe the Blam console, which this
    /// build's binding does not reach; beginning a scenario goes through the engine's own
    /// reflected campaign path instead and works without it.
    ///
    /// Conflating the two is what kept the launch sequence switched off. The gate tested
    /// for a console binding, the console binding was unresolved, and so a match could not
    /// start even though the mechanism that starts one was working the whole time.
    bool can_begin_scenario{false};

    /// Everything the host path requires.
    ///
    /// Beginning a scenario, and nothing else. The session configuration calls are Blam
    /// console commands that set privacy, bandwidth and host migration; every one of them
    /// is cosmetic next to whether the players end up in the same map, and the lobby
    /// already treats their failure as a warning rather than a refusal.
    [[nodiscard]] bool SufficientToHost() const noexcept { return can_begin_scenario; }
    /// Everything the client path requires. The same: a client that cannot load the
    /// scenario cannot be in the match, and nothing else it might fail at matters yet.
    [[nodiscard]] bool SufficientToJoin() const noexcept { return can_begin_scenario; }
    [[nodiscard]] std::string Describe() const;
};

class IEngineControl {
public:
    virtual ~IEngineControl() = default;

    [[nodiscard]] virtual EngineCapabilities Capabilities() const = 0;
    [[nodiscard]] virtual EngineLifecycle Lifecycle() const = 0;

    // --- Session configuration -------------------------------------------
    [[nodiscard]] virtual Result SetSessionClass(SessionClass value) = 0;
    [[nodiscard]] virtual Result SetSessionPrivacy(SessionPrivacy value) = 0;

    /// Caps the engine's per peer simulation stream. The lobby lowers this as
    /// peer count rises so an eight player match stays inside a home upload
    /// budget instead of self inflicting packet loss.
    [[nodiscard]] virtual Result SetSimulationBandwidth(std::uint32_t bytes_per_second) = 0;

    /// Host migration is disabled while MultiplayerEvolved owns the transport: our
    /// listen server model designates the host explicitly, and a speculative
    /// migration mid match would fight it.
    [[nodiscard]] virtual Result SetHostMigrationEnabled(bool enabled) = 0;

    // --- Match lifecycle --------------------------------------------------
    [[nodiscard]] virtual Result ApplyMatchSettings(const MatchSettings& settings) = 0;

    /// Begins loading. Asynchronous: completion is observed through
    /// Lifecycle() and QueryLoadProgress().
    [[nodiscard]] virtual Result BeginLoadScenario(std::string_view scenario,
                                                  std::uint32_t random_seed) = 0;

    /// Zero to one. Drives the shared loading screen so every peer sees real
    /// progress rather than a spinner.
    [[nodiscard]] virtual Expected<float> QueryLoadProgress() const = 0;

    /// Releases the simulation to run. Called only once every peer reports
    /// loaded, which is what makes the transition land on all machines at the
    /// same simulation tick.
    [[nodiscard]] virtual Result LaunchMatch() = 0;
    [[nodiscard]] virtual Result EndMatch() = 0;
    [[nodiscard]] virtual Result ReturnToFrontEnd() = 0;

    // --- Map variant / sandbox -------------------------------------------
    /// Hands a serialized native map variant to the engine's own variant
    /// loader, the path behind net_load_and_use_map_variant.
    [[nodiscard]] virtual Result LoadMapVariant(std::string_view variant_identifier) = 0;

    /// Removes every sandbox object, returning the scenario to its base state.
    [[nodiscard]] virtual Result ClearSandbox() = 0;

    [[nodiscard]] virtual Expected<SandboxObjectHandle> SpawnSandboxObject(
        const SandboxPlacement& placement) = 0;
    [[nodiscard]] virtual Result DespawnSandboxObject(SandboxObjectHandle handle) = 0;

    /// Translates a stable content key into the engine's palette index for the
    /// currently loaded scenario. Fails with NotFound when the scenario does not
    /// carry that content, which is how the parser reports an unplaceable map
    /// before a single object is spawned.
    [[nodiscard]] virtual Expected<std::int32_t> ResolvePaletteIndex(
        std::string_view palette_key) const = 0;

    /// Escape hatch for commands that have no typed wrapper yet. Community
    /// contributions start here and graduate to a typed method.
    [[nodiscard]] virtual Result ExecuteConsoleCommand(std::string_view command_line) = 0;
};

} // namespace mpe::engine
