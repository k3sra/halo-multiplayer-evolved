// SPDX-License-Identifier: MIT
// MultiplayerEvolved: Steam/SteamApi.cpp
#define MPE_LOG_CATEGORY "Steam.Api"

#include "Steam/SteamApi.h"

#include "Core/Log.h"
#include "Lobby/Discovery.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <algorithm>
#include <format>
#include <mutex>

namespace mpe::steam {
namespace {

// ---------------------------------------------------------------------------
// Function pointer table
// ---------------------------------------------------------------------------

using PFN_GetHSteamUser          = HSteamUser (*)();
using PFN_FindOrCreateUserIface  = void* (*)(HSteamUser, const char*);
using PFN_SteamApiInit           = bool (*)();
using PFN_SteamApiShutdown       = void (*)();
using PFN_SteamApiRunCallbacks   = void (*)();
using PFN_RegisterCallback       = void (*)(CallbackBase*, int);
using PFN_UnregisterCallback     = void (*)(CallbackBase*);
using PFN_RegisterCallResult     = void (*)(CallbackBase*, SteamApiCall);
using PFN_UnregisterCallResult   = void (*)(CallbackBase*, SteamApiCall);

// ISteamUser
using PFN_User_GetSteamId  = SteamId (*)(void*);
using PFN_User_BLoggedOn   = bool (*)(void*);

// ISteamFriends
using PFN_Friends_GetPersonaName        = const char* (*)(void*);
using PFN_Friends_GetFriendPersonaName  = const char* (*)(void*, SteamId);
using PFN_Friends_ActivateInviteDialog  = void (*)(void*, SteamId);
using PFN_Friends_SetRichPresence       = bool (*)(void*, const char*, const char*);

// ISteamMatchmaking
using PFN_MM_CreateLobby          = SteamApiCall (*)(void*, ELobbyType, int);
using PFN_MM_JoinLobby            = SteamApiCall (*)(void*, SteamId);
using PFN_MM_LeaveLobby           = void (*)(void*, SteamId);
using PFN_MM_SetLobbyData         = bool (*)(void*, SteamId, const char*, const char*);
using PFN_MM_GetLobbyData         = const char* (*)(void*, SteamId, const char*);
using PFN_MM_SetLobbyMemberData   = void (*)(void*, SteamId, const char*, const char*);
using PFN_MM_GetLobbyMemberData   = const char* (*)(void*, SteamId, SteamId, const char*);
using PFN_MM_GetNumLobbyMembers   = int (*)(void*, SteamId);
using PFN_MM_RequestLobbyList     = SteamApiCall (*)(void*);
using PFN_MM_GetLobbyByIndex      = SteamId (*)(void*, int);
using PFN_MM_AddDistanceFilter    = void (*)(void*, int);
using PFN_MM_AddResultCountFilter = void (*)(void*, int);
using PFN_MM_GetLobbyMemberLimit  = int (*)(void*, SteamId);
using PFN_MM_GetLobbyMemberByIndex = SteamId (*)(void*, SteamId, int);
using PFN_MM_GetLobbyOwner        = SteamId (*)(void*, SteamId);
using PFN_MM_SetLobbyType         = bool (*)(void*, SteamId, ELobbyType);

// ISteamNetworkingUtils
using PFN_NU_InitRelayNetworkAccess = void (*)(void*);
using PFN_NU_AllocateMessage        = SteamNetworkingMessage* (*)(void*, int);

// ISteamNetworkingSockets
using PFN_NS_CreateListenSocketP2P = HSteamListenSocket (*)(void*, int, int,
                                                            const SteamNetworkingConfigValue*);
using PFN_NS_CloseListenSocket     = bool (*)(void*, HSteamListenSocket);
using PFN_NS_ConnectP2P            = HSteamNetConnection (*)(void*,
                                                             const SteamNetworkingIdentity*, int,
                                                             int,
                                                             const SteamNetworkingConfigValue*);
using PFN_NS_AcceptConnection      = EResult (*)(void*, HSteamNetConnection);
using PFN_NS_CloseConnection       = bool (*)(void*, HSteamNetConnection, int, const char*, bool);
using PFN_NS_CreatePollGroup       = HSteamNetPollGroup (*)(void*);
using PFN_NS_DestroyPollGroup      = bool (*)(void*, HSteamNetPollGroup);
using PFN_NS_SetConnectionPollGroup = bool (*)(void*, HSteamNetConnection, HSteamNetPollGroup);
using PFN_NS_SendMessages          = void (*)(void*, int, SteamNetworkingMessage* const*,
                                              std::int64_t*);
using PFN_NS_ReceiveOnPollGroup    = int (*)(void*, HSteamNetPollGroup,
                                             SteamNetworkingMessage**, int);
using PFN_NS_FlushMessages         = EResult (*)(void*, HSteamNetConnection);
using PFN_NS_ConfigureLanes        = EResult (*)(void*, HSteamNetConnection, int, const int*,
                                                 const std::uint16_t*);
using PFN_NS_GetConnectionInfo     = bool (*)(void*, HSteamNetConnection,
                                              SteamNetConnectionInfo*);
using PFN_NS_GetRealTimeStatus     = EResult (*)(void*, HSteamNetConnection,
                                                 SteamNetConnectionRealTimeStatus*, int, void*);

struct Binding {
    HMODULE module{nullptr};
    bool    owns_module{false}; ///< True when we called LoadLibrary ourselves.
    bool    initialized{false};

    // Core
    PFN_GetHSteamUser         GetHSteamUser{nullptr};
    PFN_FindOrCreateUserIface FindOrCreateUserInterface{nullptr};
    PFN_RegisterCallback      RegisterCallback{nullptr};
    PFN_UnregisterCallback    UnregisterCallback{nullptr};
    PFN_RegisterCallResult    RegisterCallResult{nullptr};
    PFN_UnregisterCallResult  UnregisterCallResult{nullptr};
    PFN_SteamApiInit          SteamApiInit{nullptr};
    PFN_SteamApiShutdown      SteamApiShutdown{nullptr};
    PFN_SteamApiRunCallbacks  SteamApiRunCallbacks{nullptr};

    /// True when this binding called SteamAPI_Init and therefore owes a Shutdown.
    bool initialized_steam_api{false};

    // Interface pointers
    void* user{nullptr};
    void* friends{nullptr};
    void* matchmaking{nullptr};
    void* networking_utils{nullptr};
    void* networking_sockets{nullptr};

    // Resolved version strings, for the log.
    std::string user_version;
    std::string friends_version;
    std::string matchmaking_version;
    std::string networking_utils_version;
    std::string networking_sockets_version;

    PFN_User_GetSteamId user_GetSteamId{nullptr};
    PFN_User_BLoggedOn  user_BLoggedOn{nullptr};

    PFN_Friends_GetPersonaName       friends_GetPersonaName{nullptr};
    PFN_Friends_GetFriendPersonaName friends_GetFriendPersonaName{nullptr};
    int  (*friends_GetFriendCount)(void*, int){nullptr};
    SteamId (*friends_GetFriendByIndex)(void*, int, int){nullptr};
    bool (*friends_GetFriendGamePlayed)(void*, SteamId, void*){nullptr};
    bool (*mm_InviteUserToLobby)(void*, SteamId, SteamId){nullptr};
    PFN_Friends_ActivateInviteDialog friends_ActivateInviteDialog{nullptr};
    PFN_Friends_SetRichPresence      friends_SetRichPresence{nullptr};

