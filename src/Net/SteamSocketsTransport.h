// SPDX-License-Identifier: MIT
// MultiplayerEvolved: Net/SteamSocketsTransport.h
//
// IPeerTransport over ISteamNetworkingSockets with the Steam Datagram Relay.
//
// WHY SDR
//
// The engine's own diagnostic strings show it already loses connections to NAT
// (join_failed_unable_to_connect_party_strict_nat and the moderate and open
// variants). Rather than reimplement NAT traversal, a P2P listen socket routes
// through Valve's relay network: the host never opens a port, neither side learns
// the other's IP address, and the relay absorbs the traversal problem. This is
// what makes "click invite, play" work for a non technical user.
//
// NO SDK REQUIRED
//
// Every Steam call goes through mpe::steam, which binds dynamically to the
// steam_api64.dll the game already ships. Building the mod needs a compiler and
// nothing else.
//
// THREADING CONTRACT
//
// Steam connection status callbacks fire on whichever thread pumps
// SteamAPI_RunCallbacks. Inside this process that is the game's thread, not ours.
// So the callback handler does the minimum possible work: it copies the event into
// a mutex guarded queue. Poll() then drains that queue on the mod tick thread and
// issues every ITransportObserver notification from there.
//
// The consequence is the reason the lobby has no locks anywhere: every observer
// callback is guaranteed to run on the mod tick thread.
//
// Received data is pulled, not pushed, via ReceiveMessagesOnPollGroup, so it needs
// no queue of its own.
#pragma once

#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "Net/IPeerTransport.h"
#include "Steam/SteamApi.h"

namespace mpe::net {

/// Construction time options.
struct SteamTransportOptions {
    /// Virtual port for the P2P listen socket. Peers must agree. Distinct from any
    /// port the game itself uses, so MultiplayerEvolved traffic cannot collide with the
    /// shipped session code.
    int virtual_port{22701};

    /// When true this transport calls SteamAPI_RunCallbacks itself. Left false
    /// inside the game process, where the host application already pumps callbacks
    /// and two pumps on different threads would race. Only a standalone dedicated
    /// server or a test harness that owns the Steam pipe sets this.
    bool owns_callback_pump{false};

    /// Initial per connection send rate ceiling. The lobby lowers this as the
    /// roster grows so the host's uplink is not the thing that breaks the match.
    int send_rate_min_bytes_per_second{64 * 1024};
    int send_rate_max_bytes_per_second{512 * 1024};
};

class SteamSocketsTransport final : public IPeerTransport {
public:
    /// Verifies that the Steam networking interfaces resolved before constructing.
    /// mpe::steam::Initialize must already have succeeded.
    [[nodiscard]] static Expected<std::unique_ptr<SteamSocketsTransport>> Create(
        const SteamTransportOptions& options);

    ~SteamSocketsTransport() override;

    SteamSocketsTransport(const SteamSocketsTransport&)            = delete;
    SteamSocketsTransport& operator=(const SteamSocketsTransport&) = delete;

    // --- IPeerTransport ---------------------------------------------------
    [[nodiscard]] TransportKind Kind() const noexcept override {
        return TransportKind::SteamRelay;
    }

    [[nodiscard]] Expected<PeerIdentity> LocalIdentity() const override;

    [[nodiscard]] Result Listen(const ListenConfig& config) override;
    [[nodiscard]] Result Connect(const PeerIdentity& host,
                                std::uint32_t timeout_milliseconds) override;
    void Shutdown() override;

    [[nodiscard]] bool IsListening() const noexcept override {
        return listen_socket_ != steam::kInvalidListenSocket;
    }
    [[nodiscard]] std::size_t PeerCount() const noexcept override { return peers_.size(); }
    [[nodiscard]] std::vector<PeerHandle> Peers() const override;
    [[nodiscard]] Expected<PeerIdentity> IdentityOf(PeerHandle peer) const override;

