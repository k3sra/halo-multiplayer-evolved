// SPDX-License-Identifier: MIT
// MultiplayerEvolved: Lobby/SteamMatchmakingHooks.cpp
#define MPE_LOG_CATEGORY "Lobby.Steam"

#include "Lobby/SteamMatchmakingHooks.h"

#include "Core/GameBuild.h"
#include "Core/Log.h"
#include "Net/PacketProtocol.h"

#include <algorithm>
#include <format>

namespace mpe::lobby {
namespace {

/// Steam caps lobby metadata values. Longer values are rejected by the platform
/// rather than truncated, so we check before the call and report a clear error.
constexpr std::size_t kMaxLobbyDataValue = 8192;
constexpr std::size_t kMaxLobbyDataKey   = 255;

/// Upper bound on members Steam will accept for one lobby.
constexpr std::uint32_t kMaxLobbyMembers = 250;

} // namespace

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

Expected<std::unique_ptr<SteamMatchmakingHooks>> SteamMatchmakingHooks::CreateInstance() {
    if (!steam::IsInitialized()) {
        return Error{ErrorCode::SteamUnavailable,
                     "the Steam binding is not initialized; call mpe::steam::Initialize first"};
    }
    if (!steam::IsLoggedOn()) {
        return Error{ErrorCode::SteamUnavailable,
                     "the local user is not logged on to Steam; multiplayer is unavailable in "
                     "offline mode"};
    }
    const steam::SteamId local = steam::GetLocalSteamId();
    if (local == 0) {
        return Error{ErrorCode::SteamUnavailable, "the local Steam ID is not available"};
    }

    auto hooks = std::unique_ptr<SteamMatchmakingHooks>(new SteamMatchmakingHooks());

    // Registered only once the object is fully constructed.
    hooks->lobby_enter_callback_.Register(hooks.get(), &SteamMatchmakingHooks::OnLobbyEnter);
    hooks->lobby_chat_update_callback_.Register(hooks.get(),
                                                &SteamMatchmakingHooks::OnLobbyChatUpdate);
    hooks->lobby_data_update_callback_.Register(hooks.get(),
                                                &SteamMatchmakingHooks::OnLobbyDataUpdate);
    hooks->lobby_join_requested_callback_.Register(
        hooks.get(), &SteamMatchmakingHooks::OnGameLobbyJoinRequested);
    hooks->rich_presence_join_callback_.Register(
        hooks.get(), &SteamMatchmakingHooks::OnRichPresenceJoinRequested);

    // What the browser searches for, set from the same constant the host publishes so a
    // search and an advertisement can never be looking for different things.
    steam::SetBrowseMarker(keys::kBrowseMarker, std::to_string(net::kProtocolVersion));

    MPE_LOG_INFO("steam matchmaking hooks registered for user {}", local);
    return hooks;
}

SteamMatchmakingHooks::~SteamMatchmakingHooks() {
    // Unregister first so a callback cannot land on a dying object, then release
    // the lobby.
    lobby_created_call_.Cancel();
    lobby_enter_callback_.Unregister();
    lobby_chat_update_callback_.Unregister();
    lobby_data_update_callback_.Unregister();
    lobby_join_requested_callback_.Unregister();
    rich_presence_join_callback_.Unregister();

    Leave();
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

steam::ELobbyType SteamMatchmakingHooks::TranslateVisibility(
    LobbyVisibility visibility) noexcept {
    switch (visibility) {
        case LobbyVisibility::Public:      return steam::ELobbyType::Public;
        case LobbyVisibility::FriendsOnly: return steam::ELobbyType::FriendsOnly;
        case LobbyVisibility::InviteOnly:  return steam::ELobbyType::Private;
    }
    return steam::ELobbyType::FriendsOnly;
}

Result SteamMatchmakingHooks::Create(LobbyVisibility visibility, std::uint32_t max_members) {
    if (InLobby()) {
        return Result::Fail(ErrorCode::InvalidState,
                            std::format("already in lobby {}", current_lobby_));
    }
    if (operation_in_flight_) {
        return Result::Fail(ErrorCode::InvalidState, "a lobby operation is already in flight");
    }
    if (max_members == 0 || max_members > kMaxLobbyMembers) {
        return Result::Fail(ErrorCode::InvalidArgument,
                            std::format("max_members {} is outside 1..{}", max_members,
                                        kMaxLobbyMembers));
    }

    const steam::SteamApiCall call =
        steam::CreateLobby(TranslateVisibility(visibility), static_cast<int>(max_members));
    if (call == steam::kInvalidApiCall) {
        return Result::Fail(ErrorCode::SteamCallFailed, "CreateLobby was not accepted by Steam");
    }

    lobby_created_call_.Set(call, this, &SteamMatchmakingHooks::OnLobbyCreatedResult);
    operation_in_flight_ = true;

    MPE_LOG_INFO("creating {} lobby for up to {} member(s)", ToString(visibility), max_members);
    return Result::Success();
}

Result SteamMatchmakingHooks::Join(LobbyId lobby) {
    if (InLobby()) {
        return Result::Fail(ErrorCode::InvalidState,
                            std::format("already in lobby {}", current_lobby_));
    }
    if (operation_in_flight_) {
        return Result::Fail(ErrorCode::InvalidState, "a lobby operation is already in flight");
    }
    if (lobby == 0) {
        return Result::Fail(ErrorCode::InvalidArgument, "lobby id is zero");
    }

    // JoinLobby also produces a LobbyEnter_t broadcast callback, which is what we
    // handle. The call result is not needed, so it is intentionally not tracked.
    const steam::SteamApiCall call = steam::JoinLobby(lobby);
    if (call == steam::kInvalidApiCall) {
        return Result::Fail(ErrorCode::SteamCallFailed, "JoinLobby was not accepted by Steam");
    }
    operation_in_flight_ = true;
    requested_lobby_     = lobby;

    MPE_LOG_INFO("joining lobby {}", lobby);
    return Result::Success();
}

void SteamMatchmakingHooks::Leave() {
    if (current_lobby_ != 0) {
        steam::LeaveLobby(current_lobby_);
        MPE_LOG_INFO("left lobby {}", current_lobby_);
    }
    current_lobby_       = 0;
    steam::SetCurrentLobby(0);
    is_owner_            = false;
    operation_in_flight_ = false;
    requested_lobby_     = 0;
    name_cache_.clear();
    ClearJoinablePresence();

    // The queue goes too.
    //
    // Anything still in it describes the lobby being left, and the next thing this backend
    // does is almost always join a different one. A leftover entry event would then be
    // delivered as though it were the answer to that join, which is how a player who backed
    // out to the menu and then picked a server ended up waiting on a lobby they had already
    // left.
    std::lock_guard lock(queue_mutex_);
    pending_.clear();
}

// ---------------------------------------------------------------------------
// Metadata
// ---------------------------------------------------------------------------

Result SteamMatchmakingHooks::SetLobbyData(std::string_view key, std::string_view value) {
    if (!InLobby()) {
        return Result::Fail(ErrorCode::InvalidState, "not in a lobby");
    }
    if (!is_owner_) {
        return Result::Fail(ErrorCode::InvalidState, "only the lobby owner may set lobby data");
    }
    if (key.empty() || key.size() > kMaxLobbyDataKey) {
        return Result::Fail(ErrorCode::InvalidArgument,
                            std::format("lobby data key length {} is outside 1..{}", key.size(),
                                        kMaxLobbyDataKey));
    }
    if (value.size() > kMaxLobbyDataValue) {
        return Result::Fail(ErrorCode::InvalidArgument,
                            std::format("lobby data value length {} exceeds {}", value.size(),
                                        kMaxLobbyDataValue));
    }

    // Steam's API is NUL terminated string based, so views must be materialized.
    const std::string key_z(key);
    const std::string value_z(value);
    if (!steam::SetLobbyData(current_lobby_, key_z.c_str(), value_z.c_str())) {
        return Result::Fail(ErrorCode::SteamCallFailed,
                            std::format("SetLobbyData('{}') was rejected", key_z));
    }
    return Result::Success();
}

Expected<std::string> SteamMatchmakingHooks::GetLobbyData(std::string_view key) const {
    if (!InLobby()) {
        return Error{ErrorCode::InvalidState, "not in a lobby"};
    }

    const std::string key_z(key);
    const char* const value = steam::GetLobbyData(current_lobby_, key_z.c_str());

    // Steam returns an empty string for an unset key rather than nullptr, so an
    // empty result is reported as NotFound. Callers that treat absence as a default
    // get a clean signal instead of silently reading "".
    if (value == nullptr || *value == '\0') {
        return Error{ErrorCode::NotFound, std::format("lobby data '{}' is not set", key_z)};
    }
    return std::string(value);
}

Result SteamMatchmakingHooks::SetMemberData(std::string_view key, std::string_view value) {
    if (!InLobby()) {
        return Result::Fail(ErrorCode::InvalidState, "not in a lobby");
    }
    if (key.empty() || key.size() > kMaxLobbyDataKey) {
        return Result::Fail(ErrorCode::InvalidArgument, "member data key length is out of range");
    }
    if (value.size() > kMaxLobbyDataValue) {
        return Result::Fail(ErrorCode::InvalidArgument, "member data value is too long");
    }

    const std::string key_z(key);
    const std::string value_z(value);
    // SetLobbyMemberData returns void: Steam applies it locally and replicates it,
    // and there is no synchronous failure to report.
    steam::SetLobbyMemberData(current_lobby_, key_z.c_str(), value_z.c_str());
    return Result::Success();
}

Expected<std::string> SteamMatchmakingHooks::GetMemberData(PlatformId member,
                                                            std::string_view key) const {
    if (!InLobby()) {
        return Error{ErrorCode::InvalidState, "not in a lobby"};
    }

    const std::string key_z(key);
    const char* const value = steam::GetLobbyMemberData(current_lobby_, member, key_z.c_str());
    if (value == nullptr || *value == '\0') {
        return Error{ErrorCode::NotFound,
                     std::format("member data '{}' is not set for {}", key_z, member)};
    }
    return std::string(value);
}

std::vector<LobbyMember> SteamMatchmakingHooks::Members() const {
    std::vector<LobbyMember> members;
    if (!InLobby()) {
        return members;
    }

    const steam::SteamId owner = steam::GetLobbyOwner(current_lobby_);
    const int            count = steam::GetNumLobbyMembers(current_lobby_);

    members.reserve(static_cast<std::size_t>(count > 0 ? count : 0));
    for (int i = 0; i < count; ++i) {
        const steam::SteamId id = steam::GetLobbyMemberByIndex(current_lobby_, i);
        if (id == 0) {
            continue;
        }

        LobbyMember member;
        member.platform_id = id;
        member.is_owner    = (id == owner);

        const char* const name = steam::GetFriendPersonaName(id);
        member.display_name = (name != nullptr) ? name : "";
        if (member.display_name.empty()) {
            // A persona name is unavailable until Steam has downloaded it. Fall back
            // to the cache, then to the numeric id, so the roster never shows a blank
            // row.
            const auto cached = name_cache_.find(member.platform_id);
            member.display_name = (cached != name_cache_.end())
                                      ? cached->second
                                      : std::format("Player {}", member.platform_id);
        }
        members.push_back(std::move(member));
    }
    return members;
}

std::size_t SteamMatchmakingHooks::MemberCount() const noexcept {
    if (!InLobby()) {
        return 0;
    }
    const int count = steam::GetNumLobbyMembers(current_lobby_);
    return count > 0 ? static_cast<std::size_t>(count) : 0;
}

Result SteamMatchmakingHooks::SetVisibility(LobbyVisibility visibility) {
    if (!InLobby()) {
        return Result::Fail(ErrorCode::InvalidState, "not in a lobby");
    }
    if (!is_owner_) {
        return Result::Fail(ErrorCode::InvalidState, "only the lobby owner may set visibility");
    }
    if (!steam::SetLobbyType(current_lobby_, TranslateVisibility(visibility))) {
        return Result::Fail(ErrorCode::SteamCallFailed, "SetLobbyType was rejected");
    }
    MPE_LOG_INFO("lobby visibility set to {}", ToString(visibility));
    return Result::Success();
}

// ---------------------------------------------------------------------------
// Invitations
// ---------------------------------------------------------------------------

Result SteamMatchmakingHooks::OpenInviteOverlay() {
    if (!InLobby()) {
        return Result::Fail(ErrorCode::InvalidState, "not in a lobby");
    }
    steam::ActivateGameOverlayInviteDialog(current_lobby_);
    MPE_LOG_INFO("opened the Steam invite overlay for lobby {}", current_lobby_);
    return Result::Success();
}

Result SteamMatchmakingHooks::PublishJoinablePresence(std::string_view status_text) {
    if (!InLobby()) {
        return Result::Fail(ErrorCode::InvalidState, "not in a lobby");
    }

    // The "connect" key is what puts a working Join Game entry in the friends list.
    // Steam hands this string back through GameRichPresenceJoinRequested_t when a
    // friend uses it, so it carries the lobby id and nothing else.
    const std::string connect = std::format("+fe_lobby {}", current_lobby_);
    if (!steam::SetRichPresence("connect", connect.c_str())) {
        return Result::Fail(ErrorCode::SteamCallFailed,
                            "SetRichPresence('connect') was rejected");
    }

    const std::string status(status_text);
    if (!status.empty() && !steam::SetRichPresence("status", status.c_str())) {
        MPE_LOG_WARN("SetRichPresence('status') was rejected; presence text will be absent");
    }
    return Result::Success();
}

void SteamMatchmakingHooks::ClearJoinablePresence() {
    (void)steam::SetRichPresence("connect", "");
    (void)steam::SetRichPresence("status", "");
}

// ---------------------------------------------------------------------------
// Local identity
// ---------------------------------------------------------------------------

Expected<PlatformId> SteamMatchmakingHooks::LocalId() const {
    const steam::SteamId id = steam::GetLocalSteamId();
    if (id == 0) {
        return Error{ErrorCode::SteamUnavailable, "local Steam ID is not valid"};
    }
    return id;
}

Expected<std::string> SteamMatchmakingHooks::LocalDisplayName() const {
    const char* const name = steam::GetPersonaName();
    if (name == nullptr || *name == '\0') {
        return Error{ErrorCode::NotFound, "local persona name is unavailable"};
    }
    return std::string(name);
}

// ---------------------------------------------------------------------------
// Event queue
// ---------------------------------------------------------------------------

void SteamMatchmakingHooks::Enqueue(Event event) {
    std::lock_guard lock(queue_mutex_);
    pending_.push_back(std::move(event));
}

void SteamMatchmakingHooks::Poll(ILobbyBackendObserver& observer) {
    NoticeEntryWithoutCallback();

    std::deque<Event> batch;
    {
        std::lock_guard lock(queue_mutex_);
        batch.swap(pending_);
    }
    for (const Event& event : batch) {
        Dispatch(event, observer);
    }
}

void SteamMatchmakingHooks::NoticeEntryWithoutCallback() {
    // Entering a lobby is detected by asking, not only by being told.
    //
    // LobbyEnter_t is a broadcast callback, and inside the game this mod does not own the
    // Steam callback pump: the host application does. Every entry therefore waited on
    // whenever the game next chose to dispatch callbacks, which is not something this mod
    // controls or can measure, and it is the largest part of why joining somebody took long
    // enough for a player to conclude it had hung.
    //
    // Membership is readable synchronously, so it is read. A player appears in a lobby's own
    // member list only once they are actually in it, which makes finding our own id there
    // proof of entry that owes the callback nothing. The callback still arrives later and is
    // ignored, because requested_lobby_ has been cleared by then.
    if (requested_lobby_ == 0) {
        return;
    }

    const steam::SteamId local = steam::GetLocalSteamId();
    if (local == 0) {
        return;
    }

    const int members = steam::GetNumLobbyMembers(requested_lobby_);
    for (int index = 0; index < members; ++index) {
        if (steam::GetLobbyMemberByIndex(requested_lobby_, index) != local) {
            continue;
        }
        MPE_LOG_INFO("entry into lobby {} observed directly; not waiting for the callback",
                    requested_lobby_);
        Enqueue(Event{EventKind::LobbyEntered, requested_lobby_, 0, false, ErrorCode::None,
                      ""});
        // Cleared here rather than in the dispatch, so a second pass before the queue drains
        // cannot enqueue the same entry twice.
        requested_lobby_ = 0;
        return;
    }
}

void SteamMatchmakingHooks::Dispatch(const Event& event, ILobbyBackendObserver& observer) {
    switch (event.kind) {
        case EventKind::LobbyCreated: {
            current_lobby_       = event.lobby;
            is_owner_            = true;
            operation_in_flight_ = false;
            steam::SetCurrentLobby(event.lobby);

            const Result published = PublishJoinMetadata();
            if (!published.ok()) {
                // The lobby exists but is not describable, so nobody could join it
                // correctly. Report it as a creation failure and release the lobby.
                const Error error = published.error();
                MPE_LOG_ERROR("publishing join metadata failed: {}", error.message);
                Leave();
                observer.OnLobbyCreateFailed(error);
                return;
            }
            observer.OnLobbyCreated(event.lobby);
            return;
        }

        case EventKind::LobbyCreateFailed:
            operation_in_flight_ = false;
            observer.OnLobbyCreateFailed(Error{event.error_code, event.detail});
            return;

        case EventKind::LobbyEntered:
            // Only lobbies this mod asked for, and only the one it asked for.
            //
            // Steam raises LobbyEnter for every lobby the process joins, and the game uses
            // Steam lobbies for its own fireteam. Recording those made the backend believe
            // it was already in a lobby it had never created, so hosting was refused with
            // "already in lobby" and no multiplayer session could exist at all.
            //
            // Which lobby matters as much as whether one was asked for. Testing only
            // "is a request outstanding" cannot tell one lobby from another, and leaving a
            // lobby to join a different one is precisely where that fails: the enter event
            // for the lobby just left can still be in the queue, and it would consume the
            // outstanding request, be reported as the lobby that was joined, and leave the
            // real one to arrive with nothing expecting it and be discarded. The join then
            // neither completed nor failed. That is a session stuck on Joining forever,
            // which is what a player saw after picking a server out of the browser.
            if (requested_lobby_ != 0 && event.lobby != requested_lobby_) {
                MPE_LOG_INFO("ignoring entry into lobby {} while waiting for {}", event.lobby,
                            requested_lobby_);
                return;
            }
            if (requested_lobby_ == 0 && !operation_in_flight_ &&
                event.lobby != current_lobby_) {
                MPE_LOG_INFO("ignoring lobby {}, which this mod did not open", event.lobby);
                return;
            }
            current_lobby_       = event.lobby;
            operation_in_flight_ = false;
            requested_lobby_     = 0;
            steam::SetCurrentLobby(event.lobby);
            RefreshOwnership();
            observer.OnLobbyEntered(event.lobby, is_owner_);
            return;

        case EventKind::LobbyEnterFailed:
            if (requested_lobby_ != 0 && event.lobby != requested_lobby_) {
                MPE_LOG_INFO("ignoring a failure to enter lobby {} while waiting for {}",
                            event.lobby, requested_lobby_);
                return;
            }
            current_lobby_       = 0;
            steam::SetCurrentLobby(0);
            is_owner_            = false;
            operation_in_flight_ = false;
            requested_lobby_     = 0;
            observer.OnLobbyEnterFailed(Error{event.error_code, event.detail});
            return;

        case EventKind::MemberJoined: {
            LobbyMember member;
            member.platform_id  = event.member;
            member.display_name = event.detail;
            member.is_owner     = (steam::GetLobbyOwner(current_lobby_) == event.member);
            if (!member.display_name.empty()) {
                name_cache_[member.platform_id] = member.display_name;
            }
            observer.OnMemberJoined(member);
            return;
        }

        case EventKind::MemberLeft:
            // Ownership can change when the owner is the one who left: Steam promotes
            // another member, and the roster must reflect that before the observer
            // decides what to do.
            RefreshOwnership();
            observer.OnMemberLeft(event.member, event.flag);
            return;

        case EventKind::LobbyDataChanged:
            observer.OnLobbyDataChanged(event.lobby);
            return;

        case EventKind::MemberDataChanged:
            observer.OnMemberDataChanged(event.member);
            return;

        case EventKind::JoinRequested:
            observer.OnJoinRequested(event.lobby, event.member);
            return;
    }
}

void SteamMatchmakingHooks::RefreshOwnership() {
    is_owner_ = false;
    if (current_lobby_ == 0) {
        return;
    }
    const steam::SteamId local = steam::GetLocalSteamId();
    is_owner_ = (local != 0) && (steam::GetLobbyOwner(current_lobby_) == local);
}

Result SteamMatchmakingHooks::PublishJoinMetadata() {
    MPE_ASSIGN_OR_RETURN(const PlatformId local_id, LocalId());

    // First, and checked, because the server browser's Steam side filter matches on it.
    // A lobby without it is invisible to everybody searching, however well the rest of
    // the session works.
    MPE_TRY(SetLobbyData(keys::kBrowseMarker, std::to_string(net::kProtocolVersion)));
    MPE_TRY(SetLobbyData(keys::kProtocolVersion, std::to_string(net::kProtocolVersion)));
    MPE_TRY(SetLobbyData(keys::kGameBuild, GameBuildString()));
    MPE_TRY(SetLobbyData(keys::kHostId, std::to_string(local_id)));
    return Result::Success();
}

// ---------------------------------------------------------------------------
// Steam callbacks. Enqueue only.
// ---------------------------------------------------------------------------

void SteamMatchmakingHooks::OnLobbyCreatedResult(steam::LobbyCreatedCallback* result,
                                                 bool io_failure) {
    if (result == nullptr || io_failure) {
        Enqueue(Event{EventKind::LobbyCreateFailed, 0, 0, false, ErrorCode::SteamCallFailed,
                      "CreateLobby failed in transit to the Steam backend"});
        return;
    }
    if (result->m_eResult != steam::EResult::Ok) {
        Enqueue(Event{EventKind::LobbyCreateFailed, 0, 0, false, ErrorCode::LobbyUnavailable,
                      std::format("CreateLobby failed with EResult {}",
                                  static_cast<int>(result->m_eResult))});
        return;
    }
    Enqueue(Event{EventKind::LobbyCreated, result->m_ulSteamIDLobby, 0, true, ErrorCode::None,
                  ""});
}

void SteamMatchmakingHooks::OnLobbyEnter(steam::LobbyEnterCallback* callback) {
    if (callback == nullptr) {
        return;
    }
    if (callback->m_EChatRoomEnterResponse !=
        static_cast<std::uint32_t>(steam::EChatRoomEnterResponse::Success)) {
        // Each response maps to something a player can act on, so it is reported
        // verbatim rather than collapsed into a generic failure.
        Enqueue(Event{EventKind::LobbyEnterFailed, callback->m_ulSteamIDLobby, 0, false,
                      ErrorCode::LobbyUnavailable,
                      std::format("could not enter the lobby (chat room response {})",
                                  callback->m_EChatRoomEnterResponse)});
        return;
    }
    Enqueue(Event{EventKind::LobbyEntered, callback->m_ulSteamIDLobby, 0, false, ErrorCode::None,
                  ""});
}

void SteamMatchmakingHooks::OnLobbyChatUpdate(steam::LobbyChatUpdateCallback* callback) {
    if (callback == nullptr) {
        return;
    }
    const PlatformId    changed = callback->m_ulSteamIDUserChanged;
    const std::uint32_t state   = callback->m_rgfChatMemberStateChange;

    if ((state & steam::kChatMemberStateChangeEntered) != 0) {
        std::string name;
        if (const char* const persona = steam::GetFriendPersonaName(changed)) {
            name.assign(persona);
        }
        Enqueue(Event{EventKind::MemberJoined, callback->m_ulSteamIDLobby, changed, false,
                      ErrorCode::None, std::move(name)});
        return;
    }

    // Left, disconnected, kicked and banned all mean the member is gone. Only
    // kicked and banned are distinguished, because those are the cases where the
    // player deserves a different message.
    constexpr std::uint32_t kGone =
        steam::kChatMemberStateChangeLeft | steam::kChatMemberStateChangeDisconnected |
        steam::kChatMemberStateChangeKicked | steam::kChatMemberStateChangeBanned;
    if ((state & kGone) != 0) {
        const bool was_kicked = (state & (steam::kChatMemberStateChangeKicked |
                                          steam::kChatMemberStateChangeBanned)) != 0;
        Enqueue(Event{EventKind::MemberLeft, callback->m_ulSteamIDLobby, changed, was_kicked,
                      ErrorCode::None, ""});
    }
}

void SteamMatchmakingHooks::OnLobbyDataUpdate(steam::LobbyDataUpdateCallback* callback) {
    if (callback == nullptr || callback->m_bSuccess == 0) {
        return;
    }
    // Steam uses one callback for both cases and distinguishes them by whether the
    // changed id equals the lobby id.
    if (callback->m_ulSteamIDMember == callback->m_ulSteamIDLobby) {
        Enqueue(Event{EventKind::LobbyDataChanged, callback->m_ulSteamIDLobby, 0, false,
                      ErrorCode::None, ""});
    } else {
        Enqueue(Event{EventKind::MemberDataChanged, callback->m_ulSteamIDLobby,
                      callback->m_ulSteamIDMember, false, ErrorCode::None, ""});
    }
}

void SteamMatchmakingHooks::OnGameLobbyJoinRequested(
    steam::GameLobbyJoinRequestedCallback* callback) {
    if (callback == nullptr) {
        return;
    }
    // Fired when the player accepts an invitation while the game is already
    // running. This is the overlay invite path.
    Enqueue(Event{EventKind::JoinRequested, callback->m_steamIDLobby, callback->m_steamIDFriend,
                  false, ErrorCode::None, ""});
}

void SteamMatchmakingHooks::OnRichPresenceJoinRequested(
    steam::GameRichPresenceJoinRequestedCallback* callback) {
    if (callback == nullptr) {
        return;
    }

    // The connect string is the one PublishJoinablePresence wrote: "+fe_lobby <id>".
    // Parsed defensively: this value round trips through the Steam backend and must
    // not be trusted to be well formed or NUL terminated.
    const char* const raw = callback->m_rgchConnect;
    const std::size_t raw_length =
        static_cast<std::size_t>(std::find(raw, raw + sizeof(callback->m_rgchConnect), '\0') - raw);
    const std::string_view connect(raw, raw_length);

    constexpr std::string_view kPrefix = "+fe_lobby ";
    if (!connect.starts_with(kPrefix)) {
        MPE_LOG_WARN("ignoring a rich presence join with an unrecognized connect string");
        return;
    }

    const std::string_view id_text = connect.substr(kPrefix.size());
    if (id_text.empty()) {
        MPE_LOG_WARN("ignoring a rich presence join with no lobby id");
        return;
    }

    LobbyId lobby = 0;
    for (const char c : id_text) {
        if (c < '0' || c > '9') {
            MPE_LOG_WARN("ignoring a rich presence join with a non numeric lobby id");
            return;
        }
        // Overflow guard: a value that would wrap is malformed.
        if (lobby > (UINT64_MAX - static_cast<std::uint64_t>(c - '0')) / 10u) {
            MPE_LOG_WARN("ignoring a rich presence join with an out of range lobby id");
            return;
        }
        lobby = lobby * 10u + static_cast<std::uint64_t>(c - '0');
    }
    if (lobby == 0) {
        MPE_LOG_WARN("ignoring a rich presence join with a zero lobby id");
        return;
    }

    Enqueue(Event{EventKind::JoinRequested, lobby, callback->m_steamIDFriend, false,
                  ErrorCode::None, ""});
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

std::string_view ToString(LobbyVisibility visibility) noexcept {
    switch (visibility) {
        case LobbyVisibility::Public:      return "public";
        case LobbyVisibility::FriendsOnly: return "friends_only";
        case LobbyVisibility::InviteOnly:  return "invite_only";
    }
    return "unknown";
}

} // namespace mpe::lobby
