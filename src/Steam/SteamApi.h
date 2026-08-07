// SPDX-License-Identifier: MIT
// MultiplayerEvolved: Steam/SteamApi.h
//
// Dynamic binding to the steam_api64.dll the game already ships.
//
// LOADING STRATEGY
//
// GetModuleHandle first: inside the game process steam_api64.dll is already
// loaded, and binding to that instance is essential. A second copy loaded from a
// different path would have its own callback registry, so our callbacks would
// never be dispatched by the game's SteamAPI_RunCallbacks.
//
// LoadLibrary from the game's own ThirdParty path is the fallback, for the case
// where the mod starts before the shell has loaded Steam.
//
// INTERFACE VERSIONS
//
// steam_api64.dll exports flat accessors for some interfaces
// (SteamAPI_SteamMatchmaking_v009, SteamAPI_SteamFriends_v017) but not for the
// networking ones. So every interface is acquired through
// SteamInternal_FindOrCreateUserInterface with an explicit version string, which
// is what the SDK's own inline accessors do. Each interface has a list of
// candidate versions tried newest first, so a Steam client update that moves a
// version does not break the mod.
//
// CALLBACK ABI
//
// CallbackBase reproduces CCallbackBase exactly: three virtuals in a fixed order,
// then a flags byte and the callback id. steam_api64.dll walks that layout, so the
// order matters and a virtual destructor must not be added. sizeof is asserted.
#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <string_view>

#include "Steam/SteamTypes.h"

namespace mpe::steam {

// ---------------------------------------------------------------------------
// Callback ABI
// ---------------------------------------------------------------------------

/// Layout compatible with the SDK's CCallbackBase.
///
/// Do not add virtual functions, do not reorder, and do not add a virtual
/// destructor. steam_api64.dll indexes this vtable directly.
class CallbackBase {
public:
    CallbackBase() = default;

    CallbackBase(const CallbackBase&)            = delete;
    CallbackBase& operator=(const CallbackBase&) = delete;

    /// Slot 0: a broadcast callback fired.
    virtual void Run(void* param) = 0;

    /// Slot 1: a call result completed.
    virtual void Run(void* param, bool io_failure, SteamApiCall call) = 0;

    /// Slot 2: Steam asks how large the payload is before copying into it. A wrong
    /// answer here is a buffer overrun, which is why every payload asserts its size.
    virtual int GetCallbackSizeBytes() = 0;

    [[nodiscard]] int GetCallbackId() const noexcept { return m_iCallback; }

protected:
    ~CallbackBase() = default; ///< Non virtual by design; never deleted through a base pointer.

    enum : std::uint8_t {
        kFlagRegistered = 0x01,
        kFlagGameServer = 0x02,
    };

    std::uint8_t m_nCallbackFlags{0};
    int          m_iCallback{0};
};

static_assert(sizeof(CallbackBase) == 16,
              "CallbackBase must match CCallbackBase: vptr, flags byte, callback id");

// Registration, implemented against the DLL's exports.
void RegisterCallback(CallbackBase* callback, int callback_id);
void UnregisterCallback(CallbackBase* callback);
void RegisterCallResult(CallbackBase* callback, SteamApiCall call);
void UnregisterCallResult(CallbackBase* callback, SteamApiCall call);

/// Broadcast callback bound to a member function.
///
/// Usage:
///   Callback<MyClass, LobbyEnterCallback> lobby_enter_;
///   lobby_enter_.Register(this, &MyClass::OnLobbyEnter);
template <typename Owner, typename Payload>
class Callback final : public CallbackBase {
public:
    using Handler = void (Owner::*)(Payload*);

    Callback() { m_iCallback = Payload::kCallbackId; }
    ~Callback() { Unregister(); }

    /// Registers with Steam. Registering twice is a no-op after the first.
    void Register(Owner* owner, Handler handler) {
        if (owner == nullptr || handler == nullptr) {
            return;
        }
        if ((m_nCallbackFlags & kFlagRegistered) != 0) {
            return;
        }
        owner_   = owner;
        handler_ = handler;
        RegisterCallback(this, Payload::kCallbackId);
        m_nCallbackFlags |= kFlagRegistered;
    }

