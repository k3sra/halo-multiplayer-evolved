// SPDX-License-Identifier: MIT
// MultiplayerEvolved: Lobby/ILobbyBackend.h
//
// Platform facade for lobby discovery, membership and invitations.
//
// SEPARATION OF CONCERNS
//
// There are two distinct networks in this design and conflating them is the
// usual source of unreliable lobbies:
//
//   ILobbyBackend    Metadata and presence. Who is here, what mode is selected,
//                    which map, invite plumbing, the friends list "Join Game"
//                    button. Low rate, backed by the platform's own service.
//
//   IPeerTransport   Actual gameplay and control traffic. High rate, direct
//                    peer connections over the relay.
//
// A lobby exists before any transport connection does. The lobby is how a client
// learns the host's identity; the transport is what it then connects to. Keeping
// them apart is also what makes the dedicated server path clean: a server has no
// lobby at all, just a transport, and none of the lobby code needs to know.
//
// Every method here is deliberately expressed in platform neutral terms. No
// Steam type appears in this header, so a future backend needs no changes above
// this line.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "Core/Result.h"

namespace mpe::lobby {

/// Platform lobby identifier. Zero means no lobby.
using LobbyId = std::uint64_t;

/// Platform account identifier of a player.
using PlatformId = std::uint64_t;

/// Who may find and join the lobby. Mirrors the engine's own privacy vocabulary
/// (network_session_privacy_open, _friends_only, _invitation_only), so the two
/// stay in step.
enum class LobbyVisibility : std::uint8_t {
    Public = 0,
    FriendsOnly,
    InviteOnly,
};

[[nodiscard]] std::string_view ToString(LobbyVisibility visibility) noexcept;

/// One member as the platform reports them.
struct LobbyMember {
    PlatformId  platform_id{0};
    std::string display_name;
    bool        is_owner{false};
};

/// Well known lobby data keys. Centralized so host and client cannot disagree
/// about spelling, which is otherwise a silent failure that looks like a bug in
/// the state machine.
namespace keys {

/// Marks a lobby as one this mod opened, and which protocol it speaks.
///
/// Steam filters the server browser's search on this key, so it does two jobs at once:
/// the game's own co-op fireteam lobbies are excluded before they can fill the result
/// set, and a session is only listed for players whose build can actually talk to it.
inline constexpr const char* kBrowseMarker = "mpe.v";

/// Wire protocol version. A client compares this before joining and refuses
/// early with a clear message rather than failing mid handshake.
inline constexpr const char* kProtocolVersion = "fe.protocol";

/// Game build string, from the executable's version resource. Must match
/// exactly: the simulation is not compatible across builds.
inline constexpr const char* kGameBuild = "fe.build";

/// The host's platform id, which is the address a client hands to
/// IPeerTransport::Connect.
inline constexpr const char* kHostId = "fe.host";

inline constexpr const char* kGameMode      = "fe.mode";
inline constexpr const char* kMapName       = "fe.map";
inline constexpr const char* kMapHash       = "fe.map_hash";
inline constexpr const char* kBaseScenario  = "fe.scenario";
inline constexpr const char* kLobbyPhase    = "fe.phase";
inline constexpr const char* kPlayerCount   = "fe.players";

} // namespace keys

/// Notifications raised during Poll, on the mod tick thread.
class ILobbyBackendObserver {
public:
    virtual ~ILobbyBackendObserver() = default;

    /// This machine created a lobby and owns it.
    virtual void OnLobbyCreated(LobbyId lobby) = 0;
    virtual void OnLobbyCreateFailed(const Error& error) = 0;

    /// This machine entered a lobby, either its own or a remote one.
    virtual void OnLobbyEntered(LobbyId lobby, bool is_owner) = 0;
    virtual void OnLobbyEnterFailed(const Error& error) = 0;

    virtual void OnMemberJoined(const LobbyMember& member) = 0;
    virtual void OnMemberLeft(PlatformId member, bool was_kicked) = 0;

    /// Lobby level metadata changed. The observer re-reads whichever keys it
    /// cares about rather than receiving a diff, which keeps the backend free of
    /// any knowledge of what the keys mean.
    virtual void OnLobbyDataChanged(LobbyId lobby) = 0;

    /// Per member metadata changed, such as a display name published before the
    /// transport connection exists.
    virtual void OnMemberDataChanged(PlatformId member) = 0;

    /// The local player accepted an invitation or used the friends list join
    /// button. The implementation has not joined anything yet; the observer
    /// decides, because joining while already in a match has to be handled.
    virtual void OnJoinRequested(LobbyId lobby, PlatformId inviter) = 0;
};

class ILobbyBackend {
public:
    virtual ~ILobbyBackend() = default;

    // --- Lifecycle --------------------------------------------------------
    /// Asynchronous. Completion arrives as OnLobbyCreated or OnLobbyCreateFailed.
    [[nodiscard]] virtual Result Create(LobbyVisibility visibility,
                                       std::uint32_t max_members) = 0;

    /// Asynchronous. Completion arrives as OnLobbyEntered or OnLobbyEnterFailed.
    [[nodiscard]] virtual Result Join(LobbyId lobby) = 0;

    /// Idempotent and safe from an error path.
    virtual void Leave() = 0;

    [[nodiscard]] virtual bool InLobby() const noexcept = 0;
    [[nodiscard]] virtual LobbyId CurrentLobby() const noexcept = 0;
    [[nodiscard]] virtual bool IsOwner() const noexcept = 0;

    // --- Metadata ---------------------------------------------------------
    /// Owner only. Fails with InvalidState on a non owner, which is the
    /// platform's rule and not something to work around.
    [[nodiscard]] virtual Result SetLobbyData(std::string_view key,
                                             std::string_view value) = 0;
    [[nodiscard]] virtual Expected<std::string> GetLobbyData(std::string_view key) const = 0;

    /// Any member may set their own data. This is how a member tells the host
    /// something before a transport connection exists.
    [[nodiscard]] virtual Result SetMemberData(std::string_view key,
                                              std::string_view value) = 0;
    [[nodiscard]] virtual Expected<std::string> GetMemberData(PlatformId member,
                                                              std::string_view key) const = 0;

    [[nodiscard]] virtual std::vector<LobbyMember> Members() const = 0;
    [[nodiscard]] virtual std::size_t MemberCount() const noexcept = 0;

    /// Owner only. Changing visibility mid lobby is how "friends only" becomes
    /// "invite only" once a match is underway.
    [[nodiscard]] virtual Result SetVisibility(LobbyVisibility visibility) = 0;

    // --- Invitations ------------------------------------------------------
    /// Opens the platform's own invite dialog. Using the platform overlay rather
    /// than a custom friend picker is deliberate: it is the flow players already
    /// know, and it needs no friends list permissions of our own.
    [[nodiscard]] virtual Result OpenInviteOverlay() = 0;

    /// Publishes joinable presence so the friends list shows a working
    /// "Join Game" entry.
    [[nodiscard]] virtual Result PublishJoinablePresence(std::string_view status_text) = 0;
    virtual void ClearJoinablePresence() = 0;

    // --- Local identity ---------------------------------------------------
    [[nodiscard]] virtual Expected<PlatformId> LocalId() const = 0;
    [[nodiscard]] virtual Expected<std::string> LocalDisplayName() const = 0;

    // --- Pump -------------------------------------------------------------
    /// Drains platform callbacks. Every observer notification happens inside
    /// this call, on the calling thread.
    virtual void Poll(ILobbyBackendObserver& observer) = 0;
};

} // namespace mpe::lobby
