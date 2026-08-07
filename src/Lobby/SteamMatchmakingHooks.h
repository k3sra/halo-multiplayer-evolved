// SPDX-License-Identifier: MIT
// MultiplayerEvolved: Lobby/SteamMatchmakingHooks.h
//
// ILobbyBackend implemented on ISteamMatchmaking and ISteamFriends.
//
// This is the only file in the project that knows Steam lobbies exist.
//
// CALLBACKS THIS CLASS OWNS
//
//   LobbyCreated_t                   our own CreateLobby completed
//   LobbyEnter_t                     we finished entering a lobby
//   LobbyChatUpdate_t                a member joined, left, disconnected or was kicked
//   LobbyDataUpdate_t                lobby or member metadata changed
//   GameLobbyJoinRequested_t         an invitation was accepted while the game runs
//   GameRichPresenceJoinRequested_t  friends list "Join Game" was used
//
// The last two are what make "invite friends directly via the Steam overlay" work.
// Steam delivers them to the running process, and because this DLL registers for
// them inside the game, no launcher or restart is involved: the invitee goes
// straight from the overlay into the lobby.
//
// THREADING
//
// Identical discipline to SteamSocketsTransport. Steam callbacks fire on the host
// application's callback thread, so each handler only enqueues. Poll drains the
// queue on the mod tick thread, and every ILobbyBackendObserver call happens
// there. Nothing above this class needs a lock.
#pragma once

#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "Lobby/ILobbyBackend.h"
#include "Steam/SteamApi.h"

namespace mpe::lobby {

class SteamMatchmakingHooks final : public ILobbyBackend {
public:
    /// Verifies the required Steam interfaces are live, then registers callbacks.
    /// mpe::steam::Initialize must already have succeeded.
    ///
    /// Named CreateInstance rather than Create so it is never confused with the
    /// ILobbyBackend::Create that makes a lobby.
    [[nodiscard]] static Expected<std::unique_ptr<SteamMatchmakingHooks>> CreateInstance();

    ~SteamMatchmakingHooks() override;

    SteamMatchmakingHooks(const SteamMatchmakingHooks&)            = delete;
    SteamMatchmakingHooks& operator=(const SteamMatchmakingHooks&) = delete;

    // --- ILobbyBackend ----------------------------------------------------
    [[nodiscard]] Result Create(LobbyVisibility visibility, std::uint32_t max_members) override;
    [[nodiscard]] Result Join(LobbyId lobby) override;
    void Leave() override;

    [[nodiscard]] bool InLobby() const noexcept override { return current_lobby_ != 0; }
    [[nodiscard]] LobbyId CurrentLobby() const noexcept override { return current_lobby_; }
    [[nodiscard]] bool IsOwner() const noexcept override { return is_owner_; }

    [[nodiscard]] Result SetLobbyData(std::string_view key, std::string_view value) override;
    [[nodiscard]] Expected<std::string> GetLobbyData(std::string_view key) const override;
    [[nodiscard]] Result SetMemberData(std::string_view key, std::string_view value) override;
    [[nodiscard]] Expected<std::string> GetMemberData(PlatformId member,
                                                      std::string_view key) const override;

    [[nodiscard]] std::vector<LobbyMember> Members() const override;
    [[nodiscard]] std::size_t MemberCount() const noexcept override;

    [[nodiscard]] Result SetVisibility(LobbyVisibility visibility) override;

    [[nodiscard]] Result OpenInviteOverlay() override;
    [[nodiscard]] Result PublishJoinablePresence(std::string_view status_text) override;
    void ClearJoinablePresence() override;

    [[nodiscard]] Expected<PlatformId> LocalId() const override;
    [[nodiscard]] Expected<std::string> LocalDisplayName() const override;

    void Poll(ILobbyBackendObserver& observer) override;

private:
    SteamMatchmakingHooks() = default;

    /// Kind of queued platform event.
    enum class EventKind : std::uint8_t {
        LobbyCreated,
        LobbyCreateFailed,
        LobbyEntered,
        LobbyEnterFailed,
        MemberJoined,
        MemberLeft,
        LobbyDataChanged,
        MemberDataChanged,
        JoinRequested,
    };