    void Unregister() {
        if ((m_nCallbackFlags & kFlagRegistered) == 0) {
            return;
        }
        UnregisterCallback(this);
        m_nCallbackFlags = static_cast<std::uint8_t>(m_nCallbackFlags & ~kFlagRegistered);
        owner_   = nullptr;
        handler_ = nullptr;
    }

    [[nodiscard]] bool IsRegistered() const noexcept {
        return (m_nCallbackFlags & kFlagRegistered) != 0;
    }

    void Run(void* param) override {
        if (owner_ != nullptr && handler_ != nullptr && param != nullptr) {
            (owner_->*handler_)(static_cast<Payload*>(param));
        }
    }

    /// A broadcast callback never arrives through the call result slot, but Steam
    /// requires the entry to exist. Forwarded rather than ignored so a future
    /// change in dispatch cannot silently drop events.
    void Run(void* param, bool /*io_failure*/, SteamApiCall /*call*/) override { Run(param); }

    int GetCallbackSizeBytes() override { return static_cast<int>(sizeof(Payload)); }

private:
    Owner*  owner_{nullptr};
    Handler handler_{nullptr};
};

/// Result of one asynchronous Steam call.
template <typename Owner, typename Payload>
class CallResult final : public CallbackBase {
public:
    using Handler = void (Owner::*)(Payload*, bool io_failure);

    CallResult() { m_iCallback = Payload::kCallbackId; }
    ~CallResult() { Cancel(); }

    void Set(SteamApiCall call, Owner* owner, Handler handler) {
        Cancel();
        if (call == kInvalidApiCall || owner == nullptr || handler == nullptr) {
            return;
        }
        call_    = call;
        owner_   = owner;
        handler_ = handler;
        RegisterCallResult(this, call);
    }

    [[nodiscard]] bool IsActive() const noexcept { return call_ != kInvalidApiCall; }

    void Cancel() {
        if (call_ == kInvalidApiCall) {
            return;
        }
        UnregisterCallResult(this, call_);
        call_    = kInvalidApiCall;
        owner_   = nullptr;
        handler_ = nullptr;
    }

    /// Not used for a call result, but the vtable slot must be filled.
    void Run(void* param) override { Run(param, false, call_); }

    void Run(void* param, bool io_failure, SteamApiCall call) override {
        // A stale completion for a call we already cancelled must be ignored,
        // otherwise a cancelled lobby creation could still drive the state machine.
        if (call != call_ || param == nullptr) {
            return;
        }
        Owner* const  owner   = owner_;
        Handler const handler = handler_;

        // Cleared before the handler runs, so the handler may safely start a new
        // call on this same object.
        call_    = kInvalidApiCall;
        owner_   = nullptr;
        handler_ = nullptr;

        if (owner != nullptr && handler != nullptr) {
            (owner->*handler)(static_cast<Payload*>(param), io_failure);
        }
    }

