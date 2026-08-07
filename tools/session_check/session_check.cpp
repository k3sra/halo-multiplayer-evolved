// SPDX-License-Identifier: MIT
// MultiplayerEvolved: tools/session_check/session_check.cpp
//
// Runs a host and a client against each other, in one process, with no Steam.
//
// WHY THIS EXISTS
//
// Everything that only happens when two people are in a session was, until this existed,
// verified by two people being in a session. That is a slow, unreliable and expensive way
// to find out that a client hangs up on a roster message, and it is how that bug was found:
// two machines, two players, a twenty second timeout and a log from each.
//
// LobbyManager talks to three interfaces and owns no platform types, which was always the
// point. Two of them wired back to back through a loopback transport is a real host and a
// real client exchanging real packets through the real protocol, and it runs in a second on
// one machine. The parts it cannot cover are the ones that are genuinely Steam's: relay
// routing, lobby search, and invitations.
//
// What it does cover is every rule the two managers apply to each other.
#include <cstdio>
#include <deque>
#include <map>
#include <string>
#include <vector>

#include "Engine/IEngineControl.h"
#include "Lobby/LobbyManager.h"
#include "Net/IPeerTransport.h"
#include "Net/PacketProtocol.h"

namespace {

using namespace mpe;
using namespace mpe::lobby;
using namespace mpe::net;

int g_failures = 0;

void Check(bool condition, const std::string& what) {
    std::printf(condition ? "  ok    %s\n" : "  FAIL  %s\n", what.c_str());
    if (!condition) {
        ++g_failures;
    }
}

// ---------------------------------------------------------------------------
// Fakes
// ---------------------------------------------------------------------------

/// An engine that accepts everything and remembers what it was told.
class FakeEngine final : public engine::IEngineControl {
public:
    [[nodiscard]] engine::EngineCapabilities Capabilities() const override {
        engine::EngineCapabilities caps;
        caps.can_execute_commands     = true;
        caps.can_configure_session    = true;
        caps.can_load_map_variant     = true;
        caps.can_place_sandbox_objects = true;
        caps.can_query_load_progress  = true;
        return caps;
    }
    [[nodiscard]] engine::EngineLifecycle Lifecycle() const override {
        return engine::EngineLifecycle::Idle;
    }
    Result SetSessionClass(engine::SessionClass) override { return Result::Success(); }
    Result SetSessionPrivacy(engine::SessionPrivacy) override { return Result::Success(); }
    Result SetSimulationBandwidth(std::uint32_t) override { return Result::Success(); }
    Result SetHostMigrationEnabled(bool) override { return Result::Success(); }
    Result ApplyMatchSettings(const engine::MatchSettings& settings) override {
        applied = settings;
        return Result::Success();
    }
    Result BeginLoadScenario(std::string_view, std::uint32_t) override {
        return Result::Success();
    }
    [[nodiscard]] Expected<float> QueryLoadProgress() const override { return 1.0f; }
    Result LaunchMatch() override { return Result::Success(); }
    Result EndMatch() override { return Result::Success(); }
    Result ReturnToFrontEnd() override { return Result::Success(); }
    Result LoadMapVariant(std::string_view) override { return Result::Success(); }
    Result ClearSandbox() override { return Result::Success(); }
    [[nodiscard]] Expected<engine::SandboxObjectHandle> SpawnSandboxObject(
        const engine::SandboxPlacement&) override {
        return engine::SandboxObjectHandle{1};
    }
    Result DespawnSandboxObject(engine::SandboxObjectHandle) override {
        return Result::Success();
    }
    [[nodiscard]] Expected<std::int32_t> ResolvePaletteIndex(std::string_view) const override {
        return 0;
    }
    Result ExecuteConsoleCommand(std::string_view) override { return Result::Success(); }

    engine::MatchSettings applied;
};

/// A lobby backend with no platform behind it.
class FakeBackend final : public ILobbyBackend {
public:
    FakeBackend(PlatformId id, std::string name) : id_(id), name_(std::move(name)) {}