    [[nodiscard]] Result Send(PeerHandle peer, Channel channel,
                             std::span<const std::byte> payload, SendMode mode) override;
    [[nodiscard]] Result Broadcast(Channel channel, std::span<const std::byte> payload,
                                  SendMode mode) override;
    [[nodiscard]] Result BroadcastExcept(PeerHandle exclude, Channel channel,
                                         std::span<const std::byte> payload,
                                         SendMode mode) override;
    void Flush() override;

    void Poll(ITransportObserver& observer) override;
    void Disconnect(PeerHandle peer, DisconnectReason reason,
                    std::string_view detail) override;
    [[nodiscard]] Expected<PeerStats> QueryStats(PeerHandle peer) const override;

private:
    explicit SteamSocketsTransport(const SteamTransportOptions& options);

    /// One connection.
    struct PeerEntry {
        steam::HSteamNetConnection connection{steam::kInvalidConnection};
        PeerIdentity               identity;
        bool                       connected{false}; ///< False while still handshaking.
    };

    /// A status change copied out of the Steam callback for deferred handling.
    struct PendingStatusChange {
        steam::HSteamNetConnection             connection{steam::kInvalidConnection};
        steam::ESteamNetworkingConnectionState state{
            steam::ESteamNetworkingConnectionState::None};
        steam::ESteamNetworkingConnectionState old_state{
            steam::ESteamNetworkingConnectionState::None};
        std::uint64_t remote_platform_id{0};
        int           end_reason{0};
        std::string   end_debug;
        bool          inbound{false}; ///< Arrived on our listen socket.
    };

    /// Steam callback. Runs on the host application's callback thread and does
    /// nothing but enqueue, because observer notifications must originate from the
    /// mod tick thread.
    void OnConnectionStatusChanged(steam::SteamNetConnectionStatusChangedCallback* info);

    steam::Callback<SteamSocketsTransport, steam::SteamNetConnectionStatusChangedCallback>
        connection_status_callback_;

    /// Handles one dequeued status change on the mod tick thread.
    void ProcessStatusChange(const PendingStatusChange& change, ITransportObserver& observer);

    /// Reads the state of every connection not yet reported connected.
    ///
    /// Inside the game the host application owns the Steam callback pump, so being told
    /// about a transition means waiting on something this mod neither controls nor can
    /// measure. Asking is synchronous and costs a handful of reads per tick.
    void NoticeConnectionStates();

    /// Pulls and dispatches everything waiting on the poll group.
    void DrainReceivedMessages(ITransportObserver& observer);

    /// Applies lane configuration to a newly accepted or opened connection. Lanes
    /// give control traffic strict priority over bulk map transfer, so a map
    /// download cannot delay a launch message.
    [[nodiscard]] Result ConfigureConnection(steam::HSteamNetConnection connection);

    [[nodiscard]] PeerEntry* FindPeer(PeerHandle handle) noexcept;
    [[nodiscard]] const PeerEntry* FindPeer(PeerHandle handle) const noexcept;
    [[nodiscard]] PeerHandle HandleForConnection(
        steam::HSteamNetConnection connection) const noexcept;
    [[nodiscard]] PeerHandle AllocateHandle() noexcept;

    /// Removes a peer from both maps. Safe to call for an unknown handle.
    void ForgetPeer(PeerHandle handle);

    /// Maps a Steam close reason onto our vocabulary.
    [[nodiscard]] static DisconnectReason TranslateEndReason(int steam_reason,
                                                             bool was_connected) noexcept;

    SteamTransportOptions options_;

    steam::HSteamListenSocket listen_socket_{steam::kInvalidListenSocket};
    steam::HSteamNetPollGroup poll_group_{steam::kInvalidPollGroup};

    /// Set on a client; the single connection to the host.
    steam::HSteamNetConnection host_connection_{steam::kInvalidConnection};

    std::unordered_map<PeerHandle, PeerEntry>                        peers_;
    std::unordered_map<steam::HSteamNetConnection, PeerHandle>       by_connection_;
    std::uint32_t                                                    next_handle_{1};

    ListenConfig listen_config_;
    bool         accepting_{false}; ///< True once Listen succeeded.

    std::mutex                      queue_mutex_;
    std::deque<PendingStatusChange> pending_; ///< Guarded by queue_mutex_.
};

} // namespace mpe::net
