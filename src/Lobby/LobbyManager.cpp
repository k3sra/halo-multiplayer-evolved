// SPDX-License-Identifier: MIT
// MultiplayerEvolved: Lobby/LobbyManager.cpp
#define MPE_LOG_CATEGORY "Lobby"

#include "Lobby/LobbyManager.h"

#include "Core/GameBuild.h"
#include "Core/Log.h"
#include "Map/MapVariantParser.h"

#include <algorithm>
#include <random>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <format>

namespace mpe::lobby {
namespace {

using namespace mpe::net;

/// Wall clock in milliseconds, used only to stamp a launch so peers can log the
/// spread between machines. Never used for simulation timing.
[[nodiscard]] std::uint64_t EpochMilliseconds() {
    using namespace std::chrono;
    return static_cast<std::uint64_t>(
        duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count());
}

/// Seeds the shared match seed. Every peer receives this value from the host, so
/// any engine behaviour driven by it stays identical across machines.
[[nodiscard]] std::uint32_t MakeRandomSeed() {
    const std::uint64_t now = EpochMilliseconds();
    // Mixed so consecutive matches do not produce adjacent seeds.
    std::uint64_t x = now * 0x9E3779B97F4A7C15ULL;
    x ^= x >> 30;
    x *= 0xBF58476D1CE4E5B9ULL;
    x ^= x >> 27;
    return static_cast<std::uint32_t>(x ^ (x >> 32));
}

/// Total host uplink we are willing to assume, in bytes per second. Deliberately
/// conservative: a listen server on a home connection is the common case, and
/// overshooting produces loss that the engine then has to recover from.
constexpr std::uint32_t kAssumedHostUplinkBytesPerSecond = 512 * 1024;

/// Floor for the per peer stream, below which the simulation cannot keep up.
constexpr std::uint32_t kMinimumPerPeerBytesPerSecond = 24 * 1024;

/// Converts protocol settings to engine settings.
[[nodiscard]] engine::MatchSettings FromWire(const MatchSettingsBody& body) {
    engine::MatchSettings settings;
    settings.mode = static_cast<engine::GameMode>(body.mode);
    settings.scenario              = body.scenario;
    settings.variant_name          = body.variant_name;
    settings.score_to_win          = body.score_to_win;
    settings.time_limit_seconds    = body.time_limit_seconds;
    settings.team_count            = body.team_count;
    settings.friendly_fire         = body.friendly_fire;
    settings.respawn_enabled       = body.respawn_enabled;
    settings.respawn_delay_seconds = body.respawn_delay_seconds;
    settings.random_seed           = body.random_seed;
    return settings;
}

[[nodiscard]] MatchSettingsBody ToWire(const engine::MatchSettings& settings) {
    MatchSettingsBody body;
    body.mode                  = static_cast<std::uint8_t>(settings.mode);
    body.scenario              = settings.scenario;
    body.variant_name          = settings.variant_name;
    body.score_to_win          = settings.score_to_win;
    body.time_limit_seconds    = settings.time_limit_seconds;
    body.team_count            = settings.team_count;
    body.friendly_fire         = settings.friendly_fire;
    body.respawn_enabled       = settings.respawn_enabled;
    body.respawn_delay_seconds = settings.respawn_delay_seconds;
    body.random_seed           = settings.random_seed;
    return body;
}

/// Maps the lobby phase onto the protocol's coarser phase for the state gate.
[[nodiscard]] ProtocolPhase ToProtocolPhase(LobbyPhase phase) noexcept {
    switch (phase) {
        case LobbyPhase::Idle:
        case LobbyPhase::Creating:
        case LobbyPhase::Joining:
        case LobbyPhase::Connecting:
        case LobbyPhase::Handshaking:
            return ProtocolPhase::Handshaking;
        case LobbyPhase::Hosting:
        case LobbyPhase::InLobby:
        case LobbyPhase::Countdown:
            return ProtocolPhase::InLobby;
        case LobbyPhase::Loading:
            return ProtocolPhase::Loading;
        case LobbyPhase::InMatch:
        case LobbyPhase::PostMatch:
            return ProtocolPhase::InMatch;
        case LobbyPhase::Faulted:
            // Nothing is acceptable while faulted; Handshaking is the most
            // restrictive phase, which is the correct fail closed choice.
            return ProtocolPhase::Handshaking;
    }
    return ProtocolPhase::Handshaking;
}

} // namespace

std::string_view ToString(LobbyPhase phase) noexcept {
    switch (phase) {
        case LobbyPhase::Idle:        return "idle";
        case LobbyPhase::Creating:    return "creating";
        case LobbyPhase::Hosting:     return "hosting";
        case LobbyPhase::Joining:     return "joining";
        case LobbyPhase::Connecting:  return "connecting";
        case LobbyPhase::Handshaking: return "handshaking";
        case LobbyPhase::InLobby:     return "in_lobby";
        case LobbyPhase::Countdown:   return "countdown";
        case LobbyPhase::Loading:     return "loading";
        case LobbyPhase::InMatch:     return "in_match";
        case LobbyPhase::PostMatch:   return "post_match";
        case LobbyPhase::Faulted:     return "faulted";
    }
    return "unknown";
}

// READINESS WAS REMOVED
//
// Everybody in a session is ready, always. There is no ready button, nothing to toggle,
// and nothing that waits for one.
//
// It was modelled on a lobby where players opt in before a match starts. This is Halo's
// multiplayer: the host decides when the game begins and everyone present plays. A ready
// gate here could only ever do one thing, which is stop a match from starting for a reason
// nobody asked for, and it very nearly did: the countdown refused to run and then cancelled
// itself whenever a client's ready flag had not arrived yet.
//
// The one byte it occupied on the wire is still written, always true, so a build from
// before this change still reads a roster it understands. That is cheaper than a protocol
// version bump, which would stop two players who are mid session from seeing each other.

std::size_t LobbySnapshot::ReadyCount() const noexcept {
    return players.size();
}

bool LobbySnapshot::EveryoneReady() const noexcept {
    return !players.empty();
}

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

LobbyManager::LobbyManager(ILobbyBackend& backend, IPeerTransport& transport,
                           engine::IEngineControl& engine, ILobbyEventSink& sink,
                           LobbyTimings timings)
    : backend_(backend), transport_(transport), engine_(engine), sink_(sink),
      timings_(timings) {
    RebuildSnapshot();
}

LobbyManager::~LobbyManager() {
    // Never throw or assert out of a destructor; LeaveSession is safe in any
    // phase and is what releases the lobby and the transport.
    LeaveSession();
}

// ---------------------------------------------------------------------------
// Commands
// ---------------------------------------------------------------------------

Result LobbyManager::HostSession(const HostOptions& options) {
    if (phase_ != LobbyPhase::Idle) {
        return Result::Fail(ErrorCode::InvalidState,
                            std::format("cannot host while {}", ToString(phase_)));
    }

    // No capability gate here.
    //
    // Hosting a platform lobby is gathering people: a Steam lobby, a roster, invites and
    // chat. None of that touches the engine. What needs the engine is launching the match,
    // and that is where the gate now lives, so a build that cannot yet start a match can
    // still be used to get everybody into a lobby and ready.
    //
    // Refusing here instead meant the session never existed at all, which showed up as a
    // status line reading OFFLINE and slot buttons that opened no invite, because there was
    // nothing to invite anyone to.
    MPE_TRY(options.settings.Validate());

    if (options.max_players < 2 || options.max_players > 16) {
        return Result::Fail(ErrorCode::InvalidArgument,
                            std::format("max_players {} is outside 2..16", options.max_players));
    }

    MPE_ASSIGN_OR_RETURN(const PlatformId local_id, backend_.LocalId());
    MPE_ASSIGN_OR_RETURN(const std::string local_name, backend_.LocalDisplayName());

    settings_    = options.settings;
    max_players_ = options.max_players;
    local_id_    = local_id;
    is_host_     = true;

    // Seed now so it is part of the very first settings broadcast.
    if (settings_.random_seed == 0) {
        settings_.random_seed = MakeRandomSeed();
    }

    // Map selection happens before the lobby opens, so a joining player is never
    // told about a map the host has not yet validated.
    selected_map_.reset();
    if (!options.map_variant_path.empty()) {
        const Result loaded = LoadSelectedMap(options.map_variant_path);
        if (!loaded.ok()) {
            is_host_ = false;
            return loaded;
        }
    }

    // Configure the engine's session, but do not require it to succeed.
    //
    // These describe how the Blam engine should treat a session, and they only matter once
    // a match is actually launching. Treating them as fatal here meant a build whose engine
    // binding is not finished could not open a lobby at all, so nobody could gather, be
    // invited, or get ready. StartCountdown holds the gate that protects the launch itself,
    // which is where an unconfigured engine would really cost something.
    //
    // Reported rather than swallowed: this is a real limitation of the build and the log
    // should say so once, at the moment it is discovered.
    const Result session_class = engine_.SetSessionClass(engine::SessionClass::SystemLink);
    const Result session_privacy =
        engine_.SetSessionPrivacy(options.visibility == LobbyVisibility::Public
                                      ? engine::SessionPrivacy::Open
                                      : options.visibility == LobbyVisibility::FriendsOnly
                                            ? engine::SessionPrivacy::FriendsOnly
                                            : engine::SessionPrivacy::InvitationOnly);
    // Our listen server designates the host explicitly. A speculative migration would hand
    // authority to a peer that is not running our transport as host.
    const Result migration = engine_.SetHostMigrationEnabled(false);
    if (!session_class.ok() || !session_privacy.ok() || !migration.ok()) {
        MPE_LOG_WARN("the lobby is open but the engine's own session was not configured: {}. "
                     "Players can gather and be invited; starting a match is what this "
                     "blocks.",
                     session_class.ok() ? (session_privacy.ok() ? migration.message()
                                                                : session_privacy.message())
                                        : session_class.message());
    }

    ListenConfig listen;
    listen.max_clients = options.max_players - 1;
    listen.use_relay   = true;
    MPE_TRY(transport_.Listen(listen));

    const Result created = backend_.Create(options.visibility, options.max_players);
    if (!created.ok()) {
        transport_.Shutdown();
        is_host_ = false;
        return created;
    }
    hosted_visibility_ = options.visibility;

    // The host occupies slot zero from the outset so the roster is never empty.
    players_.clear();
    PlayerSlot host_slot;
    host_slot.platform_id  = local_id;
    host_slot.display_name = local_name;
    host_slot.peer         = PeerHandle::Invalid;
    host_slot.slot         = 0;
    host_slot.team         = 0;
    host_slot.is_host      = true;
    host_slot.is_local     = true;
    host_slot.is_ready     = true;
    host_slot.has_map      = true;
    players_.push_back(std::move(host_slot));

    ApplyBandwidthBudget();
    TransitionTo(LobbyPhase::Creating, "Creating lobby");
    MPE_LOG_INFO("hosting: mode {}, scenario '{}', up to {} players",
                engine::ToString(settings_.mode), settings_.scenario, options.max_players);
    return Result::Success();
}

Result LobbyManager::JoinSession(LobbyId lobby) {
    if (phase_ != LobbyPhase::Idle) {
        return Result::Fail(ErrorCode::InvalidState,
                            std::format("cannot join while {}", ToString(phase_)));
    }
    // No capability gate here, for the same reason there is none on hosting.
    //
    // Joining a lobby is entering a roster and a chat: it is how two people end up in the
    // same place before anything is launched. Refusing it because the engine command
    // binding is unfinished made the one thing this mod exists for impossible, since the
    // second player was turned away before they ever reached the lobby.
    //
    // The launch path keeps its gate, so a match that cannot start is still refused, which
    // is where the protection actually belongs.
    MPE_ASSIGN_OR_RETURN(const PlatformId local_id, backend_.LocalId());
    local_id_ = local_id;
    is_host_  = false;

    MPE_TRY(backend_.Join(lobby));
    TransitionTo(LobbyPhase::Joining, "Joining lobby");
    return Result::Success();
}

void LobbyManager::LeaveSession() {
    if (phase_ == LobbyPhase::Idle) {
        return;
    }
    MPE_LOG_INFO("leaving session from phase {}", ToString(phase_));

    if (is_host_) {
        // Tell every client before the socket closes so they see HostShutdown
        // rather than a bare timeout.
        std::vector<std::byte> packet;
        PacketBuilder builder(packet, MessageType::Goodbye, Channel::Control);
        builder.Body().WriteU8(static_cast<std::uint8_t>(DisconnectReason::HostShutdown));
        const Result sent = transport_.Broadcast(Channel::Control, packet, SendMode::Reliable);
        if (!sent.ok()) {
            MPE_LOG_DEBUG("goodbye broadcast failed: {}", sent.message());
        }
        transport_.Flush();
    }

    // Engine first: returning to the front end while the transport is still up
    // lets the engine tear its own session down through a live connection.
    if (phase_ == LobbyPhase::InMatch || phase_ == LobbyPhase::Loading ||
        phase_ == LobbyPhase::PostMatch) {
        if (const Result ended = engine_.EndMatch(); !ended.ok()) {
            MPE_LOG_WARN("EndMatch during teardown failed: {}", ended.message());
        }
        if (const Result returned = engine_.ReturnToFrontEnd(); !returned.ok()) {
            MPE_LOG_WARN("ReturnToFrontEnd during teardown failed: {}", returned.message());
        }
    }

    transport_.Shutdown();
    backend_.Leave();

    players_.clear();
    selected_map_.reset();
    map_receive_.reset();
    host_peer_             = PeerHandle::Invalid;
    pending_host_identity_ = PeerIdentity{};
    lobby_id_              = 0;
    is_host_               = false;
    local_has_map_         = false;
    roster_revision_       = 0;
    countdown_remaining_   = 0.0;
    countdown_last_announced_ = 0;
    settings_              = engine::MatchSettings{};

    TransitionTo(LobbyPhase::Idle, "Idle");
}

Result LobbyManager::SetLocalReady(bool ready) {
    // Kept only so the export that calls it still links. Readiness was removed: everybody
    // in a session is ready, always, so there is nothing for this to change.
    (void)ready;
    return Result::Success();
}

Result LobbyManager::SendChat(std::string_view text) {
    if (phase_ == LobbyPhase::Idle || phase_ == LobbyPhase::Faulted) {
        return Result::Fail(ErrorCode::InvalidState, "not in a session");
    }
    if (text.empty()) {
        return Result::Fail(ErrorCode::InvalidArgument, "chat message is empty");
    }

    std::vector<std::byte> packet;
    PacketBuilder builder(packet, MessageType::ChatMessage, Channel::Lobby);
    ChatMessageBody body;
    body.author_platform_id = local_id_;
    body.text.assign(text.substr(0, ChatMessageBody::kMaxTextLength));
    body.Write(builder.Body());

    if (is_host_) {
        // The host is the relay: it echoes to everyone including itself so the
        // local view and the remote view are produced by the same code path.
        MPE_TRY(transport_.Broadcast(Channel::Lobby, packet, SendMode::Reliable));
        sink_.OnChatMessage(local_id_, "", body.text);
        return Result::Success();
    }
    return SendTo(host_peer_, MessageType::ChatMessage, packet, SendMode::Reliable);
}

Result LobbyManager::OpenInviteOverlay() {
    if (!backend_.InLobby()) {
        return Result::Fail(ErrorCode::InvalidState, "not in a lobby");
    }
    return backend_.OpenInviteOverlay();
}

Result LobbyManager::SelectGameMode(engine::GameMode mode) {
    if (!is_host_) {
        return Result::Fail(ErrorCode::InvalidState, "only the host selects the game mode");
    }
    if (phase_ != LobbyPhase::Hosting) {
        return Result::Fail(ErrorCode::InvalidState,
                            "the game mode can only change while the lobby is open");
    }

    engine::MatchSettings updated = settings_;
    updated.mode = mode;
    // Team modes need at least two teams; a mode change adjusts the team count
    // rather than failing, because the player's intent is unambiguous.
    if ((mode == engine::GameMode::TeamSlayer || mode == engine::GameMode::CaptureTheFlag ||
         mode == engine::GameMode::Territories || mode == engine::GameMode::Infection) &&
        updated.team_count < 2) {
        updated.team_count = 2;
    }
    return UpdateMatchSettings(updated);
}

Result LobbyManager::UpdateMatchSettings(const engine::MatchSettings& settings) {
    if (!is_host_) {
        return Result::Fail(ErrorCode::InvalidState, "only the host changes match settings");
    }
    if (phase_ != LobbyPhase::Hosting) {
        return Result::Fail(ErrorCode::InvalidState,
                            "settings can only change while the lobby is open");
    }
    MPE_TRY(settings.Validate());

    settings_ = settings;
    // Preserve the seed across a settings edit so the host cannot accidentally
    // reroll it, which would invalidate anything a client has already prepared.
    if (settings_.random_seed == 0) {
        settings_.random_seed = MakeRandomSeed();
    }

    BroadcastSettings();
    if (const Result published =
            backend_.SetLobbyData(keys::kGameMode, engine::ToString(settings_.mode));
        !published.ok()) {
        MPE_LOG_WARN("publishing game mode to lobby metadata failed: {}", published.message());
    }
    MarkDirty();
    return Result::Success();
}

Result LobbyManager::SelectMapVariant(std::string_view path) {
    if (!is_host_) {
        return Result::Fail(ErrorCode::InvalidState, "only the host selects the map");
    }
    if (phase_ != LobbyPhase::Hosting) {
        return Result::Fail(ErrorCode::InvalidState,
                            "the map can only change while the lobby is open");
    }

    MPE_TRY(LoadSelectedMap(path));

    // Every client's copy is now stale. Clear their flags and push the new
    // manifest; a client cannot ready up again until it holds the new map.
    for (PlayerSlot& player : players_) {
        if (!player.is_host) {
            player.has_map  = false;
            player.is_ready = true; // Readiness was removed; everyone always is.
        }
    }
    // A map change while a countdown is running invalidates it.
    if (phase_ == LobbyPhase::Countdown) {
        const Result cancelled = CancelCountdown("the host changed the map");
        if (!cancelled.ok()) {
            MPE_LOG_WARN("cancelling the countdown after a map change failed: {}",
                        cancelled.message());
        }
    }

    for (const PlayerSlot& player : players_) {
        if (player.is_host || player.peer == PeerHandle::Invalid) {
            continue;
        }
        if (const Result started = BeginMapTransferTo(player.peer); !started.ok()) {
            MPE_LOG_WARN("starting map transfer to {} failed: {}", player.platform_id,
                        started.message());
        }
    }

    BroadcastSettings();
    BroadcastRoster();
    MarkDirty();
    return Result::Success();
}

Result LobbyManager::StartCountdown() {
    if (!is_host_) {
        return Result::Fail(ErrorCode::InvalidState, "only the host starts the match");
    }
    if (phase_ != LobbyPhase::Hosting) {
        return Result::Fail(ErrorCode::InvalidState,
                            std::format("cannot start a countdown while {}", ToString(phase_)));
    }
    if (players_.size() < 2) {
        return Result::Fail(ErrorCode::InvalidState, "at least one other player is required");
    }
    MPE_TRY(settings_.Validate());

    // Capability gate, moved here from HostSession. This is the point where a missing
    // engine binding actually costs something: a lobby full of people whose match cannot
    // start. Refusing before the countdown begins is cheap; discovering it mid launch is
    // not.
    const engine::EngineCapabilities capabilities = engine_.Capabilities();
    if (!capabilities.SufficientToHost()) {
        return Result::Fail(ErrorCode::InvalidState,
                            std::format("this game build cannot start a match yet: {}",
                                        capabilities.Describe()));
    }

    // Everyone must be ready and hold the map. Reported specifically so the host
    // knows who to wait for instead of seeing a generic refusal.
    for (const PlayerSlot& player : players_) {
        if (player.is_host) {
            continue;
        }
        if (!player.has_map) {
            return Result::Fail(ErrorCode::InvalidState,
                                std::format("{} is still downloading the map",
                                            player.display_name));
        }
    }

    countdown_remaining_      = static_cast<double>(timings_.countdown_seconds);
    countdown_last_announced_ = timings_.countdown_seconds + 1; // force a first announce
    TransitionTo(LobbyPhase::Countdown, "Starting match");
    return Result::Success();
}

Result LobbyManager::CancelCountdown(std::string_view reason) {
    if (!is_host_) {
        return Result::Fail(ErrorCode::InvalidState, "only the host cancels the countdown");
    }
    if (phase_ != LobbyPhase::Countdown) {
        return Result::Fail(ErrorCode::InvalidState, "no countdown is running");
    }

    countdown_remaining_ = 0.0;
    BroadcastCountdown(0, true, reason);
    TransitionTo(LobbyPhase::Hosting, std::format("Countdown cancelled: {}", reason));
    MPE_LOG_INFO("countdown cancelled: {}", reason);
    return Result::Success();
}

Result LobbyManager::KickPlayer(PlatformId player) {
    if (!is_host_) {
        return Result::Fail(ErrorCode::InvalidState, "only the host kicks players");
    }
    PlayerSlot* const slot = FindPlayer(player);
    if (slot == nullptr) {
        return Result::Fail(ErrorCode::NotFound, std::format("{} is not in this lobby", player));
    }
    if (slot->is_host) {
        return Result::Fail(ErrorCode::InvalidArgument, "the host cannot kick themselves");
    }

    transport_.Disconnect(slot->peer, DisconnectReason::Kicked, "removed by the host");
    // The transport reports the disconnect, which is what removes the roster
    // entry, so there is exactly one code path that mutates the roster.
    return Result::Success();
}

Result LobbyManager::EndMatch() {
    if (!is_host_) {
        return Result::Fail(ErrorCode::InvalidState, "only the host ends the match");
    }
    if (phase_ != LobbyPhase::InMatch) {
        return Result::Fail(ErrorCode::InvalidState, "no match is running");
    }

    std::vector<std::byte> packet;
    PacketBuilder builder(packet, MessageType::MatchEnded, Channel::Lobby);
    builder.Body().WriteU8(0); // reason: host ended the match
    if (const Result sent = transport_.Broadcast(Channel::Lobby, packet, SendMode::Reliable);
        !sent.ok()) {
        MPE_LOG_WARN("MatchEnded broadcast failed: {}", sent.message());
    }
    transport_.Flush();

    MPE_TRY(engine_.EndMatch());
    TransitionTo(LobbyPhase::PostMatch, "Match complete");
    return Result::Success();
}

// ---------------------------------------------------------------------------
// Tick
// ---------------------------------------------------------------------------

void LobbyManager::Tick(double delta_seconds) {
    if (delta_seconds < 0.0) {
        delta_seconds = 0.0;
    }
    phase_elapsed_ += delta_seconds;

    // Platform first, then transport: a lobby entry event can produce a connect
    // attempt whose result should be handled on the same tick.
    backend_.Poll(*this);
    transport_.Poll(*this);

    if (phase_ == LobbyPhase::Idle || phase_ == LobbyPhase::Faulted) {
        if (snapshot_dirty_) {
            RebuildSnapshot();
        }
        return;
    }

    if (is_host_) {
        TickHost(delta_seconds);
    } else {
        TickClient(delta_seconds);
    }

    if (phase_ == LobbyPhase::Countdown) {
        TickCountdown(delta_seconds);
    } else if (phase_ == LobbyPhase::Loading) {
        TickLoading(delta_seconds);
    }

    // Keepalive on the control channel. Without it a lobby that sits idle longer
    // than the transport timeout would drop peers who have done nothing wrong.
    keepalive_elapsed_ += delta_seconds;
    if (keepalive_elapsed_ >= timings_.keepalive_interval_seconds) {
        keepalive_elapsed_ = 0.0;
        std::vector<std::byte> packet;
        PacketBuilder builder(packet, MessageType::Keepalive, Channel::Control);
        builder.Body().WriteU64(EpochMilliseconds());

        const Result sent = is_host_
                                ? transport_.Broadcast(Channel::Control, packet,
                                                       SendMode::Unreliable)
                                : (host_peer_ != PeerHandle::Invalid
                                       ? transport_.Send(host_peer_, Channel::Control, packet,
                                                         SendMode::Unreliable)
                                       : Result::Success());
        if (!sent.ok()) {
            MPE_LOG_DEBUG("keepalive send failed: {}", sent.message());
        }
    }

    if (snapshot_dirty_) {
        RebuildSnapshot();
    }
}

void LobbyManager::TickHost(double delta_seconds) {
    // Refresh pings and rebroadcast the roster at a low, fixed rate. Ping is
    // cosmetic, so it rides the periodic snapshot rather than its own message.
    roster_broadcast_elapsed_ += delta_seconds;
    if (roster_broadcast_elapsed_ < timings_.roster_broadcast_interval_seconds) {
        return;
    }
    roster_broadcast_elapsed_ = 0.0;

    bool ping_changed = false;
    for (PlayerSlot& player : players_) {
        if (player.peer == PeerHandle::Invalid) {
            continue;
        }
        const Expected<PeerStats> stats = transport_.QueryStats(player.peer);
        if (!stats.ok()) {
            continue;
        }
        const auto ping = static_cast<std::uint16_t>(
            std::min<std::uint32_t>(stats.value().ping_milliseconds, 9999u));
        if (player.ping_milliseconds != ping) {
            player.ping_milliseconds = ping;
            ping_changed = true;
        }
    }
    if (ping_changed) {
        MarkDirty();
    }
    if (phase_ == LobbyPhase::Hosting || phase_ == LobbyPhase::Countdown) {
        BroadcastRoster();
    }
}

void LobbyManager::TickClient(double delta_seconds) {
    (void)delta_seconds;

    switch (phase_) {
        case LobbyPhase::Connecting:
            if (phase_elapsed_ > timings_.connect_timeout_seconds) {
                Fault(Error{ErrorCode::Timeout,
                            "could not reach the host; they may have closed the lobby or be "
                            "behind a connection the relay cannot route"});
            }
            return;

        case LobbyPhase::Handshaking:
            if (phase_elapsed_ > timings_.handshake_timeout_seconds) {
                Fault(Error{ErrorCode::Timeout, "the host did not complete the handshake"});
            }
            return;

        default:
            return;
    }
}

void LobbyManager::TickCountdown(double delta_seconds) {
    if (!is_host_) {
        return; // Clients display the host's announcements; they do not count down.
    }

    // Any regression in readiness aborts the launch. Checked every tick rather
    // than only at zero, so a player who unreadies at the last moment does not
    // get dragged into a match.
    for (const PlayerSlot& player : players_) {
        if (player.is_host) {
            continue;
        }
        if (!player.has_map) {
            const Result cancelled = CancelCountdown(
                std::format("{} is still downloading the map", player.display_name));
            if (!cancelled.ok()) {
                MPE_LOG_WARN("cancel after a map regression failed: {}", cancelled.message());
            }
            return;
        }
    }
    if (players_.size() < 2) {
        const Result cancelled = CancelCountdown("everyone else left the lobby");
        if (!cancelled.ok()) {
            MPE_LOG_WARN("cancel after roster collapse failed: {}", cancelled.message());
        }
        return;
    }

    countdown_remaining_ -= delta_seconds;
    const auto whole_seconds = static_cast<std::uint8_t>(
        std::max(0.0, std::ceil(countdown_remaining_)));

    if (whole_seconds != countdown_last_announced_) {
        countdown_last_announced_ = whole_seconds;
        BroadcastCountdown(whole_seconds, false, {});
        MarkDirty();
    }

    if (countdown_remaining_ > 0.0) {
        return;
    }

    // Launch. The seed and hash are fixed at this instant and every peer,
    // including the host, then follows the identical load path.
    std::vector<std::byte> packet;
    PacketBuilder builder(packet, MessageType::LaunchNow, Channel::Lobby);
    LaunchNowBody body;
    body.scenario                  = settings_.scenario;
    body.map_content_hash_hex      = selected_map_ ? selected_map_->digest_hex : std::string{};
    body.random_seed               = settings_.random_seed;
    body.launch_epoch_milliseconds = EpochMilliseconds();
    body.Write(builder.Body());

    if (const Result sent = transport_.Broadcast(Channel::Lobby, packet, SendMode::Reliable);
        !sent.ok()) {
        MPE_LOG_ERROR("LaunchNow broadcast failed: {}", sent.message());
        const Result cancelled = CancelCountdown("the launch message could not be delivered");
        if (!cancelled.ok()) {
            Fault(Error{ErrorCode::TransportSendFailed, "launch failed and could not be cancelled"});
        }
        return;
    }
    // Flushed immediately: the launch must not sit in a coalescing buffer while
    // the host has already started loading.
    transport_.Flush();

    if (const Result loading = BeginLoad(settings_.scenario, settings_.random_seed);
        !loading.ok()) {
        Fault(loading.error());
    }
}

void LobbyManager::TickLoading(double delta_seconds) {
    (void)delta_seconds;

    if (phase_elapsed_ > timings_.load_timeout_seconds) {
        Fault(Error{ErrorCode::Timeout,
                    "loading did not finish within the time limit; a peer may have stalled"});
        return;
    }

    const Expected<float> progress = engine_.QueryLoadProgress();
    const float local_progress = progress.ok() ? progress.value() : 0.0f;

    if (PlayerSlot* const self = FindPlayer(local_id_); self != nullptr) {
        if (self->load_progress != local_progress) {
            self->load_progress = local_progress;
            MarkDirty();
        }
    }

    if (!is_host_) {
        // Report progress upstream. Unreliable is correct: a dropped progress
        // report is superseded by the next one, and the completion condition is
        // re-evaluated by the host every tick.
        std::vector<std::byte> packet;
        PacketBuilder builder(packet, MessageType::LoadProgress, Channel::Lobby);
        LoadProgressBody body;
        body.progress = local_progress;
        body.Write(builder.Body());
        const Result sent = SendTo(host_peer_, MessageType::LoadProgress, packet,
                                   SendMode::UnreliableSequenced);
        if (!sent.ok()) {
            MPE_LOG_DEBUG("load progress send failed: {}", sent.message());
        }
        return;
    }

    // Host: release only when every peer, itself included, has finished.
    const bool everyone_loaded =
        std::all_of(players_.begin(), players_.end(),
                    [](const PlayerSlot& p) { return p.load_progress >= 1.0f; });
    if (!everyone_loaded) {
        return;
    }

    std::vector<std::byte> packet;
    PacketBuilder builder(packet, MessageType::AllPeersLoaded, Channel::Lobby);
    builder.Body().WriteU64(EpochMilliseconds());
    if (const Result sent = transport_.Broadcast(Channel::Lobby, packet, SendMode::Reliable);
        !sent.ok()) {
        MPE_LOG_ERROR("AllPeersLoaded broadcast failed: {}", sent.message());
        Fault(Error{ErrorCode::TransportSendFailed,
                    "could not tell peers to start; the match would desynchronize"});
        return;
    }
    transport_.Flush();

    if (const Result launched = engine_.LaunchMatch(); !launched.ok()) {
        Fault(launched.error());
        return;
    }
    TransitionTo(LobbyPhase::InMatch, "In match");
    MPE_LOG_INFO("match live with {} player(s)", players_.size());
}

// ---------------------------------------------------------------------------
// Backend observer
// ---------------------------------------------------------------------------

void LobbyManager::OnLobbyCreated(LobbyId lobby) {
    lobby_id_ = lobby;

    // Publish selection metadata so a prospective joiner can evaluate the lobby
    // before committing to a transport connection.
    const auto publish = [this](const char* key, std::string_view value) {
        if (const Result result = backend_.SetLobbyData(key, value); !result.ok()) {
            MPE_LOG_WARN("publishing lobby data '{}' failed: {}", key, result.message());
        }
    };
    publish(keys::kGameMode, engine::ToString(settings_.mode));
    publish(keys::kBaseScenario, settings_.scenario);
    publish(keys::kMapName, selected_map_ ? selected_map_->name : "");
    publish(keys::kMapHash, selected_map_ ? selected_map_->digest_hex : "");
    publish(keys::kLobbyPhase, ToString(LobbyPhase::Hosting));

    if (const Result presence = backend_.PublishJoinablePresence(
            std::format("{} on {}", engine::ToString(settings_.mode),
                        selected_map_ ? selected_map_->name : settings_.scenario));
        !presence.ok()) {
        MPE_LOG_WARN("publishing joinable presence failed: {}", presence.message());
    }

    TransitionTo(LobbyPhase::Hosting, "Lobby open");
    MPE_LOG_INFO("lobby {} created", lobby);
}

void LobbyManager::OnLobbyCreateFailed(const Error& error) {
    transport_.Shutdown();
    is_host_ = false;
    Fault(error);
}

void LobbyManager::OnLobbyEntered(LobbyId lobby, bool is_owner) {
    lobby_id_ = lobby;

    if (is_owner) {
        // Entering our own lobby. Creation already moved us to Hosting.
        return;
    }

    // Validate compatibility from metadata before connecting, so a mismatched
    // client gets an explanation instead of a failed handshake.
    const Expected<std::string> protocol = backend_.GetLobbyData(keys::kProtocolVersion);
    if (!protocol.ok()) {
        Fault(Error{ErrorCode::LobbyUnavailable,
                    "this lobby does not advertise MultiplayerEvolved; the host may not have the mod "
                    "installed"});
        return;
    }
    if (protocol.value() != std::to_string(net::kProtocolVersion)) {
        Fault(Error{ErrorCode::ProtocolVersionMismatch,
                    std::format("the host runs MultiplayerEvolved protocol {} and this install runs {}; "
                                "one of you needs to update",
                                protocol.value(), net::kProtocolVersion)});
        return;
    }

    const Expected<std::string> build = backend_.GetLobbyData(keys::kGameBuild);
    if (!build.ok()) {
        Fault(Error{ErrorCode::LobbyUnavailable, "this lobby does not advertise a game build"});
        return;
    }
    if (build.value() != GameBuildString()) {
        Fault(Error{ErrorCode::IncompatibleGameBuild,
                    std::format("the host is on game build '{}' and this install is on '{}'",
                                build.value(), GameBuildString())});
        return;
    }

    const Expected<std::string> host_id = backend_.GetLobbyData(keys::kHostId);
    if (!host_id.ok()) {
        Fault(Error{ErrorCode::LobbyUnavailable, "this lobby does not advertise a host identity"});
        return;
    }

    PlatformId parsed_host = 0;
    for (const char c : host_id.value()) {
        if (c < '0' || c > '9') {
            parsed_host = 0;
            break;
        }
        parsed_host = parsed_host * 10u + static_cast<PlatformId>(c - '0');
    }
    if (parsed_host == 0) {
        Fault(Error{ErrorCode::LobbyUnavailable, "the advertised host identity is malformed"});
        return;
    }

    pending_host_identity_ = PeerIdentity{parsed_host};
    const Result connecting = transport_.Connect(
        pending_host_identity_,
        static_cast<std::uint32_t>(timings_.connect_timeout_seconds * 1000.0));
    if (!connecting.ok()) {
        Fault(connecting.error());
        return;
    }

    TransitionTo(LobbyPhase::Connecting, "Connecting to host");
}

void LobbyManager::OnLobbyEnterFailed(const Error& error) {
    Fault(error);
}

void LobbyManager::OnMemberJoined(const LobbyMember& member) {
    // Membership in the platform lobby is not membership in the match. The
    // roster entry is created when the peer completes the transport handshake,
    // which is the point at which we can actually exchange state with them.
    MPE_LOG_DEBUG("{} entered the platform lobby", member.platform_id);
}

void LobbyManager::OnMemberLeft(PlatformId member, bool was_kicked) {
    MPE_LOG_DEBUG("{} left the platform lobby (kicked={})", member, was_kicked);

    if (!is_host_ && pending_host_identity_.platform_id == member) {
        Fault(Error{ErrorCode::LobbyUnavailable, "the host closed the lobby"});
    }
}

void LobbyManager::OnLobbyDataChanged(LobbyId lobby) {
    (void)lobby;
    // Clients take authoritative settings from MatchSettingsSync over the
    // transport, not from lobby metadata. Metadata exists for players who have
    // not connected yet, so there is nothing to apply here.
}

void LobbyManager::OnMemberDataChanged(PlatformId member) {
    if (!is_host_) {
        return;
    }
    // Ready state also arrives over the transport. Metadata is the fallback path
    // for the window between lobby entry and a completed handshake.
    // Nothing to read. Readiness was removed, and it was the only thing this member data
    // carried, so a change to it can no longer mean anything.
    (void)member;
}

void LobbyManager::OnJoinRequested(LobbyId lobby, PlatformId inviter) {
    MPE_LOG_INFO("join requested for lobby {} via {}", lobby, inviter);

    if (phase_ == LobbyPhase::InMatch || phase_ == LobbyPhase::Loading ||
        phase_ == LobbyPhase::Countdown) {
        // Accepting an invitation mid match would tear down the running game
        // without warning. Surfaced as an error so the UI can offer the choice.
        sink_.OnError(Error{ErrorCode::InvalidState,
                            "leave the current match before accepting an invitation"});
        return;
    }

    if (phase_ != LobbyPhase::Idle) {
        LeaveSession();
    }
    if (const Result joined = JoinSession(lobby); !joined.ok()) {
        sink_.OnError(joined.error());
    }
}

// ---------------------------------------------------------------------------
// Transport observer
// ---------------------------------------------------------------------------

void LobbyManager::OnPeerConnected(PeerHandle peer, const PeerIdentity& identity) {
    if (is_host_) {
        // The roster entry waits for HandshakeRequest, which is what carries the
        // client's build and name. Until then the peer is connected but unknown.
        MPE_LOG_INFO("peer {} connected, awaiting handshake",
                    static_cast<std::uint32_t>(peer));
        return;
    }

    // Client: the host is up. Introduce ourselves.
    host_peer_ = peer;

    const Expected<std::string> name = backend_.LocalDisplayName();
    std::vector<std::byte> packet;
    PacketBuilder builder(packet, MessageType::HandshakeRequest, Channel::Control);
    HandshakeRequestBody body;
    body.mod_version  = net::kProtocolVersion;
    body.game_build   = GameBuildString();
    body.display_name = name.ok() ? name.value() : std::format("Player {}", local_id_);
    body.platform_id  = local_id_;
    body.Write(builder.Body());

    if (const Result sent = SendTo(peer, MessageType::HandshakeRequest, packet,
                                   SendMode::Reliable);
        !sent.ok()) {
        Fault(sent.error());
        return;
    }
    (void)identity;
    TransitionTo(LobbyPhase::Handshaking, "Joining match");
}

void LobbyManager::OnPeerDisconnected(PeerHandle peer, DisconnectReason reason,
                                      std::string_view detail) {
    if (is_host_) {
        PlayerSlot* const player = FindPlayerByPeer(peer);
        const std::string name = (player != nullptr) ? player->display_name : std::string("a peer");
        MPE_LOG_INFO("{} disconnected: {} ({})", name, ToString(reason), detail);

        RemovePlayerByPeer(peer);
        ApplyBandwidthBudget();
        BroadcastRoster();
        MarkDirty();

        // A disconnect during a countdown or a load invalidates the launch,
        // because the remaining peers would be waiting on a machine that is gone.
        if (phase_ == LobbyPhase::Countdown) {
            const Result cancelled = CancelCountdown(std::format("{} left", name));
            if (!cancelled.ok()) {
                MPE_LOG_WARN("cancel after disconnect failed: {}", cancelled.message());
            }
        }
        return;
    }

    // Client: losing the host ends the session.
    if (peer == host_peer_) {
        host_peer_ = PeerHandle::Invalid;
        const auto message =
            reason == DisconnectReason::Kicked
                ? std::format("removed from the match by the host: {}", detail)
                : reason == DisconnectReason::HostShutdown
                      ? std::string("the host ended the session")
                      : std::format("lost connection to the host: {} ({})", ToString(reason),
                                    detail);
        Fault(Error{ErrorCode::PeerRejected, message});
    }
}

void LobbyManager::OnConnectFailed(DisconnectReason reason, std::string_view detail) {
    if (is_host_) {
        MPE_LOG_WARN("an inbound connection failed: {} ({})", ToString(reason), detail);
        return;
    }
    Fault(Error{ErrorCode::TransportUnavailable,
                std::format("could not connect to the host: {} ({})", ToString(reason), detail)});
}

void LobbyManager::OnPacketReceived(PeerHandle peer, Channel channel,
                                    std::span<const std::byte> payload) {
    HandlePacket(peer, channel, payload);
}

// ---------------------------------------------------------------------------
// Packet routing
// ---------------------------------------------------------------------------

void LobbyManager::HandlePacket(PeerHandle peer, Channel channel,
                                std::span<const std::byte> payload) {
    const Expected<DecodedPacket> decoded = DecodePacket(payload, channel);
    if (!decoded.ok()) {
        MPE_LOG_WARN("dropping a packet from peer {}: {}", static_cast<std::uint32_t>(peer),
                    decoded.message());
        transport_.Disconnect(peer, DisconnectReason::ProtocolViolation, decoded.message());
        return;
    }

    const DecodedPacket& packet = decoded.value();
    const PeerRole role = is_host_ ? PeerRole::Host : PeerRole::Client;

    // The authorization gate. A client cannot send host authored messages and no
    // message can arrive in a phase where it makes no sense.
    //
    // Dropped rather than disconnected on. Arriving in the wrong phase is not evidence of
    // an attack: a connection carries several independently ordered lanes, so a message
    // sent after another can be delivered before it, and a message sent legitimately can
    // land a moment after the phase it belonged to has ended. Hanging up on the peer for
    // that turns a message worth ignoring into a session nobody can recover, which is
    // exactly what happened to the first client that ever reached a host. Ignoring one
    // packet costs nothing by comparison, and a peer genuinely sending nonsense is still
    // caught by the decoder above and by the role check below.
    if (!IsMessageAcceptable(packet.type, role, ToProtocolPhase(phase_))) {
        MPE_LOG_WARN("peer {}: dropping {} as {} while {}", static_cast<std::uint32_t>(peer),
                    ToString(packet.type), role == PeerRole::Host ? "host" : "client",
                    ToString(phase_));
        return;
    }

    // A client accepts lobby traffic only from the host. Without this a peer that
    // learned our identity could inject a roster or a launch.
    if (!is_host_ && peer != host_peer_ && packet.type != MessageType::HandshakeAccept &&
        packet.type != MessageType::HandshakeReject) {
        MPE_LOG_WARN("dropping {} from non host peer {}", ToString(packet.type),
                    static_cast<std::uint32_t>(peer));
        transport_.Disconnect(peer, DisconnectReason::ProtocolViolation,
                              "only the host may send lobby traffic");
        return;
    }

    ByteReader reader(packet.body);
    Result     handled = Result::Success();

    switch (packet.type) {
        case MessageType::HandshakeRequest:  handled = HandleHandshakeRequest(peer, reader); break;
        case MessageType::HandshakeAccept:   handled = HandleHandshakeAccept(reader); break;
        case MessageType::HandshakeReject:   handled = HandleHandshakeReject(reader); break;
        case MessageType::RosterUpdate:      handled = HandleRosterUpdate(reader); break;
        case MessageType::MatchSettingsSync: handled = HandleMatchSettings(reader); break;
        case MessageType::ReadyStateChange:  handled = HandleReadyStateChange(peer, reader); break;
        case MessageType::LaunchCountdown:   handled = HandleLaunchCountdown(reader); break;
        case MessageType::LaunchNow:         handled = HandleLaunchNow(reader); break;
        case MessageType::LoadProgress:      handled = HandleLoadProgress(peer, reader); break;
        case MessageType::AllPeersLoaded:    handled = HandleAllPeersLoaded(); break;
        case MessageType::MatchEnded:        handled = HandleMatchEnded(); break;
        case MessageType::ChatMessage:       handled = HandleChatMessage(peer, reader); break;
        case MessageType::MapManifest:       handled = HandleMapManifest(reader); break;
        case MessageType::MapChunkRequest:   handled = HandleMapChunkRequest(peer, reader); break;
        case MessageType::MapChunk:          handled = HandleMapChunk(reader); break;
        case MessageType::MapTransferDone:   handled = HandleMapTransferDone(peer); break;
        case MessageType::SimulationDatagram:
            handled = HandleSimulationDatagram(peer, packet.body);
            break;

        case MessageType::Keepalive:
            // Nothing to do: arrival is the entire payload's purpose.
            break;

        case MessageType::Goodbye: {
            std::uint8_t raw_reason = 0;
            if (!reader.ReadU8(raw_reason)) {
                handled = Result::Fail(ErrorCode::ProtocolViolation, "malformed Goodbye");
                break;
            }
            transport_.Disconnect(peer, DisconnectReason::RemoteRequest, "peer said goodbye");
            break;
        }
    }

    if (!handled.ok()) {
        MPE_LOG_WARN("handling {} from peer {} failed: {}", ToString(packet.type),
                    static_cast<std::uint32_t>(peer), handled.message());

        // A malformed or unauthorized message is a disconnect. Anything else is a
        // local failure and must not punish the peer.
        if (handled.code() == ErrorCode::ProtocolViolation ||
            handled.code() == ErrorCode::IntegrityMismatch ||
            handled.code() == ErrorCode::ProtocolVersionMismatch) {
            transport_.Disconnect(peer, DisconnectReason::ProtocolViolation, handled.message());
        } else if (!is_host_) {
            Fault(handled.error());
        }
    }
}

Result LobbyManager::HandleHandshakeRequest(PeerHandle peer, ByteReader& reader) {
    MPE_ASSIGN_OR_RETURN(const HandshakeRequestBody body, HandshakeRequestBody::Read(reader));

    const auto reject = [&](DisconnectReason reason, std::string detail) -> Result {
        std::vector<std::byte> packet;
        PacketBuilder builder(packet, MessageType::HandshakeReject, Channel::Control);
        HandshakeRejectBody rejection;
        rejection.reason = reason;
        rejection.detail = detail;
        rejection.Write(builder.Body());
        if (const Result sent = SendTo(peer, MessageType::HandshakeReject, packet,
                                       SendMode::Reliable);
            !sent.ok()) {
            MPE_LOG_DEBUG("sending the rejection failed: {}", sent.message());
        }
        transport_.Flush();
        transport_.Disconnect(peer, reason, detail);
        MPE_LOG_INFO("rejected peer {}: {}", static_cast<std::uint32_t>(peer), detail);
        return Result::Success(); // Handled; the peer is gone.
    };

    // The claimed identity must match the one the transport authenticated. Steam
    // authenticates the connection, so a mismatch is an impersonation attempt.
    const Expected<PeerIdentity> authenticated = transport_.IdentityOf(peer);
    if (!authenticated.ok()) {
        return reject(DisconnectReason::InternalError, "the connection identity is unavailable");
    }
    if (authenticated.value().platform_id != body.platform_id) {
        return reject(DisconnectReason::ProtocolViolation,
                      std::format("claimed identity {} does not match the authenticated {}",
                                  body.platform_id, authenticated.value().platform_id));
    }

    if (body.game_build != GameBuildString()) {
        return reject(DisconnectReason::VersionMismatch,
                      std::format("your game build is '{}' and the host is on '{}'",
                                  body.game_build, GameBuildString()));
    }
    if (players_.size() >= max_players_) {
        return reject(DisconnectReason::Kicked, "the lobby is full");
    }
    // Late arrivals are admitted, not turned away.
    //
    // This used to accept only while Hosting, so pressing start closed the session to
    // everybody who had not already arrived: a friend a minute late got "the host is no
    // longer accepting players" and had no way in until the match ended. Halo's own
    // multiplayer never worked that way and neither should this.
    //
    // The phases that genuinely cannot take a new player are the ones where there is
    // nothing coherent to put them into. Faulted has no session left, and the two client
    // side phases mean this machine is not the host of anything.
    if (phase_ == LobbyPhase::Faulted || phase_ == LobbyPhase::Idle ||
        phase_ == LobbyPhase::Creating) {
        return reject(DisconnectReason::Kicked,
                      std::format("the host is not running a session ({})", ToString(phase_)));
    }
    if (!is_host_) {
        return reject(DisconnectReason::Kicked, "this machine is not hosting");
    }
    if (FindPlayer(body.platform_id) != nullptr) {
        return reject(DisconnectReason::ProtocolViolation, "already in this lobby");
    }

    PlayerSlot player;
    player.platform_id  = body.platform_id;
    player.display_name = body.display_name.empty()
                              ? std::format("Player {}", body.platform_id)
                              : body.display_name;
    player.peer     = peer;
    player.slot     = AllocateSlot();
    player.team     = AssignTeam();
    player.is_host  = false;
    player.is_local = false;
    player.is_ready = true; // Readiness was removed; everyone always is.
    player.has_map  = !selected_map_.has_value(); // No map means nothing to fetch.
    players_.push_back(player);

    std::vector<std::byte> packet;
    PacketBuilder builder(packet, MessageType::HandshakeAccept, Channel::Control);
    HandshakeAcceptBody accept;
    accept.assigned_slot  = player.slot;
    accept.assigned_team  = player.team;
    accept.host_tick_rate = 60;
    // Told what they are joining, since it is no longer always a lobby.
    accept.host_phase = static_cast<std::uint8_t>(ToProtocolPhase(phase_));
    accept.Write(builder.Body());
    MPE_TRY(SendTo(peer, MessageType::HandshakeAccept, packet, SendMode::Reliable));

    MPE_LOG_INFO("{} joined as slot {} on team {}", player.display_name, player.slot, player.team);

    ApplyBandwidthBudget();
    BroadcastSettings();
    BroadcastRoster();

    if (selected_map_.has_value()) {
        MPE_TRY(BeginMapTransferTo(peer));
    }
    MarkDirty();
    return Result::Success();
}

Result LobbyManager::HandleHandshakeAccept(ByteReader& reader) {
    MPE_ASSIGN_OR_RETURN(const HandshakeAcceptBody body, HandshakeAcceptBody::Read(reader));

    MPE_LOG_INFO("accepted as slot {} on team {}, host is {}", body.assigned_slot,
                body.assigned_team, static_cast<int>(body.host_phase));

    // The roster arrives separately; this only confirms admission.
    //
    // Where the client lands depends on what the host is doing, because a host will now
    // admit somebody during a match rather than only before one. Landing in the lobby
    // regardless would leave a late arrival watching a lobby screen for a launch that had
    // already happened without them.
    switch (static_cast<ProtocolPhase>(body.host_phase)) {
        case ProtocolPhase::Loading:
            TransitionTo(LobbyPhase::Loading, "Joining a match in progress");
            break;
        case ProtocolPhase::InMatch:
            TransitionTo(LobbyPhase::Loading, "Joining a match in progress");
            break;
        case ProtocolPhase::DistributingMap:
        case ProtocolPhase::InLobby:
        case ProtocolPhase::Handshaking:
        default:
            TransitionTo(LobbyPhase::InLobby, "In lobby");
            break;
    }
    return Result::Success();
}

Result LobbyManager::HandleHandshakeReject(ByteReader& reader) {
    MPE_ASSIGN_OR_RETURN(const HandshakeRejectBody body, HandshakeRejectBody::Read(reader));

    // Reported as a fault carrying the host's own words, which is far more useful
    // than a generic connection failure.
    Fault(Error{ErrorCode::PeerRejected,
                body.detail.empty() ? std::string(ToString(body.reason)) : body.detail});
    return Result::Success();
}

Result LobbyManager::HandleRosterUpdate(ByteReader& reader) {
    MPE_ASSIGN_OR_RETURN(RosterUpdateBody body, RosterUpdateBody::Read(reader));

    // Stale snapshots are discarded rather than applied out of order.
    if (body.revision < roster_revision_) {
        return Result::Success();
    }
    roster_revision_ = body.revision;

    // Preserve local only fields that the host does not own.
    const float local_progress = [this] {
        const PlayerSlot* const self =
            const_cast<LobbyManager*>(this)->FindPlayer(local_id_);
        return self != nullptr ? self->load_progress : 0.0f;
    }();

    players_.clear();
    players_.reserve(body.entries.size());
    for (const RosterEntry& entry : body.entries) {
        PlayerSlot player;
        player.platform_id       = entry.platform_id;
        player.display_name      = entry.display_name;
        player.peer              = (entry.is_host) ? host_peer_ : PeerHandle::Invalid;
        player.slot              = entry.slot;
        player.team              = entry.team;
        player.is_host           = entry.is_host;
        player.is_local          = (entry.platform_id == local_id_);
        player.is_ready          = true; // Readiness was removed; everyone always is.
        player.has_map           = entry.has_map;
        player.ping_milliseconds = entry.ping_milliseconds;
        player.load_progress     = player.is_local ? local_progress : 0.0f;
        players_.push_back(std::move(player));
    }

    MarkDirty();
    return Result::Success();
}

Result LobbyManager::HandleMatchSettings(ByteReader& reader) {
    MPE_ASSIGN_OR_RETURN(const MatchSettingsBody body, MatchSettingsBody::Read(reader));

    engine::MatchSettings incoming = FromWire(body);
    // The host's settings are authoritative but still validated: a host running
    // a modified build must not be able to drive this client into a state its own
    // engine cannot represent.
    MPE_TRY(incoming.Validate());

    settings_ = incoming;
    MarkDirty();
    return Result::Success();
}

Result LobbyManager::HandleReadyStateChange(PeerHandle peer, ByteReader& reader) {
    bool ready = false;
    if (!reader.ReadBool(ready)) {
        return Result::Fail(ErrorCode::ProtocolViolation, "malformed ReadyStateChange");
    }

    PlayerSlot* const player = FindPlayerByPeer(peer);
    if (player == nullptr) {
        return Result::Fail(ErrorCode::ProtocolViolation,
                            "ready state from a peer with no roster entry");
    }

    // Accepted and ignored. Readiness was removed, so a build from before that change can
    // still send this without being disconnected for it, and it changes nothing.
    (void)player;
    return Result::Success();
}

Result LobbyManager::HandleLaunchCountdown(ByteReader& reader) {
    MPE_ASSIGN_OR_RETURN(const LaunchCountdownBody body, LaunchCountdownBody::Read(reader));

    if (body.cancelled) {
        countdown_remaining_ = 0.0;
        TransitionTo(LobbyPhase::InLobby,
                     body.cancel_reason.empty()
                         ? std::string("Countdown cancelled")
                         : std::format("Countdown cancelled: {}", body.cancel_reason));
        return Result::Success();
    }

    countdown_remaining_      = static_cast<double>(body.seconds_remaining);
    countdown_last_announced_ = body.seconds_remaining;
    if (phase_ != LobbyPhase::Countdown) {
        TransitionTo(LobbyPhase::Countdown, "Starting match");
    }
    MarkDirty();
    return Result::Success();
}

Result LobbyManager::HandleLaunchNow(ByteReader& reader) {
    MPE_ASSIGN_OR_RETURN(const LaunchNowBody body, LaunchNowBody::Read(reader));

    // The host's map must be the one we hold. Loading a different layout would
    // put objects in different places on different machines, which the engine's
    // replication cannot reconcile.
    if (!body.map_content_hash_hex.empty()) {
        if (!selected_map_.has_value() ||
            selected_map_->digest_hex != body.map_content_hash_hex) {
            return Result::Fail(ErrorCode::IntegrityMismatch,
                                "the host launched a map this machine does not hold");
        }
    }
    if (body.scenario != settings_.scenario) {
        MPE_LOG_INFO("host launched scenario '{}', updating from '{}'", body.scenario,
                    settings_.scenario);
        settings_.scenario = body.scenario;
    }
    settings_.random_seed = body.random_seed;

    return BeginLoad(body.scenario, body.random_seed);
}

Result LobbyManager::HandleLoadProgress(PeerHandle peer, ByteReader& reader) {
    MPE_ASSIGN_OR_RETURN(const LoadProgressBody body, LoadProgressBody::Read(reader));

    PlayerSlot* const player = FindPlayerByPeer(peer);
    if (player == nullptr) {
        return Result::Fail(ErrorCode::ProtocolViolation,
                            "load progress from a peer with no roster entry");
    }
    // Monotonic: progress never moves backward, so a reordered unreliable packet
    // cannot make a finished peer look unfinished and stall the launch.
    if (body.progress > player->load_progress) {
        player->load_progress = body.progress;
        MarkDirty();
    }
    return Result::Success();
}

Result LobbyManager::HandleAllPeersLoaded() {
    MPE_TRY(engine_.LaunchMatch());
    TransitionTo(LobbyPhase::InMatch, "In match");
    MPE_LOG_INFO("match live");
    return Result::Success();
}

Result LobbyManager::HandleMatchEnded() {
    if (const Result ended = engine_.EndMatch(); !ended.ok()) {
        MPE_LOG_WARN("EndMatch failed: {}", ended.message());
    }
    TransitionTo(LobbyPhase::PostMatch, "Match complete");
    return Result::Success();
}

Result LobbyManager::HandleChatMessage(PeerHandle peer, ByteReader& reader) {
    MPE_ASSIGN_OR_RETURN(const ChatMessageBody body, ChatMessageBody::Read(reader));

    if (is_host_) {
        // The host stamps the author from the authenticated connection rather
        // than trusting the body, then relays. This is what stops a client from
        // sending chat as another player.
        PlayerSlot* const author = FindPlayerByPeer(peer);
        if (author == nullptr) {
            return Result::Fail(ErrorCode::ProtocolViolation,
                                "chat from a peer with no roster entry");
        }

        std::vector<std::byte> packet;
        PacketBuilder builder(packet, MessageType::ChatMessage, Channel::Lobby);
        ChatMessageBody relayed;
        relayed.author_platform_id = author->platform_id;
        relayed.text               = body.text;
        relayed.Write(builder.Body());

        if (const Result sent = transport_.Broadcast(Channel::Lobby, packet, SendMode::Reliable);
            !sent.ok()) {
            MPE_LOG_DEBUG("relaying chat failed: {}", sent.message());
        }
        sink_.OnChatMessage(author->platform_id, author->display_name, body.text);
        return Result::Success();
    }

    const PlayerSlot* const author = FindPlayer(body.author_platform_id);
    sink_.OnChatMessage(body.author_platform_id,
                        author != nullptr ? author->display_name : std::string_view{}, body.text);
    return Result::Success();
}

// ---------------------------------------------------------------------------
// Map distribution
// ---------------------------------------------------------------------------

Result LobbyManager::HandleMapManifest(ByteReader& reader) {
    MPE_ASSIGN_OR_RETURN(const MapManifestBody manifest, MapManifestBody::Read(reader));

    // Already holding exactly this map: nothing to transfer.
    if (selected_map_.has_value() && selected_map_->digest_hex == manifest.content_hash_hex) {
        local_has_map_ = true;
        std::vector<std::byte> packet;
        PacketBuilder builder(packet, MessageType::MapTransferDone, Channel::MapTransfer);
        builder.Body().WriteString(manifest.content_hash_hex);
        MPE_TRY(SendTo(host_peer_, MessageType::MapTransferDone, packet, SendMode::Reliable));
        MarkDirty();
        return Result::Success();
    }

    MapReceive receive;
    receive.manifest = manifest;
    receive.bytes.assign(manifest.total_bytes, std::byte{0});
    receive.chunk_present.assign(manifest.chunk_count, false);
    map_receive_ = std::move(receive);
    local_has_map_ = false;

    MPE_LOG_INFO("receiving map '{}' ({} bytes in {} chunks)", manifest.map_name,
                manifest.total_bytes, manifest.chunk_count);

    // Request every chunk up front. The transport's reliable lane handles
    // pacing, and the bulk lane's low priority keeps this out of the way of
    // control traffic.
    for (std::uint32_t index = 0; index < manifest.chunk_count; ++index) {
        std::vector<std::byte> packet;
        PacketBuilder builder(packet, MessageType::MapChunkRequest, Channel::MapTransfer);
        builder.Body().WriteU32(index);
        MPE_TRY(SendTo(host_peer_, MessageType::MapChunkRequest, packet, SendMode::Reliable));
    }
    MarkDirty();
    return Result::Success();
}

Result LobbyManager::HandleMapChunkRequest(PeerHandle peer, ByteReader& reader) {
    std::uint32_t index = 0;
    if (!reader.ReadU32(index)) {
        return Result::Fail(ErrorCode::ProtocolViolation, "malformed MapChunkRequest");
    }
    if (!selected_map_.has_value()) {
        return Result::Fail(ErrorCode::InvalidState, "a chunk was requested but no map is selected");
    }

    const std::vector<std::byte>& payload = selected_map_->bytes;
    const std::size_t offset = static_cast<std::size_t>(index) * kMapChunkBytes;
    if (offset >= payload.size()) {
        return Result::Fail(ErrorCode::ProtocolViolation,
                            std::format("chunk {} is past the end of a {} byte map", index,
                                        payload.size()));
    }
    const std::size_t length = std::min(kMapChunkBytes, payload.size() - offset);
    const auto data = std::span(payload.data() + offset, length);

    std::vector<std::byte> packet;
    PacketBuilder builder(packet, MessageType::MapChunk, Channel::MapTransfer);
    MapChunkBody chunk;
    chunk.chunk_index = index;
    chunk.crc32       = hash::Crc32(data);
    chunk.data        = data;
    chunk.Write(builder.Body());

    return SendTo(peer, MessageType::MapChunk, packet, SendMode::Reliable);
}

Result LobbyManager::HandleMapChunk(ByteReader& reader) {
    if (!map_receive_.has_value()) {
        // A late chunk after the transfer completed. Harmless, so it is dropped
        // rather than treated as a violation.
        return Result::Success();
    }
    // Read validates the CRC and rejects an oversized chunk.
    MPE_ASSIGN_OR_RETURN(const MapChunkBody chunk, MapChunkBody::Read(reader));

    MapReceive& receive = *map_receive_;
    if (chunk.chunk_index >= receive.manifest.chunk_count) {
        return Result::Fail(ErrorCode::ProtocolViolation,
                            std::format("chunk index {} exceeds the declared count {}",
                                        chunk.chunk_index, receive.manifest.chunk_count));
    }

    const std::size_t offset = static_cast<std::size_t>(chunk.chunk_index) * kMapChunkBytes;
    if (offset + chunk.data.size() > receive.bytes.size()) {
        return Result::Fail(ErrorCode::ProtocolViolation,
                            std::format("chunk {} of {} bytes overruns the declared total",
                                        chunk.chunk_index, chunk.data.size()));
    }

    if (!receive.chunk_present[chunk.chunk_index]) {
        std::memcpy(receive.bytes.data() + offset, chunk.data.data(), chunk.data.size());
        receive.chunk_present[chunk.chunk_index] = true;
        ++receive.chunks_received;
        MarkDirty();
    }

    if (receive.chunks_received < receive.manifest.chunk_count) {
        return Result::Success();
    }

    // Whole payload received. Verify the content hash before it is parsed: this
    // is the check that makes a host unable to hand a client a different map than
    // it announced.
    const hash::Digest256 digest = hash::Sha256::Compute(receive.bytes);
    const std::string     digest_hex = hash::ToHex(digest);
    if (digest_hex != receive.manifest.content_hash_hex) {
        map_receive_.reset();
        return Result::Fail(ErrorCode::IntegrityMismatch,
                            std::format("the assembled map hashes to {} but {} was announced",
                                        digest_hex, receive.manifest.content_hash_hex));
    }

    // Parse it before claiming to hold it. A payload that hashes correctly but
    // does not deserialize is still unusable, and finding that out now is far
    // better than at launch.
    const Expected<map::MapVariant> variant = map::ReadCanonicalBinary(receive.bytes);
    if (!variant.ok()) {
        map_receive_.reset();
        return Result::Fail(ErrorCode::ValidationFailed,
                            std::format("the received map could not be parsed: {}",
                                        variant.message()));
    }

    MapPayload payload;
    payload.name          = receive.manifest.map_name;
    payload.base_scenario = receive.manifest.base_scenario;
    payload.bytes         = std::move(receive.bytes);
    payload.digest        = digest;
    payload.digest_hex    = digest_hex;
    selected_map_         = std::move(payload);
    local_has_map_        = true;
    map_receive_.reset();

    MPE_LOG_INFO("map '{}' received and verified ({})", selected_map_->name, digest_hex);

    std::vector<std::byte> packet;
    PacketBuilder builder(packet, MessageType::MapTransferDone, Channel::MapTransfer);
    builder.Body().WriteString(digest_hex);
    MPE_TRY(SendTo(host_peer_, MessageType::MapTransferDone, packet, SendMode::Reliable));

    MarkDirty();
    return Result::Success();
}

Result LobbyManager::HandleMapTransferDone(PeerHandle peer) {
    PlayerSlot* const player = FindPlayerByPeer(peer);
    if (player == nullptr) {
        return Result::Fail(ErrorCode::ProtocolViolation,
                            "transfer completion from a peer with no roster entry");
    }
    if (!player->has_map) {
        player->has_map = true;
        MPE_LOG_INFO("{} now holds the map", player->display_name);
        BroadcastRoster();
        MarkDirty();
    }
    return Result::Success();
}

Result LobbyManager::HandleSimulationDatagram(PeerHandle peer,
                                              std::span<const std::byte> body) {
    // MultiplayerEvolved does not interpret engine traffic. The body is handed to the
    // engine's own session layer, which is what lets the shipped replication,
    // interpolation and priority systems operate unchanged over Steam.
    if (body.empty()) {
        return Result::Fail(ErrorCode::ProtocolViolation, "empty simulation datagram");
    }
    if (simulation_sink_ == nullptr || !simulation_sink_->IsReady()) {
        // Expected during the lobby and between scenarios. Dropped rather than
        // buffered: the engine's recovery assumes loss, and replaying stale state
        // later would be worse than never delivering it.
        return Result::Success();
    }
    return simulation_sink_->DeliverToEngine(peer, body);
}

// ---------------------------------------------------------------------------
// Sending helpers
// ---------------------------------------------------------------------------

Result LobbyManager::SendTo(PeerHandle peer, MessageType type,
                            const std::vector<std::byte>& packet, SendMode mode) {
    if (peer == PeerHandle::Invalid) {
        return Result::Fail(ErrorCode::PeerNotFound,
                            std::format("no peer to send {} to", ToString(type)));
    }
    return transport_.Send(peer, ExpectedChannel(type), packet, mode);
}

void LobbyManager::BroadcastRoster() {
    if (!is_host_) {
        return;
    }
    ++roster_revision_;

    std::vector<std::byte> packet;
    PacketBuilder builder(packet, MessageType::RosterUpdate, Channel::Lobby);
    RosterUpdateBody body;
    body.revision = roster_revision_;
    body.entries.reserve(players_.size());
    for (const PlayerSlot& player : players_) {
        RosterEntry entry;
        entry.platform_id       = player.platform_id;
        entry.display_name      = player.display_name;
        entry.slot              = player.slot;
        entry.team              = player.team;
        entry.is_host           = player.is_host;
        entry.is_ready          = true; // Always true on the wire, for older builds.
        entry.has_map           = player.has_map;
        entry.ping_milliseconds = player.ping_milliseconds;
        body.entries.push_back(std::move(entry));
    }
    body.Write(builder.Body());

    if (const Result sent = transport_.Broadcast(Channel::Lobby, packet, SendMode::Reliable);
        !sent.ok()) {
        MPE_LOG_DEBUG("roster broadcast failed: {}", sent.message());
    }

    if (const Result published =
            backend_.SetLobbyData(keys::kPlayerCount, std::to_string(players_.size()));
        !published.ok()) {
        MPE_LOG_DEBUG("publishing player count failed: {}", published.message());
    }
}

void LobbyManager::BroadcastSettings() {
    if (!is_host_) {
        return;
    }
    std::vector<std::byte> packet;
    PacketBuilder builder(packet, MessageType::MatchSettingsSync, Channel::Lobby);
    ToWire(settings_).Write(builder.Body());

    if (const Result sent = transport_.Broadcast(Channel::Lobby, packet, SendMode::Reliable);
        !sent.ok()) {
        MPE_LOG_DEBUG("settings broadcast failed: {}", sent.message());
    }
}

void LobbyManager::BroadcastCountdown(std::uint8_t seconds, bool cancelled,
                                      std::string_view reason) {
    std::vector<std::byte> packet;
    PacketBuilder builder(packet, MessageType::LaunchCountdown, Channel::Lobby);
    LaunchCountdownBody body;
    body.seconds_remaining = seconds;
    body.cancelled         = cancelled;
    body.cancel_reason.assign(reason);
    body.Write(builder.Body());

    if (const Result sent = transport_.Broadcast(Channel::Lobby, packet, SendMode::Reliable);
        !sent.ok()) {
        MPE_LOG_WARN("countdown broadcast failed: {}", sent.message());
    }
    transport_.Flush();
}

// ---------------------------------------------------------------------------
// Map and load
// ---------------------------------------------------------------------------

Result LobbyManager::LoadSelectedMap(std::string_view path) {
    map::ParseOptions options;
    // Host side parsing is strict. A map with warnings is playable, but one with
    // errors would produce a different layout on a peer running a stricter build.
    options.treat_warnings_as_errors = false;

    MPE_ASSIGN_OR_RETURN(map::ParseResult parsed,
                        map::ParseJsonFile(std::filesystem::path(path), options));

    for (const map::Diagnostic& diagnostic : parsed.diagnostics) {
        if (diagnostic.severity == map::Severity::Warning) {
            MPE_LOG_WARN("map '{}': {} ({})", path, diagnostic.message, diagnostic.json_path);
        }
    }

    std::vector<std::byte> canonical = map::WriteCanonicalBinary(parsed.variant);
    if (canonical.size() > MapManifestBody::kMaxTotalBytes) {
        return Result::Fail(ErrorCode::ValidationFailed,
                            std::format("the map serializes to {} bytes, which exceeds the {} byte "
                                        "transfer ceiling",
                                        canonical.size(), MapManifestBody::kMaxTotalBytes));
    }

    MapPayload payload;
    payload.name          = parsed.variant.name;
    payload.base_scenario = parsed.variant.base_scenario;
    payload.digest        = hash::Sha256::Compute(canonical);
    payload.digest_hex    = hash::ToHex(payload.digest);
    payload.bytes         = std::move(canonical);

    // The scenario is dictated by the map, never chosen independently, so the two
    // can never disagree.
    settings_.scenario     = payload.base_scenario;
    settings_.variant_name = payload.name;

    MPE_LOG_INFO("selected map '{}' on scenario '{}' ({} bytes, {})", payload.name,
                payload.base_scenario, payload.bytes.size(), payload.digest_hex);

    selected_map_  = std::move(payload);
    local_has_map_ = true;
    return Result::Success();
}

Result LobbyManager::BeginMapTransferTo(PeerHandle peer) {
    if (!selected_map_.has_value()) {
        return Result::Success(); // Nothing to send.
    }

    const MapPayload& payload = *selected_map_;
    std::vector<std::byte> packet;
    PacketBuilder builder(packet, MessageType::MapManifest, Channel::MapTransfer);
    MapManifestBody manifest;
    manifest.map_name         = payload.name;
    manifest.content_hash_hex = payload.digest_hex;
    manifest.total_bytes      = static_cast<std::uint32_t>(payload.bytes.size());
    manifest.chunk_count      = static_cast<std::uint32_t>(
        (payload.bytes.size() + kMapChunkBytes - 1) / kMapChunkBytes);
    manifest.base_scenario = payload.base_scenario;

    // Validated before sending, so a bug here surfaces on the host rather than as
    // a mysterious rejection on a client.
    MPE_TRY(manifest.Validate());
    manifest.Write(builder.Body());

    return SendTo(peer, MessageType::MapManifest, packet, SendMode::Reliable);
}

Result LobbyManager::BeginLoad(std::string_view scenario, std::uint32_t seed) {
    engine::MatchSettings applied = settings_;
    applied.scenario    = std::string(scenario);
    applied.random_seed = seed;
    MPE_TRY(applied.Validate());
    MPE_TRY(engine_.ApplyMatchSettings(applied));

    // The map variant is handed over before the scenario loads, so the engine's
    // own variant loader places objects as part of the load rather than after it.
    if (selected_map_.has_value()) {
        MPE_TRY(engine_.LoadMapVariant(selected_map_->digest_hex));
    }

    MPE_TRY(engine_.BeginLoadScenario(scenario, seed));

    for (PlayerSlot& player : players_) {
        player.load_progress = 0.0f;
    }
    TransitionTo(LobbyPhase::Loading, "Loading");
    MPE_LOG_INFO("loading scenario '{}' with seed {}", scenario, seed);
    return Result::Success();
}

void LobbyManager::ApplyBandwidthBudget() {
    const std::size_t peer_count = players_.empty() ? 1 : players_.size() - 1;
    const std::uint32_t divisor = static_cast<std::uint32_t>(std::max<std::size_t>(1, peer_count));
    const std::uint32_t per_peer =
        std::max(kMinimumPerPeerBytesPerSecond, kAssumedHostUplinkBytesPerSecond / divisor);

    if (const Result applied = engine_.SetSimulationBandwidth(per_peer); !applied.ok()) {
        MPE_LOG_WARN("setting the simulation bandwidth to {} B/s failed: {}", per_peer,
                    applied.message());
        return;
    }
    MPE_LOG_DEBUG("simulation bandwidth set to {} B/s per peer for {} peer(s)", per_peer,
                 peer_count);
}

// ---------------------------------------------------------------------------
// Roster helpers
// ---------------------------------------------------------------------------

PlayerSlot* LobbyManager::FindPlayer(PlatformId id) noexcept {
    const auto it = std::find_if(players_.begin(), players_.end(),
                                 [id](const PlayerSlot& p) { return p.platform_id == id; });
    return it == players_.end() ? nullptr : &*it;
}

PlayerSlot* LobbyManager::FindPlayerByPeer(PeerHandle peer) noexcept {
    if (peer == PeerHandle::Invalid) {
        return nullptr;
    }
    const auto it = std::find_if(players_.begin(), players_.end(),
                                 [peer](const PlayerSlot& p) { return p.peer == peer; });
    return it == players_.end() ? nullptr : &*it;
}

std::uint8_t LobbyManager::AllocateSlot() const {
    // Lowest unused index, so a player who leaves frees their slot for the next
    // joiner and the roster stays compact.
    for (std::uint8_t candidate = 0; candidate < 32; ++candidate) {
        const bool taken = std::any_of(players_.begin(), players_.end(),
                                       [candidate](const PlayerSlot& p) {
                                           return p.slot == candidate;
                                       });
        if (!taken) {
            return candidate;
        }
    }
    return 31;
}

std::uint8_t LobbyManager::AssignTeam() const {
    if (settings_.team_count <= 1) {
        return 0;
    }
    // Smallest team wins the new player, which keeps teams balanced as people
    // join and leave without any manual shuffling.
    std::vector<std::size_t> counts(settings_.team_count, 0);
    for (const PlayerSlot& player : players_) {
        if (player.team < counts.size()) {
            ++counts[player.team];
        }
    }

    const std::size_t fewest = *std::min_element(counts.begin(), counts.end());

    // Ties are broken at random rather than by team number.
    //
    // Taking the first smallest is correct arithmetic and a bad experience: every session
    // starts level, so the first player to arrive always landed on blue, and with people
    // joining and leaving in pairs the same side filled first every time. Choosing among
    // the tied teams means an even lobby is genuinely even.
    std::vector<std::uint8_t> candidates;
    candidates.reserve(counts.size());
    for (std::size_t team = 0; team < counts.size(); ++team) {
        if (counts[team] == fewest) {
            candidates.push_back(static_cast<std::uint8_t>(team));
        }
    }
    if (candidates.size() == 1) {
        return candidates.front();
    }

    static std::mt19937 generator{std::random_device{}()};
    std::uniform_int_distribution<std::size_t> pick(0, candidates.size() - 1);
    return candidates[pick(generator)];
}

void LobbyManager::RemovePlayerByPeer(PeerHandle peer) {
    const auto removed = std::remove_if(players_.begin(), players_.end(),
                                        [peer](const PlayerSlot& p) { return p.peer == peer; });
    players_.erase(removed, players_.end());
}

// ---------------------------------------------------------------------------
// Phase management
// ---------------------------------------------------------------------------

void LobbyManager::TransitionTo(LobbyPhase phase, std::string_view status_text) {
    const LobbyPhase previous = phase_;
    if (previous == phase) {
        // Still refresh the status text: the same phase can carry new detail, for
        // example a cancellation reason while staying in the lobby.
        snapshot_.status_text.assign(status_text);
        MarkDirty();
        return;
    }

    phase_         = phase;
    phase_elapsed_ = 0.0;
    snapshot_.status_text.assign(status_text);

    MPE_LOG_INFO("phase {} -> {} ({})", ToString(previous), ToString(phase), status_text);

    if (is_host_ && backend_.InLobby()) {
        if (const Result published = backend_.SetLobbyData(keys::kLobbyPhase, ToString(phase));
            !published.ok()) {
            MPE_LOG_DEBUG("publishing the phase failed: {}", published.message());
        }
        // The lobby stays as the host asked for it, in every phase.
        //
        // It used to be reopened as friends-only whenever the phase changed, which meant a
        // session hosted as public was demoted the moment it finished being created and
        // never appeared in anybody's browser. Then it was closed to invite-only while
        // loading or in a match, on the reasoning that nobody should join into a load they
        // cannot participate in.
        //
        // That reasoning belongs at the join, not at the advertisement. Hiding a running
        // game means a friend who arrives two minutes late cannot find it, cannot be
        // invited into it, and has no way to tell that it exists at all. Whether a late
        // arrival can be admitted is the host's decision to make when they ask, and it is
        // made in HandleHandshakeRequest where the answer can be explained.
        if (const Result visibility = backend_.SetVisibility(hosted_visibility_);
            !visibility.ok()) {
            MPE_LOG_DEBUG("adjusting visibility failed: {}", visibility.message());
        }
    }

    MarkDirty();
    RebuildSnapshot();
    sink_.OnPhaseChanged(previous, phase);
}

void LobbyManager::Fault(const Error& error) {
    MPE_LOG_ERROR("faulted: {} ({})", error.message, ToString(error.code));

    snapshot_.last_error = error.message;
    // The transport and lobby are released immediately, but the phase is left as
    // Faulted rather than Idle so the UI can show the reason until the player
    // acknowledges it by leaving.
    transport_.Shutdown();
    backend_.Leave();
    players_.clear();
    map_receive_.reset();
    host_peer_ = PeerHandle::Invalid;

    TransitionTo(LobbyPhase::Faulted, error.message);
    sink_.OnError(error);
}

void LobbyManager::RebuildSnapshot() {
    snapshot_dirty_ = false;

    snapshot_.phase    = phase_;
    snapshot_.is_host  = is_host_;
    snapshot_.lobby_id = lobby_id_;
    snapshot_.settings = settings_;
    snapshot_.players  = players_;
    snapshot_.countdown_seconds =
        static_cast<std::uint8_t>(std::max(0.0, std::ceil(countdown_remaining_)));

    if (map_receive_.has_value() && map_receive_->manifest.chunk_count > 0) {
        snapshot_.map_transfer_progress =
            static_cast<float>(map_receive_->chunks_received) /
            static_cast<float>(map_receive_->manifest.chunk_count);
    } else {
        snapshot_.map_transfer_progress = local_has_map_ ? 1.0f : 0.0f;
    }

    // Stable ordering by slot so the UI never reorders rows between frames.
    std::sort(snapshot_.players.begin(), snapshot_.players.end(),
              [](const PlayerSlot& a, const PlayerSlot& b) { return a.slot < b.slot; });

    sink_.OnSnapshotChanged(snapshot_);
}

} // namespace mpe::lobby