    Result Create(LobbyVisibility visibility, std::uint32_t) override {
        in_lobby_   = true;
        owner_      = true;
        visibility_ = visibility;
        return Result::Success();
    }
    Result Join(LobbyId) override {
        in_lobby_ = true;
        owner_    = false;
        return Result::Success();
    }
    void Leave() override { in_lobby_ = false; owner_ = false; }
    [[nodiscard]] bool InLobby() const noexcept override { return in_lobby_; }
    [[nodiscard]] LobbyId CurrentLobby() const noexcept override { return in_lobby_ ? 77 : 0; }
    [[nodiscard]] bool IsOwner() const noexcept override { return owner_; }

    Result SetLobbyData(std::string_view key, std::string_view value) override {
        data_[std::string(key)] = std::string(value);
        return Result::Success();
    }
    [[nodiscard]] Expected<std::string> GetLobbyData(std::string_view key) const override {
        const auto it = data_.find(std::string(key));
        if (it == data_.end()) {
            return Error{ErrorCode::NotFound, "no such key"};
        }
        return it->second;
    }
    Result SetMemberData(std::string_view, std::string_view) override {
        return Result::Success();
    }
    [[nodiscard]] Expected<std::string> GetMemberData(PlatformId,
                                                       std::string_view) const override {
        return Error{ErrorCode::NotFound, "no member data"};
    }
    [[nodiscard]] std::vector<LobbyMember> Members() const override { return {}; }
    [[nodiscard]] std::size_t MemberCount() const noexcept override { return 0; }
    Result SetVisibility(LobbyVisibility visibility) override {
        visibility_ = visibility;
        return Result::Success();
    }
    Result OpenInviteOverlay() override { return Result::Success(); }
    Result PublishJoinablePresence(std::string_view) override { return Result::Success(); }
    void ClearJoinablePresence() override {}
    [[nodiscard]] Expected<PlatformId> LocalId() const override { return id_; }
    [[nodiscard]] Expected<std::string> LocalDisplayName() const override { return name_; }
    void Poll(ILobbyBackendObserver& observer) override {
        // The manager's observer methods are private and belong to the interface, so this
        // is the only way in, which is also how the real backend delivers them.
        while (!created_.empty()) {
            const LobbyId lobby = created_.front();
            created_.pop_front();
            observer.OnLobbyCreated(lobby);
        }
        while (!entered_.empty()) {
            const auto entry = entered_.front();
            entered_.pop_front();
            observer.OnLobbyEntered(entry.first, entry.second);
        }
    }

    void RaiseCreated(LobbyId lobby) { created_.push_back(lobby); }
    void RaiseEntered(LobbyId lobby, bool owner) { entered_.push_back({lobby, owner}); }

    [[nodiscard]] LobbyVisibility Visibility() const { return visibility_; }
    [[nodiscard]] std::string Published(const std::string& key) const {
        const auto it = data_.find(key);
        return it == data_.end() ? std::string{} : it->second;
    }

private:
    PlatformId                        id_;
    std::string                       name_;
    bool                              in_lobby_{false};
    bool                              owner_{false};
    LobbyVisibility                   visibility_{LobbyVisibility::Public};
    std::map<std::string, std::string> data_;
    std::deque<LobbyId>                created_;
    std::deque<std::pair<LobbyId, bool>> entered_;
};

/// Two of these are wired to each other, so a Send on one becomes a Poll delivery on the
/// other. Ordering is per channel, which is the whole point: the real transport gives no
/// ordering between channels, and pretending otherwise is what hid the handshake bug.
class LoopbackTransport final : public IPeerTransport {
public:
    struct Datagram {
        Channel                channel;
        std::vector<std::byte> payload;
    };

    explicit LoopbackTransport(std::uint64_t id) : id_(id) {}

    void ConnectTo(LoopbackTransport& other, PeerHandle local_handle,
                   PeerHandle remote_handle) {
        peer_        = &other;
        peer_handle_ = local_handle;
        other.peer_        = this;
        other.peer_handle_ = remote_handle;
    }