    PFN_MM_CreateLobby           mm_CreateLobby{nullptr};
    PFN_MM_JoinLobby             mm_JoinLobby{nullptr};
    PFN_MM_LeaveLobby            mm_LeaveLobby{nullptr};
    PFN_MM_SetLobbyData          mm_SetLobbyData{nullptr};
    PFN_MM_GetLobbyData          mm_GetLobbyData{nullptr};
    void*                        utils{nullptr};
    std::string                  utils_version;
    bool (*utils_IsOverlayEnabled)(void*){nullptr};
    PFN_MM_SetLobbyMemberData    mm_SetLobbyMemberData{nullptr};
    PFN_MM_GetLobbyMemberData    mm_GetLobbyMemberData{nullptr};
    PFN_MM_GetNumLobbyMembers    mm_GetNumLobbyMembers{nullptr};
    PFN_MM_GetLobbyMemberByIndex mm_GetLobbyMemberByIndex{nullptr};
    PFN_MM_GetLobbyOwner         mm_GetLobbyOwner{nullptr};
    PFN_MM_SetLobbyType          mm_SetLobbyType{nullptr};
    PFN_MM_RequestLobbyList      mm_RequestLobbyList{nullptr};
    PFN_MM_GetLobbyByIndex       mm_GetLobbyByIndex{nullptr};
    PFN_MM_AddDistanceFilter     mm_AddDistanceFilter{nullptr};
    void (*mm_AddStringFilter)(void*, const char*, const char*, int){nullptr};
    PFN_MM_AddResultCountFilter  mm_AddResultCountFilter{nullptr};
    PFN_MM_GetLobbyMemberLimit   mm_GetLobbyMemberLimit{nullptr};

    PFN_NU_InitRelayNetworkAccess nu_InitRelayNetworkAccess{nullptr};
    PFN_NU_AllocateMessage        nu_AllocateMessage{nullptr};

