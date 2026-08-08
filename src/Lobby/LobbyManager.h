// SPDX-License-Identifier: MIT
// MultiplayerEvolved: Lobby/LobbyManager.h
//
// The lobby state machine: one owner for the entire path from a player pressing
// Multiplayer to every peer standing in the same match.
//
// WHAT IT COORDINATES
//
//   ILobbyBackend    discovery, membership, invitations   (metadata plane)
//   IPeerTransport   handshake, control, map, simulation  (data plane)
//   IEngineControl   session config, load, launch         (game plane)
//
// LobbyManager is the only class that talks to all three. Each of them is an
// interface, so the whole machine runs headless in tests against fakes, and a
// dedicated server swaps the transport without touching this file.
//
// AUTHORITY MODEL
//
// The host is authoritative over everything a player can see: roster, settings,
// map selection, countdown, launch. A client never mutates shared
// state directly; it sends a request and waits for the host's snapshot to come
// back. That is why RosterUpdate is a full snapshot rather than a delta. A client that tries to
// act with authority is disconnected by the protocol gate in PacketProtocol
// before its message body is even parsed.
//
// LAUNCH SEQUENCE, mirroring the original Combat Evolved transition
//
//   1. Host verifies every peer holds the selected map.
//   2. Host broadcasts LaunchCountdown once per second. Any peer losing the map
//      or disconnecting cancels it with a reason everyone sees.
//   3. At zero the host broadcasts LaunchNow carrying the scenario, the map
//      content hash and a shared random seed, then begins loading itself.
//   4. Every peer loads and reports LoadProgress. Nobody starts simulating.
//   5. When every peer reports 1.0 the host broadcasts AllPeersLoaded and calls
//      LaunchMatch. Peers release on receipt.
//
// Step 4 is what makes the transition feel like the original: the match becomes
// live on every machine at the same tick, rather than early joiners spawning
// into an empty map while others still load.
//
// THREADING
//
// Single threaded by construction. Tick is called from the mod thread, and both
// Poll implementations guarantee their callbacks arrive on the calling thread.
// There are no locks in this class, and adding one would be a sign that the
// threading contract has been broken somewhere below.
#pragma once

#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "Core/Hash.h"
#include "Core/Result.h"
#include "Engine/IEngineControl.h"
#include "Lobby/ILobbyBackend.h"
#include "Net/IPeerTransport.h"
#include "Net/ISimulationSink.h"
#include "Net/PacketProtocol.h"

namespace mpe::lobby {

/// Externally visible phase. Drives the UI and is published as lobby metadata so
/// a prospective joiner can see that a match is already underway.
enum class LobbyPhase : std::uint8_t {
    Idle = 0,        ///< No lobby.
    Creating,        ///< Host: waiting for the platform to create the lobby.
    Hosting,         ///< Host: lobby open, accepting players.
    Joining,         ///< Client: entering the platform lobby.
    Connecting,      ///< Client: transport connecting to the host.
    Handshaking,     ///< Client: transport up, awaiting HandshakeAccept.
    InLobby,         ///< Client: in the lobby, waiting for the host to start.
    Countdown,       ///< Launch countdown running.
    Loading,         ///< Every peer loading the scenario.
    InMatch,
    PostMatch,
    Faulted,         ///< Unrecoverable; the only exit is LeaveSession.
};

[[nodiscard]] std::string_view ToString(LobbyPhase phase) noexcept;

/// One player as the lobby sees them. Merges platform identity with transport state.
struct PlayerSlot {
    PlatformId       platform_id{0};
    std::string      display_name;
    net::PeerHandle  peer{net::PeerHandle::Invalid}; ///< Invalid for the local player.
    std::uint8_t     slot{0};
    std::uint8_t     team{0};
    bool             is_host{false};
    bool             is_local{false};
    /// Always true. Readiness was removed and this is kept only because the roster
    /// carries one byte for it on the wire, which an older build still expects.
    bool             is_ready{true};
    bool             has_map{false};
    float            load_progress{0.0f};
    std::uint16_t    ping_milliseconds{0};
};

/// Immutable view of everything the UI needs, rebuilt whenever state changes.
struct LobbySnapshot {
    LobbyPhase               phase{LobbyPhase::Idle};
    bool                     is_host{false};
    LobbyId                  lobby_id{0};
    engine::MatchSettings    settings;
    std::vector<PlayerSlot>  players;
    std::uint8_t             countdown_seconds{0};
    std::string              status_text;      ///< Human readable phase detail.
    std::string              last_error;       ///< Empty when there is no error.
    float                    map_transfer_progress{0.0f}; ///< Zero to one.