    [[nodiscard]] TransportKind Kind() const noexcept override {
        return TransportKind::Loopback;
    }
    [[nodiscard]] Expected<PeerIdentity> LocalIdentity() const override {
        return PeerIdentity{id_};
    }
    Result Listen(const ListenConfig&) override { listening_ = true; return Result::Success(); }
    Result Connect(const PeerIdentity&, std::uint32_t) override { return Result::Success(); }
    void Shutdown() override { listening_ = false; }
    [[nodiscard]] bool IsListening() const noexcept override { return listening_; }
    [[nodiscard]] std::size_t PeerCount() const noexcept override {
        return peer_ != nullptr ? 1u : 0u;
    }
    [[nodiscard]] std::vector<PeerHandle> Peers() const override {
        return peer_ != nullptr ? std::vector<PeerHandle>{peer_handle_}
                                : std::vector<PeerHandle>{};
    }
    [[nodiscard]] Expected<PeerIdentity> IdentityOf(PeerHandle) const override {
        if (peer_ == nullptr) {
            return Error{ErrorCode::NotFound, "no peer"};
        }
        return PeerIdentity{peer_->id_};
    }

    Result Send(PeerHandle, Channel channel, std::span<const std::byte> payload,
                SendMode) override {
        if (peer_ == nullptr) {
            return Result::Fail(ErrorCode::TransportUnavailable, "not connected");
        }
        peer_->inbox_[static_cast<std::size_t>(channel)].push_back(
            Datagram{channel, std::vector<std::byte>(payload.begin(), payload.end())});
        return Result::Success();
    }
    Result Broadcast(Channel channel, std::span<const std::byte> payload,
                     SendMode mode) override {
        return Send(peer_handle_, channel, payload, mode);
    }
    Result BroadcastExcept(PeerHandle, Channel channel, std::span<const std::byte> payload,
                           SendMode mode) override {
        return Send(peer_handle_, channel, payload, mode);
    }
    void Flush() override {}

    void Poll(ITransportObserver& observer) override {
        if (announce_connect_ && peer_ != nullptr) {
            announce_connect_ = false;
            observer.OnPeerConnected(peer_handle_, PeerIdentity{peer_->id_});
        }

        // Channels are drained in the order chosen by deliver_order_, so a test can make
        // the lobby channel arrive before the control channel. That is not a contrivance:
        // separate channels are separate lanes with no ordering between them, and the
        // first client that ever reached a host disconnected it over exactly this.
        for (const std::size_t channel : deliver_order_) {
            auto& queue = inbox_[channel];
            while (!queue.empty()) {
                const Datagram datagram = queue.front();
                queue.pop_front();
                observer.OnPacketReceived(peer_handle_, datagram.channel, datagram.payload);
            }
        }
    }

    void Disconnect(PeerHandle, DisconnectReason reason, std::string_view detail) override {
        disconnected_       = true;
        disconnect_reason_  = reason;
        disconnect_detail_  = std::string(detail);
    }
    [[nodiscard]] Expected<PeerStats> QueryStats(PeerHandle) const override {
        PeerStats stats;
        stats.ping_milliseconds = ping_;
        return stats;
    }

    void AnnounceConnect() { announce_connect_ = true; }
    void SetPing(std::uint32_t ping) { ping_ = ping; }
    void DeliverLobbyChannelFirst() {
        deliver_order_ = {static_cast<std::size_t>(Channel::Lobby),
                          static_cast<std::size_t>(Channel::Control),
                          static_cast<std::size_t>(Channel::MapTransfer),
                          static_cast<std::size_t>(Channel::Simulation)};
    }
    [[nodiscard]] bool Disconnected() const { return disconnected_; }
    [[nodiscard]] const std::string& DisconnectDetail() const { return disconnect_detail_; }

private:
    static constexpr std::size_t kChannels = 4;

    std::uint64_t                     id_;
    LoopbackTransport*                peer_{nullptr};
    PeerHandle                        peer_handle_{PeerHandle::Invalid};
    bool                              listening_{false};
    bool                              announce_connect_{false};
    bool                              disconnected_{false};
    DisconnectReason                  disconnect_reason_{DisconnectReason::InternalError};
    std::string                       disconnect_detail_;
    std::uint32_t                     ping_{0};
    std::deque<Datagram>              inbox_[kChannels];
    std::vector<std::size_t>          deliver_order_{0, 1, 2, 3};
};

class RecordingSink final : public ILobbyEventSink {
public:
    void OnPhaseChanged(LobbyPhase, LobbyPhase current) override { phase = current; }
    void OnSnapshotChanged(const LobbySnapshot&) override {}
    void OnChatMessage(PlatformId, std::string_view, std::string_view) override {}
    void OnError(const Error& error) override { last_error = error.message; }