    PFN_NS_CreateListenSocketP2P  ns_CreateListenSocketP2P{nullptr};
    PFN_NS_CloseListenSocket      ns_CloseListenSocket{nullptr};
    PFN_NS_ConnectP2P             ns_ConnectP2P{nullptr};
    PFN_NS_AcceptConnection       ns_AcceptConnection{nullptr};
    PFN_NS_CloseConnection        ns_CloseConnection{nullptr};
    PFN_NS_CreatePollGroup        ns_CreatePollGroup{nullptr};
    PFN_NS_DestroyPollGroup       ns_DestroyPollGroup{nullptr};
    PFN_NS_SetConnectionPollGroup ns_SetConnectionPollGroup{nullptr};
    PFN_NS_SendMessages           ns_SendMessages{nullptr};
    PFN_NS_ReceiveOnPollGroup     ns_ReceiveOnPollGroup{nullptr};
    PFN_NS_FlushMessages          ns_FlushMessages{nullptr};
    PFN_NS_ConfigureLanes         ns_ConfigureLanes{nullptr};
    PFN_NS_GetConnectionInfo      ns_GetConnectionInfo{nullptr};
    PFN_NS_GetRealTimeStatus      ns_GetRealTimeStatus{nullptr};
};

Binding g_binding;

/// Resolves one export, recording a failure in out_missing.
template <typename Fn>
[[nodiscard]] bool Resolve(HMODULE module, const char* name, Fn& out_function,
                           std::string& out_missing) {
    // reinterpret_cast through FARPROC is the documented way; the cast is
    // unavoidable and localized here rather than repeated 40 times.
    const FARPROC address = ::GetProcAddress(module, name);
    if (address == nullptr) {
        if (!out_missing.empty()) {
            out_missing += ", ";
        }
        out_missing += name;
        return false;
    }
    out_function = reinterpret_cast<Fn>(address);
    return true;
}

/// One interface version this mod knows, paired with the module export that returns it.
struct InterfaceVersion {
    /// The accessor steam_api64.dll exports for this version, e.g. SteamAPI_SteamFriends_v018.
    const char* accessor;
    /// The version string the Steam client knows it by, e.g. SteamFriends018.
    const char* version;
};

/// Acquires an interface at the version this particular steam_api64.dll speaks.
///
/// WHY THE ACCESSOR EXPORT AND NOT JUST THE VERSION STRING
///
/// Asking the Steam client for a version by name always succeeds for any version the
/// client supports, which is every version, because the client keeps compatibility
/// shims. That answer is about the client and says nothing about the module whose flat
/// wrappers we then call.
///
/// The two are not interchangeable. SteamAPI_ISteamFriends_GetFriendCount inside a given
/// steam_api64.dll casts the pointer to the interface layout it was compiled against and
/// calls a fixed vtable slot. Hand it a newer interface and that slot is a different
/// method, which reads its arguments as something else entirely. This game ships two
/// copies of the SDK, Steamv157 speaking SteamFriends017 and Steamv163 speaking
/// SteamFriends018, so guessing newest-first picked the wrong one half the time and the
/// wrong method was called with the friend flags where a struct pointer was expected.
///
/// The accessor export settles it, because it exists only for the version the module was
/// built for. Finding SteamAPI_SteamFriends_v017 in the module is proof that this module
/// speaks 017, and calling it returns a pointer that matches its own wrappers.
[[nodiscard]] void* AcquireInterface(HMODULE module, HSteamUser user,
                                     const InterfaceVersion* versions, std::size_t version_count,
                                     std::string& out_version) {
    using Accessor = void* (*)();

    for (std::size_t i = 0; i < version_count; ++i) {
        const auto accessor =
            reinterpret_cast<Accessor>(::GetProcAddress(module, versions[i].accessor));
        if (accessor == nullptr) {
            continue;
        }

        // This module's own version, settled. Nothing below may override it, even on
        // failure: falling through to another version is what caused the crash.
        out_version = versions[i].version;
        if (void* const iface = accessor(); iface != nullptr) {
            return iface;
        }
        if (g_binding.FindOrCreateUserInterface != nullptr) {
            return g_binding.FindOrCreateUserInterface(user, versions[i].version);
        }
        return nullptr;
    }

    // No accessor export at all: an unusual build, so fall back to asking by name and
    // record that the pairing is unverified.
    if (g_binding.FindOrCreateUserInterface == nullptr) {
        return nullptr;
    }
    for (std::size_t i = 0; i < version_count; ++i) {
        if (void* const iface = g_binding.FindOrCreateUserInterface(user, versions[i].version);
            iface != nullptr) {
            out_version = std::format("{} (unverified)", versions[i].version);
            return iface;
        }
    }
    return nullptr;
}

/// Locates the already loaded module, or loads it from the game's ThirdParty path.
[[nodiscard]] HMODULE AcquireModule(std::string_view game_binaries_directory,
                                    bool allow_load_if_absent, bool& out_owns) {
    out_owns = false;

    // Already loaded is the case that matters: binding to the instance the game
    // uses is what puts our callbacks in the registry its RunCallbacks drains.
    if (const HMODULE existing = ::GetModuleHandleW(L"steam_api64.dll"); existing != nullptr) {
        return existing;
    }

    // Inside the game the caller polls with this false, so we never pre-empt the
    // shell's own SteamAPI_Init.
    if (!allow_load_if_absent) {
        return nullptr;
    }

    // Fallback: the copy the game ships. Path is derived from the game binaries
    // directory, which is <game>/Meteorite/Binaries/Win64, so the SDK redist is
    // four levels up at Engine/Binaries/ThirdParty/Steamworks/Steamv157/Win64.
    std::wstring wide;
    wide.reserve(game_binaries_directory.size());
    for (const char c : game_binaries_directory) {
        wide.push_back(static_cast<wchar_t>(static_cast<unsigned char>(c)));
    }

    static constexpr const wchar_t* kRelativeCandidates[] = {
        L"\\..\\..\\..\\Engine\\Binaries\\ThirdParty\\Steamworks\\Steamv157\\Win64\\steam_api64.dll",
        L"\\steam_api64.dll",
    };

    for (const wchar_t* relative : kRelativeCandidates) {
        const std::wstring candidate = wide + relative;
        if (const HMODULE loaded =
                ::LoadLibraryExW(candidate.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
            loaded != nullptr) {
            out_owns = true;
            return loaded;
        }
    }

    // Last resort: the search path, which finds it if the game loaded it from
    // somewhere unexpected.
    if (const HMODULE loaded = ::LoadLibraryW(L"steam_api64.dll"); loaded != nullptr) {
        out_owns = true;
        return loaded;
    }
    return nullptr;
}

} // namespace

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

bool Initialize(std::string_view game_binaries_directory, bool allow_load_if_absent,
                std::string& out_error) {
    if (g_binding.initialized) {
        return true;
    }

    bool owns = false;
    HMODULE const module = AcquireModule(game_binaries_directory, allow_load_if_absent, owns);
    if (module == nullptr) {
        out_error = allow_load_if_absent
                        ? "steam_api64.dll could not be located; is Steam running and the game "
                          "launched through it?"
                        : "steam_api64.dll is not loaded in this process yet; waiting for the "
                          "game to load it rather than pre-empting its SteamAPI_Init";
        return false;
    }
    g_binding.module      = module;
    g_binding.owns_module = owns;

    std::string missing;

    // Core exports. Without these nothing else is reachable.
    const bool core_ok =
        Resolve(module, "SteamAPI_GetHSteamUser", g_binding.GetHSteamUser, missing) &&
        Resolve(module, "SteamInternal_FindOrCreateUserInterface",
                g_binding.FindOrCreateUserInterface, missing) &&
        Resolve(module, "SteamAPI_RegisterCallback", g_binding.RegisterCallback, missing) &&
        Resolve(module, "SteamAPI_UnregisterCallback", g_binding.UnregisterCallback, missing) &&
        Resolve(module, "SteamAPI_RegisterCallResult", g_binding.RegisterCallResult, missing) &&
        Resolve(module, "SteamAPI_UnregisterCallResult", g_binding.UnregisterCallResult, missing);

    if (!core_ok) {
        out_error = std::format("steam_api64.dll is missing required exports: {}", missing);
        Shutdown();
        return false;
    }

    // SteamAPI_Init is called only when this binding loaded the module itself.
    //
    // Inside the game the host application already called it, and calling it again
    // from a DLL sharing the same module is not how the flat API is meant to be
    // shared. But when we loaded the module (a test harness, or a headless dedicated
    // server), nobody has initialized Steam and every interface query would return
    // null. So ownership of the module decides ownership of initialization.
    if (g_binding.owns_module) {
        std::string init_missing;
        (void)Resolve(module, "SteamAPI_Init", g_binding.SteamApiInit, init_missing);
        (void)Resolve(module, "SteamAPI_Shutdown", g_binding.SteamApiShutdown, init_missing);
        // Owning the API means owning the callback pump. Without it no call result
        // ever completes.
        (void)Resolve(module, "SteamAPI_RunCallbacks", g_binding.SteamApiRunCallbacks,
                      init_missing);

        if (g_binding.SteamApiInit == nullptr) {
            out_error = "this process loaded steam_api64.dll itself but SteamAPI_Init is not "
                        "exported, so Steam cannot be initialized";
            Shutdown();
            return false;
        }
        if (!g_binding.SteamApiInit()) {
            out_error = "SteamAPI_Init failed. The Steam client must be running, and this "
                        "process needs a steam_appid.txt containing 2806050 beside its "
                        "executable when it is not launched by Steam.";
            Shutdown();
            return false;
        }
        g_binding.initialized_steam_api = true;
        MPE_LOG_INFO("this process owns the Steam API: SteamAPI_Init succeeded");
    }

    const HSteamUser user_handle = g_binding.GetHSteamUser();

    // Interface acquisition. Newest first, but only the version this module exports an
    // accessor for is ever used; see AcquireInterface for why that distinction matters.
    static constexpr InterfaceVersion kUserVersions[] = {
        {"SteamAPI_SteamUser_v023", "SteamUser023"},
        {"SteamAPI_SteamUser_v022", "SteamUser022"},
        {"SteamAPI_SteamUser_v021", "SteamUser021"},
        {"SteamAPI_SteamUser_v020", "SteamUser020"},
    };
    static constexpr InterfaceVersion kFriendsVersions[] = {
        {"SteamAPI_SteamFriends_v018", "SteamFriends018"},
        {"SteamAPI_SteamFriends_v017", "SteamFriends017"},
        {"SteamAPI_SteamFriends_v015", "SteamFriends015"},
    };
    static constexpr InterfaceVersion kMatchmakingVersions[] = {
        {"SteamAPI_SteamMatchmaking_v010", "SteamMatchMaking010"},
        {"SteamAPI_SteamMatchmaking_v009", "SteamMatchMaking009"},
    };
    static constexpr InterfaceVersion kNetUtilsVersions[] = {
        {"SteamAPI_SteamNetworkingUtils_SteamAPI_v004", "SteamNetworkingUtils004"},
        {"SteamAPI_SteamNetworkingUtils_SteamAPI_v003", "SteamNetworkingUtils003"},
    };
    static constexpr InterfaceVersion kUtilsVersions[] = {
        {"SteamAPI_SteamUtils_v010", "SteamUtils010"},
        {"SteamAPI_SteamUtils_v009", "SteamUtils009"},
    };
    static constexpr InterfaceVersion kNetSocketsVersions[] = {
        {"SteamAPI_SteamNetworkingSockets_SteamAPI_v012", "SteamNetworkingSockets012"},
        {"SteamAPI_SteamNetworkingSockets_SteamAPI_v011", "SteamNetworkingSockets011"},
    };

    g_binding.user = AcquireInterface(module, user_handle, kUserVersions,
                                      std::size(kUserVersions), g_binding.user_version);
    g_binding.friends = AcquireInterface(module, user_handle, kFriendsVersions,
                                         std::size(kFriendsVersions), g_binding.friends_version);
    g_binding.utils = AcquireInterface(module, user_handle, kUtilsVersions,
                                       std::size(kUtilsVersions), g_binding.utils_version);
    g_binding.matchmaking = AcquireInterface(module, user_handle, kMatchmakingVersions,
                                             std::size(kMatchmakingVersions),
                                             g_binding.matchmaking_version);
    g_binding.networking_sockets =
        AcquireInterface(module, user_handle, kNetSocketsVersions, std::size(kNetSocketsVersions),
                         g_binding.networking_sockets_version);

    // Networking utils is not user scoped in the SDK's own accessor, so a user
    // handle is tried first and zero second.
    g_binding.networking_utils = AcquireInterface(module, user_handle, kNetUtilsVersions,
                                                  std::size(kNetUtilsVersions),
                                                  g_binding.networking_utils_version);
    if (g_binding.networking_utils == nullptr) {
        g_binding.networking_utils = AcquireInterface(module, 0, kNetUtilsVersions,
                                                      std::size(kNetUtilsVersions),
                                                      g_binding.networking_utils_version);
    }

    if (g_binding.user == nullptr || g_binding.friends == nullptr ||
        g_binding.matchmaking == nullptr) {
        out_error = "the Steam client did not provide ISteamUser, ISteamFriends and "
                    "ISteamMatchmaking; the user may be offline or not signed in";
        Shutdown();
        return false;
    }

    // Method resolution. Grouped so a missing one names itself.
    (void)Resolve(module, "SteamAPI_ISteamUser_GetSteamID", g_binding.user_GetSteamId, missing);
    (void)Resolve(module, "SteamAPI_ISteamUser_BLoggedOn", g_binding.user_BLoggedOn, missing);

    (void)Resolve(module, "SteamAPI_ISteamFriends_GetPersonaName",
                  g_binding.friends_GetPersonaName, missing);
    (void)Resolve(module, "SteamAPI_ISteamFriends_GetFriendPersonaName",
                  g_binding.friends_GetFriendPersonaName, missing);
    (void)Resolve(module, "SteamAPI_ISteamFriends_GetFriendCount",
                  g_binding.friends_GetFriendCount, missing);
    (void)Resolve(module, "SteamAPI_ISteamFriends_GetFriendByIndex",
                  g_binding.friends_GetFriendByIndex, missing);
    (void)Resolve(module, "SteamAPI_ISteamFriends_GetFriendGamePlayed",
                  g_binding.friends_GetFriendGamePlayed, missing);
    (void)Resolve(module, "SteamAPI_ISteamMatchmaking_InviteUserToLobby",
                  g_binding.mm_InviteUserToLobby, missing);
    (void)Resolve(module, "SteamAPI_ISteamUtils_IsOverlayEnabled",
                  g_binding.utils_IsOverlayEnabled, missing);
    (void)Resolve(module, "SteamAPI_ISteamFriends_ActivateGameOverlayInviteDialog",
                  g_binding.friends_ActivateInviteDialog, missing);
    (void)Resolve(module, "SteamAPI_ISteamFriends_SetRichPresence",
                  g_binding.friends_SetRichPresence, missing);

    (void)Resolve(module, "SteamAPI_ISteamMatchmaking_CreateLobby", g_binding.mm_CreateLobby,
                  missing);
    (void)Resolve(module, "SteamAPI_ISteamMatchmaking_JoinLobby", g_binding.mm_JoinLobby, missing);
    (void)Resolve(module, "SteamAPI_ISteamMatchmaking_LeaveLobby", g_binding.mm_LeaveLobby,
                  missing);
    (void)Resolve(module, "SteamAPI_ISteamMatchmaking_SetLobbyData", g_binding.mm_SetLobbyData,
                  missing);
    (void)Resolve(module, "SteamAPI_ISteamMatchmaking_GetLobbyData", g_binding.mm_GetLobbyData,
                  missing);
    (void)Resolve(module, "SteamAPI_ISteamMatchmaking_SetLobbyMemberData",
                  g_binding.mm_SetLobbyMemberData, missing);
    (void)Resolve(module, "SteamAPI_ISteamMatchmaking_GetLobbyMemberData",
                  g_binding.mm_GetLobbyMemberData, missing);
    (void)Resolve(module, "SteamAPI_ISteamMatchmaking_GetNumLobbyMembers",
                  g_binding.mm_GetNumLobbyMembers, missing);
    (void)Resolve(module, "SteamAPI_ISteamMatchmaking_GetLobbyMemberByIndex",
                  g_binding.mm_GetLobbyMemberByIndex, missing);
    (void)Resolve(module, "SteamAPI_ISteamMatchmaking_GetLobbyOwner", g_binding.mm_GetLobbyOwner,
                  missing);
    // Lobby search, which is what a server browser is made of.
    (void)Resolve(module, "SteamAPI_ISteamMatchmaking_RequestLobbyList",
                  g_binding.mm_RequestLobbyList, missing);
    (void)Resolve(module, "SteamAPI_ISteamMatchmaking_GetLobbyByIndex",
                  g_binding.mm_GetLobbyByIndex, missing);
    (void)Resolve(module, "SteamAPI_ISteamMatchmaking_AddRequestLobbyListStringFilter",
                  g_binding.mm_AddStringFilter, missing);
    (void)Resolve(module, "SteamAPI_ISteamMatchmaking_AddRequestLobbyListDistanceFilter",
                  g_binding.mm_AddDistanceFilter, missing);
    (void)Resolve(module, "SteamAPI_ISteamMatchmaking_AddRequestLobbyListResultCountFilter",
                  g_binding.mm_AddResultCountFilter, missing);
    (void)Resolve(module, "SteamAPI_ISteamMatchmaking_GetLobbyMemberLimit",
                  g_binding.mm_GetLobbyMemberLimit, missing);
    (void)Resolve(module, "SteamAPI_ISteamMatchmaking_SetLobbyType", g_binding.mm_SetLobbyType,
                  missing);

    (void)Resolve(module, "SteamAPI_ISteamNetworkingUtils_InitRelayNetworkAccess",
                  g_binding.nu_InitRelayNetworkAccess, missing);
    (void)Resolve(module, "SteamAPI_ISteamNetworkingUtils_AllocateMessage",
                  g_binding.nu_AllocateMessage, missing);

    (void)Resolve(module, "SteamAPI_ISteamNetworkingSockets_CreateListenSocketP2P",
                  g_binding.ns_CreateListenSocketP2P, missing);
    (void)Resolve(module, "SteamAPI_ISteamNetworkingSockets_CloseListenSocket",
                  g_binding.ns_CloseListenSocket, missing);
    (void)Resolve(module, "SteamAPI_ISteamNetworkingSockets_ConnectP2P",
                  g_binding.ns_ConnectP2P, missing);
    (void)Resolve(module, "SteamAPI_ISteamNetworkingSockets_AcceptConnection",
                  g_binding.ns_AcceptConnection, missing);
    (void)Resolve(module, "SteamAPI_ISteamNetworkingSockets_CloseConnection",
                  g_binding.ns_CloseConnection, missing);
    (void)Resolve(module, "SteamAPI_ISteamNetworkingSockets_CreatePollGroup",
                  g_binding.ns_CreatePollGroup, missing);
    (void)Resolve(module, "SteamAPI_ISteamNetworkingSockets_DestroyPollGroup",
                  g_binding.ns_DestroyPollGroup, missing);
    (void)Resolve(module, "SteamAPI_ISteamNetworkingSockets_SetConnectionPollGroup",
                  g_binding.ns_SetConnectionPollGroup, missing);
    (void)Resolve(module, "SteamAPI_ISteamNetworkingSockets_SendMessages",
                  g_binding.ns_SendMessages, missing);
    (void)Resolve(module, "SteamAPI_ISteamNetworkingSockets_ReceiveMessagesOnPollGroup",
                  g_binding.ns_ReceiveOnPollGroup, missing);
    (void)Resolve(module, "SteamAPI_ISteamNetworkingSockets_FlushMessagesOnConnection",
                  g_binding.ns_FlushMessages, missing);
    (void)Resolve(module, "SteamAPI_ISteamNetworkingSockets_ConfigureConnectionLanes",
                  g_binding.ns_ConfigureLanes, missing);
    (void)Resolve(module, "SteamAPI_ISteamNetworkingSockets_GetConnectionInfo",
                  g_binding.ns_GetConnectionInfo, missing);
    (void)Resolve(module, "SteamAPI_ISteamNetworkingSockets_GetConnectionRealTimeStatus",
                  g_binding.ns_GetRealTimeStatus, missing);

    if (!missing.empty()) {
        // Recorded but not fatal: the lobby half can operate without the
        // networking half, and HasNetworkingSockets reports the difference.
        MPE_LOG_WARN("steam_api64.dll did not provide: {}", missing);
    }

    g_binding.initialized = true;
    MPE_LOG_INFO("{}", DescribeBinding());
    return true;
}

void Shutdown() {
    // Only shut down Steam if we started it. Calling SteamAPI_Shutdown inside the
    // game would tear down the host application's Steam state.
    if (g_binding.initialized_steam_api && g_binding.SteamApiShutdown != nullptr) {
        g_binding.SteamApiShutdown();
    }
    if (g_binding.owns_module && g_binding.module != nullptr) {
        ::FreeLibrary(g_binding.module);
    }
    g_binding = Binding{};
}

bool IsInitialized() noexcept {
    return g_binding.initialized;
}

bool OwnsSteamApi() noexcept {
    return g_binding.initialized_steam_api;
}

void RunCallbacks() {
    // Guarded on ownership rather than trusting the caller: pumping callbacks inside
    // the game would race with the host application's own pump.
    if (g_binding.initialized_steam_api && g_binding.SteamApiRunCallbacks != nullptr) {
        g_binding.SteamApiRunCallbacks();
    }
}

bool HasNetworkingSockets() noexcept {
    return g_binding.initialized && g_binding.networking_sockets != nullptr &&
           g_binding.networking_utils != nullptr &&
           g_binding.ns_CreateListenSocketP2P != nullptr && g_binding.ns_ConnectP2P != nullptr &&
           g_binding.ns_SendMessages != nullptr && g_binding.ns_ReceiveOnPollGroup != nullptr &&
           g_binding.nu_AllocateMessage != nullptr;
}

std::string DescribeBinding() {
    // The module path, not just its address. This game ships two copies of the Steamworks
    // redistributable at different SDK versions, and which one is loaded decides which
    // interface versions are safe to call. Guessing that cost a crash, so it is logged.
    std::string module_path = "unknown";
    if (g_binding.module != nullptr) {
        char path[MAX_PATH]{};
        if (const DWORD length = ::GetModuleFileNameA(g_binding.module, path, MAX_PATH);
            length > 0 && length < MAX_PATH) {
            const std::string_view full{path, length};
            const std::size_t      slash = full.find_last_of("\\/");
            // Two trailing components: the version folder is the part that identifies it.
            const std::size_t parent =
                (slash == std::string_view::npos) ? slash : full.find_last_of("\\/", slash - 1);
            module_path = (parent == std::string_view::npos) ? std::string(full)
                                                             : std::string(full.substr(parent + 1));
        }
    }

    return std::format(
        "steam binding: module={} at {} owned={} user='{}' friends='{}' matchmaking='{}' "
        "utils='{}' net_sockets='{}' net_utils='{}' networking_ready={} "
        "[sizes identity={} msg={} conn_info={} rt_status={} status_cb={}]",
        static_cast<const void*>(g_binding.module), module_path, g_binding.owns_module,
        g_binding.user_version.empty() ? "none" : g_binding.user_version,
        g_binding.friends_version.empty() ? "none" : g_binding.friends_version,
        g_binding.matchmaking_version.empty() ? "none" : g_binding.matchmaking_version,
        g_binding.utils_version.empty() ? "none" : g_binding.utils_version,
        g_binding.networking_sockets_version.empty() ? "none"
                                                     : g_binding.networking_sockets_version,
        g_binding.networking_utils_version.empty() ? "none" : g_binding.networking_utils_version,
        HasNetworkingSockets(), sizeof(SteamNetworkingIdentity), sizeof(SteamNetworkingMessage),
        sizeof(SteamNetConnectionInfo), sizeof(SteamNetConnectionRealTimeStatus),
        sizeof(SteamNetConnectionStatusChangedCallback));
}

// ---------------------------------------------------------------------------
// Callback registration
// ---------------------------------------------------------------------------

void RegisterCallback(CallbackBase* callback, int callback_id) {
    if (g_binding.RegisterCallback != nullptr && callback != nullptr) {
        g_binding.RegisterCallback(callback, callback_id);
    }
}

void UnregisterCallback(CallbackBase* callback) {
    if (g_binding.UnregisterCallback != nullptr && callback != nullptr) {
        g_binding.UnregisterCallback(callback);
    }
}

void RegisterCallResult(CallbackBase* callback, SteamApiCall call) {
    if (g_binding.RegisterCallResult != nullptr && callback != nullptr) {
        g_binding.RegisterCallResult(callback, call);
    }
}

void UnregisterCallResult(CallbackBase* callback, SteamApiCall call) {
    if (g_binding.UnregisterCallResult != nullptr && callback != nullptr) {
        g_binding.UnregisterCallResult(callback, call);
    }
}

// ---------------------------------------------------------------------------
// ISteamUser
// ---------------------------------------------------------------------------

SteamId GetLocalSteamId() {
    if (g_binding.user_GetSteamId == nullptr || g_binding.user == nullptr) {
        return 0;
    }
    return g_binding.user_GetSteamId(g_binding.user);
}

bool IsLoggedOn() {
    if (g_binding.user_BLoggedOn == nullptr || g_binding.user == nullptr) {
        return false;
    }
    return g_binding.user_BLoggedOn(g_binding.user);
}

// ---------------------------------------------------------------------------
// ISteamFriends
// ---------------------------------------------------------------------------

const char* GetPersonaName() {
    if (g_binding.friends_GetPersonaName == nullptr || g_binding.friends == nullptr) {
        return nullptr;
    }
    return g_binding.friends_GetPersonaName(g_binding.friends);
}

const char* GetFriendPersonaName(SteamId user) {
    if (g_binding.friends_GetFriendPersonaName == nullptr || g_binding.friends == nullptr) {
        return nullptr;
    }
    return g_binding.friends_GetFriendPersonaName(g_binding.friends, user);
}

/// Halo: Campaign Evolved on Steam. Used to tell a friend playing this game from a friend
/// playing something else, since only the former can act on an invite immediately.
constexpr std::uint64_t kAppId = 2806050;

namespace {

/// FriendGameInfo_t, as ISteamFriends fills it in.
///
/// Laid out by hand because the mod does not link the SDK. The only field read is the
/// game id, whose low 24 bits are the app id for an ordinary Steam application.
struct FriendGameInfo {
    std::uint64_t game_id{0};
    std::uint32_t game_ip{0};
    std::uint16_t game_port{0};
    std::uint16_t query_port{0};
    std::uint64_t lobby{0};
};
static_assert(sizeof(FriendGameInfo) == 24, "FriendGameInfo_t is 24 bytes in the Steamworks SDK");

// Steam calls behind a structured exception handler.
//
// The interface version is now pinned to what the loaded module speaks, which is what
// made these calls unsafe before. This is the second line rather than the first: these
// cross a boundary into a closed binary that is free to change between Steam client
// updates, and a fault inside it must not be able to close somebody's game mid-match.
// Each returns a value meaning "unavailable" instead.
//
// Kept free of C++ objects because a function using __try may not require unwinding.

int GuardedFriendCount(int flags) noexcept {
    __try {
        return g_binding.friends_GetFriendCount(g_binding.friends, flags);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

SteamId GuardedFriendByIndex(int index, int flags) noexcept {
    __try {
        return g_binding.friends_GetFriendByIndex(g_binding.friends, index, flags);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

const char* GuardedFriendName(SteamId id) noexcept {
    if (g_binding.friends_GetFriendPersonaName == nullptr) {
        return nullptr;
    }
    __try {
        return g_binding.friends_GetFriendPersonaName(g_binding.friends, id);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

bool GuardedFriendGamePlayed(SteamId id, FriendGameInfo* out) noexcept {
    if (g_binding.friends_GetFriendGamePlayed == nullptr) {
        return false;
    }
    __try {
        return g_binding.friends_GetFriendGamePlayed(g_binding.friends, id, out);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

} // namespace

std::vector<GameFriend> FriendsInGame() {
    std::vector<GameFriend> found;
    if (g_binding.friends_GetFriendCount == nullptr ||
        g_binding.friends_GetFriendByIndex == nullptr || g_binding.friends == nullptr) {
        return found;
    }

    // k_EFriendFlagImmediate: the ordinary friends list, not clan or group members.
    constexpr int kImmediate = 0x04;
    const int     count      = GuardedFriendCount(kImmediate);
    if (count < 0) {
        MPE_LOG_WARN("reading the friends list faulted inside Steam; the invite list is empty "
                     "this time rather than the game closing");
        return found;
    }

    found.reserve(static_cast<std::size_t>(count));
    for (int index = 0; index < count; ++index) {
        const SteamId id = GuardedFriendByIndex(index, kImmediate);
        if (id == 0) {
            continue;
        }

        GameFriend entry;
        entry.id = id;
        if (const char* name = GuardedFriendName(id); name != nullptr) {
            entry.name = name;
        }
        if (entry.name.empty()) {
            // Steam downloads persona names lazily, so a blank one means "not yet"
            // rather than "nobody". A numeric row is still selectable.
            entry.name = std::format("Player {}", id);
        }

        FriendGameInfo playing{};
        if (GuardedFriendGamePlayed(id, &playing)) {
            // CGameID packs the app id into the low 24 bits for an ordinary application.
            entry.in_this_game = (playing.game_id & 0xFFFFFFull) == kAppId;
        }
        found.push_back(std::move(entry));
    }

    // People already in the game first, then alphabetically, so the person most likely to
    // accept is the one at the top of the list rather than wherever Steam happened to put
    // them.
    std::stable_sort(found.begin(), found.end(), [](const GameFriend& a, const GameFriend& b) {
        if (a.in_this_game != b.in_this_game) {
            return a.in_this_game;
        }
        return a.name < b.name;
    });
    return found;
}

bool InviteUserToLobby(SteamId lobby, SteamId user) {
    if (g_binding.mm_InviteUserToLobby == nullptr || g_binding.matchmaking == nullptr) {
        return false;
    }
    return g_binding.mm_InviteUserToLobby(g_binding.matchmaking, lobby, user);
}

bool IsOverlayEnabled() {
    if (g_binding.utils_IsOverlayEnabled == nullptr || g_binding.utils == nullptr) {
        // Unknown rather than false. Reporting "off" because the binding is missing would
        // send a player to change a setting that is already correct.
        return true;
    }
    return g_binding.utils_IsOverlayEnabled(g_binding.utils);
}

void ActivateGameOverlayInviteDialog(SteamId lobby) {
    if (g_binding.friends_ActivateInviteDialog != nullptr && g_binding.friends != nullptr) {
        g_binding.friends_ActivateInviteDialog(g_binding.friends, lobby);
    }
}

bool SetRichPresence(const char* key, const char* value) {
    if (g_binding.friends_SetRichPresence == nullptr || g_binding.friends == nullptr) {
        return false;
    }
    return g_binding.friends_SetRichPresence(g_binding.friends, key, value);
}

// ---------------------------------------------------------------------------
// ISteamMatchmaking
// ---------------------------------------------------------------------------

SteamApiCall CreateLobby(ELobbyType type, int max_members) {
    if (g_binding.mm_CreateLobby == nullptr || g_binding.matchmaking == nullptr) {
        return kInvalidApiCall;
    }
    return g_binding.mm_CreateLobby(g_binding.matchmaking, type, max_members);
}

SteamApiCall JoinLobby(SteamId lobby) {
    if (g_binding.mm_JoinLobby == nullptr || g_binding.matchmaking == nullptr) {
        return kInvalidApiCall;
    }
    return g_binding.mm_JoinLobby(g_binding.matchmaking, lobby);
}

void LeaveLobby(SteamId lobby) {
    if (g_binding.mm_LeaveLobby != nullptr && g_binding.matchmaking != nullptr) {
        g_binding.mm_LeaveLobby(g_binding.matchmaking, lobby);
    }
}

bool SetLobbyData(SteamId lobby, const char* key, const char* value) {
    if (g_binding.mm_SetLobbyData == nullptr || g_binding.matchmaking == nullptr) {
        return false;
    }
    return g_binding.mm_SetLobbyData(g_binding.matchmaking, lobby, key, value);
}

const char* GetLobbyData(SteamId lobby, const char* key) {
    if (g_binding.mm_GetLobbyData == nullptr || g_binding.matchmaking == nullptr) {
        return nullptr;
    }
    return g_binding.mm_GetLobbyData(g_binding.matchmaking, lobby, key);
}

void SetLobbyMemberData(SteamId lobby, const char* key, const char* value) {
    if (g_binding.mm_SetLobbyMemberData != nullptr && g_binding.matchmaking != nullptr) {
        g_binding.mm_SetLobbyMemberData(g_binding.matchmaking, lobby, key, value);
    }
}

const char* GetLobbyMemberData(SteamId lobby, SteamId user, const char* key) {
    if (g_binding.mm_GetLobbyMemberData == nullptr || g_binding.matchmaking == nullptr) {
        return nullptr;
    }
    return g_binding.mm_GetLobbyMemberData(g_binding.matchmaking, lobby, user, key);
}

namespace {

/// Lobbies from the last completed search, and how many the search returned.
///
/// Steam answers a search with a count, and each entry is then read back by index. The
/// results are kept here so a browser can redraw without asking Steam again on every frame.
/// The lobby this process currently occupies, or zero.
///
/// Published by the matchmaking layer so the browser can leave it out of its own results.
std::atomic<SteamId> g_current_lobby{0};

std::mutex                g_browse_mutex;
std::vector<LobbyListing> g_browse_results;
int                       g_browse_count = 0;

/// Whether the last search returned this machine's own lobby.
///
/// Proof, from Steam itself, that the whole discovery chain works: the marker was
/// published in a form Steam's own string filter matches, the search executed, and a lobby
/// carrying it came back. Our own is the only lobby whose presence can be checked without
/// a second person, and it travels the identical path a friend's does.
bool g_browse_saw_own = false;

/// The key and value that identify a lobby as this mod's, shared by the search filter and
/// the listing check so the two cannot drift apart. They did once, and the browser
/// returned nothing for it.
std::string g_browse_marker_key;
std::string g_browse_marker_value;

} // namespace

/// Receives the result of RequestLobbyList.
///
/// Without this the search was started and its answer was never collected, so the count of
/// matching lobbies stayed at zero and the browser was permanently empty. The host could
/// not even see their own session, which is what made the fault obvious.
///
/// A file scope owner rather than a member, because the search is a property of the
/// process: only one can be outstanding, and the last result set is what Steam keeps
/// addressable by index.
class LobbyListReceiver {
public:
    void Await(SteamApiCall call) {
        result_.Set(call, this, &LobbyListReceiver::OnLobbyMatchList);
    }

private:
    void OnLobbyMatchList(LobbyMatchListCallback* payload, bool io_failure) {
        if (io_failure || payload == nullptr) {
            SetBrowseResultCount(0);
            MPE_LOG_WARN("the lobby search failed");
            return;
        }
        SetBrowseResultCount(static_cast<int>(payload->m_nLobbiesMatching));
        MPE_LOG_INFO("lobby search returned {} lobby(ies)",
                     payload->m_nLobbiesMatching);
    }

    CallResult<LobbyListReceiver, LobbyMatchListCallback> result_;
};

LobbyListReceiver g_lobby_list_receiver;

SteamId CurrentLobby() { return g_current_lobby.load(std::memory_order_acquire); }

void SetCurrentLobby(SteamId lobby) {
    g_current_lobby.store(lobby, std::memory_order_release);
}

void RequestLobbyList() {
    if (g_binding.mm_RequestLobbyList == nullptr || g_binding.matchmaking == nullptr) {
        return;
    }

    // Filtered by Steam, not only by us.
    //
    // Every lobby this app has open is a candidate, and the game opens Steam lobbies for
    // its own co-op fireteams. Asking for fifty and discarding the ones that are not ours
    // means fifty fireteams can fill the answer and leave no room for a single session
    // this mod hosts, on a search that reported success. Asking Steam to match the marker
    // makes the fifty ours.
    std::string marker_key;
    std::string marker_value;
    {
        // Copied under the lock. The marker is written from the lobby layer's thread and
        // read from whichever thread refreshes the browser.
        std::lock_guard lock(g_browse_mutex);
        marker_key   = g_browse_marker_key;
        marker_value = g_browse_marker_value;
    }
    if (g_binding.mm_AddStringFilter != nullptr && !marker_key.empty()) {
        g_binding.mm_AddStringFilter(g_binding.matchmaking, marker_key.c_str(),
                                     marker_value.c_str(), 0 /* equal */);
    }

    // Worldwide, and capped, so a browser cannot be handed an unbounded list.
    if (g_binding.mm_AddDistanceFilter != nullptr) {
        g_binding.mm_AddDistanceFilter(g_binding.matchmaking, 3 /* worldwide */);
    }
    if (g_binding.mm_AddResultCountFilter != nullptr) {
        g_binding.mm_AddResultCountFilter(g_binding.matchmaking, 50);
    }
    const SteamApiCall call = g_binding.mm_RequestLobbyList(g_binding.matchmaking);
    if (call == kInvalidApiCall) {
        SetBrowseResultCount(0);
        return;
    }
    g_lobby_list_receiver.Await(call);
}

bool LastSearchSawOwnLobby() {
    std::lock_guard lock(g_browse_mutex);
    return g_browse_saw_own;
}

void SetBrowseMarker(std::string_view key, std::string_view value) {
    std::lock_guard lock(g_browse_mutex);
    g_browse_marker_key.assign(key);
    g_browse_marker_value.assign(value);
    MPE_LOG_INFO("browse marker is '{}' = '{}'", g_browse_marker_key, g_browse_marker_value);
}

std::vector<LobbyListing> BrowseLobbies() {
    if (g_binding.mm_GetLobbyByIndex == nullptr || g_binding.matchmaking == nullptr) {
        return {};
    }

    std::lock_guard lock(g_browse_mutex);
    g_browse_results.clear();

    // Counted so an empty browser can be explained rather than guessed at. "Nobody is
    // hosting" and "everything found was filtered out" look identical on screen, and the
    // second one has been a bug twice.
    int  skipped_own      = 0;
    int  skipped_unmarked = 0;
    bool saw_own          = false;

    // Read here rather than at each row, and used both to look the value up and to compare
    // it, so a search and a listing can never disagree about what counts as ours.
    const std::string marker_key   = g_browse_marker_key;
    const std::string marker_value = g_browse_marker_value;

    // Steam keeps the last result set addressable by index. Reading past the end returns an
    // invalid id, which is the natural place to stop.
    for (int index = 0; index < g_browse_count; ++index) {
        const SteamId lobby = g_binding.mm_GetLobbyByIndex(g_binding.matchmaking, index);
        if (lobby == 0) {
            break;
        }

        LobbyListing listing;
        listing.id = lobby;

        const auto read = [lobby](const char* key) -> std::string {
            const char* value = GetLobbyData(lobby, key);
            return (value != nullptr) ? std::string{value} : std::string{};
        };
        listing.name = read("name");
        listing.mode = read("mode");
        listing.map  = read("map");

        listing.members = GetNumLobbyMembers(lobby);
        if (g_binding.mm_GetLobbyMemberLimit != nullptr) {
            listing.capacity = g_binding.mm_GetLobbyMemberLimit(g_binding.matchmaking, lobby);
        }

        listing.host_id = read("fe.host");
        // The phase decides whether a row is joinable, not whether it is listed. A game
        // in progress is worth seeing.
        listing.phase = read("fe.phase");

        // The decision itself lives in Lobby/Discovery, with no Steam underneath it, and
        // is covered by tools/session_check. It used to be written inline here, which made
        // it untestable for a bad reason: not because it depends on Steam, but because it
        // was standing next to something that does. It was also wrong, discarding every
        // lobby on a key that nothing had ever written, and the browser could not return a
        // row under any circumstance.
        lobby::RawListing raw;
        raw.id      = lobby;
        raw.host_id = listing.host_id;
        raw.marker  = read(marker_key.empty() ? "mpe.v" : marker_key.c_str());

        const lobby::ListingVerdict verdict =
            lobby::JudgeListing(raw, CurrentLobby(), marker_value);
        if (verdict == lobby::ListingVerdict::OwnSession) {
            ++skipped_own;
            saw_own = true;
            continue;
        }
        if (verdict != lobby::ListingVerdict::Listed) {
            ++skipped_unmarked;
            continue;
        }

        g_browse_results.push_back(std::move(listing));
    }

    if (saw_own) {
        g_browse_saw_own = true;
    }

    // Logged only when the answer changes, because the browser asks every few seconds.
    static std::string s_last_summary;
    const std::string  summary =
        std::format("{} listed, {} own, {} unmarked, of {} returned", g_browse_results.size(),
                    skipped_own, skipped_unmarked, g_browse_count);
    if (summary != s_last_summary) {
        s_last_summary = summary;
        MPE_LOG_INFO("browse: {}", summary);
    }
    return g_browse_results;
}

/// Records how many lobbies the last search returned. Called from the callback.
void SetBrowseResultCount(int count) {
    std::lock_guard lock(g_browse_mutex);
    g_browse_count = count;
}

int GetNumLobbyMembers(SteamId lobby) {
    if (g_binding.mm_GetNumLobbyMembers == nullptr || g_binding.matchmaking == nullptr) {
        return 0;
    }
    return g_binding.mm_GetNumLobbyMembers(g_binding.matchmaking, lobby);
}

SteamId GetLobbyMemberByIndex(SteamId lobby, int index) {
    if (g_binding.mm_GetLobbyMemberByIndex == nullptr || g_binding.matchmaking == nullptr) {
        return 0;
    }
    return g_binding.mm_GetLobbyMemberByIndex(g_binding.matchmaking, lobby, index);
}

SteamId GetLobbyOwner(SteamId lobby) {
    if (g_binding.mm_GetLobbyOwner == nullptr || g_binding.matchmaking == nullptr) {
        return 0;
    }
    return g_binding.mm_GetLobbyOwner(g_binding.matchmaking, lobby);
}

bool SetLobbyType(SteamId lobby, ELobbyType type) {
    if (g_binding.mm_SetLobbyType == nullptr || g_binding.matchmaking == nullptr) {
        return false;
    }
    return g_binding.mm_SetLobbyType(g_binding.matchmaking, lobby, type);
}

// ---------------------------------------------------------------------------
// ISteamNetworkingUtils
// ---------------------------------------------------------------------------

void InitRelayNetworkAccess() {
    if (g_binding.nu_InitRelayNetworkAccess != nullptr && g_binding.networking_utils != nullptr) {
        g_binding.nu_InitRelayNetworkAccess(g_binding.networking_utils);
    }
}

SteamNetworkingMessage* AllocateMessage(int payload_size) {
    if (g_binding.nu_AllocateMessage == nullptr || g_binding.networking_utils == nullptr) {
        return nullptr;
    }
    return g_binding.nu_AllocateMessage(g_binding.networking_utils, payload_size);
}

// ---------------------------------------------------------------------------
// ISteamNetworkingSockets
// ---------------------------------------------------------------------------

HSteamListenSocket CreateListenSocketP2P(int virtual_port, int option_count,
                                         const SteamNetworkingConfigValue* options) {
    if (g_binding.ns_CreateListenSocketP2P == nullptr ||
        g_binding.networking_sockets == nullptr) {
        return kInvalidListenSocket;
    }
    return g_binding.ns_CreateListenSocketP2P(g_binding.networking_sockets, virtual_port,
                                              option_count, options);
}

bool CloseListenSocket(HSteamListenSocket socket) {
    if (g_binding.ns_CloseListenSocket == nullptr || g_binding.networking_sockets == nullptr) {
        return false;
    }
    return g_binding.ns_CloseListenSocket(g_binding.networking_sockets, socket);
}

HSteamNetConnection ConnectP2P(const SteamNetworkingIdentity& peer, int virtual_port,
                               int option_count, const SteamNetworkingConfigValue* options) {
    if (g_binding.ns_ConnectP2P == nullptr || g_binding.networking_sockets == nullptr) {
        return kInvalidConnection;
    }
    return g_binding.ns_ConnectP2P(g_binding.networking_sockets, &peer, virtual_port,
                                   option_count, options);
}

EResult AcceptConnection(HSteamNetConnection connection) {
    if (g_binding.ns_AcceptConnection == nullptr || g_binding.networking_sockets == nullptr) {
        return EResult::Fail;
    }
    return g_binding.ns_AcceptConnection(g_binding.networking_sockets, connection);
}

bool CloseConnection(HSteamNetConnection connection, int reason, const char* debug_text,
                     bool enable_linger) {
    if (g_binding.ns_CloseConnection == nullptr || g_binding.networking_sockets == nullptr) {
        return false;
    }
    return g_binding.ns_CloseConnection(g_binding.networking_sockets, connection, reason,
                                        debug_text, enable_linger);
}

HSteamNetPollGroup CreatePollGroup() {
    if (g_binding.ns_CreatePollGroup == nullptr || g_binding.networking_sockets == nullptr) {
        return kInvalidPollGroup;
    }
    return g_binding.ns_CreatePollGroup(g_binding.networking_sockets);
}

bool DestroyPollGroup(HSteamNetPollGroup group) {
    if (g_binding.ns_DestroyPollGroup == nullptr || g_binding.networking_sockets == nullptr) {
        return false;
    }
    return g_binding.ns_DestroyPollGroup(g_binding.networking_sockets, group);
}

bool SetConnectionPollGroup(HSteamNetConnection connection, HSteamNetPollGroup group) {
    if (g_binding.ns_SetConnectionPollGroup == nullptr ||
        g_binding.networking_sockets == nullptr) {
        return false;
    }
    return g_binding.ns_SetConnectionPollGroup(g_binding.networking_sockets, connection, group);
}

void SendMessages(int message_count, SteamNetworkingMessage* const* messages,
                  std::int64_t* out_message_numbers) {
    if (g_binding.ns_SendMessages == nullptr || g_binding.networking_sockets == nullptr) {
        // Steam takes ownership of a message passed to SendMessages. If the call
        // cannot be made the caller would leak, so the messages are released here.
        for (int i = 0; i < message_count; ++i) {
            if (messages[i] != nullptr) {
                messages[i]->Release();
            }
            if (out_message_numbers != nullptr) {
                out_message_numbers[i] = -static_cast<std::int64_t>(EResult::NoConnection);
            }
        }
        return;
    }
    g_binding.ns_SendMessages(g_binding.networking_sockets, message_count, messages,
                              out_message_numbers);
}

int ReceiveMessagesOnPollGroup(HSteamNetPollGroup group, SteamNetworkingMessage** out_messages,
                               int max_messages) {
    if (g_binding.ns_ReceiveOnPollGroup == nullptr || g_binding.networking_sockets == nullptr) {
        return 0;
    }
    return g_binding.ns_ReceiveOnPollGroup(g_binding.networking_sockets, group, out_messages,
                                           max_messages);
}

EResult FlushMessagesOnConnection(HSteamNetConnection connection) {
    if (g_binding.ns_FlushMessages == nullptr || g_binding.networking_sockets == nullptr) {
        return EResult::Fail;
    }
    return g_binding.ns_FlushMessages(g_binding.networking_sockets, connection);
}

EResult ConfigureConnectionLanes(HSteamNetConnection connection, int lane_count,
                                 const int* priorities, const std::uint16_t* weights) {
    if (g_binding.ns_ConfigureLanes == nullptr || g_binding.networking_sockets == nullptr) {
        return EResult::Fail;
    }
    return g_binding.ns_ConfigureLanes(g_binding.networking_sockets, connection, lane_count,
                                       priorities, weights);
}

bool GetConnectionInfo(HSteamNetConnection connection, SteamNetConnectionInfo* out_info) {
    if (g_binding.ns_GetConnectionInfo == nullptr || g_binding.networking_sockets == nullptr) {
        return false;
    }
    return g_binding.ns_GetConnectionInfo(g_binding.networking_sockets, connection, out_info);
}

EResult GetConnectionRealTimeStatus(HSteamNetConnection connection,
                                    SteamNetConnectionRealTimeStatus* out_status, int lane_count,
                                    void* out_lanes) {
    if (g_binding.ns_GetRealTimeStatus == nullptr || g_binding.networking_sockets == nullptr) {
        return EResult::Fail;
    }
    return g_binding.ns_GetRealTimeStatus(g_binding.networking_sockets, connection, out_status,
                                          lane_count, out_lanes);
}

} // namespace mpe::steam

