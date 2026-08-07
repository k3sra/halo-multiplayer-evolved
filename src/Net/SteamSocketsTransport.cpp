// SPDX-License-Identifier: MIT
// MultiplayerEvolved: Net/SteamSocketsTransport.cpp
#define MPE_LOG_CATEGORY "Net.Steam"

#include "Net/SteamSocketsTransport.h"

#include "Core/Log.h"
#include "Net/PacketProtocol.h"

#include <algorithm>
#include <cstring>
#include <format>

namespace mpe::net {
namespace {

/// Application close reasons. Steam reserves the App range for us, so the remote
/// end receives our own DisconnectReason instead of a generic code.
constexpr int kAppEndReasonBase = steam::kConnectionEndApp_Min;

[[nodiscard]] int EncodeEndReason(DisconnectReason reason) noexcept {
    const int encoded = kAppEndReasonBase + static_cast<int>(reason);
    return encoded <= steam::kConnectionEndApp_Max ? encoded : kAppEndReasonBase;
}

/// Lane index per channel. Lane count and order must match ConfigureConnection.
[[nodiscard]] std::uint16_t LaneOf(Channel channel) noexcept {
    return static_cast<std::uint16_t>(channel);
}

constexpr int kLaneCount = 4;

/// Steam send flags for one delivery mode. NoNagle is set on everything except
/// bulk transfer: lobby control messages are small and latency sensitive, and
/// coalescing them delays a launch by up to the Nagle interval.
[[nodiscard]] int SendFlagsFor(SendMode mode, Channel channel) noexcept {
    int flags = 0;
    switch (mode) {
        case SendMode::Unreliable:
            flags = steam::kSendUnreliable;
            break;
        case SendMode::UnreliableSequenced:
            // Steam has no dedicated sequenced mode; unreliable messages carry a
            // sequence number and the receiver drops anything older than the newest
            // already delivered. NoDelay skips the send buffer entirely.
            flags = steam::kSendUnreliable | steam::kSendNoDelay;
            break;
        case SendMode::Reliable:
            flags = steam::kSendReliable;
            break;
    }
    if (channel != Channel::MapTransfer) {
        flags |= steam::kSendNoNagle;
    }
    return flags;
}

} // namespace

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

SteamSocketsTransport::SteamSocketsTransport(const SteamTransportOptions& options)
    : options_(options) {}

Expected<std::unique_ptr<SteamSocketsTransport>> SteamSocketsTransport::Create(
    const SteamTransportOptions& options) {
    if (options.virtual_port < 0 || options.virtual_port > 65535) {
        return Error{ErrorCode::InvalidArgument,
                     std::format("virtual_port {} is out of range", options.virtual_port)};
    }
    if (!steam::IsInitialized()) {
        return Error{ErrorCode::SteamUnavailable,
                     "the Steam binding is not initialized; call mpe::steam::Initialize first"};
    }
    if (!steam::HasNetworkingSockets()) {
        return Error{ErrorCode::SteamUnavailable,
                     "ISteamNetworkingSockets is unavailable; the Steam client may be too old "
                     "or the user is offline"};
    }

    // Begin relay network warm up now. Without this the first connection pays for
    // ticket acquisition and route measurement, which is several seconds of
    // apparent hang when a player clicks Host.
    steam::InitRelayNetworkAccess();

    auto transport = std::unique_ptr<SteamSocketsTransport>(new SteamSocketsTransport(options));

    // Registered only once the object is fully constructed, so a callback arriving
    // on the game thread cannot observe a half built instance.
    transport->connection_status_callback_.Register(
        transport.get(), &SteamSocketsTransport::OnConnectionStatusChanged);

    MPE_LOG_INFO("steam transport ready (virtual port {}, relay warm up requested)",
                options.virtual_port);
    return transport;
}

SteamSocketsTransport::~SteamSocketsTransport() {
    // Unregister before teardown so no callback can land on a dying object.
    connection_status_callback_.Unregister();
    Shutdown();
}

// ---------------------------------------------------------------------------
// Identity
// ---------------------------------------------------------------------------

Expected<PeerIdentity> SteamSocketsTransport::LocalIdentity() const {
    const steam::SteamId id = steam::GetLocalSteamId();
    if (id == 0) {
        return Error{ErrorCode::SteamUnavailable, "the local Steam ID is not available"};
    }
    return PeerIdentity{id};
}

// ---------------------------------------------------------------------------
// Endpoint lifecycle
// ---------------------------------------------------------------------------

Result SteamSocketsTransport::Listen(const ListenConfig& config) {
    if (IsListening()) {
        return Result::Fail(ErrorCode::InvalidState, "already listening");
    }
    if (host_connection_ != steam::kInvalidConnection) {
        return Result::Fail(ErrorCode::InvalidState,
                            "this transport is already connected to a host as a client");
    }
    if (config.max_clients == 0 || config.max_clients > 31) {
        return Result::Fail(ErrorCode::InvalidArgument,
                            std::format("max_clients {} is outside 1..31", config.max_clients));
    }

    listen_config_ = config;

    steam::SteamNetworkingConfigValue values[3]{};
    values[0].SetInt32(steam::ESteamNetworkingConfigValue::TimeoutConnected,
                       static_cast<std::int32_t>(config.timeout_milliseconds));
    values[1].SetInt32(steam::ESteamNetworkingConfigValue::SendRateMin,
                       options_.send_rate_min_bytes_per_second);
    values[2].SetInt32(steam::ESteamNetworkingConfigValue::SendRateMax,
                       options_.send_rate_max_bytes_per_second);

    poll_group_ = steam::CreatePollGroup();
    if (poll_group_ == steam::kInvalidPollGroup) {
        return Result::Fail(ErrorCode::TransportUnavailable, "CreatePollGroup failed");
    }

    listen_socket_ = steam::CreateListenSocketP2P(options_.virtual_port, 3, values);
    if (listen_socket_ == steam::kInvalidListenSocket) {
        (void)steam::DestroyPollGroup(poll_group_);
        poll_group_ = steam::kInvalidPollGroup;
        return Result::Fail(ErrorCode::TransportUnavailable,
                            "CreateListenSocketP2P failed; the Steam client may be offline");
    }

    accepting_ = true;
    MPE_LOG_INFO("listening on P2P virtual port {} for up to {} client(s)",
                options_.virtual_port, config.max_clients);
    return Result::Success();
}

Result SteamSocketsTransport::Connect(const PeerIdentity& host,
                                     std::uint32_t timeout_milliseconds) {
    if (IsListening()) {
        return Result::Fail(ErrorCode::InvalidState, "cannot connect while hosting");
    }
    if (host_connection_ != steam::kInvalidConnection) {
        return Result::Fail(ErrorCode::InvalidState, "a connection attempt is already in flight");
    }
    if (!host.IsValid()) {
        return Result::Fail(ErrorCode::InvalidArgument, "host identity is not valid");
    }

    steam::SteamNetworkingIdentity identity{};
    identity.SetSteamId(host.platform_id);

    steam::SteamNetworkingConfigValue values[2]{};
    values[0].SetInt32(steam::ESteamNetworkingConfigValue::TimeoutInitial,
                       static_cast<std::int32_t>(timeout_milliseconds));
    values[1].SetInt32(steam::ESteamNetworkingConfigValue::TimeoutConnected,
                       static_cast<std::int32_t>(timeout_milliseconds));

    if (poll_group_ == steam::kInvalidPollGroup) {
        poll_group_ = steam::CreatePollGroup();
        if (poll_group_ == steam::kInvalidPollGroup) {
            return Result::Fail(ErrorCode::TransportUnavailable, "CreatePollGroup failed");
        }
    }

    const steam::HSteamNetConnection connection =
        steam::ConnectP2P(identity, options_.virtual_port, 2, values);
    if (connection == steam::kInvalidConnection) {
        return Result::Fail(ErrorCode::TransportUnavailable,
                            std::format("ConnectP2P to {} failed to start", host.platform_id));
    }

    const Result configured = ConfigureConnection(connection);
    if (!configured.ok()) {
        (void)steam::CloseConnection(connection,
                                     EncodeEndReason(DisconnectReason::InternalError),
                                     "lane configuration failed", false);
        return configured;
    }

    host_connection_ = connection;

    // Registered as a peer immediately but not yet connected: the handshake
    // completes asynchronously and Poll reports it.
    const PeerHandle handle = AllocateHandle();
    peers_.emplace(handle, PeerEntry{connection, host, false});
    by_connection_.emplace(connection, handle);

    MPE_LOG_INFO("connecting to host {} (timeout {} ms)", host.platform_id, timeout_milliseconds);
    return Result::Success();
}

void SteamSocketsTransport::Shutdown() {
    for (auto& [handle, entry] : peers_) {
        if (entry.connection != steam::kInvalidConnection) {
            (void)steam::CloseConnection(entry.connection,
                                         EncodeEndReason(DisconnectReason::HostShutdown),
                                         "transport shutting down", true);
        }
    }
    peers_.clear();
    by_connection_.clear();
    host_connection_ = steam::kInvalidConnection;

    if (listen_socket_ != steam::kInvalidListenSocket) {
        (void)steam::CloseListenSocket(listen_socket_);
        listen_socket_ = steam::kInvalidListenSocket;
    }
    if (poll_group_ != steam::kInvalidPollGroup) {
        (void)steam::DestroyPollGroup(poll_group_);
        poll_group_ = steam::kInvalidPollGroup;
    }
    accepting_ = false;

    {
        std::lock_guard lock(queue_mutex_);
        pending_.clear();
    }
}

std::vector<PeerHandle> SteamSocketsTransport::Peers() const {
    std::vector<PeerHandle> out;
    out.reserve(peers_.size());
    for (const auto& [handle, entry] : peers_) {
        if (entry.connected) {
            out.push_back(handle);
        }
    }
    // Stable ordering so roster iteration is deterministic across machines.
    std::sort(out.begin(), out.end());
    return out;
}

Expected<PeerIdentity> SteamSocketsTransport::IdentityOf(PeerHandle peer) const {
    const PeerEntry* entry = FindPeer(peer);
    if (entry == nullptr) {
        return Error{ErrorCode::PeerNotFound,
                     std::format("peer {} is not known", static_cast<std::uint32_t>(peer))};
    }
    return entry->identity;
}

// ---------------------------------------------------------------------------
// Sending
// ---------------------------------------------------------------------------

Result SteamSocketsTransport::Send(PeerHandle peer, Channel channel,
                                   std::span<const std::byte> payload, SendMode mode) {
    if (payload.empty()) {
        return Result::Fail(ErrorCode::InvalidArgument, "refusing to send an empty payload");
    }
    if (payload.size() > kMaxPayloadBytes) {
        return Result::Fail(ErrorCode::InvalidArgument,
                            std::format("payload of {} bytes exceeds the {} byte ceiling",
                                        payload.size(), kMaxPayloadBytes));
    }

    PeerEntry* const entry = FindPeer(peer);
    if (entry == nullptr) {
        return Result::Fail(ErrorCode::PeerNotFound,
                            std::format("peer {} is not known",
                                        static_cast<std::uint32_t>(peer)));
    }
    if (!entry->connected) {
        return Result::Fail(ErrorCode::InvalidState,
                            std::format("peer {} is still connecting",
                                        static_cast<std::uint32_t>(peer)));
    }

    // Lane aware send. SendMessageToConnection has no lane parameter, so the
    // message is allocated and dispatched through SendMessages instead.
    steam::SteamNetworkingMessage* const message =
        steam::AllocateMessage(static_cast<int>(payload.size()));
    if (message == nullptr) {
        return Result::Fail(ErrorCode::OutOfMemory, "AllocateMessage failed");
    }
    std::memcpy(message->m_pData, payload.data(), payload.size());
    message->m_conn    = entry->connection;
    message->m_nFlags  = SendFlagsFor(mode, channel);
    message->m_idxLane = LaneOf(channel);

    std::int64_t message_number = 0;
    steam::SteamNetworkingMessage* messages[1] = {message};
    steam::SendMessages(1, messages, &message_number);

    // SendMessages consumes the message regardless of outcome. A negative result
    // is the negated EResult.
    if (message_number < 0) {
        return Result::Fail(ErrorCode::TransportSendFailed,
                            std::format("SendMessages to peer {} failed with EResult {}",
                                        static_cast<std::uint32_t>(peer), -message_number));
    }
    return Result::Success();
}

Result SteamSocketsTransport::Broadcast(Channel channel, std::span<const std::byte> payload,
                                        SendMode mode) {
    return BroadcastExcept(PeerHandle::Invalid, channel, payload, mode);
}

Result SteamSocketsTransport::BroadcastExcept(PeerHandle exclude, Channel channel,
                                              std::span<const std::byte> payload,
                                              SendMode mode) {
    // Every peer is attempted even if one fails: a single bad connection must not
    // silently deprive the rest of the roster of a launch message. The first error
    // is reported once all sends have been attempted.
    Result first_error = Result::Success();
    for (const PeerHandle peer : Peers()) {
        if (peer == exclude) {
            continue;
        }
        Result sent = Send(peer, channel, payload, mode);
        if (!sent.ok() && first_error.ok()) {
            first_error = Result{sent.error()};
        }
    }
    return first_error;
}

void SteamSocketsTransport::Flush() {
    for (const auto& [handle, entry] : peers_) {
        if (entry.connected) {
            (void)steam::FlushMessagesOnConnection(entry.connection);
        }
    }
}

// ---------------------------------------------------------------------------
// Pump
// ---------------------------------------------------------------------------

void SteamSocketsTransport::OnConnectionStatusChanged(
    steam::SteamNetConnectionStatusChangedCallback* info) {
    if (info == nullptr) {
        return;
    }

    // Runs on the host application's callback thread. Copy and get out.
    PendingStatusChange change;
    change.connection = info->m_hConn;
    change.state      = info->m_info.m_eState;
    change.old_state  = info->m_eOldState;
    change.end_reason = info->m_info.m_eEndReason;
    change.inbound    = (info->m_info.m_hListenSocket != steam::kInvalidListenSocket);

    // m_szEndDebug is a fixed buffer that need not be NUL terminated if full.
    const char* const debug = info->m_info.m_szEndDebug;
    const std::size_t debug_length =
        std::find(debug, debug + sizeof(info->m_info.m_szEndDebug), '\0') - debug;
    change.end_debug.assign(debug, debug_length);

    change.remote_platform_id = info->m_info.m_identityRemote.GetSteamId();

    std::lock_guard lock(queue_mutex_);
    pending_.push_back(std::move(change));
}

void SteamSocketsTransport::Poll(ITransportObserver& observer) {
    // Only in a process where this transport owns the Steam pipe. Inside the game the
    // host application pumps callbacks and doing it here too would race, which is why
    // RunCallbacks itself is also guarded on ownership.
    if (options_.owns_callback_pump) {
        steam::RunCallbacks();
    }

    NoticeConnectionStates();

    // Drain under the lock, process outside it, so an observer callback that calls
    // back into the transport cannot deadlock.
    std::deque<PendingStatusChange> batch;
    {
        std::lock_guard lock(queue_mutex_);
        batch.swap(pending_);
    }
    for (const PendingStatusChange& change : batch) {
        ProcessStatusChange(change, observer);
    }

    DrainReceivedMessages(observer);
}

void SteamSocketsTransport::NoticeConnectionStates() {
    // Connection state is asked for, not only waited on.
    //
    // SteamNetConnectionStatusChanged is a broadcast callback, and inside the game this
    // mod does not own the Steam callback pump: the host application does. So every
    // transition on a connection this mod opened arrived whenever the game next chose to
    // dispatch, which is not something this mod controls, cannot measure, and has no
    // upper bound on. That is time a player spends watching a screen that says nothing is
    // happening, because as far as this code knows nothing is.
    //
    // The state of a connection we already hold is readable synchronously, so it is read
    // every tick and any change is queued exactly as the callback would have queued it.
    // Discovering a brand new inbound connection still needs the callback, because there
    // is no handle to ask about until one exists; everything after that no longer does.
    //
    // Duplicate notifications are harmless: ProcessStatusChange already ignores a
    // Connected for a peer it has marked connected, which is what makes reading and being
    // told safe to have at once.
    // Peers are only touched from this thread, so the map is read directly.
    std::vector<steam::HSteamNetConnection> waiting;
    waiting.reserve(peers_.size());
    for (const auto& [handle, entry] : peers_) {
        (void)handle;
        if (!entry.connected) {
            waiting.push_back(entry.connection);
        }
    }
    if (waiting.empty()) {
        return;
    }

    for (const steam::HSteamNetConnection connection : waiting) {
        steam::SteamNetConnectionInfo info{};
        if (!steam::GetConnectionInfo(connection, &info)) {
            continue;
        }
        if (info.m_eState != steam::ESteamNetworkingConnectionState::Connected) {
            continue;
        }
        PendingStatusChange change{};
        change.connection         = connection;
        change.state              = info.m_eState;
        change.inbound            = false;
        change.remote_platform_id = info.m_identityRemote.m_steamID64;
        std::lock_guard lock(queue_mutex_);
        pending_.push_back(change);
    }
}

void SteamSocketsTransport::ProcessStatusChange(const PendingStatusChange& change,
                                                ITransportObserver& observer) {
    switch (change.state) {
        case steam::ESteamNetworkingConnectionState::Connecting: {
            if (!change.inbound) {
                // Our own outbound attempt progressing. Nothing to do.
                return;
            }
            if (!accepting_) {
                (void)steam::CloseConnection(change.connection,
                                             EncodeEndReason(DisconnectReason::HostShutdown),
                                             "host is not accepting connections", false);
                return;
            }
            if (peers_.size() >= listen_config_.max_clients) {
                MPE_LOG_INFO("rejecting {} : session is full ({} clients)",
                            change.remote_platform_id, listen_config_.max_clients);
                (void)steam::CloseConnection(change.connection,
                                             EncodeEndReason(DisconnectReason::Kicked),
                                             "session is full", false);
                return;
            }

            const steam::EResult accepted = steam::AcceptConnection(change.connection);
            if (accepted != steam::EResult::Ok) {
                MPE_LOG_WARN("AcceptConnection for {} failed with EResult {}",
                            change.remote_platform_id, static_cast<int>(accepted));
                (void)steam::CloseConnection(change.connection,
                                             EncodeEndReason(DisconnectReason::InternalError),
                                             "accept failed", false);
                return;
            }
            const Result configured = ConfigureConnection(change.connection);
            if (!configured.ok()) {
                MPE_LOG_WARN("configuring inbound connection from {} failed: {}",
                            change.remote_platform_id, configured.message());
                (void)steam::CloseConnection(change.connection,
                                             EncodeEndReason(DisconnectReason::InternalError),
                                             "configuration failed", false);
                return;
            }

            const PeerHandle handle = AllocateHandle();
            peers_.emplace(handle, PeerEntry{change.connection,
                                             PeerIdentity{change.remote_platform_id}, false});
            by_connection_.emplace(change.connection, handle);
            MPE_LOG_INFO("accepted inbound connection from {} as peer {}",
                        change.remote_platform_id, static_cast<std::uint32_t>(handle));
            return;
        }

        case steam::ESteamNetworkingConnectionState::Connected: {
            const PeerHandle handle = HandleForConnection(change.connection);
            if (handle == PeerHandle::Invalid) {
                // A connection we do not track reaching Connected means our maps and
                // Steam's state have diverged. Close it rather than leak it.
                MPE_LOG_WARN("untracked connection {} reached Connected; closing",
                            change.connection);
                (void)steam::CloseConnection(change.connection,
                                             EncodeEndReason(DisconnectReason::InternalError),
                                             "untracked connection", false);
                return;
            }

            PeerEntry* const entry = FindPeer(handle);
            if (entry == nullptr || entry->connected) {
                return; // Duplicate notification.
            }

            if (!steam::SetConnectionPollGroup(change.connection, poll_group_)) {
                MPE_LOG_WARN("SetConnectionPollGroup failed for peer {}",
                            static_cast<std::uint32_t>(handle));
            }
            entry->connected = true;
            if (change.remote_platform_id != 0) {
                entry->identity.platform_id = change.remote_platform_id;
            }

            MPE_LOG_INFO("peer {} ({}) connected", static_cast<std::uint32_t>(handle),
                        entry->identity.platform_id);
            observer.OnPeerConnected(handle, entry->identity);
            return;
        }

        case steam::ESteamNetworkingConnectionState::ClosedByPeer:
        case steam::ESteamNetworkingConnectionState::ProblemDetectedLocally: {
            const PeerHandle handle = HandleForConnection(change.connection);
            const bool was_connected = [&] {
                const PeerEntry* const entry = FindPeer(handle);
                return entry != nullptr && entry->connected;
            }();
            const DisconnectReason reason =
                TranslateEndReason(change.end_reason, was_connected);

            // Steam requires the connection be closed even after the remote end
            // closed it, otherwise the handle leaks.
            (void)steam::CloseConnection(change.connection, 0, nullptr, false);

            if (change.connection == host_connection_) {
                host_connection_ = steam::kInvalidConnection;
            }

            if (handle == PeerHandle::Invalid) {
                observer.OnConnectFailed(reason, change.end_debug);
                return;
            }

            ForgetPeer(handle);

            if (was_connected) {
                MPE_LOG_INFO("peer {} disconnected: {} ({})", static_cast<std::uint32_t>(handle),
                            ToString(reason), change.end_debug);
                observer.OnPeerDisconnected(handle, reason, change.end_debug);
            } else {
                MPE_LOG_INFO("connection attempt failed: {} ({})", ToString(reason),
                            change.end_debug);
                observer.OnConnectFailed(reason, change.end_debug);
            }
            return;
        }

        default:
            // None, FindingRoute and any future state need no action: either a
            // transient step toward Connected, or a terminal state Steam has already
            // accounted for.
            return;
    }
}

void SteamSocketsTransport::DrainReceivedMessages(ITransportObserver& observer) {
    if (poll_group_ == steam::kInvalidPollGroup) {
        return;
    }

    // Bounded per tick so a flood cannot starve the rest of the frame. Anything
    // left over is picked up next tick, which is what the transport's own flow
    // control is for.
    constexpr int kMaxMessagesPerPoll = 128;
    steam::SteamNetworkingMessage* messages[kMaxMessagesPerPoll] = {};

    for (;;) {
        const int received =
            steam::ReceiveMessagesOnPollGroup(poll_group_, messages, kMaxMessagesPerPoll);
        if (received <= 0) {
            return;
        }

        for (int i = 0; i < received; ++i) {
            steam::SteamNetworkingMessage* const message = messages[i];
            if (message == nullptr) {
                continue;
            }

            const PeerHandle handle = HandleForConnection(message->m_conn);
            if (handle == PeerHandle::Invalid) {
                // Data from a connection we no longer track. Drop it.
                message->Release();
                continue;
            }

            // The lane the sender used is the channel. An out of range lane is a
            // protocol violation, not something to guess at.
            if (message->m_idxLane >= kLaneCount) {
                MPE_LOG_WARN("peer {} sent on lane {}, which is out of range",
                            static_cast<std::uint32_t>(handle), message->m_idxLane);
                message->Release();
                Disconnect(handle, DisconnectReason::ProtocolViolation, "invalid lane index");
                continue;
            }

            const auto channel = static_cast<Channel>(message->m_idxLane);
            const auto payload =
                std::span(static_cast<const std::byte*>(message->m_pData),
                          static_cast<std::size_t>(message->m_cbSize < 0 ? 0 : message->m_cbSize));
            observer.OnPacketReceived(handle, channel, payload);

            // Released after the observer returns; OnPacketReceived is documented to
            // treat the payload as valid only for the call.
            message->Release();
        }

        if (received < kMaxMessagesPerPoll) {
            return;
        }
    }
}

void SteamSocketsTransport::Disconnect(PeerHandle peer, DisconnectReason reason,
                                        std::string_view detail) {
    PeerEntry* const entry = FindPeer(peer);
    if (entry == nullptr) {
        return;
    }

    const std::string detail_z(detail);
    (void)steam::CloseConnection(entry->connection, EncodeEndReason(reason), detail_z.c_str(),
                                 true);

    if (entry->connection == host_connection_) {
        host_connection_ = steam::kInvalidConnection;
    }
    MPE_LOG_INFO("closing peer {}: {} ({})", static_cast<std::uint32_t>(peer), ToString(reason),
                detail_z);
    ForgetPeer(peer);
}

Expected<PeerStats> SteamSocketsTransport::QueryStats(PeerHandle peer) const {
    const PeerEntry* const entry = FindPeer(peer);
    if (entry == nullptr) {
        return Error{ErrorCode::PeerNotFound,
                     std::format("peer {} is not known", static_cast<std::uint32_t>(peer))};
    }

    steam::SteamNetConnectionRealTimeStatus status{};
    const steam::EResult result =
        steam::GetConnectionRealTimeStatus(entry->connection, &status, 0, nullptr);
    if (result != steam::EResult::Ok) {
        return Error{ErrorCode::SteamCallFailed,
                     std::format("GetConnectionRealTimeStatus failed with EResult {}",
                                 static_cast<int>(result))};
    }

    PeerStats stats;
    stats.ping_milliseconds = static_cast<std::uint32_t>(std::max(0, status.m_nPing));
    // Connection quality is reported as a fraction of packets delivered.
    stats.packet_loss_out = std::clamp(1.0f - status.m_flConnectionQualityLocal, 0.0f, 1.0f);
    stats.packet_loss_in  = std::clamp(1.0f - status.m_flConnectionQualityRemote, 0.0f, 1.0f);
    stats.pending_reliable_bytes =
        static_cast<std::uint32_t>(std::max(0, status.m_cbPendingReliable));
    stats.send_rate_bytes_per_second =
        static_cast<std::uint32_t>(std::max(0, status.m_nSendRateBytesPerSecond));

    steam::SteamNetConnectionInfo info{};
    if (steam::GetConnectionInfo(entry->connection, &info)) {
        stats.is_relayed = (info.m_nFlags & steam::kConnectionInfoFlagRelayed) != 0;
    }
    return stats;
}

// ---------------------------------------------------------------------------
// Internals
// ---------------------------------------------------------------------------

Result SteamSocketsTransport::ConfigureConnection(steam::HSteamNetConnection connection) {
    // Lane priorities: a lower value is sent first. Control and Lobby outrank
    // Simulation, which outranks bulk MapTransfer. That ordering is what keeps a
    // 2 MB map download from delaying a LaunchNow message behind it.
    //
    // Weights only apply between lanes of equal priority, so the values here matter
    // for Control against Lobby, where an even split is correct.
    const int priorities[kLaneCount] = {
        0,  // Control
        0,  // Lobby
        20, // MapTransfer
        5,  // Simulation
    };
    const std::uint16_t weights[kLaneCount] = {1, 1, 1, 1};

    const steam::EResult result =
        steam::ConfigureConnectionLanes(connection, kLaneCount, priorities, weights);
    if (result != steam::EResult::Ok) {
        return Result::Fail(ErrorCode::TransportUnavailable,
                            std::format("ConfigureConnectionLanes failed with EResult {}",
                                        static_cast<int>(result)));
    }
    return Result::Success();
}

SteamSocketsTransport::PeerEntry* SteamSocketsTransport::FindPeer(PeerHandle handle) noexcept {
    const auto it = peers_.find(handle);
    return it == peers_.end() ? nullptr : &it->second;
}

const SteamSocketsTransport::PeerEntry* SteamSocketsTransport::FindPeer(
    PeerHandle handle) const noexcept {
    const auto it = peers_.find(handle);
    return it == peers_.end() ? nullptr : &it->second;
}

PeerHandle SteamSocketsTransport::HandleForConnection(
    steam::HSteamNetConnection connection) const noexcept {
    const auto it = by_connection_.find(connection);
    return it == by_connection_.end() ? PeerHandle::Invalid : it->second;
}

PeerHandle SteamSocketsTransport::AllocateHandle() noexcept {
    // Never reused within a session, so a stale handle held by higher layers
    // resolves to PeerNotFound instead of silently addressing a new player.
    return static_cast<PeerHandle>(next_handle_++);
}

void SteamSocketsTransport::ForgetPeer(PeerHandle handle) {
    const auto it = peers_.find(handle);
    if (it == peers_.end()) {
        return;
    }
    by_connection_.erase(it->second.connection);
    peers_.erase(it);
}

DisconnectReason SteamSocketsTransport::TranslateEndReason(int steam_reason,
                                                            bool was_connected) noexcept {
    // Reasons we encoded ourselves round trip exactly.
    if (steam_reason >= steam::kConnectionEndApp_Min &&
        steam_reason <= steam::kConnectionEndApp_Max) {
        const int offset = steam_reason - kAppEndReasonBase;
        if (offset >= 0 && offset <= static_cast<int>(DisconnectReason::InternalError)) {
            return static_cast<DisconnectReason>(offset);
        }
        return DisconnectReason::RemoteRequest;
    }

    switch (steam_reason) {
        case steam::kConnectionEndMisc_Timeout:
        case steam::kConnectionEndRemote_Timeout:
            return DisconnectReason::Timeout;
        case steam::kConnectionEndMisc_P2P_Rendezvous:
        case steam::kConnectionEndMisc_RelayConnectivity:
        case steam::kConnectionEndMisc_NoRelaySessionsToClient:
        case steam::kConnectionEndLocal_ManyRelayConnectivity:
            return DisconnectReason::RelayFailure;
        case steam::kConnectionEndRemote_BadCrypt:
        case steam::kConnectionEndRemote_BadCert:
            return DisconnectReason::ProtocolViolation;
        default:
            break;
    }
    return was_connected ? DisconnectReason::RemoteRequest : DisconnectReason::RelayFailure;
}

} // namespace mpe::net