    LobbyPhase  phase{LobbyPhase::Idle};
    std::string last_error;
};

/// A host and a client, wired to each other.
struct Pair {
    FakeBackend       host_backend{1001, "Host"};
    FakeBackend       client_backend{2002, "Guest"};
    LoopbackTransport host_transport{1001};
    LoopbackTransport client_transport{2002};
    FakeEngine        host_engine;
    FakeEngine        client_engine;
    RecordingSink     host_sink;
    RecordingSink     client_sink;

    LobbyManager host{host_backend, host_transport, host_engine, host_sink};
    LobbyManager client{client_backend, client_transport, client_engine, client_sink};

    void Pump(int times = 8) {
        for (int i = 0; i < times; ++i) {
            host.Tick(0.016);
            client.Tick(0.016);
        }
    }
};

[[nodiscard]] HostOptions DefaultHost() {
    HostOptions options;
    options.visibility  = LobbyVisibility::Public;
    options.max_players = 10;
    options.settings.mode         = engine::GameMode::CaptureTheFlag;
    options.settings.scenario     = "a30";
    options.settings.variant_name = "a30";
    options.settings.team_count   = 2;
    return options;
}

// ---------------------------------------------------------------------------
// The cases
// ---------------------------------------------------------------------------

void JoinCompletes() {
    std::printf("a client joining a host\n");

    Pair pair;
    Check(pair.host.HostSession(DefaultHost()).ok(), "the host opens a session");
    pair.host_backend.RaiseCreated(77);
    pair.Pump();
    Check(pair.host.Phase() == LobbyPhase::Hosting, "the host reaches Hosting");

    // The lobby channel is drained before the control channel on the client, so the roster
    // arrives before the acceptance. This is the ordering that used to make the client
    // disconnect the host and then report that the host never replied.
    pair.client_transport.DeliverLobbyChannelFirst();

    Check(pair.client.JoinSession(77).ok(), "the client asks to join");
    pair.client_backend.RaiseEntered(77, false);
    pair.host_transport.ConnectTo(pair.client_transport, static_cast<PeerHandle>(1),
                                  static_cast<PeerHandle>(1));
    pair.host_transport.AnnounceConnect();
    pair.client_transport.AnnounceConnect();
    pair.Pump(24);

    Check(!pair.client_transport.Disconnected(),
          "the client does not hang up on an early roster");
    Check(pair.client.Phase() == LobbyPhase::InLobby,
          "the client reaches InLobby rather than timing out");
    Check(pair.host.Snapshot().players.size() == 2, "the host has both players");
    Check(pair.client.Snapshot().players.size() == 2, "the client has both players");
}

void GuestFollowsTheHost() {
    std::printf("a guest following the host's choices\n");

    Pair pair;
    (void)pair.host.HostSession(DefaultHost());
    pair.host_backend.RaiseCreated(77);
    pair.Pump();
    (void)pair.client.JoinSession(77);
    pair.client_backend.RaiseEntered(77, false);
    pair.host_transport.ConnectTo(pair.client_transport, static_cast<PeerHandle>(1),
                                  static_cast<PeerHandle>(1));
    pair.host_transport.AnnounceConnect();
    pair.client_transport.AnnounceConnect();
    pair.Pump(24);

    Check(pair.client.Snapshot().settings.mode == engine::GameMode::CaptureTheFlag,
          "the guest starts on the host's mode");

    // What the lobby screen does when the host presses SLAYER and then a different map.
    engine::MatchSettings changed = pair.host.Snapshot().settings;
    changed.mode         = engine::GameMode::TeamSlayer;
    changed.scenario     = "b40";
    changed.variant_name = "b40";
    Check(pair.host.UpdateMatchSettings(changed).ok(), "the host changes mode and map");
    pair.Pump(16);

    Check(pair.client.Snapshot().settings.mode == engine::GameMode::TeamSlayer,
          "the guest is told the new mode");
    Check(pair.client.Snapshot().settings.scenario == "b40",
          "the guest is told the new map");
    Check(!pair.client.IsHost(), "the guest knows it is not the host");
    Check(pair.host.IsHost(), "the host knows it is");
}

void TeamsBalance() {
    std::printf("teams balancing as people arrive\n");

    // Ten arrivals into a fresh session, counted. The host takes a side on creation, so
    // the sides can never be more than one apart at any point.
    for (int trial = 0; trial < 20; ++trial) {
        Pair pair;
        (void)pair.host.HostSession(DefaultHost());
        pair.host_backend.RaiseCreated(77);
        pair.Pump();

        std::size_t blue = 0;
        std::size_t red  = 0;
        for (const PlayerSlot& player : pair.host.Snapshot().players) {
            (player.team == 0 ? blue : red)++;
        }
        const std::size_t difference = blue > red ? blue - red : red - blue;
        if (difference > 1) {
            Check(false, "the sides stay within one of each other");
            return;
        }
    }
    Check(true, "the sides stay within one of each other over twenty sessions");
}

void NothingWaitsOnReadiness() {
    std::printf("readiness never gating anything\n");

    Pair pair;
    (void)pair.host.HostSession(DefaultHost());
    pair.host_backend.RaiseCreated(77);
    pair.Pump();

    const LobbySnapshot& snapshot = pair.host.Snapshot();
    Check(snapshot.EveryoneReady(), "everybody in a session counts as ready");
    Check(snapshot.ReadyCount() == snapshot.players.size(), "everybody, not some of them");
    for (const PlayerSlot& player : snapshot.players) {
        Check(player.is_ready, "every slot is ready");
    }
    Check(pair.host.SetLocalReady(false).ok(),
          "asking to be unready is accepted and changes nothing");
    Check(pair.host.Snapshot().EveryoneReady(), "and everybody is still ready");
}

void PublicStaysPublic() {
    std::printf("a public session staying findable\n");

    Pair pair;
    HostOptions options = DefaultHost();
    options.visibility  = LobbyVisibility::Public;
    (void)pair.host.HostSession(options);
    pair.host_backend.RaiseCreated(77);
    pair.Pump();

    Check(pair.host_backend.Visibility() == LobbyVisibility::Public,
          "hosting public leaves the lobby public");
    // The browse marker is deliberately not asserted here. It is published by the Steam
    // backend rather than by the manager, which is the right layer for it: it exists so a
    // Steam lobby search can tell this mod's lobbies from the game's own, and a fake
    // backend has no search to be found by. What belongs to the manager is the mode and
    // the phase, and those are what this checks.
    Check(pair.host_backend.Published(keys::kGameMode) == "capture_the_flag",
          "the mode is published for the browser to show");
    Check(pair.host_backend.Published(keys::kLobbyPhase) == "hosting",
          "the phase is published for the browser to show");
}


void HostLeavingFaultsTheGuest() {
    std::printf("a guest leaving a session\n");

    Pair pair;
    (void)pair.host.HostSession(DefaultHost());
    pair.host_backend.RaiseCreated(77);
    pair.Pump();
    (void)pair.client.JoinSession(77);
    pair.client_backend.RaiseEntered(77, false);
    pair.host_transport.ConnectTo(pair.client_transport, static_cast<PeerHandle>(1),
                                  static_cast<PeerHandle>(1));
    pair.host_transport.AnnounceConnect();
    pair.client_transport.AnnounceConnect();
    pair.Pump(24);
    Check(pair.client.Phase() == LobbyPhase::InLobby, "the guest is in the lobby");

    // The host stops answering. The guest has to notice and say why, rather than sitting
    // in a session that no longer exists.
    pair.client.LeaveSession();
    Check(pair.client.Phase() == LobbyPhase::Idle,
          "leaving puts the guest back to idle, ready to host their own");
    Check(pair.client.Snapshot().players.empty(), "and the roster is emptied with it");
}

} // namespace

int main() {
    JoinCompletes();
    GuestFollowsTheHost();
    TeamsBalance();
    NothingWaitsOnReadiness();
    PublicStaysPublic();
    HostLeavingFaultsTheGuest();

    std::printf("\n%s (%d failure(s))\n", g_failures == 0 ? "PASSED" : "FAILED", g_failures);
    return g_failures == 0 ? 0 : 1;
}