    /// Both are now trivially true. Readiness was removed: everybody in a session is
    /// ready, always. Kept because callers outside this file still ask.
    [[nodiscard]] std::size_t ReadyCount() const noexcept;
    [[nodiscard]] bool EveryoneReady() const noexcept;
};

/// UI facing notifications. Deliberately coarse: the UI re-reads the snapshot
/// rather than tracking deltas, which keeps it impossible for the UI to hold a
/// view that the state machine has moved past.
class ILobbyEventSink {
public:
    virtual ~ILobbyEventSink() = default;

    virtual void OnPhaseChanged(LobbyPhase previous, LobbyPhase current) = 0;
    virtual void OnSnapshotChanged(const LobbySnapshot& snapshot) = 0;
    virtual void OnChatMessage(PlatformId author, std::string_view display_name,
                               std::string_view text) = 0;

    /// A failure the player needs to see. The lobby has already handled it; this
    /// is for presentation.
    virtual void OnError(const Error& error) = 0;
};

/// Host side options.
struct HostOptions {
    LobbyVisibility visibility{LobbyVisibility::FriendsOnly};

    /// Total players including the host. The transport's client cap is derived
    /// from this.
    std::uint32_t max_players{8};

    engine::MatchSettings settings;

    /// Absolute path to a .fmap.json map variant, or empty for the base scenario
    /// with no custom layout.
    std::string map_variant_path;
};

/// Tunables with defaults chosen to be forgiving on a poor connection while
/// never leaving a player staring at a frozen screen.
struct LobbyTimings {
    /// Entering the platform lobby. One Steam round trip, normally instant.
    ///
    /// Bounded because it used not to be, and an unbounded wait on a step that can silently
    /// never complete is a session stuck forever with no error to show for it.
    double join_timeout_seconds{15.0};
    double handshake_timeout_seconds{20.0};
    /// How often a client re-greets a host that has not answered.
    double handshake_resend_seconds{2.0};
    double connect_timeout_seconds{30.0};
    double load_timeout_seconds{180.0};
    double keepalive_interval_seconds{2.0};
    double roster_broadcast_interval_seconds{1.0};

    /// How long a client waits without hearing from the host before giving up on it.
    ///
    /// The host rebroadcasts the roster every second while a lobby is open, so silence for
    /// twelve of those is twelve missed messages and not a slow frame. Short enough that a
    /// player is not left staring at a lobby nobody is running, long enough that a stall
    /// does not throw them out of one that is.
    double host_silence_seconds{12.0};
    std::uint8_t countdown_seconds{5};
};

class LobbyManager final : public ILobbyBackendObserver, public net::ITransportObserver {
public:
    /// All three dependencies are required and must outlive the manager. Passed
    /// by reference rather than owned, because the mod entry point owns them and
    /// controls teardown order.
    LobbyManager(ILobbyBackend& backend, net::IPeerTransport& transport,
                 engine::IEngineControl& engine, ILobbyEventSink& sink,
                 LobbyTimings timings = {});

    ~LobbyManager() override;

    LobbyManager(const LobbyManager&)            = delete;
    LobbyManager& operator=(const LobbyManager&) = delete;

    // --- Commands from the UI ---------------------------------------------
    /// Validates settings and capabilities, then creates the lobby. Fails fast
    /// and changes no state when the engine cannot host.
    [[nodiscard]] Result HostSession(const HostOptions& options);

    /// Joins a lobby the player was invited to.
    [[nodiscard]] Result JoinSession(LobbyId lobby);

    /// Tears down cleanly from any phase, including Faulted. Idempotent.
    void LeaveSession();

    [[nodiscard]] Result SendChat(std::string_view text);
    [[nodiscard]] Result OpenInviteOverlay();

    // Host only. Each fails with InvalidState on a client.
    [[nodiscard]] Result SelectGameMode(engine::GameMode mode);
    [[nodiscard]] Result SelectMapVariant(std::string_view path);
    [[nodiscard]] Result UpdateMatchSettings(const engine::MatchSettings& settings);
    [[nodiscard]] Result StartCountdown();
    [[nodiscard]] Result CancelCountdown(std::string_view reason);
    [[nodiscard]] Result KickPlayer(PlatformId player);
    [[nodiscard]] Result EndMatch();

    // --- Pump -------------------------------------------------------------
    /// Drives everything: platform events, transport events, timers, engine
    /// lifecycle transitions. Must be called every frame.
    void Tick(double delta_seconds);