    int GetCallbackSizeBytes() override { return static_cast<int>(sizeof(Payload)); }

private:
    SteamApiCall call_{kInvalidApiCall};
    Owner*       owner_{nullptr};
    Handler      handler_{nullptr};
};

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

/// Binds to steam_api64.dll and resolves every function and interface.
///
/// Idempotent. Returns false with a reason in out_error when Steam is not usable,
/// which is a normal condition (client not running, user offline) and not a bug.
///
/// game_binaries_directory is used only for the LoadLibrary fallback path.
///
/// allow_load_if_absent decides what happens when steam_api64.dll is not already in
/// the process:
///
///   false  Fail, leaving the module untouched. This is what the mod uses inside the
///          game while it waits for the shell to load Steam itself. Loading it
///          ourselves would mean calling SteamAPI_Init before the host application
///          does, which is not ours to do and makes startup order depend on who wins
///          a race. That actually happened: two runs of the same build produced
///          owned=false and owned=true.
///
///   true   Load it and call SteamAPI_Init, taking ownership. Correct for a test
///          harness or a headless server, where nobody else will.
[[nodiscard]] bool Initialize(std::string_view game_binaries_directory,
                             bool allow_load_if_absent, std::string& out_error);

/// Releases the module handle. Does not shut down Steam; the game owns that.
void Shutdown();

[[nodiscard]] bool IsInitialized() noexcept;

/// True when this binding called SteamAPI_Init, meaning this process owns the Steam
/// API and is therefore responsible for pumping its callbacks.
///
/// False inside the game, where the host application owns both.
[[nodiscard]] bool OwnsSteamApi() noexcept;

/// Dispatches queued Steam callbacks.
///
/// Only call this when OwnsSteamApi() is true. Inside the game the host application
/// pumps callbacks on its own thread, and a second pump would race with it.
///
/// Without a pump, no callback and no call result ever fires: CreateLobby returns a
/// handle and then nothing happens, which presents as a timeout.
void RunCallbacks();

/// True when the interfaces needed for networking resolved. Lobby only features
/// can work without this.
[[nodiscard]] bool HasNetworkingSockets() noexcept;

/// Multi line report of the resolved versions and struct sizes, for the log.
[[nodiscard]] std::string DescribeBinding();

// ---------------------------------------------------------------------------
// ISteamUser
// ---------------------------------------------------------------------------

[[nodiscard]] SteamId GetLocalSteamId();
[[nodiscard]] bool IsLoggedOn();

// ---------------------------------------------------------------------------
// ISteamFriends
// ---------------------------------------------------------------------------

[[nodiscard]] const char* GetPersonaName();
[[nodiscard]] const char* GetFriendPersonaName(SteamId user);
void ActivateGameOverlayInviteDialog(SteamId lobby);

/// Whether Steam's in-game overlay is available in this process.
///
/// The invite dialog is drawn by the overlay, so with the overlay off the call succeeds and
/// nothing appears. Knowing this is the difference between telling a player their Steam
/// setting is off and letting them conclude the mod is broken.
[[nodiscard]] bool IsOverlayEnabled();

/// One person on the friends list, and whether they can act on an invite.
struct GameFriend {
    SteamId     id{0};
    std::string name;
    /// True when Steam reports them playing this game. Invites reach anybody, but only
    /// somebody already in the game can accept one without a download first.
    bool in_this_game{false};
};

/// The friends list, with the people playing this game sorted to the front.
///
/// The overlay's invite dialog does not draw over this title, so the mod asks Steam who is
/// there and presents them itself. Everybody is listed rather than only players of this
/// game: a friend who owns it and is online can still accept, and hiding them would leave
/// an empty picker for no reason the player can see.
[[nodiscard]] std::vector<GameFriend> FriendsInGame();

/// Invites one person to a lobby. Independent of the overlay.
[[nodiscard]] bool InviteUserToLobby(SteamId lobby, SteamId user);
[[nodiscard]] bool SetRichPresence(const char* key, const char* value);

// ---------------------------------------------------------------------------
// ISteamMatchmaking
// ---------------------------------------------------------------------------

[[nodiscard]] SteamApiCall CreateLobby(ELobbyType type, int max_members);
[[nodiscard]] SteamApiCall JoinLobby(SteamId lobby);
void LeaveLobby(SteamId lobby);
[[nodiscard]] bool SetLobbyData(SteamId lobby, const char* key, const char* value);
[[nodiscard]] const char* GetLobbyData(SteamId lobby, const char* key);
void SetLobbyMemberData(SteamId lobby, const char* key, const char* value);
[[nodiscard]] const char* GetLobbyMemberData(SteamId lobby, SteamId user, const char* key);
[[nodiscard]] int GetNumLobbyMembers(SteamId lobby);
[[nodiscard]] SteamId GetLobbyMemberByIndex(SteamId lobby, int index);
[[nodiscard]] SteamId GetLobbyOwner(SteamId lobby);

/// One public lobby found by a search.
struct LobbyListing {
    SteamId     id{0};
    std::string name;
    std::string mode;
    std::string map;
    int         members{0};
    int         capacity{0};
    int         ping_milliseconds{0};
    /// The host's Steam id as text. Present on every lobby this mod created, which is
    /// what tells one apart from the game's own fireteam lobbies.
    std::string host_id;
    /// What the session is doing, as the host last published it: hosting, in_match and
    /// so on. Empty when the host has not published one yet.
    std::string phase;
};

/// Sets the key and value that marks a lobby as one of this mod's.
///
/// Both the search filter and the listing check use it, so the two can never disagree
/// about what counts as ours. They did once: the reader looked for a key named after the
/// project's old name that no writer had ever set, and the browser silently returned
/// nothing forever.
///
/// The value carries the protocol version, so a search only ever returns sessions this
/// build can actually speak to.
void SetBrowseMarker(std::string_view key, std::string_view value);

/// True once a Steam search has returned this machine's own lobby.
///
/// The one part of discovery that can be proven without a second person. If our own lobby
/// comes back from a real search then the marker was published in a form Steam's string
/// filter matches, the filter was applied, and a lobby carrying it was returned; a friend's
/// lobby travels the identical path with a different id. If it never comes back, the
/// browser will be empty for everybody and this says so before anyone tries.
[[nodiscard]] bool LastSearchSawOwnLobby();

/// Asks Steam for public lobbies advertising this game.
///
/// The request is asynchronous, so this starts one and returns immediately. Results appear
/// through BrowseLobbies once Steam answers, which is what lets a browser refresh without
/// stalling the caller.
void RequestLobbyList();

/// Lobbies from the most recent completed search.
[[nodiscard]] std::vector<LobbyListing> BrowseLobbies();

/// Records how many lobbies the last search returned.
void SetBrowseResultCount(int count);

/// The lobby this process occupies, so the browser can leave it out of its own results.
[[nodiscard]] SteamId CurrentLobby();
void SetCurrentLobby(SteamId lobby);
[[nodiscard]] bool SetLobbyType(SteamId lobby, ELobbyType type);

// ---------------------------------------------------------------------------
// ISteamNetworkingUtils
// ---------------------------------------------------------------------------

void InitRelayNetworkAccess();
[[nodiscard]] SteamNetworkingMessage* AllocateMessage(int payload_size);

// ---------------------------------------------------------------------------
// ISteamNetworkingSockets
// ---------------------------------------------------------------------------

[[nodiscard]] HSteamListenSocket CreateListenSocketP2P(int virtual_port, int option_count,
                                                       const SteamNetworkingConfigValue* options);
[[nodiscard]] bool CloseListenSocket(HSteamListenSocket socket);

[[nodiscard]] HSteamNetConnection ConnectP2P(const SteamNetworkingIdentity& peer,
                                            int virtual_port, int option_count,
                                            const SteamNetworkingConfigValue* options);
[[nodiscard]] EResult AcceptConnection(HSteamNetConnection connection);
[[nodiscard]] bool CloseConnection(HSteamNetConnection connection, int reason,
                                  const char* debug_text, bool enable_linger);

[[nodiscard]] HSteamNetPollGroup CreatePollGroup();
[[nodiscard]] bool DestroyPollGroup(HSteamNetPollGroup group);
[[nodiscard]] bool SetConnectionPollGroup(HSteamNetConnection connection,
                                         HSteamNetPollGroup group);

void SendMessages(int message_count, SteamNetworkingMessage* const* messages,
                  std::int64_t* out_message_numbers);
[[nodiscard]] int ReceiveMessagesOnPollGroup(HSteamNetPollGroup group,
                                            SteamNetworkingMessage** out_messages,
                                            int max_messages);
[[nodiscard]] EResult FlushMessagesOnConnection(HSteamNetConnection connection);

[[nodiscard]] EResult ConfigureConnectionLanes(HSteamNetConnection connection, int lane_count,
                                              const int* priorities,
                                              const std::uint16_t* weights);

[[nodiscard]] bool GetConnectionInfo(HSteamNetConnection connection,
                                    SteamNetConnectionInfo* out_info);
[[nodiscard]] EResult GetConnectionRealTimeStatus(HSteamNetConnection connection,
                                                 SteamNetConnectionRealTimeStatus* out_status,
                                                 int lane_count, void* out_lanes);

} // namespace mpe::steam