    /// One queued event. A flat struct rather than a variant so enqueueing from a
    /// callback is a trivial copy with no allocation beyond the strings.
    struct Event {
        EventKind   kind{EventKind::LobbyDataChanged};
        LobbyId     lobby{0};
        PlatformId  member{0};
        bool        flag{false}; ///< is_owner, or was_kicked.
        ErrorCode   error_code{ErrorCode::None};
        std::string detail;
    };

    void Enqueue(Event event);

    /// Applies one drained event: updates internal state, then notifies.
    void Dispatch(const Event& event, ILobbyBackendObserver& observer);

    /// Recomputes is_owner_ from the platform. Called whenever membership or
    /// ownership could have changed, including after a host leaves and Steam
    /// promotes a new owner.
    void RefreshOwnership();

    /// Publishes the metadata a client needs in order to evaluate and join, namely
    /// protocol version, game build and host identity. Owner only, called once
    /// immediately after the lobby is created.
    [[nodiscard]] Result PublishJoinMetadata();

    [[nodiscard]] static steam::ELobbyType TranslateVisibility(
        LobbyVisibility visibility) noexcept;

    // --- Steam callbacks (host application thread) ------------------------
    void OnLobbyEnter(steam::LobbyEnterCallback* callback);
    void OnLobbyChatUpdate(steam::LobbyChatUpdateCallback* callback);
    void OnLobbyDataUpdate(steam::LobbyDataUpdateCallback* callback);
    void OnGameLobbyJoinRequested(steam::GameLobbyJoinRequestedCallback* callback);
    void OnRichPresenceJoinRequested(steam::GameRichPresenceJoinRequestedCallback* callback);
    void OnLobbyCreatedResult(steam::LobbyCreatedCallback* result, bool io_failure);

    steam::Callback<SteamMatchmakingHooks, steam::LobbyEnterCallback> lobby_enter_callback_;
    steam::Callback<SteamMatchmakingHooks, steam::LobbyChatUpdateCallback>
        lobby_chat_update_callback_;
    steam::Callback<SteamMatchmakingHooks, steam::LobbyDataUpdateCallback>
        lobby_data_update_callback_;
    steam::Callback<SteamMatchmakingHooks, steam::GameLobbyJoinRequestedCallback>
        lobby_join_requested_callback_;
    steam::Callback<SteamMatchmakingHooks, steam::GameRichPresenceJoinRequestedCallback>
        rich_presence_join_callback_;

    /// CreateLobby is a call result rather than a broadcast callback.
    steam::CallResult<SteamMatchmakingHooks, steam::LobbyCreatedCallback> lobby_created_call_;

    LobbyId current_lobby_{0};
    bool    is_owner_{false};

    /// True between Create or Join being issued and its completion, so a second
    /// request cannot be started concurrently.
    bool operation_in_flight_{false};

    /// Which lobby Join was asked for, until entering it succeeds or fails.
    ///
    /// Without it, "is this enter event ours" was answered by "is anything outstanding",
    /// which cannot tell one lobby from another. Leaving a lobby and immediately joining
    /// another is exactly the case that breaks: the enter event for the lobby just left can
    /// still be in the queue, and it would consume the outstanding request, be reported as
    /// the lobby that was joined, and leave the real one to arrive with nothing expecting it
    /// and be discarded. The join then never completed and never failed, which is a session
    /// stuck on Joining with nothing to time it out.
    LobbyId requested_lobby_{0};

    /// Detects a completed entry by reading the lobby's member list.
    ///
    /// Inside the game the host application owns the Steam callback pump, so waiting to be
    /// told about an entry means waiting on something this mod neither controls nor can
    /// measure. Asking is synchronous and owes it nothing.
    void NoticeEntryWithoutCallback();

    /// Cached display names, so a member who has left can still be named in the
    /// roster and in log output.
    std::unordered_map<PlatformId, std::string> name_cache_;

    mutable std::mutex queue_mutex_;
    std::deque<Event>  pending_; ///< Guarded by queue_mutex_.
};

} // namespace mpe::lobby