    [[nodiscard]] const LobbySnapshot& Snapshot() const noexcept { return snapshot_; }
    [[nodiscard]] LobbyPhase Phase() const noexcept { return phase_; }
    [[nodiscard]] bool IsHost() const noexcept { return is_host_; }

    /// Attaches the engine side of the simulation tunnel. Optional: without it
    /// the lobby works fully and simulation datagrams are dropped, which is the
    /// correct behaviour before the engine bridge has resolved. Pass nullptr to
    /// detach, for example while the engine is tearing a scenario down.
    void SetSimulationSink(net::ISimulationSink* sink) noexcept { simulation_sink_ = sink; }

private:
    // --- ILobbyBackendObserver -------------------------------------------
    void OnLobbyCreated(LobbyId lobby) override;
    void OnLobbyCreateFailed(const Error& error) override;
    void OnLobbyEntered(LobbyId lobby, bool is_owner) override;
    void OnLobbyEnterFailed(const Error& error) override;
    void OnMemberJoined(const LobbyMember& member) override;
    void OnMemberLeft(PlatformId member, bool was_kicked) override;
    void OnLobbyDataChanged(LobbyId lobby) override;
    void OnMemberDataChanged(PlatformId member) override;
    void OnJoinRequested(LobbyId lobby, PlatformId inviter) override;

    // --- ITransportObserver ----------------------------------------------
    void OnPeerConnected(net::PeerHandle peer, const net::PeerIdentity& identity) override;
    void OnPeerDisconnected(net::PeerHandle peer, net::DisconnectReason reason,
                            std::string_view detail) override;
    void OnPacketReceived(net::PeerHandle peer, net::Channel channel,
                          std::span<const std::byte> payload) override;
    void OnConnectFailed(net::DisconnectReason reason, std::string_view detail) override;

    // --- Message handling -------------------------------------------------
    /// Validates the packet against the current phase and role, then routes it.
    /// Any failure disconnects the peer rather than ignoring the message,
    /// because a peer sending traffic we cannot account for is not a peer we can
    /// keep a consistent match with.
    void HandlePacket(net::PeerHandle peer, net::Channel channel,
                      std::span<const std::byte> payload);

    [[nodiscard]] Result HandleHandshakeRequest(net::PeerHandle peer, ByteReader& reader);
    [[nodiscard]] Result HandleHandshakeAccept(ByteReader& reader);
    [[nodiscard]] Result HandleHandshakeReject(ByteReader& reader);
    /// Greets the host, or greets it again when it has not answered.
    [[nodiscard]] Result SendHandshakeRequest();

    [[nodiscard]] Result HandleRosterUpdate(ByteReader& reader);
    [[nodiscard]] Result HandleMatchSettings(ByteReader& reader);
    [[nodiscard]] Result HandleReadyStateChange(net::PeerHandle peer, ByteReader& reader);
    [[nodiscard]] Result HandleLaunchCountdown(ByteReader& reader);
    [[nodiscard]] Result HandleLaunchNow(ByteReader& reader);
    [[nodiscard]] Result HandleLoadProgress(net::PeerHandle peer, ByteReader& reader);
    [[nodiscard]] Result HandleAllPeersLoaded();
    [[nodiscard]] Result HandleMatchEnded();
    [[nodiscard]] Result HandleChatMessage(net::PeerHandle peer, ByteReader& reader);
    [[nodiscard]] Result HandleMapManifest(ByteReader& reader);
    [[nodiscard]] Result HandleMapChunkRequest(net::PeerHandle peer, ByteReader& reader);
    [[nodiscard]] Result HandleMapChunk(ByteReader& reader);
    [[nodiscard]] Result HandleMapTransferDone(net::PeerHandle peer);
    [[nodiscard]] Result HandleSimulationDatagram(net::PeerHandle peer,
                                                 std::span<const std::byte> body);

    // --- Sending helpers --------------------------------------------------
    [[nodiscard]] Result SendTo(net::PeerHandle peer, net::MessageType type,
                                const std::vector<std::byte>& packet, net::SendMode mode);
    void BroadcastRoster();
    void BroadcastSettings();
    void BroadcastCountdown(std::uint8_t seconds, bool cancelled, std::string_view reason);

    // --- Phase management -------------------------------------------------
    void TransitionTo(LobbyPhase phase, std::string_view status_text);
    void Fault(const Error& error);
    void RebuildSnapshot();

    /// Marks the snapshot dirty. Coalesced so a burst of changes produces one
    /// UI notification per tick instead of one per change.
    void MarkDirty() noexcept { snapshot_dirty_ = true; }

    // --- Roster -----------------------------------------------------------
    [[nodiscard]] PlayerSlot* FindPlayer(PlatformId id) noexcept;
    [[nodiscard]] PlayerSlot* FindPlayerByPeer(net::PeerHandle peer) noexcept;
    [[nodiscard]] std::uint8_t AllocateSlot() const;
    [[nodiscard]] std::uint8_t AssignTeam() const;
    void RemovePlayerByPeer(net::PeerHandle peer);

    // --- Host side steps --------------------------------------------------
    void TickHost(double delta_seconds);
    void TickClient(double delta_seconds);
    void TickCountdown(double delta_seconds);
    void TickLoading(double delta_seconds);

    /// Reduces the engine's per peer bandwidth as the roster grows so the host's
    /// uplink is not oversubscribed. Called whenever the roster changes.
    void ApplyBandwidthBudget();

    /// Sends the manifest for the selected map to one peer, or notes that the
    /// peer already holds it.
    [[nodiscard]] Result BeginMapTransferTo(net::PeerHandle peer);

    /// Loads and validates the selected map, producing the canonical payload and
    /// its content hash. Host only, called from SelectMapVariant.
    [[nodiscard]] Result LoadSelectedMap(std::string_view path);

    /// Applies settings to the engine and begins loading. Shared by host and
    /// client so both take exactly the same path into the match.
    [[nodiscard]] Result BeginLoad(std::string_view scenario, std::uint32_t seed);

    // --- Dependencies -----------------------------------------------------
    ILobbyBackend&          backend_;
    net::IPeerTransport&    transport_;
    engine::IEngineControl& engine_;
    ILobbyEventSink&        sink_;
    LobbyTimings            timings_;

    // --- State ------------------------------------------------------------
    LobbyPhase  phase_{LobbyPhase::Idle};
    bool        is_host_{false};
    PlatformId  local_id_{0};
    LobbyId     lobby_id_{0};
    std::uint32_t max_players_{8};
    /// What the host asked for, so a phase change can restore it rather than guess.
    LobbyVisibility hosted_visibility_{LobbyVisibility::Public};

    engine::MatchSettings   settings_;
    std::vector<PlayerSlot> players_;
    std::uint32_t           roster_revision_{0};

    LobbySnapshot snapshot_;
    bool          snapshot_dirty_{true};

    /// Set on a client: the transport handle of the host.
    net::PeerHandle host_peer_{net::PeerHandle::Invalid};

    /// Host identity read from lobby metadata while joining.
    net::PeerIdentity pending_host_identity_;

    // Timers, all in seconds.
    double phase_elapsed_{0.0};
    double keepalive_elapsed_{0.0};

    /// Since the last time this client greeted the host.
    ///
    /// The greeting used to be sent once, so anything that lost it or its reply cost the
    /// whole handshake timeout and then a failure. Repeating it turns that into a pause.
    double handshake_resend_elapsed_{0.0};
    double roster_broadcast_elapsed_{0.0};
    /// Time since a client last heard anything at all from the host.
    double host_silence_elapsed_{0.0};
    double countdown_remaining_{0.0};
    /// Wall clock instant the countdown ends, in epoch milliseconds.
    ///
    /// A deadline rather than a running total, because a total is only as accurate as the
    /// tick that decrements it and a stalled tick stretched a five second countdown into a
    /// minute while still announcing five.
    std::int64_t countdown_deadline_ms_{0};
    std::uint8_t countdown_last_announced_{0};

    /// Host: the serialized map every client must hold.
    struct MapPayload {
        std::string            name;
        std::string            base_scenario;
        std::vector<std::byte> bytes;
        hash::Digest256        digest{};
        std::string            digest_hex;
    };
    std::optional<MapPayload> selected_map_;

    /// Client: in progress reassembly.
    struct MapReceive {
        net::MapManifestBody   manifest;
        std::vector<std::byte> bytes;
        std::vector<bool>      chunk_present;
        std::uint32_t          chunks_received{0};
        double                 elapsed{0.0};
    };
    std::optional<MapReceive> map_receive_;

    /// Client: whether the local machine holds the host's selected map.
    bool local_has_map_{false};

    /// Engine side of the simulation tunnel. Not owned; may be null.
    net::ISimulationSink* simulation_sink_{nullptr};
};

} // namespace mpe::lobby
