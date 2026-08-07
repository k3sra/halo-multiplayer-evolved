// SPDX-License-Identifier: MIT
// MultiplayerEvolved: ModMain.cpp
//
// Process entry point, dependency wiring and the tick loop.
//
// STARTUP ORDER
//
// The proxy loads us very early, before the engine shell exists and before Steam
// has initialized. Nothing here may assume either is ready, so startup is a
// bounded poll rather than a sequence of assumptions:
//
//   1. Open the log immediately, so a failure in any later step is recorded.
//   2. Wait for HaloSimulation_tag_release.dll to appear in the process.
//   3. Attach to it, discover the debug table, validate the required symbols.
//   4. Wait for Steam to be usable, then create the matchmaking hooks and the
//      relay transport.
//   5. Construct the lobby manager and start ticking.
//
// Any step that fails leaves the mod loaded but inert, with the reason in the log
// and in every refusal the UI receives. The game stays fully playable.
//
// PUBLIC C API
//
// The exported MPE_* functions are the surface a UI layer, a script host or a
// community front end drives. A flat C ABI is used deliberately: it is callable
// from any language, and it keeps the C++ types out of the boundary so a UI built
// against one mod version keeps working against the next.

#define MPE_LOG_CATEGORY "Mod"

#include "Blam/DebugGlobals.h"
#include "Blam/ModuleImage.h"
#include "Blam/SymbolRegistry.h"
#include "Core/GameBuild.h"
#include "Core/Log.h"
#include "Core/Pacing.h"
#include "Core/Text.h"
#include "Debug/AccessTrap.h"
#include "Unreal/FNameTrampoline.h"
#include "Unreal/GameThread.h"
#include "Unreal/LoadingLines.h"
#include "Unreal/LobbyUI.h"
#include "Update/UpdateCheck.h"
#include "Engine/CampaignEngineControl.h"
#include "Engine/InertEngineControl.h"
#include "Lobby/Discovery.h"
#include "Lobby/LobbyManager.h"
#include "Lobby/SteamMatchmakingHooks.h"
#include "Net/SteamSocketsTransport.h"
#include "Steam/SteamApi.h"
#include "Unreal/NamePool.h"
#include "Unreal/ObjectArray.h"
#include "Unreal/Reflection.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <algorithm>
#include <random>
#include <cstring>
#include <atomic>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <format>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <string>
#include <thread>
#include <vector>

#include "Unreal/ProcessMemory.h"

namespace mpe {
namespace {

/// Tick rate for the mod loop. 60 Hz matches the rate the lobby advertises in the
/// handshake and is comfortably faster than anything the lobby needs to react to.
constexpr auto kTickInterval = std::chrono::microseconds(16'667);

/// How long to wait for the engine module and for Steam before giving up. The
/// engine shell can take a while on a cold start with a slow disk.
constexpr auto kStartupTimeout = std::chrono::seconds(120);
constexpr auto kStartupPollInterval = std::chrono::milliseconds(250);

/// How long to wait for the host application to load and initialize Steam before this
/// process takes ownership of it. Inside the game the shell does it within a few
/// seconds; a standalone tool never will, and pays this wait once.
constexpr auto kSteamHostWait = std::chrono::seconds(45);

/// Logs lobby events. A UI layer registers its own sink; this one guarantees that
/// everything is recorded even with no UI attached, which is what makes a bug
/// report from a player useful.
class LoggingEventSink final : public lobby::ILobbyEventSink {
public:
    void OnPhaseChanged(lobby::LobbyPhase previous, lobby::LobbyPhase current) override {
        MPE_LOG_INFO("lobby phase {} -> {}", lobby::ToString(previous), lobby::ToString(current));
    }

    void OnSnapshotChanged(const lobby::LobbySnapshot& snapshot) override {
        // Logged at trace: this fires on every roster or progress change and would
        // drown the log at a higher level.
        MPE_LOG_TRACE("snapshot: phase={} players={} countdown={} map={:.0f}%",
                     lobby::ToString(snapshot.phase), snapshot.players.size(),
                     snapshot.countdown_seconds,
                     snapshot.map_transfer_progress * 100.0f);
    }

    void OnChatMessage(lobby::PlatformId author, std::string_view display_name,
                       std::string_view text) override {
        MPE_LOG_INFO("chat {}: {}", display_name.empty() ? std::to_string(author)
                                                        : std::string(display_name),
                    text);
    }

    void OnError(const Error& error) override {
        MPE_LOG_ERROR("lobby error [{}]: {}", ToString(error.code), error.message);
    }
};

/// Everything the mod owns, in construction order. Declared as one struct so
/// teardown order is the reverse of construction without any manual sequencing.
struct ModState {
    LoggingEventSink                                sink;
    std::unique_ptr<engine::IEngineControl>         engine;
    std::unique_ptr<lobby::SteamMatchmakingHooks>   backend;
    std::unique_ptr<net::SteamSocketsTransport>     transport;
    std::unique_ptr<lobby::LobbyManager>            manager;

    /// Retained so the discovery report can be logged on demand.
    std::optional<blam::ModuleImage>    simulation_module;
    std::optional<blam::SymbolRegistry> symbols;

    /// Name addressable access to the engine's writable globals. Present only once
    /// symbol discovery has succeeded.
    std::optional<blam::DebugGlobals> globals;

    /// UE5 FName pool. The prerequisite for reaching reflected properties such as
    /// bFriendlyFireEnabled, which is where the game engine variant actually lives.
    std::optional<unreal::NamePool> names;

    /// UE5 global object array. Declared after names_ so it is destroyed first, since
    /// it holds a pointer to the pool.
    std::optional<unreal::ObjectArray> objects;

    /// Property layout reader. Also holds a pointer to the pool.
    std::optional<unreal::Reflection> reflection;

    /// One boolean field being watched for changes.
    ///
    /// Several independent copies of the friendly fire flag exist and they disagree:
    /// the save record reads true while the frontend setup parameters read false. Only
    /// one of them can be what a match is actually configured from. Watching which one
    /// the game itself writes, as the player moves through menus, is the cheapest way to
    /// find out, and far more reliable than guessing from names.
    /// A watch stores where the byte came from, not just where it currently is.
    ///
    /// A bare address is not enough. Unreal collects objects at level transitions, and
    /// freed heap memory almost always stays committed, so a guarded read of a dead
    /// object succeeds and returns whatever now occupies the byte. That failure is
    /// silent and produces a confident wrong answer: this was observed reporting a
    /// collected widget's flag as false while the hardware trap recorded no write to it.
    ///
    /// Keeping the owning instance and its class allows the address to be revalidated
    /// against the object array instead of trusted.
    struct WatchedField {
        std::string    label;
        std::uintptr_t address{0};          ///< Address of the byte itself.
        std::uintptr_t instance_address{0}; ///< Object the byte lives inside.
        std::uintptr_t owner_class{0};      ///< That object's class, for revalidation.
        std::uint32_t  byte_offset{0};      ///< address - instance_address.
        bool           last_value{false};
        bool           valid{false};
    };
    std::vector<WatchedField> watched;

    /// Wall clock of the last watch poll, so it runs at 1 Hz rather than 60 Hz.
    std::chrono::steady_clock::time_point last_watch_poll{};
};

/// Defined below; declared here because the tick loop calls it.
void PollWatchedFields();

/// Adds the multiplayer entry to the main menu whenever a new menu appears.
void MaintainMainMenuButton();

/// Opens the multiplayer lobby in place of the main menu list.
void OnMultiplayerClicked();
void OnStartMatch();
void OnJoinMatch();
void OnLeaveLobby();
void RefreshServerList();
void SwitchLobbyTab(bool browsing);
void SelectLobbyMode(bool slayer);
void PublishSelectedMatchSettings();
void SelectLobbyMap(int map_index);
void EnsureSessionHosted();
void InviteToSession();
void ApplyServerFilter();
void PublishSessionDetails();
void CaptureServerName();
void OpenSessionInvite();
void RefreshLobbyStatus();
void ShowSessionNotice(std::string title, std::string detail);
void ReportLaunchFailure(std::string_view reason);
void HandleLaunchFailureLocked();
void CheckOwnSessionIsFindable();
void RefreshLobbyAuthority();
void PrepareLobby();
void PrepareStatusOverlay();
void PrepareLoadingOverlay();
void RegisterLoadingCancel();
void RefreshLoadingScreen();
void OnCancelLoading();
void InviteFriendAt(int row);
void CloseInviteList();
void PageFriendList(int direction);
void ShowInviteList(bool visible);
void RefreshLobbyRoster();
[[nodiscard]] std::string LoadServerName();
void                      SaveServerName(const std::string& name);
[[nodiscard]] lobby::LobbyId HostedLobbyLocked();
[[nodiscard]] lobby::LobbyId CurrentSessionLobbyLocked();

/// True when this machine may change what the lobby is playing.
///
/// The mode, the map, the settings, the server name and the start of the match all belong
/// to whoever is hosting. Not being in a session at all counts as being the host, because
/// pressing MULTIPLAYER and choosing a mode before anybody has joined is how a session gets
/// its settings in the first place.
[[nodiscard]] bool LocalPlayerHasAuthority();

/// The same, said on screen. Returns false when the press should be ignored.
[[nodiscard]] bool RefuseWithoutAuthority(std::string_view what);

/// What the invite panel should say about the session behind it.
struct InviteSessionState {
    lobby::LobbyId          lobby{0};
    std::string             text;
    unreal::InviteReadiness readiness{unreal::InviteReadiness::Preparing};
};
[[nodiscard]] InviteSessionState DescribeInviteSession();
void                             RefreshInvitePanelState();

/// Which multiplayer screen is showing.
///
/// Nested screens rather than one long list. The menu container is a plain vertical stack
/// with no scrolling, so everything shown has to fit on screen at once; a flat list of the
/// settings, both team rosters and the actions ran off the bottom. Each screen below is
/// short enough to fit, which is also how the game's own menus are arranged.
enum class MultiplayerScreen {
    Home,
    MatchSettings,
    Players,
    ServerBrowser,
};
MultiplayerScreen g_screen = MultiplayerScreen::Home;

/// The entry added to the main menu, kept so it can be taken out again when the lobby
/// replaces the menu; leaving it in showed MULTIPLAYER twice.
std::uintptr_t g_multiplayer_button = 0;

/// Root canvas of the open lobby, so it can be taken down again.
std::uintptr_t g_lobby_root = 0;

/// The lobby's function and class handles, resolved once.
///
/// This is the whole reason opening the lobby used to take fifteen seconds. Resolving it
/// is around a dozen passes over fifty thousand objects, and it was being done inside the
/// click handler, under the state lock, on every press. None of it depends on anything a
/// menu rebuild or a level load can change, so it is found once and kept, and a click then
/// costs two guarded reads to point it at the live menu.
unreal::LobbyUIContext g_lobby_ui;
bool                   g_lobby_ui_ready = false;

/// The menu the entry was last added to. A click attaches the lobby to this exact
/// instance, rather than searching for one again and hoping the same one comes back.
std::uintptr_t g_live_menu = 0;

/// When to read back the size the open lobby was actually given, or the epoch when there
/// is nothing to read. Deferred by a frame because desired size reports whatever the last
/// layout pass cached, and a widget built this frame has not been through one.
std::chrono::steady_clock::time_point g_lobby_measure_at{};

/// The buttons on the open lobby and what each one does.
std::vector<unreal::LobbyControl> g_lobby_controls;

/// How the server browser is filtered, and which row is chosen.
unreal::ServerFilter g_server_filter;
int                  g_selected_server = 0;

/// The session line the invite panel is currently showing.
///
/// Compared against what the session says now, so the panel is rewritten when the answer
/// changes and left alone the rest of the time. Cleared when the panel closes, so reopening
/// it always writes the line rather than assuming the widget still holds it.
std::string g_invite_state_shown;

/// True when the pool and the object array were read from the static image rather than
/// searched for. It decides whether the slow search, and the waiting that exists only to
/// schedule that search safely, need to happen at all.
bool g_reflection_was_fast = false;

/// The invite list, as it is currently shown.
///
/// Read from Steam when a slot is pressed rather than continuously: the friends list only
/// changes when somebody signs in or out, and asking on every frame would be a call into
/// the Steam client sixty times a second for a panel that is usually not even open.
std::vector<unreal::LobbyFriend> g_friend_list;
/// Steam ids matching g_friend_list, kept apart from it because the screen has no use for
/// them and inviting is the only thing that does.
std::vector<steam::SteamId> g_friend_ids;
int                         g_friend_page = 0;

/// This build's version, compared against the newest GitHub release to decide whether the
/// status panel should tell the player to update.
constexpr const char* kModVersion = "0.2.1";

/// The newest version seen on GitHub, empty until a check has succeeded.
///
/// Written by a one shot background thread and read by the status panel, so it is guarded
/// rather than shared raw: the two run on different threads from the moment the check
/// starts.
std::mutex  g_version_mutex;
std::string g_latest_version;
/// Percentage while a download is running, or -1 when none is.
int  g_update_progress = -1;
/// True once a new build is staged and waiting for the next start.
bool g_update_ready = false;
/// Where the game's binaries live, which is where the staged update has to land.
std::wstring g_binaries_directory;

/// Where releases are published. A mod that talks to other players is only useful when
/// everyone agrees on the protocol, so an out of date copy is worth saying out loud.
constexpr const char* kReleaseRepository = "k3sra/halo-multiplayer-evolved";

/// Asks GitHub for the newest release, once, off the game's threads.
///
/// Failure is not reported to the player. Being unable to reach GitHub means the version is
/// unknown, not that anything is wrong, and a mod that complains about its own update check
/// on a flaky connection is worse than one that stays quiet.
void StartUpdateCheck() {
    std::thread([] {
        const Expected<update::ReleaseInfo> release =
            update::FetchLatestRelease(kReleaseRepository);
        if (!release.ok()) {
            MPE_LOG_INFO("update check skipped: {}", release.message());
            return;
        }

        {
            std::lock_guard lock(g_version_mutex);
            g_latest_version = release.value().version;
        }
        if (!update::IsNewer(release.value().version, kModVersion)) {
            MPE_LOG_INFO("this build is current ({})", kModVersion);
            return;
        }
        MPE_LOG_INFO("an update is available: {} (this build is {})", release.value().version,
                    kModVersion);

        // Fetched now and applied at the next start, because the running DLL cannot replace
        // itself while it is mapped. The player sees the progress on the lobby's status
        // line and never has to visit a website.
        const Result downloaded = update::DownloadRelease(
            release.value(), g_binaries_directory, [](long long done, long long total) {
                std::lock_guard lock(g_version_mutex);
                g_update_progress = total > 0 ? static_cast<int>(done * 100 / total) : -1;
            });

        std::lock_guard lock(g_version_mutex);
        g_update_progress = -1;
        g_update_ready    = downloaded.ok();
        if (!downloaded.ok()) {
            MPE_LOG_WARN("the update could not be downloaded: {}", downloaded.message());
        }
    }).detach();
}

/// The campaign asset a match is begun against.
///
/// BeginCampaign needs an active campaign to begin: without one there is nothing to start
/// and it returns false rather than loading anything. This is the same asset the campaign
/// command uses, so the multiplayer path starts from something already proven to work.
constexpr const char* kDefaultCampaignAsset = "DA_FirstPlayableCampaign";

/// Servers currently known from Steam.
///
/// Returns what matchmaking actually reports. An empty result is shown as an empty list
/// rather than filled with examples, because a browser that lists games nobody can join is
/// worse than one that honestly shows none.
[[nodiscard]] std::vector<unreal::ServerEntry> DiscoveredServers() {
    std::vector<unreal::ServerEntry> servers;
    for (const steam::LobbyListing& listing : steam::BrowseLobbies()) {
        unreal::ServerEntry entry;
        entry.name     = listing.name;
        entry.mode     = listing.mode;
        entry.map      = listing.map;
        entry.players  = listing.members;
        // Ten is the lobby size this mod hosts; a listing that reports nothing sensible is
        // shown against that rather than as zero.
        entry.capacity = listing.capacity > 0 ? listing.capacity : 10;
        entry.ping     = listing.ping_milliseconds;

        // Shared with everything else that has to answer this, and tested, rather than
        // written out again here.
        entry.status = lobby::SessionStatusFromPhase(listing.phase);

        servers.push_back(std::move(entry));
    }
    return servers;
}

/// The local player's Steam name, for the host's lobby slot.
[[nodiscard]] std::string SteamPlayerName() {
    if (const char* name = steam::GetPersonaName(); name != nullptr && *name != '\0') {
        return name;
    }
    return "PLAYER";
}

/// A player in the lobby.
struct LobbyPlayer {
    std::string name;
    int         team{0}; ///< 0 red, 1 blue.
    bool        host{false};
};

/// The match the lobby is configured to host.
///
/// Ten slots rather than the shipped fireteam's four, because the aim is the classic
/// arrangement: two teams, up to five a side, shuffled between matches so the same people
/// are not always paired.
struct LobbyState {
    static constexpr int kMaxPlayers = 10;

    std::string              mode{"CAPTURE THE FLAG"};
    std::string              scenario{"a30"};
    std::string              server_name;
    bool                     friendly_fire{true};
    int                      difficulty{1};
    int                      game_time_minutes{15};
    int                      respawn_seconds{10};
    bool                     teams{true};
    std::vector<LobbyPlayer> players;

    /// Adds a player and rebalances.
    ///
    /// Balancing happens here, and again before a match starts, rather than being something
    /// anyone has to ask for. A player arriving mid match is placed on whichever side is
    /// short instead of triggering a full reshuffle, because moving people already playing
    /// between teams mid game is worse than a brief imbalance.
    void AddPlayer(std::string name, bool host = false) {
        if (players.size() >= static_cast<std::size_t>(kMaxPlayers)) {
            return;
        }
        const int red  = CountOn(0);
        const int blue = CountOn(1);
        players.push_back(LobbyPlayer{std::move(name), red <= blue ? 0 : 1, host});
    }

    /// Splits everyone present into two even sides at random.
    ///
    /// Shuffling the whole list before dealing alternately keeps the sides within one
    /// player of each other for any count from two to ten, which an independent coin flip
    /// per player would not.
    void ShuffleTeams() {
        if (players.size() < 2) {
            return;
        }
        static std::mt19937 generator{std::random_device{}()};
        std::shuffle(players.begin(), players.end(), generator);
        for (std::size_t index = 0; index < players.size(); ++index) {
            players[index].team = static_cast<int>(index % 2);
        }
    }

    [[nodiscard]] int CountOn(int team) const {
        return static_cast<int>(std::count_if(players.begin(), players.end(),
                                              [team](const LobbyPlayer& player) {
                                                  return player.team == team;
                                              }));
    }
};
LobbyState g_lobby;

/// Logs every live object whose class name contains a fragment, and returns the count.
///
/// Reports the object's own name, its class, and its address, so a later pass can read
/// properties off anything interesting the listing turns up.
int LogObjectsMatching(std::string_view fragment, std::size_t limit);

std::mutex               g_state_mutex;
std::unique_ptr<ModState> g_state;               ///< Guarded by g_state_mutex.
std::atomic<bool>        g_running{false};
std::atomic<bool>        g_ready{false};         ///< True once the lobby manager exists.

// --- The engine view, reachable without the state lock -----------------------
//
// A second copy of the object array and the reflection, behind a lock of their own.
//
// This exists because of exactly one deadlock, and it is worth naming precisely so it is
// not reintroduced. The tick loop holds g_state_mutex while it calls LobbyManager::Tick.
// Beginning a match happens inside that tick, and it needs the object array, which lived
// only inside g_state. Reaching for it therefore meant taking g_state_mutex again: from the
// same thread if done directly, and from the game thread if done through a dispatched job.
// The second is what shipped. The game thread blocked on a lock the tick was holding, the
// tick blocked waiting for the game thread, and the pair sat there for the whole sixty
// second timeout before the wait gave up and left a job running against a stack frame that
// had already returned.
//
// Both are cheap value types: an address, a couple of counts and a pointer to the name pool,
// which outlives everything. Copying them is a handful of bytes, and having them behind a
// lock that is never held across a tick means anything, on any thread, can read them without
// ordering itself against the lobby.
std::mutex                         g_engine_view_mutex;
std::optional<unreal::ObjectArray> g_engine_objects;
std::optional<unreal::Reflection>  g_engine_reflection;

/// Publishes the object array for anything that needs it without the state lock.
void PublishEngineObjects(const unreal::ObjectArray& objects) {
    std::lock_guard lock(g_engine_view_mutex);
    g_engine_objects = objects;
}

/// The same for reflection.
void PublishEngineReflection(const unreal::Reflection& reflection) {
    std::lock_guard lock(g_engine_view_mutex);
    g_engine_reflection = reflection;
}

/// Takes a copy of both, or reports that they are not ready.
///
/// Optionals rather than references, because neither type is default constructible: a
/// Reflection is only meaningful against a name pool, and there is no such thing as one
/// without.
[[nodiscard]] bool TakeEngineView(std::optional<unreal::ObjectArray>& out_objects,
                                  std::optional<unreal::Reflection>&  out_reflection) {
    std::lock_guard lock(g_engine_view_mutex);
    if (!g_engine_objects.has_value() || !g_engine_reflection.has_value()) {
        return false;
    }
    out_objects    = g_engine_objects;
    out_reflection = g_engine_reflection;
    return true;
}

/// True from load until teardown begins.
///
/// The adaptive waits use this as their only abort condition: they wait as long as the
/// game needs, and stop only when the mod is being unloaded. That is what makes a slow
/// machine take longer rather than lose a feature.
std::atomic<bool>        g_running_or_starting{true};

/// When set, friendly fire is held on rather than merely set once.
///
/// Setting the flag by hand is not enough to play with. A level load builds fresh objects
/// and the value goes back to whatever the save record carried, so a value written at the
/// menu is gone by the time anyone is holding a weapon. Enforcing it from the poll loop
/// means it survives loading, checkpoints and respawns, which is the difference between a
/// flag that reads true and players who can actually damage each other.
std::atomic<bool>        g_enforce_friendly_fire{false};
std::thread              g_tick_thread;
HMODULE                  g_self_module = nullptr;

/// Directory holding the mod's data files, next to the game binary.
[[nodiscard]] std::filesystem::path DataDirectory() {
    return std::filesystem::path(ExecutableDirectory()) / "MultiplayerEvolved";
}

/// Waits for the Blam simulation module to be loaded by the game shell.
///
/// Lock free: the caller has already published g_state, so holding the lock across a
/// two minute poll would make every export report InvalidState for that whole time.
[[nodiscard]] Expected<blam::ModuleImage> WaitForSimulationModule() {
    // No deadline. The old two minute limit was measured on one machine; a slower disk
    // or a cold cache simply takes longer, and losing the engine binding because of that
    // would be a self inflicted failure. The wait ends when the module appears or the
    // process is going away.
    Expected<blam::ModuleImage> image = Error{ErrorCode::ModuleNotLoaded, "not attempted"};

    const bool appeared = pacing::WaitFor(
        "waiting for the simulation module",
        [&]() {
            image = blam::ModuleImage::Attach(L"HaloSimulation_tag_release.dll");
            return image.ok();
        },
        []() { return !g_running_or_starting.load(std::memory_order_acquire); });

    if (!appeared) {
        return Error{ErrorCode::ModuleNotLoaded, "gave up waiting for the simulation module"};
    }
    return image;
}

/// Discovers symbols against an image that is already in its final home, and builds
/// the globals surface.
///
/// The image must already live in state.simulation_module before this runs, because
/// SymbolRegistry stores a pointer to it. Discovering against a local and then moving
/// the image would leave that pointer dangling.
///
/// Returns the reason for failure on failure, or an empty string on success.
[[nodiscard]] std::string CommitEngineBinding(ModState& state) {
    if (!state.simulation_module.has_value()) {
        return "no simulation module to bind to";
    }

    // Per build descriptor if present, built in defaults otherwise. A community
    // member supporting a new patch edits JSON rather than code.
    const std::filesystem::path descriptor =
        DataDirectory() / "symbols" / std::format("{}.json", GameBuildString());

    blam::SymbolRegistryConfig config = blam::SymbolRegistryConfig::Default();
    if (std::filesystem::exists(descriptor)) {
        Expected<blam::SymbolRegistryConfig> loaded =
            blam::SymbolRegistryConfig::LoadFromFile(descriptor);
        if (loaded.ok()) {
            config = std::move(loaded).value();
        } else {
            MPE_LOG_WARN("using built in symbol defaults: {}", loaded.message());
        }
    } else {
        MPE_LOG_INFO("no descriptor at {}, using built in defaults", descriptor.string());
    }

    if (config.game_build != GameBuildString()) {
        MPE_LOG_WARN("the symbol descriptor targets build '{}' but this game is '{}'; discovery "
                    "will still be validated against the running binary",
                    config.game_build, GameBuildString());
    }

    Expected<blam::SymbolRegistry> registry =
        blam::SymbolRegistry::Discover(*state.simulation_module, config);
    if (!registry.ok()) {
        return std::format("symbol discovery failed: {}", registry.message());
    }
    state.symbols = std::move(registry).value();
    state.globals.emplace(*state.symbols);

    MPE_LOG_INFO("engine binding resolved: {} symbol(s) available", state.symbols->Count());

    // Count what is actually controllable, separating writable globals from string
    // ids. The distinction decides what this mod can and cannot do, so it belongs in
    // every log rather than in a comment.
    std::size_t writable = 0;
    std::size_t string_ids = 0;
    for (const blam::SymbolRecord& record : state.symbols->Records()) {
        if (record.stride >= blam::DebugGlobals::kMinimumStride) {
            ++writable;
        } else {
            ++string_ids;
        }
    }
    MPE_LOG_INFO("controllable surface: {} writable global(s), {} read only string id(s)",
                writable, string_ids);

    // Self test the write path against a debug visualization boolean, chosen because
    // toggling it has no gameplay effect even if something goes wrong. It writes,
    // reads back, and restores. Knowing the record layout is right beats assuming it.
    if (const Result verified = state.globals->VerifyWritePath("debug_damage_verbose");
        !verified.ok()) {
        MPE_LOG_ERROR("global write path self test FAILED: {}", verified.message());
        MPE_LOG_ERROR("globals will be readable but writes are not trusted on this build");
        state.globals.reset();
    }

    return {};
}

/// Waits for the engine module, then commits the binding under the state lock.
///
/// Called after g_state is published. The wait is lock free and the commit is short
/// (discovery measured at roughly 260 ms against the live module).
void ResolveEngineBinding() {
    Expected<blam::ModuleImage> image = WaitForSimulationModule();
    if (!image.ok()) {
        MPE_LOG_ERROR("{}", image.message());
        MPE_LOG_ERROR("the engine surface is unavailable. The lobby and Steam layers are "
                     "unaffected; hosting a match is not possible.");
        return;
    }

    std::lock_guard lock(g_state_mutex);
    if (!g_state) {
        return; // Shut down while we were waiting.
    }

    // Moved into its final home first, then discovered against, so the registry's
    // pointer to it stays valid.
    g_state->simulation_module = std::move(image).value();

    if (const std::string failure = CommitEngineBinding(*g_state); !failure.empty()) {
        MPE_LOG_ERROR("{}", failure);
        MPE_LOG_ERROR("report this log along with the game version so a symbol descriptor can "
                     "be added for this build.");
        g_state->symbols.reset();
        g_state->globals.reset();
        g_state->simulation_module.reset();
    }
}

/// Waits for Steam, then builds the backend and the transport.
[[nodiscard]] std::string BringUpNetworking(ModState& state) {
    const auto deadline = std::chrono::steady_clock::now() + kStartupTimeout;

    // Bind to the steam_api64.dll the game ships. Polled because the shell loads
    // Steam some time after the process starts, and until it has there is nothing
    // to bind to.
    // Two phase, and the order is deliberate.
    //
    // Phase one waits for the game shell to load steam_api64.dll and initialize Steam
    // itself, without ever loading it ourselves. Doing otherwise means calling
    // SteamAPI_Init before the host application does, and whether that happens depends
    // on who wins a startup race: two runs of the same build were observed producing
    // owned=false and owned=true. Owning the API in the game process would also mean
    // SteamAPI_Shutdown on unload tears down the game's Steam.
    //
    // Phase two, only after the wait expires, loads it ourselves. That is the correct
    // behaviour for a test harness or a headless server where nobody else will.
    std::string steam_error = "not attempted";
    bool        steam_ready = false;

    const auto wait_for_host = std::chrono::steady_clock::now() + kSteamHostWait;
    while (std::chrono::steady_clock::now() < wait_for_host) {
        if (steam::Initialize(ExecutableDirectory(), false, steam_error)) {
            steam_ready = true;
            break;
        }
        std::this_thread::sleep_for(kStartupPollInterval);
    }

    if (!steam_ready) {
        MPE_LOG_INFO("the host application did not initialize Steam within {} s; this process "
                    "will take ownership of it",
                    std::chrono::duration_cast<std::chrono::seconds>(kSteamHostWait).count());
        while (std::chrono::steady_clock::now() < deadline) {
            if (steam::Initialize(ExecutableDirectory(), true, steam_error)) {
                steam_ready = true;
                break;
            }
            std::this_thread::sleep_for(kStartupPollInterval);
        }
    }

    if (!steam_ready) {
        return std::format("the Steam API never became usable: {}", steam_error);
    }

    Expected<std::unique_ptr<lobby::SteamMatchmakingHooks>> backend =
        Error{ErrorCode::SteamUnavailable, "not attempted"};
    while (std::chrono::steady_clock::now() < deadline) {
        backend = lobby::SteamMatchmakingHooks::CreateInstance();
        if (backend.ok()) {
            break;
        }
        std::this_thread::sleep_for(kStartupPollInterval);
    }
    if (!backend.ok()) {
        return std::format("Steam matchmaking never became usable: {}", backend.message());
    }
    state.backend = std::move(backend).value();

    net::SteamTransportOptions options;
    // The game owns the Steam pipe and pumps callbacks itself. Pumping here too
    // would race with it.
    options.owns_callback_pump = false;

    Expected<std::unique_ptr<net::SteamSocketsTransport>> transport =
        net::SteamSocketsTransport::Create(options);
    if (!transport.ok()) {
        return std::format("the relay transport could not start: {}", transport.message());
    }
    state.transport = std::move(transport).value();
    return {};
}

void TickLoop() {
    auto next_tick = std::chrono::steady_clock::now();
    auto last_tick = next_tick;

    while (g_running.load(std::memory_order_acquire)) {
        next_tick += kTickInterval;

        {
            std::lock_guard lock(g_state_mutex);
            if (g_state && g_state->manager) {
                const auto now = std::chrono::steady_clock::now();
                const double delta =
                    std::chrono::duration<double>(now - last_tick).count();
                last_tick = now;

                // A delta larger than a second means the process was suspended or
                // the loading screen blocked. Clamped so timers do not jump and
                // cancel a countdown that was fine.
                g_state->manager->Tick(delta > 1.0 ? 1.0 : delta);

                // A launch that failed on the game thread is unwound here, on the thread
                // that owns the lobby.
                HandleLaunchFailureLocked();
            } else {
                last_tick = std::chrono::steady_clock::now();
            }
        }

        // Watches the friendly fire copies for changes the game makes itself. Rate
        // limited internally to 1 Hz, so this costs a handful of byte reads per second.
        PollWatchedFields();

        // New game threads start with clean debug registers, so armed watchpoints are

        // re-applied periodically. Cheap: it no-ops unless something is armed.

        // Keep the multiplayer button present.
        //
        // The menu widget is rebuilt whenever the frontend is shown, and a button added to
        // the previous instance goes away with it. Watching for a menu we have not yet
        // decorated makes the entry behave like a shipped one: it is simply always there,
        // rather than something that appears only after a command is run.
        MaintainMainMenuButton();

        // Built ahead of the player, so pressing MULTIPLAYER is a visibility change rather
        // than a hundred widget creations. Called from here rather than from the entry
        // being added, because the buttons it creates can only be watched once the watch
        // they share a virtual table with exists, and that is established last.
        PrepareStatusOverlay();
        PrepareLobby();
        PrepareLoadingOverlay();
        RegisterLoadingCancel();

        // Twelve and a half times a second, which is enough for the dots to read as a cycle
        // and the sweep as motion. Sixty would be smoother and would cost sixty game thread
        // round trips a second to animate three widgets, which is not a trade worth making
        // while the machine is loading a level.
        {
            static auto s_last_frame = std::chrono::steady_clock::time_point{};
            const auto  now          = std::chrono::steady_clock::now();
            if (now - s_last_frame >= std::chrono::milliseconds(80)) {
                s_last_frame = now;
                RefreshLoadingScreen();
            }
        }

        // Kept current while a session is up: the name, mode and map a player picks have to
        // reach the lobby's metadata or nobody browsing can tell one game from another.
        //
        // Twice a second rather than sixty times. It reads the session under the state
        // lock and formats a summary to compare against the last one, and the lock it
        // takes is the same one the game thread's callbacks need. Nothing it publishes
        // can change faster than a player can press a button, so the other fifty eight
        // passes a second were contention bought for nothing.
        {
            static auto s_last_publish = std::chrono::steady_clock::time_point{};
            const auto  now            = std::chrono::steady_clock::now();
            if (now - s_last_publish >= std::chrono::milliseconds(500)) {
                s_last_publish = now;
                PublishSessionDetails();
            }
        }

        // The invite panel's session line, while it is open. The lobby it points at finishes
        // being created a beat after the panel opens, and this is what stops the line saying
        // the session is being prepared once it is not.
        RefreshInvitePanelState();

        RefreshLobbyStatus();
        CheckOwnSessionIsFindable();

        // Four times a second. It reads the session under the state lock and formats two
        // strings to compare against the last pair; sixty passes a second to notice a
        // button press is contention bought for nothing.
        {
            static auto s_last_authority = std::chrono::steady_clock::time_point{};
            const auto  now              = std::chrono::steady_clock::now();
            if (now - s_last_authority >= std::chrono::milliseconds(250)) {
                s_last_authority = now;
                RefreshLobbyAuthority();
            }
        }

        // Four times a second. Somebody joining should appear promptly, and a quarter of
        // a second is promptly; copying the roster and building a comparison string sixty
        // times a second under the state lock is not what makes it feel that way.
        {
            static auto s_last_roster = std::chrono::steady_clock::time_point{};
            const auto  now           = std::chrono::steady_clock::now();
            if (now - s_last_roster >= std::chrono::milliseconds(250)) {
                s_last_roster = now;
                RefreshLobbyRoster();
            }
        }

        // The name is picked up on a short interval rather than only when a match starts.
        //
        // Two reasons, both about it being visible. The length limit is applied where the
        // field is read, so reading rarely would let an over long name sit on screen
        // looking accepted; and the name is what the session advertises, so a player who
        // types one and then walks away should still be findable under it.
        if (unreal::LobbyIsBuilt() && g_screen != MultiplayerScreen::ServerBrowser) {
            static auto s_last_name = std::chrono::steady_clock::time_point{};
            const auto  now         = std::chrono::steady_clock::now();
            if (now - s_last_name >= std::chrono::seconds(2)) {
                s_last_name = now;
                CaptureServerName();
            }
        }

        // The browser refreshes itself while it is open.
        //
        // A lobby search is asynchronous: asking Steam and reading the answer in the same
        // breath always reads the previous result, which on the first visit is nothing at
        // all. Re-asking on a timer and redrawing from whatever has arrived means the list
        // fills in on its own a moment after the tab is opened, with no refresh button to
        // find.
        if (g_screen == MultiplayerScreen::ServerBrowser && unreal::LobbyIsBuilt()) {
            static auto s_last_search = std::chrono::steady_clock::time_point{};
            const auto  now           = std::chrono::steady_clock::now();
            if (now - s_last_search >= std::chrono::seconds(3)) {
                s_last_search = now;
                steam::RequestLobbyList();
            }
            static auto s_last_draw = std::chrono::steady_clock::time_point{};
            if (now - s_last_draw >= std::chrono::seconds(1)) {
                s_last_draw = now;
                ApplyServerFilter();
            }
        }

        // Phase changes are logged, so a session that never becomes joinable says why
        // instead of simply never appearing.
        {
            static lobby::LobbyPhase s_phase = lobby::LobbyPhase::Idle;
            std::lock_guard          lock(g_state_mutex);
            if (g_state && g_state->manager) {
                const lobby::LobbyPhase now = g_state->manager->Phase();
                if (now != s_phase) {
                    s_phase = now;
                    MPE_LOG_INFO("session phase is now {} (lobby {}, error '{}')",
                                static_cast<int>(now),
                                g_state->manager->Snapshot().lobby_id,
                                g_state->manager->Snapshot().last_error);
                }
            }
        }

        // Act on a click as soon as it is seen. The flag is set on the game thread by the
        // widget's own event path and cleared here, so a press is never missed or handled
        // twice.
        if (unreal::ConsumeWidgetClick()) {
            // Which button decides what happens. The menu entry opens the lobby; a button
            // on the lobby itself changes what the lobby shows or does, and then the screen
            // is rebuilt so it reflects the change.
            const std::uintptr_t pressed = unreal::ConsumeClickedWidget();
            unreal::LobbyAction  action  = unreal::LobbyAction::None;
            int                  pressed_index = 0;
            for (const unreal::LobbyControl& control : g_lobby_controls) {
                if (control.widget == pressed && pressed != 0) {
                    action        = control.action;
                    pressed_index = control.index;
                    break;
                }
            }

            switch (action) {
                case unreal::LobbyAction::None:
                    MPE_LOG_INFO("MULTIPLAYER clicked");
                    OnMultiplayerClicked();
                    break;
                case unreal::LobbyAction::ShowHost:
                    g_screen = MultiplayerScreen::Home;
                    SwitchLobbyTab(false);
                    break;
                case unreal::LobbyAction::ShowBrowse:
                    g_screen = MultiplayerScreen::ServerBrowser;
                    SwitchLobbyTab(true);
                    // Asks Steam again on every visit, so the list is what is actually out
                    // there now rather than whatever it was when the screen was built.
                    steam::RequestLobbyList();
                    ApplyServerFilter();
                    break;
                case unreal::LobbyAction::SelectCaptureTheFlag:
                    if (RefuseWithoutAuthority("the game mode")) {
                        break;
                    }
                    g_lobby.mode = "CAPTURE THE FLAG";
                    SelectLobbyMode(false);
                    break;
                case unreal::LobbyAction::SelectSlayer:
                    if (RefuseWithoutAuthority("the game mode")) {
                        break;
                    }
                    g_lobby.mode = "SLAYER";
                    SelectLobbyMode(true);
                    break;
                case unreal::LobbyAction::StartMatch:
                    if (RefuseWithoutAuthority("when the match starts")) {
                        break;
                    }
                    OnStartMatch();
                    break;
                case unreal::LobbyAction::JoinMatch:
                    OnJoinMatch();
                    break;
                case unreal::LobbyAction::Back:
                    OnLeaveLobby();
                    break;
                case unreal::LobbyAction::InviteRed:
                case unreal::LobbyAction::InviteBlue:
                    // Which slot was pressed is deliberately not passed on. See
                    // InviteToSession.
                    InviteToSession();
                    break;

                case unreal::LobbyAction::FilterModeAny:
                    g_server_filter.mode.clear();
                    ApplyServerFilter();
                    break;
                case unreal::LobbyAction::FilterModeCaptureTheFlag:
                    g_server_filter.mode = "CAPTURE THE FLAG";
                    ApplyServerFilter();
                    break;
                case unreal::LobbyAction::FilterModeSlayer:
                    g_server_filter.mode = "SLAYER";
                    ApplyServerFilter();
                    break;
                case unreal::LobbyAction::FilterSlotsAny:
                    g_server_filter.slots = 0;
                    ApplyServerFilter();
                    break;
                case unreal::LobbyAction::FilterSlotsOpen:
                    g_server_filter.slots = 1;
                    ApplyServerFilter();
                    break;
                case unreal::LobbyAction::FilterSlotsFull:
                    g_server_filter.slots = 2;
                    ApplyServerFilter();
                    break;
                case unreal::LobbyAction::FilterPingAny:
                    g_server_filter.max_ping = 0;
                    ApplyServerFilter();
                    break;
                case unreal::LobbyAction::FilterPingUnder50:
                    g_server_filter.max_ping = 50;
                    ApplyServerFilter();
                    break;
                case unreal::LobbyAction::FilterPingUnder100:
                    g_server_filter.max_ping = 100;
                    ApplyServerFilter();
                    break;
                case unreal::LobbyAction::SelectServer:
                    g_selected_server = pressed_index;
                    ApplyServerFilter();
                    break;
                case unreal::LobbyAction::SelectMap:
                    if (RefuseWithoutAuthority("the map")) {
                        break;
                    }
                    SelectLobbyMap(pressed_index);
                    break;

                case unreal::LobbyAction::RefreshServers:
                    MPE_LOG_INFO("server list refresh requested");
                    steam::RequestLobbyList();
                    ApplyServerFilter();
                    break;

                case unreal::LobbyAction::SelectFriend:
                    InviteFriendAt(pressed_index);
                    break;
                case unreal::LobbyAction::CloseInvite:
                    CloseInviteList();
                    break;
                case unreal::LobbyAction::FriendsPrevious:
                    PageFriendList(-1);
                    break;
                case unreal::LobbyAction::FriendsNext:
                    PageFriendList(1);
                    break;
                case unreal::LobbyAction::CancelLoading:
                    OnCancelLoading();
                    break;
            }
        }

        // Report what size the layout gave the lobby, once, a moment after it opened.
        if (g_lobby_measure_at != std::chrono::steady_clock::time_point{} &&
            std::chrono::steady_clock::now() >= g_lobby_measure_at) {
            g_lobby_measure_at = {};
            if (g_lobby_ui_ready && unreal::OpenLobbyFrame() != 0) {
                unreal::LobbyUIContext measured = g_lobby_ui;
                if (unreal::BindLobbyMenu(g_live_menu, measured).ok()) {
                    (void)unreal::RunOnGameThread(
                        [&]() { unreal::MeasureLobby(measured); }, 5000);
                }
            }
        }

        // A trap that has run away is disarmed here rather than in the handler, because
        // disarming suspends every thread and the handler is running on one of them.
        if (debugtrap::OverBudget()) {
            MPE_LOG_WARN("a hardware trap exceeded its recording budget and is being "
                        "disarmed; the address it watched is called too often to trace "
                        "this way");
            debugtrap::DisarmAll();
        }
        debugtrap::RefreshThreads();

        // Absolute deadline scheduling, so a slow tick does not accumulate drift.
        const auto now = std::chrono::steady_clock::now();
        if (next_tick > now) {
            std::this_thread::sleep_for(next_tick - now);
        } else {
            next_tick = now;
        }
    }
}

void ResolveUnrealObjects();
void DetectPropertyLayout();

/// Adds a boolean field to the change watch list.
///
/// Duplicate addresses are ignored, because the same field is reached through more than
/// one search path and logging it twice would only add noise.
void RegisterWatch(std::string label, std::uintptr_t instance_address,
                   std::uintptr_t owner_class, std::uint32_t byte_offset, bool current) {
    std::lock_guard lock(g_state_mutex);
    if (!g_state || instance_address == 0) {
        return;
    }
    const std::uintptr_t address = instance_address + byte_offset;
    for (const ModState::WatchedField& existing : g_state->watched) {
        if (existing.address == address) {
            return;
        }
    }
    g_state->watched.push_back(ModState::WatchedField{std::move(label), address, instance_address,
                                                     owner_class, byte_offset, current, true});
}

/// Confirms a watched object is still the object it was, and rebinds if it is not.
///
/// The check is one pointer read: an object's class pointer. If the object was collected
/// and its memory handed to something else, that pointer no longer matches the class the
/// watch was created against. This costs nothing in the steady state, which is what makes
/// it affordable at poll rate.
///
/// A mismatch triggers one scan of the object array for a live instance of the same
/// class. Level transitions are the only time that scan runs, so the expensive path is
/// paid exactly when it is needed. If no instance exists any more, the watch is retired
/// rather than left pointing at reused memory.
///
/// Caller must hold g_state_mutex.
[[nodiscard]] bool RevalidateWatch(ModState::WatchedField& field) {
    if (field.owner_class == 0 || !g_state->objects.has_value()) {
        // Registered without provenance: nothing to check against, so leave it alone
        // rather than retiring a watch that may well be fine.
        return true;
    }

    if (g_state->objects->ClassOf(field.instance_address) == field.owner_class) {
        return true;
    }

    const std::vector<unreal::ObjectInfo> live =
        g_state->objects->FindInstancesOfClassAddress(field.owner_class, 16);

    // Picking the first result is wrong. That list includes the class default object, the
    // template Unreal keeps for every class, and a watch on a real instance that silently
    // rebinds onto the CDO reports the template's value under the instance's name. Worse,
    // the CDO already has its own watch, so two entries collapse onto one address and
    // appear to agree when they are simply the same byte read twice.
    //
    // A watch keeps its own character: a Default__ watch stays on the template, and an
    // instance watch stays on a real instance.
    const auto is_default = [](std::string_view name) {
        return name.rfind("Default__", 0) == 0;
    };
    const bool want_default = is_default(field.label);

    std::uintptr_t rebound = 0;
    for (const unreal::ObjectInfo& candidate : live) {
        if (is_default(candidate.name) != want_default) {
            continue;
        }
        // Never land on a byte another live watch already owns.
        const std::uintptr_t address = candidate.address + field.byte_offset;
        const bool taken = std::any_of(
            g_state->watched.begin(), g_state->watched.end(),
            [&](const ModState::WatchedField& other) {
                return &other != &field && other.valid && other.address == address;
            });
        if (!taken) {
            rebound = candidate.address;
            break;
        }
    }

    if (rebound == 0) {
        MPE_LOG_INFO("watch: {} retired; its object was collected and no distinct {} instance "
                    "of the class remains",
                    field.label, want_default ? "default" : "live");
        field.valid = false;
        return false;
    }

    MPE_LOG_INFO("watch: {} rebound from a collected object at 0x{:X} to 0x{:X}", field.label,
                field.instance_address, rebound);
    field.instance_address = rebound;
    field.address          = rebound + field.byte_offset;

    // The new object's value is its own, not a continuation of the old one, so it is
    // adopted as the baseline instead of being reported as a change.
    std::uint8_t raw = 0;
    if (unreal::memory::GuardedRead(field.address, &raw, sizeof(raw))) {
        field.last_value = (raw != 0);
    }
    return true;
}

int LogObjectsMatching(std::string_view fragment, std::size_t limit) {
    std::lock_guard lock(g_state_mutex);
    if (!g_state || !g_state->objects.has_value()) {
        MPE_LOG_WARN("the object array is not available yet");
        return 0;
    }

    const std::vector<unreal::ObjectInfo> found =
        g_state->objects->FindByClassNameContains(fragment, limit);
    if (found.empty()) {
        return 0;
    }

    MPE_LOG_INFO("'{}' matched {} live object(s):", fragment, found.size());
    for (const unreal::ObjectInfo& object : found) {
        MPE_LOG_INFO("  {} : {} @ 0x{:X}", object.name, object.class_name, object.address);
    }
    return static_cast<int>(found.size());
}

/// Resolves an address referenced by a RIP relative instruction matching a pattern.
///
/// The engine's globals live in .data, and the fastest way to find one is to look at code
/// that already refers to it rather than to search the heap for something that looks right.
/// One pass over the static image replaces a scan that took ten seconds.
[[nodiscard]] std::uintptr_t ResolveGlobalByReference(std::string_view pattern,
                                                      std::size_t displacement_offset,
                                                      std::size_t instruction_length) {
    Expected<blam::ModuleImage> image = blam::ModuleImage::Attach(L"HaloCampaignEvolved.exe");
    if (!image.ok()) {
        return 0;
    }
    const blam::Section* text = image.value().FindSection(".text");
    if (text == nullptr) {
        return 0;
    }

    const Expected<blam::BytePattern> parsed = blam::BytePattern::Parse(pattern);
    if (!parsed.ok()) {
        return 0;
    }

    const blam::PatternScanner        scanner(image.value());
    const std::vector<std::uintptr_t> matches =
        scanner.FindPattern(*text, parsed.value(), 1);
    if (matches.empty()) {
        return 0;
    }
    const std::uintptr_t match = matches.front();

    std::int32_t displacement = 0;
    if (!unreal::memory::GuardedRead(match + displacement_offset, &displacement,
                                     sizeof(displacement))) {
        return 0;
    }
    return match + instruction_length + static_cast<std::uintptr_t>(
                                            static_cast<std::intptr_t>(displacement));
}

/// Brings up UE reflection as early as possible.
///
/// The searching version could not finish before the game built its menu: waiting for the
/// simulation module, waiting for loading to settle and then scanning the heap came to
/// nearly forty seconds, by which time the menu had been on screen for a while and anything
/// added to it arrived visibly late.
///
/// Both globals are instead read from instructions that reference them, which needs only the
/// static image and so can run immediately. Each result is still verified, and a failure
/// falls back to the original search rather than proceeding on a guess.
[[nodiscard]] bool TryFastReflection() {
    // lea rcx, [rip+pool] ; xor edx, edx ; mov [rip+x], rdi
    const std::uintptr_t pool_address = ResolveGlobalByReference(
        "48 8D 0D ? ? ? ? 33 D2 48 89 3D ? ? ? ? 41 B8 00 00", 3, 7);
    // lea rax, [rip+array] ; mov dword [rip+x], 1000
    const std::uintptr_t array_address = ResolveGlobalByReference(
        "48 8D 05 ? ? ? ? C7 05 ? ? ? ? E8 03 00 00", 3, 7);

    if (pool_address == 0 || array_address == 0) {
        MPE_LOG_INFO("fast reflection unavailable; falling back to the search");
        return false;
    }

    Expected<unreal::NamePool> pool = unreal::NamePool::FromBlocks(pool_address);
    if (!pool.ok()) {
        MPE_LOG_WARN("fast pool rejected: {}", pool.error().message);
        return false;
    }

    std::lock_guard lock(g_state_mutex);
    if (!g_state) {
        return false;
    }
    g_state->names = std::move(pool.value());

    Expected<unreal::ObjectArray> objects =
        unreal::ObjectArray::FromAddress(*g_state->names, array_address);
    if (!objects.ok()) {
        MPE_LOG_WARN("fast object array rejected: {}", objects.error().message);
        g_state->names.reset();
        return false;
    }
    g_state->objects = std::move(objects.value());

    // Reflection is created here so class and object lookups work immediately, but its
    // property layout is deliberately left to the later detection pass. Detecting offsets
    // now would use a graph that is only part built, and the offsets it inferred would be
    // wrong for the rest of the session. Anything this early therefore uses fixed offsets
    // rather than reflected ones.
    g_state->reflection.emplace(*g_state->names);

    PublishEngineObjects(*g_state->objects);
    PublishEngineReflection(*g_state->reflection);
    return true;
}

void OnMultiplayerClicked() {
    // Nothing is searched for here.
    //
    // This used to resolve a menu plan and the whole lobby context on every press, which
    // is roughly a dozen passes over fifty thousand objects, under the state lock, while
    // the player waited: fifteen seconds between the click and the screen. Both are now
    // resolved once and cached, so a press costs two guarded reads and the widget calls
    // themselves.
    if (!g_lobby_ui_ready || g_live_menu == 0) {
        MPE_LOG_WARN("multiplayer was pressed before the lobby was ready to open");
        return;
    }

    // Last chance to know what to fold away.
    //
    // The fold list is built when the menu is decorated, and if that ever fails to happen
    // the lobby opens with the frontend's own panels still drawn over it: the fireteam
    // sits on top of the lobby, visible and audible. That is a bad enough outcome to be
    // worth one scan here rather than trusting the list is populated, and it costs nothing
    // in the ordinary case because the list is already there.
    if (g_lobby_ui.also_fold.empty()) {
        MPE_LOG_WARN("the fold list is empty at open; collecting it now so the frontend's "
                    "panels do not stay on screen over the lobby");
        std::lock_guard lock(g_state_mutex);
        if (g_state && g_state->objects.has_value()) {
            std::vector<std::uintptr_t> beside;
            g_state->objects->ForEach([&](const unreal::ObjectInfo& object) {
                if (object.name.rfind("Default__", 0) == 0) {
                    return true;
                }
                if (object.class_name.rfind("WBP_Squad", 0) == 0 ||
                    object.class_name == "WBP_MeteoriteUILayout_C" ||
                    object.class_name.rfind("WBP_MeteoriteBoundActionBar", 0) == 0) {
                    beside.push_back(object.address);
                }
                return true;
            });
            g_lobby_ui.also_fold = beside;
        }
    }

    unreal::LobbyUIContext ui = g_lobby_ui;
    if (const Result bound = unreal::BindLobbyMenu(g_live_menu, ui); !bound.ok()) {
        MPE_LOG_WARN("the lobby cannot attach to menu 0x{:X}: {}", g_live_menu,
                    bound.message());
        return;
    }

    // Already built and waiting, so opening it is one visibility change rather than a
    // hundred widget creations.
    if (unreal::LobbyIsBuilt()) {
        (void)unreal::RunOnGameThread([&]() { unreal::ShowLobbyUI(ui, true); }, 5000);
        // A real session behind the screen, so the slots have something to invite into and
        // the browser has something to list.
        EnsureSessionHosted();
        MPE_LOG_INFO("multiplayer lobby shown");
        return;
    }

    // The host always occupies a slot, so the lobby is never shown empty.
    if (g_lobby.players.empty()) {
        g_lobby.AddPlayer(SteamPlayerName(), true);
    }

    // Whatever this player called their game last time, so it is typed once rather than
    // once per launch. Failing that, their own name, because a server nobody has named is
    // still somebody's, and "Nessie's Server" tells a browser far more than a blank row or
    // a placeholder that is the same on every machine.
    if (g_lobby.server_name.empty()) {
        g_lobby.server_name = LoadServerName();
    }
    if (g_lobby.server_name.empty()) {
        const std::string owner = SteamPlayerName();
        if (!owner.empty()) {
            g_lobby.server_name = std::format("{}'s Server", owner);
        }
    }

    // The screen is ordinary UMG: tabs, two team columns of player cards, a settings panel
    // and a server table, created with SpawnObject and placed on the menu's own canvas. An
    // earlier version built a list of menu rows instead, which was never going to reach
    // this layout, because a vertical stack of buttons cannot express any of it.
    unreal::LobbyView lobby_view;

    // Everything shown comes from the live lobby. Nothing here is invented: an empty
    // server list is drawn empty, because a list of servers that do not exist would be
    // worse than none.
    lobby_view.browsing          = (g_screen == MultiplayerScreen::ServerBrowser);
    lobby_view.mode              = g_lobby.mode;
    lobby_view.map               = g_lobby.scenario;
    lobby_view.friendly_fire     = g_lobby.friendly_fire;
    lobby_view.game_time_minutes = g_lobby.game_time_minutes;
    lobby_view.respawn_seconds   = g_lobby.respawn_seconds;
    lobby_view.server_name       = g_lobby.server_name;
    for (const LobbyPlayer& player : g_lobby.players) {
        if (player.host) {
            lobby_view.host_name = player.name;
        }
        (player.team == 0 ? lobby_view.red : lobby_view.blue).push_back(player.name);
    }
    lobby_view.servers = DiscoveredServers();

    // The previous screen's buttons are about to be destroyed, so stop watching them
    // first: a freed address can be reused, and the next object there is not a button.
    unreal::ForgetExtraWatchedWidgets();

    std::uintptr_t                    lobby_root = 0;
    std::vector<unreal::LobbyControl> controls;
    Result                            outcome = Result::Success();
    const Result                      ran     = unreal::RunOnGameThread(
        [&]() { outcome = unreal::BuildLobbyUI(ui, lobby_view, lobby_root, controls); },
        20000);

    if (!ran.ok() || !outcome.ok()) {
        MPE_LOG_WARN("could not open the multiplayer lobby: {}",
                    ran.ok() ? outcome.message() : ran.message());
        return;
    }
    g_lobby_root     = lobby_root;
    g_lobby_controls = controls;

    // Every button on the screen becomes live, not just the entry that opened it. They are
    // all the frontend's button class, so they share its virtual table and the watch that
    // is already established can simply be extended to each of them.
    int watched = 0;
    for (const unreal::LobbyControl& control : g_lobby_controls) {
        if (control.widget != 0 && unreal::AlsoWatchWidget(control.widget).ok()) {
            ++watched;
        }
    }
    MPE_LOG_INFO("{} of {} lobby button(s) are live", watched, g_lobby_controls.size());

    // Read back a second later what size the layout actually gave the screen. A lobby that
    // reports its real size and still looks empty is a different fault from one the layout
    // collapsed to nothing, and from a blank screen the two look identical.
    g_lobby_measure_at = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    MPE_LOG_INFO("multiplayer lobby open (root 0x{:X})", lobby_root);
}

/// Switches tab without rebuilding anything.
void SwitchLobbyTab(bool browsing) {
    if (!g_lobby_ui_ready) {
        return;
    }
    unreal::LobbyUIContext ui = g_lobby_ui;
    if (!unreal::BindLobbyMenu(g_live_menu, ui).ok()) {
        return;
    }
    (void)unreal::RunOnGameThread([&]() { unreal::SetLobbyTab(ui, browsing); }, 5000);
}

/// Where the server name is kept between sessions.
[[nodiscard]] std::filesystem::path ServerNamePath() {
    return DataDirectory() / "server-name.txt";
}

/// Remembers the server name so it does not have to be typed again next time.
void SaveServerName(const std::string& name) {
    static std::string s_saved;
    if (name == s_saved) {
        return;
    }
    std::error_code error;
    std::filesystem::create_directories(DataDirectory(), error);
    std::ofstream file(ServerNamePath(), std::ios::binary | std::ios::trunc);
    if (!file) {
        MPE_LOG_WARN("could not write {}", ServerNamePath().string());
        return;
    }
    file.write(name.data(), static_cast<std::streamsize>(name.size()));
    s_saved = name;
    MPE_LOG_INFO("server name saved as '{}'", name);
}

/// Reads back a previously saved server name, or nothing on the first run.
[[nodiscard]] std::string LoadServerName() {
    std::ifstream file(ServerNamePath(), std::ios::binary);
    if (!file) {
        return {};
    }
    std::string name((std::istreambuf_iterator<char>(file)),
                     std::istreambuf_iterator<char>());
    // Trailing whitespace comes from anyone who edits the file by hand, and a name with a
    // newline on the end is advertised with a newline on the end.
    while (!name.empty() && (name.back() == '\n' || name.back() == '\r' ||
                             name.back() == ' ' || name.back() == '\t')) {
        name.pop_back();
    }
    if (name.size() > unreal::kMaxServerNameLength) {
        name.resize(unreal::kMaxServerNameLength);
    }
    return name;
}

/// Takes whatever the player typed into the server name field.
///
/// Read on demand rather than tracked as it is typed: the field owns its own text, so
/// asking it at the moment the name matters is both simpler and always current. Reading is
/// also what enforces the length limit, because that is where the field is corrected.
void CaptureServerName() {
    if (!g_lobby_ui_ready) {
        return;
    }
    // A guest's field holds the host's name, put there by the authority pass. Reading it
    // back would take somebody else's name, save it as this machine's own, and advertise it
    // the next time this player hosted anything.
    if (!LocalPlayerHasAuthority()) {
        return;
    }
    unreal::LobbyUIContext ui = g_lobby_ui;
    if (!unreal::BindLobbyMenu(g_live_menu, ui).ok()) {
        return;
    }
    std::string typed;
    (void)unreal::RunOnGameThread([&]() { typed = unreal::ReadServerName(ui); }, 5000);
    if (typed.empty() || typed == g_lobby.server_name) {
        return;
    }
    g_lobby.server_name = typed;
    MPE_LOG_INFO("server name is '{}'", g_lobby.server_name);
    SaveServerName(g_lobby.server_name);
}

/// Makes sure a real, joinable session exists behind the lobby screen.
///
/// The screen on its own is only a picture: without a session there is nothing for anyone
/// to join and nothing for an invite to point at. Hosting is idempotent here, so opening
/// the lobby twice does not create two sessions.
void EnsureSessionHosted() {
    std::lock_guard lock(g_state_mutex);
    if (!g_state || !g_state->manager) {
        MPE_LOG_WARN("no session can be hosted: networking is unavailable");
        return;
    }
    if (g_state->manager->Phase() != lobby::LobbyPhase::Idle) {
        return; // Already hosting or joining.
    }

    lobby::HostOptions options;
    // Invitable by anyone with the mod, and listed so the browser can find it. A friends
    // only lobby cannot be discovered by a stranger, which would make the server browser
    // permanently empty for everyone but the host's friends.
    options.visibility  = lobby::LobbyVisibility::Public;
    options.max_players = static_cast<std::uint32_t>(LobbyState::kMaxPlayers);

    // Carried from what the player chose on the lobby screen. These are validated before
    // the session is created, so leaving them at their defaults meant hosting was refused
    // for an empty scenario no matter what the screen showed.
    options.settings.mode = g_lobby.mode == "SLAYER" ? engine::GameMode::TeamSlayer
                                                     : engine::GameMode::CaptureTheFlag;
    options.settings.scenario     = g_lobby.scenario;
    options.settings.variant_name = g_lobby.scenario;
    options.settings.team_count   = g_lobby.teams ? 2 : 1;
    options.settings.friendly_fire = g_lobby.friendly_fire;
    options.settings.respawn_enabled = true;
    options.settings.respawn_delay_seconds =
        static_cast<std::uint16_t>(g_lobby.respawn_seconds);
    options.settings.time_limit_seconds =
        static_cast<std::uint16_t>(g_lobby.game_time_minutes * 60);

    // Capture the Flag is a team mode, so a free for all lobby cannot host it. Validation
    // would reject the pair; correcting it here keeps the refusal impossible rather than
    // merely reported.
    if (options.settings.mode == engine::GameMode::CaptureTheFlag &&
        options.settings.team_count < 2) {
        options.settings.team_count = 2;
    }

    if (const Result hosted = g_state->manager->HostSession(options); !hosted.ok()) {
        MPE_LOG_WARN("the multiplayer session could not be hosted: {}", hosted.message());
        return;
    }
    MPE_LOG_INFO("multiplayer session hosted for up to {} players",
                options.max_players);
}

/// Opens Steam's invite dialog against this session, as soon as there is one.
///
/// This is the difference the slots are asking for: the game's own invite adds someone to
/// a fireteam, which has nothing to do with a multiplayer match. The dialog is opened
/// against this lobby's own id, so whoever accepts lands in this session.
///
/// Creating a Steam lobby is asynchronous. Pressing a slot the first time therefore arrives
/// while the lobby is still being created, and asking to invite then did nothing at all,
/// silently. The request is remembered instead and opened the moment hosting completes.
void OpenSessionInvite() {
    // Read now, not continuously, and never invited in bulk.
    //
    // An earlier version invited every friend it could see the moment a slot was pressed,
    // which is spam whether or not anyone accepts. The list is shown instead and one row
    // sends one invitation.
    const std::vector<steam::GameFriend> friends = steam::FriendsInGame();

    g_friend_list.clear();
    g_friend_ids.clear();
    g_friend_list.reserve(friends.size());
    g_friend_ids.reserve(friends.size());
    for (const steam::GameFriend& entry : friends) {
        unreal::LobbyFriend row;
        row.name    = entry.name;
        row.in_game = entry.in_this_game;
        g_friend_list.push_back(std::move(row));
        g_friend_ids.push_back(entry.id);
    }
    g_friend_page = 0;

    const std::size_t in_game = static_cast<std::size_t>(
        std::count_if(friends.begin(), friends.end(),
                      [](const steam::GameFriend& f) { return f.in_this_game; }));
    MPE_LOG_INFO("invite list: {} friend(s), {} in this game", friends.size(), in_game);

    ShowInviteList(true);
}

/// Keeps the panel's session line current while it is open.
///
/// Called from the poll rather than only when the panel opens, because the interesting
/// moment is the one after: the lobby finishes being created a beat later, and the line has
/// to stop saying it is being prepared without the player having to close and reopen
/// anything to find out.
void RefreshInvitePanelState() {
    if (!g_lobby_ui_ready || !unreal::InvitePanelIsOpen()) {
        return;
    }
    const InviteSessionState state = DescribeInviteSession();
    if (state.text == g_invite_state_shown) {
        return;
    }
    g_invite_state_shown = state.text;

    // The first time a session actually exists, the lobby's own metadata is published, so a
    // friend who accepts arrives at something with a name, a mode and a map on it.
    if (state.lobby != 0) {
        PublishSessionDetails();
    }

    unreal::LobbyUIContext ui = g_lobby_ui;
    if (!unreal::BindLobbyMenu(g_live_menu, ui).ok()) {
        return;
    }
    (void)unreal::RunOnGameThread(
        [&]() { unreal::SetInvitePanelState(ui, state.text, state.readiness); }, 5000);
}

/// The lobby this machine is hosting, or zero when it is not hosting one.
///
/// Deliberately not "the phase is Hosting". Hosting means the lobby is open and waiting,
/// which is only the first part of a session's life: pressing start moves it to Countdown,
/// Loading and then InMatch, and every one of those is still a session this machine owns
/// and can invite people to. Testing for Hosting alone meant the invite slots went dead the
/// moment a match began, so a friend who arrived late could not be invited at all.
///
/// The caller must hold g_state_mutex.
[[nodiscard]] lobby::LobbyId HostedLobbyLocked() {
    if (!g_state || !g_state->manager || !g_state->manager->IsHost()) {
        return 0;
    }
    switch (g_state->manager->Phase()) {
        case lobby::LobbyPhase::Hosting:
        case lobby::LobbyPhase::Countdown:
        case lobby::LobbyPhase::Loading:
        case lobby::LobbyPhase::InMatch:
        case lobby::LobbyPhase::PostMatch:
            return g_state->manager->Snapshot().lobby_id;
        default:
            // Idle, Creating and Faulted have no lobby worth pointing anybody at, and the
            // client side phases are not this machine's session at all.
            return 0;
    }
}

/// The lobby this machine is in, whether it owns it or joined it.
///
/// Inviting is not a host's privilege. Steam lets any member of a lobby invite to it, and a
/// player who has joined a friend and wants to pull in a third has exactly the same thing to
/// offer as the host does: this lobby, this session, come in.
///
/// Only the host's lobby was ever considered, so a guest pressing an empty slot found the
/// panel refusing to open, every time, with nothing said. Leaving to the main menu and
/// coming back appeared to fix it, which it did, by ending the session and hosting a new
/// one: the guest was quietly thrown out of their friend's game to be shown a list.
///
/// The caller must hold g_state_mutex.
[[nodiscard]] lobby::LobbyId CurrentSessionLobbyLocked() {
    if (!g_state || !g_state->manager) {
        return 0;
    }
    switch (g_state->manager->Phase()) {
        case lobby::LobbyPhase::Hosting:
        case lobby::LobbyPhase::Countdown:
        case lobby::LobbyPhase::Loading:
        case lobby::LobbyPhase::InMatch:
        case lobby::LobbyPhase::PostMatch:
        case lobby::LobbyPhase::InLobby:
            return g_state->manager->Snapshot().lobby_id;
        default:
            // Idle and Faulted have nothing to point at. Creating, Joining, Connecting and
            // Handshaking are all on their way to having one, and the panel says so rather
            // than pretending otherwise.
            return 0;
    }
}

InviteSessionState DescribeInviteSession() {
    InviteSessionState state;
    std::lock_guard    lock(g_state_mutex);
    if (!g_state || !g_state->manager) {
        state.text      = "STEAM IS UNAVAILABLE, SO NOBODY CAN BE INVITED";
        state.readiness = unreal::InviteReadiness::Unavailable;
        return state;
    }

    state.lobby = CurrentSessionLobbyLocked();
    if (state.lobby != 0) {
        state.readiness = unreal::InviteReadiness::Ready;
        state.text      = g_state->manager->IsHost()
                              ? "YOUR SESSION IS OPEN. PRESS A NAME TO INVITE THEM."
                              : "PRESS A NAME TO INVITE THEM INTO THIS SESSION.";
        return state;
    }

    switch (g_state->manager->Phase()) {
        case lobby::LobbyPhase::Creating:
        case lobby::LobbyPhase::Idle:
            state.text      = "PREPARING YOUR SESSION";
            state.readiness = unreal::InviteReadiness::Preparing;
            break;
        case lobby::LobbyPhase::Joining:
        case lobby::LobbyPhase::Connecting:
        case lobby::LobbyPhase::Handshaking:
            state.text      = "JOINING A SESSION. INVITES OPEN ONCE YOU ARE IN.";
            state.readiness = unreal::InviteReadiness::Preparing;
            break;
        default:
            state.text      = "THERE IS NO SESSION TO INVITE ANYONE TO";
            state.readiness = unreal::InviteReadiness::Unavailable;
            break;
    }
    return state;
}

bool LocalPlayerHasAuthority() {
    std::lock_guard lock(g_state_mutex);
    if (!g_state || !g_state->manager) {
        return true; // Nothing to be a guest of.
    }
    if (g_state->manager->Phase() == lobby::LobbyPhase::Idle) {
        return true;
    }
    return g_state->manager->IsHost();
}

bool RefuseWithoutAuthority(std::string_view what) {
    if (LocalPlayerHasAuthority()) {
        return false;
    }
    // Two gates, not one, and the second is the one that matters.
    //
    // The screen already greys these out, and that is the right thing to show. It is not a
    // rule: a widget's hit testing is a drawing decision, one rebuild away from being wrong,
    // and a bug that lets a guest change the map is a bug that desynchronises the match. The
    // rule lives here, where every path to changing something has to pass.
    MPE_LOG_INFO("ignoring a guest's attempt to change {}", what);
    ShowSessionNotice("THE HOST DECIDES",
                      std::format("Only the host can change {} in this session.", what));
    return true;
}

/// Puts the invite list on screen, or takes it off.
void ShowInviteList(bool visible) {
    if (!g_lobby_ui_ready) {
        return;
    }
    unreal::LobbyUIContext ui = g_lobby_ui;
    if (!unreal::BindLobbyMenu(g_live_menu, ui).ok()) {
        return;
    }
    (void)unreal::RunOnGameThread(
        [&]() {
            if (visible) {
                unreal::SetLobbyFriends(ui, g_friend_list, g_friend_page);
            }
            unreal::ShowInvitePanel(ui, visible);
        },
        5000);
}

void CloseInviteList() {
    g_invite_state_shown.clear();
    ShowInviteList(false);
}

void PageFriendList(int direction) {
    // The arithmetic is in Lobby/Discovery, with tests, because a paging bug has no
    // visible symptom: it silently invites somebody other than the person pressed.
    const int stepped = lobby::StepFriendPage(g_friend_page, direction, g_friend_list.size(),
                                              unreal::kFriendRows);
    if (stepped == g_friend_page) {
        return;
    }
    g_friend_page = stepped;
    ShowInviteList(true);
}

/// Invites the person on one row of the list, and nobody else.
void InviteFriendAt(int row) {
    const std::size_t index = lobby::FriendIndexFor(g_friend_page, row, g_friend_ids.size(),
                                                    unreal::kFriendRows);
    if (index == static_cast<std::size_t>(-1)) {
        return;
    }

    lobby::LobbyId lobby = 0;
    {
        std::lock_guard lock(g_state_mutex);
        lobby = CurrentSessionLobbyLocked();
    }
    if (lobby == 0) {
        // Said on the panel, not only to the log. A press that produces no visible change is
        // the failure the whole panel exists to avoid.
        const InviteSessionState state = DescribeInviteSession();
        ShowSessionNotice("CANNOT INVITE YET", state.text);
        MPE_LOG_WARN("cannot invite {}: there is no session on this machine to invite to",
                    g_friend_list[index].name);
        return;
    }

    if (!steam::InviteUserToLobby(lobby, g_friend_ids[index])) {
        MPE_LOG_WARN("Steam refused the invitation to {}", g_friend_list[index].name);
        return;
    }

    // Marked on the row rather than reported only to the log, because from the player's
    // side an invitation that produces no visible change is indistinguishable from one
    // that was never sent. This is the whole reason the overlay route was unusable.
    g_friend_list[index].invited = true;
    MPE_LOG_INFO("invited {} ({}) to session {}", g_friend_list[index].name,
                g_friend_ids[index], lobby);
    ShowInviteList(true);
}

void InviteToSession() {
    // The panel opens on the press. Not on the session.
    //
    // It used to open only once a Steam lobby id existed and only on a machine that was
    // hosting, which made the first press of every session do nothing at all: creating a
    // lobby is asynchronous, so the id arrives a moment after the button. A player pressing
    // a slot and seeing nothing has no way to tell that from a broken button, and pressing
    // it again is the obvious thing to do, which is why this looked like it needed two
    // attempts and a trip back to the main menu.
    //
    // Opening first and describing the session on the panel is the honest order: the list is
    // there instantly, and the line under the title says whether an invitation can go out
    // yet. The poll keeps that line current, so it stops saying the session is being
    // prepared the moment it is not.
    //
    // The slot that was pressed decides nothing.
    //
    // It reads like it should: press a red slot, get a red team mate. It cannot work that
    // way and stay balanced. Somebody invited into a chosen slot would have to keep it
    // even after two other people join the other side, and the alternative, honouring it
    // only sometimes, is worse than never honouring it.
    //
    // Teams are assigned on arrival by whichever side is smallest, so every slot is the
    // same button. This used to take the team as an argument and use it for nothing but a
    // log line, which was itself wrong: the red slot logged blue and the blue slot logged
    // red. Taking the argument away is what makes it impossible to start honouring it by
    // accident later.
    MPE_LOG_INFO("opening the invite list for this multiplayer session");

    // Hosting first, so the lobby is already being created while the list is drawn. It is a
    // no-op for anyone who is already in a session, which is what lets a guest invite a
    // third person into their host's game rather than being thrown out of it.
    EnsureSessionHosted();
    OpenSessionInvite();
    RefreshInvitePanelState();
}

/// Advertises what this session is, so the browser shows it as something recognisable.
///
/// Without this a hosted lobby appears in everyone's browser as a blank row: the browser
/// reads its name, mode and map from the lobby's own metadata, and metadata that was never
/// written reads back empty. Published whenever it changes, including the moment hosting
/// finishes, because the lobby does not exist to write to until then.
void PublishSessionDetails() {
    static std::string s_published;

    lobby::LobbyId lobby = 0;
    {
        std::lock_guard lock(g_state_mutex);
        if (!g_state || !g_state->manager) {
            return;
        }
        lobby = HostedLobbyLocked();
    }
    if (lobby == 0) {
        return;
    }

    const std::string name =
        g_lobby.server_name.empty() ? std::format("{}'s Server", SteamPlayerName())
                                    : g_lobby.server_name;
    const std::string summary =
        std::format("{}|{}|{}|{}", name, g_lobby.mode, g_lobby.scenario,
                    LobbyState::kMaxPlayers);
    if (summary == s_published) {
        return;
    }
    s_published = summary;

    (void)steam::SetLobbyData(lobby, "name", name.c_str());
    (void)steam::SetLobbyData(lobby, "mode", g_lobby.mode.c_str());
    (void)steam::SetLobbyData(lobby, "map", g_lobby.scenario.c_str());
    (void)steam::SetLobbyData(
        lobby, "capacity", std::to_string(LobbyState::kMaxPlayers).c_str());
    MPE_LOG_INFO("session advertised as '{}' ({} on {})", name, g_lobby.mode,
                g_lobby.scenario);
}

/// Keeps the status panel honest about the network, the session and the build.
///
/// Rewritten in place on a timer rather than rebuilt, and only while the lobby is actually
/// on screen, so it costs nothing when nobody is looking at it.
/// Puts whoever is actually in the session onto the team cards.
///
/// The names are the Steam persona names the lobby backend reads for every member, so a
/// slot shows the person in it as Steam knows them. Anything else, a numeric id or a
/// "Player 2", is a placeholder that says nothing about who you are playing with.
///
/// Only rewritten when the roster has actually changed, so a lobby nobody is joining costs
/// one comparison a tick rather than fifty widget writes.
void RefreshLobbyRoster() {
    static std::string s_shown;

    if (!g_lobby_ui_ready || !unreal::LobbyIsBuilt() || g_lobby_root == 0) {
        return;
    }

    std::vector<std::string> blue;
    std::vector<std::string> red;
    std::string              host_name;
    {
        std::lock_guard lock(g_state_mutex);
        if (!g_state || !g_state->manager) {
            return;
        }
        for (const lobby::PlayerSlot& player : g_state->manager->Snapshot().players) {
            std::vector<std::string>& side = (player.team == 0) ? blue : red;
            if (side.size() >= 5) {
                continue;
            }
            if (player.is_host) {
                host_name = player.display_name;
            }
            side.push_back(player.display_name);
        }
    }

    // An empty roster is a roster, and it has to be drawn.
    //
    // Leaving the cards alone when the session has no players sounds harmless and is not:
    // accepting an invitation leaves the session being hosted before joining the new one,
    // and skipping the empty moment in between left the old roster on screen. A player who
    // had just left his own session was still shown in it, marked Owner, while the mod was
    // connecting him to somebody else's.
    //
    // Before any session exists there is still one person to show, which is the player
    // themselves, so that is what an empty roster falls back to rather than five empty
    // slots on a screen they have just opened.
    if (blue.empty() && red.empty()) {
        host_name = SteamPlayerName();
        blue.push_back(host_name);
    }

    std::string signature = host_name;
    for (const std::vector<std::string>& side : {blue, red}) {
        for (const std::string& name : side) {
            signature += '\x1F';
            signature += name;
        }
        signature += '\x1E';
    }
    if (signature == s_shown) {
        return;
    }
    s_shown = signature;

    unreal::LobbyUIContext ui = g_lobby_ui;
    if (!unreal::BindLobbyMenu(g_live_menu, ui).ok()) {
        return;
    }
    (void)unreal::RunOnGameThread(
        [&]() { unreal::SetLobbyRoster(ui, blue, red, host_name); }, 5000);
    MPE_LOG_INFO("lobby roster: {} on blue, {} on red", blue.size(), red.size());
}

/// The mode as the lobby names it, rather than as the engine spells it on the wire.
[[nodiscard]] std::string DisplayMode(engine::GameMode mode) {
    switch (mode) {
        case engine::GameMode::CaptureTheFlag: return "CAPTURE THE FLAG";
        case engine::GameMode::TeamSlayer:     return "SLAYER";
        default:                               break;
    }
    // Anything without a lobby name falls back to the engine's, upper cased and with the
    // underscores opened out, which is still readable.
    std::string text(engine::ToString(mode));
    for (char& character : text) {
        character = (character == '_') ? ' '
                                       : static_cast<char>(std::toupper(
                                             static_cast<unsigned char>(character)));
    }
    return text;
}

/// The map as the lobby names it, from the scenario code the engine uses.
[[nodiscard]] std::string DisplayMap(std::string_view scenario) {
    for (const unreal::LobbyMap& map : unreal::kLobbyMaps) {
        if (scenario == map.scenario) {
            // The label carries the code in brackets, which the panel has no room for and
            // the player does not need twice.
            const std::string_view label{map.label};
            const std::size_t      bracket = label.find(" (");
            return std::string(bracket == std::string_view::npos ? label
                                                                 : label.substr(0, bracket));
        }
    }
    std::string text(scenario);
    for (char& character : text) {
        character = static_cast<char>(std::toupper(static_cast<unsigned char>(character)));
    }
    return text;
}

/// Keeps a client's screen following the host, and hides what a client cannot change.
///
/// Two things a guest needs and did not have. The mode and map are the host's to pick, so
/// the controls for picking them come off the screen entirely; and when the host does pick
/// something, the choice has to arrive here rather than the guest staring at whatever was
/// selected before they joined.
///
/// Driven from the session snapshot rather than from what this machine last clicked,
/// because for a client those are different things and only one of them is true.
void RefreshLobbyAuthority() {
    if (!g_lobby_ui_ready || !unreal::LobbyIsBuilt()) {
        return;
    }

    bool                      is_host    = true;
    bool                      in_session = false;
    std::string               mode;
    std::string               scenario;
    lobby::LobbyId            lobby = 0;
    unreal::LobbySettingsView settings;
    {
        std::lock_guard lock(g_state_mutex);
        if (!g_state || !g_state->manager) {
            return;
        }
        const lobby::LobbySnapshot& snapshot = g_state->manager->Snapshot();
        in_session = snapshot.phase != lobby::LobbyPhase::Idle;
        is_host    = !in_session || g_state->manager->IsHost();
        mode       = (snapshot.settings.mode == engine::GameMode::TeamSlayer)
                         ? "SLAYER"
                         : "CAPTURE THE FLAG";
        scenario   = snapshot.settings.scenario;
        lobby      = snapshot.lobby_id;

        settings.game_time_minutes = snapshot.settings.time_limit_seconds / 60;
        settings.friendly_fire     = snapshot.settings.friendly_fire;
        settings.respawn_seconds   = snapshot.settings.respawn_delay_seconds;
    }

    // The server name is lobby metadata rather than a match setting, because it names the
    // session rather than the rules. A guest reads the host's out of the lobby it joined;
    // a host is left alone, since the field is the one they are typing into.
    if (!is_host && lobby != 0) {
        if (const char* const advertised = steam::GetLobbyData(lobby, "name");
            advertised != nullptr) {
            settings.server_name = advertised;
        }
    }

    // Only when something changed, so this costs one comparison a tick while nobody is
    // touching anything.
    // The build id is part of the comparison, not decoration.
    //
    // A rebuilt lobby has entirely new widgets, and the old ones are what these values
    // describe. Comparing settings alone meant a screen rebuilt with the same mode and map
    // was left exactly as the new widgets happened to default to, which for a guest is the
    // host's controls, visible and useless.
    static bool          s_host = true;
    static std::string   s_mode;
    static std::string   s_scenario;
    static std::string   s_settings;
    static std::uint32_t s_build = 0;
    const std::uint32_t  build   = unreal::LobbyBuildId();
    const std::string    summary =
        std::format("{}|{}|{}|{}", settings.game_time_minutes,
                    settings.friendly_fire ? 1 : 0, settings.respawn_seconds,
                    settings.server_name);
    if (is_host == s_host && mode == s_mode && scenario == s_scenario && build == s_build &&
        summary == s_settings) {
        return;
    }
    s_host     = is_host;
    s_mode     = mode;
    s_scenario = scenario;
    s_settings = summary;
    s_build    = build;

    int chosen = 0;
    for (std::size_t index = 0; index < std::size(unreal::kLobbyMaps); ++index) {
        if (scenario == unreal::kLobbyMaps[index].scenario) {
            chosen = static_cast<int>(index);
        }
    }
    const bool slayer = (mode == "SLAYER");

    // A guest's own idea of the settings follows the host's, so leaving and hosting again
    // does not resurrect a choice somebody else made.
    if (!is_host) {
        g_lobby.mode     = mode;
        g_lobby.scenario = scenario;
    }

    unreal::LobbyUIContext ui = g_lobby_ui;
    if (!unreal::BindLobbyMenu(g_live_menu, ui).ok()) {
        return;
    }
    (void)unreal::RunOnGameThread(
        [&]() {
            unreal::SetLobbySettings(ui, settings);
            unreal::SetLobbyHostControls(ui, is_host);
            unreal::SetLobbyMode(ui, slayer);
            unreal::SetLobbyMap(ui, chosen);
        },
        5000);
    MPE_LOG_INFO("lobby authority: {}, mode {}, map {}, {}min, friendly fire {}, respawn {}s",
                is_host ? "host" : "guest", mode, scenario, settings.game_time_minutes,
                settings.friendly_fire ? "on" : "off", settings.respawn_seconds);
}

/// Proves, against real Steam, that a hosted session can be found by a search.
///
/// This is the one part of discovery that can be checked without a second person. If a
/// real search returns this machine's own lobby then the marker was published in a form
/// Steam's string filter matches, the filter was applied, and a lobby carrying it came
/// back; a friend's lobby travels the identical path with a different id. If it never
/// comes back, the browser will be empty for everybody, and the answer is here rather than
/// in a report from two people who could not find each other.
///
/// The result is logged once per session. Searching is what the browser does anyway.
void CheckOwnSessionIsFindable() {
    static bool s_asked     = false;
    static bool s_reported  = false;
    static auto s_asked_at  = std::chrono::steady_clock::time_point{};

    lobby::LobbyId lobby = 0;
    {
        std::lock_guard lock(g_state_mutex);
        lobby = HostedLobbyLocked();
    }
    if (lobby == 0) {
        // Not hosting. Reset so the next session is checked in its own right.
        s_asked    = false;
        s_reported = false;
        return;
    }
    if (s_reported) {
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    if (!s_asked) {
        s_asked    = true;
        s_asked_at = now;
        steam::RequestLobbyList();
        return;
    }

    // Steam answers asynchronously. A few seconds is long enough for a search that is
    // going to succeed, and short enough that a failure is reported while the player is
    // still on the screen it matters for.
    if (now - s_asked_at < std::chrono::seconds(6)) {
        return;
    }
    s_reported = true;

    // Reading the results is what records whether our own lobby was among them.
    (void)steam::BrowseLobbies();
    if (steam::LastSearchSawOwnLobby()) {
        MPE_LOG_INFO("discovery self test: a Steam search returned this session, so the "
                    "marker, the filter and the search all work; a friend's game will "
                    "appear the same way");
    } else {
        MPE_LOG_WARN("discovery self test: a Steam search did not return this session. The "
                    "server browser will be empty for everybody until that is fixed, and "
                    "it is not a problem with the other player");
    }
}

/// A short lived message for the notice panel, from anywhere.
///
/// The panel already carries a staged update and a session error, both of which the status
/// refresh works out for itself. This is for the things it cannot: a refusal that happened
/// because of something the player just pressed, which has no phase of its own and would
/// otherwise only reach the log.
///
/// Clears itself, because a message about a press that has passed should not outlive the
/// player's memory of pressing it.
struct TransientNotice {
    std::string                           title;
    std::string                           detail;
    std::chrono::steady_clock::time_point until{};
};
std::mutex      g_notice_mutex;
TransientNotice g_notice;

void ShowSessionNotice(std::string title, std::string detail) {
    std::lock_guard lock(g_notice_mutex);
    g_notice.title  = std::move(title);
    g_notice.detail = std::move(detail);
    g_notice.until  = std::chrono::steady_clock::now() + std::chrono::seconds(8);
}

/// Says that a launch this machine had already committed to did not happen.
///
/// The campaign call is posted rather than waited on, so its failure arrives after the
/// lobby has moved on and there is no Result left to return it through. Left unreported it
/// would be the worst possible outcome: a screen that says the match is loading and a game
/// that is doing nothing.
///
/// Recorded rather than acted on, and this is not fastidiousness. It runs on the game
/// thread, and LobbyManager is single threaded by construction: the tick thread is very
/// likely inside Tick on the same object at this moment. Touching the session from here
/// would be a data race on the state machine that owns the whole session, to report that a
/// match did not start. The tick picks it up on its next pass, which is within sixteen
/// milliseconds, on the thread that owns the lobby.
std::atomic<bool> g_launch_failed{false};
std::mutex        g_launch_failure_mutex;
std::string       g_launch_failure_reason;

void ReportLaunchFailure(std::string_view reason) {
    {
        std::lock_guard lock(g_launch_failure_mutex);
        g_launch_failure_reason = std::string(reason);
    }
    g_launch_failed.store(true, std::memory_order_release);
}

/// Acts on a failed launch, on the tick thread. Caller must hold g_state_mutex.
void HandleLaunchFailureLocked() {
    if (!g_launch_failed.exchange(false, std::memory_order_acq_rel)) {
        return;
    }
    std::string reason;
    {
        std::lock_guard lock(g_launch_failure_mutex);
        reason = g_launch_failure_reason;
    }
    ShowSessionNotice("MATCH DID NOT START", reason);
    if (g_state && g_state->manager) {
        // A clean departure, so the next attempt starts from a lobby rather than from
        // whatever half-launched state this left behind.
        g_state->manager->LeaveSession();
    }
}

void RefreshLobbyStatus() {
    static auto s_last = std::chrono::steady_clock::time_point{};

    // Gated on the status overlay, not on the lobby.
    //
    // The panel is on the main menu too now, so waiting for the lobby to be built would
    // leave it blank exactly where a player is most likely to be looking at it.
    if (!g_lobby_ui_ready || !unreal::StatusOverlayIsBuilt()) {
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (now - s_last < std::chrono::milliseconds(500)) {
        return;
    }
    s_last = now;

    unreal::LobbyStatus status;
    status.online = steam::IsInitialized() && steam::IsLoggedOn();
    /// Why the session failed, when it has. Read under the lock, shown outside it.
    std::string session_error;
    bool        faulted = false;

    {
        std::lock_guard lock(g_state_mutex);
        if (g_state && g_state->manager) {
            const lobby::LobbySnapshot& snapshot = g_state->manager->Snapshot();

            // Every phase named, with nothing falling through to a word that covers
            // several of them.
            //
            // There are twelve phases and this used to name four, so joining somebody
            // else's session read as BUSY from the moment the Steam lobby was entered
            // until the match started. A player watching that cannot tell a handshake in
            // progress from one that has stopped, and neither could anybody reading their
            // report of it: the first two player test came back as "it says busy", which
            // is the screen's fault rather than the session's.
            switch (g_state->manager->Phase()) {
                case lobby::LobbyPhase::Idle:
                    status.session = "OFFLINE";
                    break;
                case lobby::LobbyPhase::Creating:
                    status.session = "STARTING";
                    break;
                case lobby::LobbyPhase::Hosting:
                    status.session = std::format("OPEN  {}/{}", snapshot.players.size(),
                                                 LobbyState::kMaxPlayers);
                    status.invitable = status.online;
                    break;
                case lobby::LobbyPhase::Joining:
                    status.session = "JOINING LOBBY";
                    break;
                case lobby::LobbyPhase::Connecting:
                    status.session = "CONNECTING TO HOST";
                    break;
                case lobby::LobbyPhase::Handshaking:
                    status.session = "HANDSHAKING";
                    break;
                case lobby::LobbyPhase::InLobby:
                    status.session = std::format("JOINED  {}/{}", snapshot.players.size(),
                                                 LobbyState::kMaxPlayers);
                    break;
                case lobby::LobbyPhase::Countdown:
                    // The number, not just the word. Every peer is counting the same one
                    // down from the host's broadcast, so seeing it is what makes a
                    // synchronised launch feel synchronised rather than sudden.
                    status.session = snapshot.countdown_seconds > 0
                                         ? std::format("STARTING IN {}",
                                                       snapshot.countdown_seconds)
                                         : std::string{"STARTING MATCH"};
                    break;
                case lobby::LobbyPhase::Loading:
                    status.session = "LOADING";
                    break;
                case lobby::LobbyPhase::InMatch:
                    status.session = "IN MATCH";
                    break;
                case lobby::LobbyPhase::PostMatch:
                    status.session = "MATCH OVER";
                    break;
                case lobby::LobbyPhase::Faulted:
                    // The word only. The reason is a sentence and goes to the notice
                    // panel, which has the width to show all of it.
                    status.session = "ERROR";
                    session_error  = snapshot.last_error;
                    faulted        = true;
                    break;
            }

            // Who else is here, how far away they are, and what is being played.
            //
            // The ping shown is the round trip to the other side of the session, which is
            // the only one a player can do anything about. A host measures each client
            // once a second and reports the worst, because a lobby is only as good as its
            // furthest member; a client is told its own by the host's roster, which is the
            // same round trip seen from the other end.
            //
            // Left at minus one when this machine is alone, so the panel shows nothing
            // rather than a made up zero.
            for (const lobby::PlayerSlot& player : snapshot.players) {
                if (player.is_host && !player.is_local) {
                    status.host_name = player.display_name;
                }
                const bool remote = !player.is_local;
                if (remote && player.ping_milliseconds > 0) {
                    status.ping_ms = std::max(status.ping_ms,
                                              static_cast<int>(player.ping_milliseconds));
                }
                if (player.is_local && !snapshot.is_host && player.ping_milliseconds > 0) {
                    status.ping_ms = std::max(status.ping_ms,
                                              static_cast<int>(player.ping_milliseconds));
                }
            }
            if (snapshot.players.size() < 2) {
                status.ping_ms = -1;
            }

            // Shown the way the lobby names them, not the way the engine does.
            //
            // The engine's own vocabulary is what goes on the wire and into lobby data:
            // capture_the_flag, a30. Correct there and wrong on a panel a player reads,
            // where it looks like a debug string that escaped.
            // Only while there is a session. On the main menu the manager is idle and its
            // settings are whatever it was constructed with, so reporting them there
            // described a match that did not exist.
            if (g_state->manager->Phase() != lobby::LobbyPhase::Idle) {
                status.mode = DisplayMode(snapshot.settings.mode);
                status.map  = DisplayMap(snapshot.settings.scenario);
            }

            // A phase that is going nowhere says so.
            //
            // Connecting and handshaking are supposed to be brief. Left as they are, one
            // that never completes looks exactly like one that is about to, and the player
            // waits indefinitely on a screen that reads as normal.
            static lobby::LobbyPhase                     s_phase = lobby::LobbyPhase::Idle;
            static std::chrono::steady_clock::time_point s_since{};
            const lobby::LobbyPhase current = g_state->manager->Phase();
            if (current != s_phase) {
                s_phase = current;
                s_since = now;
            }
            const bool transient = current == lobby::LobbyPhase::Joining ||
                                   current == lobby::LobbyPhase::Connecting ||
                                   current == lobby::LobbyPhase::Handshaking;
            if (transient && now - s_since > std::chrono::seconds(20)) {
                status.session += "  STALLED";
            }
        } else {
            status.session = "UNAVAILABLE";
        }
    }
    if (!status.online) {
        status.session   = "OFFLINE";
        status.invitable = false;
    }

    // A dead session recovers on its own, with the player watching it happen.
    //
    // When the host leaves, the client faults and the manager stays faulted forever: the
    // error stayed on screen and there was no session left to be in, so the lobby was a
    // screen showing a failure with nothing to do about it but press BACK.
    //
    // The failure is worth reading, so it is left up for a few seconds and counted down
    // rather than cleared instantly, and then this machine hosts its own session again.
    // That is the state a player expects to land in: their own empty lobby, ready to
    // invite somebody, exactly as if they had just opened the screen.
    constexpr auto kFaultLinger = std::chrono::seconds(10);
    static std::chrono::steady_clock::time_point s_faulted_at{};
    bool                                         recover_now = false;
    if (faulted) {
        if (s_faulted_at == std::chrono::steady_clock::time_point{}) {
            s_faulted_at = now;
        }
        // The timing lives in Lobby/Discovery, with tests, because waiting ten seconds is
        // an expensive way to find out that a countdown shows nine or reaches zero and
        // sits there.
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 now - s_faulted_at)
                                 .count();
        const lobby::FaultRecovery recovery = lobby::JudgeFault(
            elapsed,
            std::chrono::duration_cast<std::chrono::milliseconds>(kFaultLinger).count());
        if (recovery.recover_now) {
            recover_now = true;
        } else {
            session_error += std::format("  Starting your own session in {}s.",
                                         recovery.seconds_remaining);
        }
    } else {
        s_faulted_at = {};
    }

    std::string latest;
    int         progress = -1;
    bool        staged   = false;
    {
        std::lock_guard lock(g_version_mutex);
        latest   = g_latest_version;
        progress = g_update_progress;
        staged   = g_update_ready;
    }
    const bool behind = !latest.empty() && update::IsNewer(latest, kModVersion);
    status.update_available = behind;
    status.restart_required = staged;
    status.staged_version   = staged ? latest : std::string{};

    // The notice panel carries whichever of these matters more.
    //
    // A session error wins over a staged update: one explains why the thing the player is
    // trying to do right now did not work, and the other can wait until they next look at
    // the screen.
    std::string flash_title;
    std::string flash_detail;
    {
        std::lock_guard lock(g_notice_mutex);
        if (!g_notice.title.empty()) {
            if (now < g_notice.until) {
                flash_title  = g_notice.title;
                flash_detail = g_notice.detail;
            } else {
                g_notice = TransientNotice{};
            }
        }
    }

    if (!session_error.empty()) {
        status.notice_title  = "SESSION ERROR";
        status.notice_detail = session_error;
    } else if (!flash_title.empty()) {
        // Above a staged update: this is about something the player did a moment ago, and
        // an update can wait until they are not being told why a press did nothing.
        status.notice_title  = flash_title;
        status.notice_detail = flash_detail;
    } else if (staged) {
        status.notice_title  = latest.empty()
                                   ? std::string{"UPDATE INSTALLED"}
                                   : std::format("UPDATE {} INSTALLED", latest);
        status.notice_detail = "Quit and relaunch the game to start using it.";
    }

    if (staged) {
        status.version = std::format("v{}  UPDATE {} INSTALLED", kModVersion, latest);
    } else if (progress >= 0) {
        status.version = std::format("v{}  DOWNLOADING {}%", kModVersion, progress);
    } else if (behind) {
        status.version = std::format("v{}  UPDATE {} FOUND", kModVersion, latest);
    } else {
        status.version = std::format("v{}  UP TO DATE", kModVersion);
    }

    unreal::LobbyUIContext ui = g_lobby_ui;
    if (!unreal::BindLobbyMenu(g_live_menu, ui).ok()) {
        return;
    }
    (void)unreal::RunOnGameThread([&]() { unreal::SetLobbyStatus(ui, status); }, 5000);

    // Done last, and outside the state lock, because both of these take it themselves.
    // The status above is written first so the final second of the countdown is drawn
    // before the session it describes is replaced.
    if (recover_now) {
        s_faulted_at = {};
        MPE_LOG_INFO("the session failed; leaving it and hosting a new one");
        {
            std::lock_guard lock(g_state_mutex);
            if (g_state && g_state->manager) {
                g_state->manager->LeaveSession();
            }
        }
        EnsureSessionHosted();
    }
}

/// Applies the current filter to the live listing and rewrites the table.
///
/// Nothing is rebuilt: the rows exist already and only their text and visibility change,
/// so filtering stays as immediate as everything else on the screen.
void ApplyServerFilter() {
    if (!g_lobby_ui_ready) {
        return;
    }
    unreal::LobbyUIContext ui = g_lobby_ui;
    if (!unreal::BindLobbyMenu(g_live_menu, ui).ok()) {
        return;
    }

    std::vector<unreal::ServerEntry> matching;
    for (const unreal::ServerEntry& entry : DiscoveredServers()) {
        if (g_server_filter.Accepts(entry)) {
            matching.push_back(entry);
        }
    }
    if (g_selected_server >= static_cast<int>(matching.size())) {
        g_selected_server = matching.empty() ? 0 : static_cast<int>(matching.size()) - 1;
    }

    const int selected = g_selected_server;
    (void)unreal::RunOnGameThread(
        [&]() {
            unreal::SetLobbyServers(ui, matching, selected);
            unreal::SetLobbyFilters(ui, g_server_filter);
        },
        5000);
    MPE_LOG_INFO("server filter applied: {} of {} server(s) match (mode '{}', slots {}, "
                "ping {})",
                matching.size(), DiscoveredServers().size(),
                g_server_filter.mode.empty() ? "any" : g_server_filter.mode,
                g_server_filter.slots, g_server_filter.max_ping);
}

/// Marks the chosen mode on screen without rebuilding.
/// Pushes the lobby screen's mode and map into the live session.
///
/// Without this the two drifted apart the moment a session existed. The screen tracked what
/// the host had picked and the session kept whatever it was created with, so the status
/// panel reported the original choice forever and a client was never told about a change at
/// all: nothing was broadcast because nothing had changed as far as the session knew.
///
/// Host only, and quiet on a client, because a client has nothing to publish.
void PublishSelectedMatchSettings() {
    std::lock_guard lock(g_state_mutex);
    if (!g_state || !g_state->manager || !g_state->manager->IsHost()) {
        return;
    }
    engine::MatchSettings settings = g_state->manager->Snapshot().settings;
    settings.mode     = (g_lobby.mode == "SLAYER") ? engine::GameMode::TeamSlayer
                                                   : engine::GameMode::CaptureTheFlag;
    settings.scenario     = g_lobby.scenario;
    settings.variant_name = g_lobby.scenario;
    settings.team_count   = g_lobby.teams ? 2 : 1;
    // Capture the flag has no meaning without two sides to carry it between.
    if (settings.mode == engine::GameMode::CaptureTheFlag && settings.team_count < 2) {
        settings.team_count = 2;
    }
    settings.friendly_fire = g_lobby.friendly_fire;

    if (const Result updated = g_state->manager->UpdateMatchSettings(settings); !updated.ok()) {
        MPE_LOG_WARN("the session did not accept the new settings: {}", updated.message());
    }
}

void SelectLobbyMode(bool slayer) {
    MPE_LOG_INFO("mode is now {}", g_lobby.mode);
    if (!g_lobby_ui_ready) {
        return;
    }
    unreal::LobbyUIContext ui = g_lobby_ui;
    if (!unreal::BindLobbyMenu(g_live_menu, ui).ok()) {
        return;
    }
    (void)unreal::RunOnGameThread([&]() { unreal::SetLobbyMode(ui, slayer); }, 5000);

    // Told to the session as well as the screen, so everybody else sees it.
    PublishSelectedMatchSettings();
}

/// Chooses the scenario a match will be played on, and marks it on screen.
void SelectLobbyMap(int map_index) {
    if (map_index < 0 || map_index >= static_cast<int>(std::size(unreal::kLobbyMaps))) {
        return;
    }
    g_lobby.scenario = unreal::kLobbyMaps[map_index].scenario;
    MPE_LOG_INFO("map is now {}", g_lobby.scenario);

    if (!g_lobby_ui_ready) {
        return;
    }
    unreal::LobbyUIContext ui = g_lobby_ui;
    if (!unreal::BindLobbyMenu(g_live_menu, ui).ok()) {
        return;
    }
    (void)unreal::RunOnGameThread([&]() { unreal::SetLobbyMap(ui, map_index); }, 5000);
    PublishSelectedMatchSettings();
}

/// Builds the whole lobby ahead of time and leaves it hidden.
///
/// This is what makes opening it instant. Building the screen creates around a hundred
/// widgets and duplicates a widget tree for every one of the frontend's buttons, and doing
/// that when the player presses MULTIPLAYER is work they sit and wait through. Done here it
/// happens while they are still looking at the main menu, and pressing the entry then costs
/// one visibility change.
/// Puts the status panel on screen, once, as soon as there is a menu to host it against.
///
/// Separate from the lobby because it outlives it: the panel reports on the mod rather than
/// on whichever screen is showing, and a player on the main menu wants to know they are
/// signed in and whether the build is current without opening anything.
void PrepareStatusOverlay() {
    if (!g_lobby_ui_ready || g_live_menu == 0) {
        return;
    }

    // Built once, but only while it still exists.
    //
    // The widgets are created with the menu as their outer, so a menu the engine collects
    // takes them with it. The handles stay non zero and stop being addresses of anything,
    // which shows up as a panel that silently stops updating while the mod is certain it
    // is on screen. Checking the object is still in the array is what turns that into a
    // rebuild.
    if (unreal::StatusOverlayIsBuilt()) {
        std::lock_guard lock(g_state_mutex);
        if (!g_state || !g_state->objects.has_value()) {
            return;
        }
        if (g_state->objects->ClassOf(unreal::StatusOverlayWidget()) != 0) {
            return; // Still there.
        }
        MPE_LOG_INFO("the status overlay was collected with its menu; building it again");
        unreal::ForgetStatusOverlay();
    }
    unreal::LobbyUIContext ui = g_lobby_ui;
    if (!unreal::BindLobbyMenu(g_live_menu, ui).ok()) {
        return;
    }
    Result outcome = Result::Success();
    (void)unreal::RunOnGameThread([&]() { outcome = unreal::BuildStatusOverlay(ui); }, 10000);
    if (!outcome.ok()) {
        static bool s_complained = false;
        if (!s_complained) {
            s_complained = true;
            MPE_LOG_WARN("the status overlay could not be built: {}", outcome.message());
        }
    }
}

void PrepareLobby() {
    if (!g_lobby_ui_ready || g_live_menu == 0 || unreal::LobbyIsBuilt()) {
        return;
    }
    unreal::LobbyUIContext ui = g_lobby_ui;
    if (!unreal::BindLobbyMenu(g_live_menu, ui).ok()) {
        return;
    }

    if (g_lobby.players.empty()) {
        g_lobby.AddPlayer(SteamPlayerName(), true);
    }

    unreal::LobbyView view;
    view.mode              = g_lobby.mode;
    view.map               = g_lobby.scenario;
    view.friendly_fire     = g_lobby.friendly_fire;
    view.game_time_minutes = g_lobby.game_time_minutes;
    view.respawn_seconds   = g_lobby.respawn_seconds;
    view.server_name       = g_lobby.server_name;
    for (const LobbyPlayer& player : g_lobby.players) {
        if (player.host) {
            view.host_name = player.name;
        }
        (player.team == 0 ? view.red : view.blue).push_back(player.name);
    }
    view.servers = DiscoveredServers();

    unreal::ForgetExtraWatchedWidgets();
    std::uintptr_t                    root = 0;
    std::vector<unreal::LobbyControl> controls;
    Result                            outcome = Result::Success();
    const Result                      ran     = unreal::RunOnGameThread(
        [&]() {
            outcome = unreal::BuildLobbyUI(ui, view, root, controls);
            if (outcome.ok()) {
                unreal::ShowLobbyUI(ui, false);
            }
        },
        20000);

    if (!ran.ok() || !outcome.ok()) {
        MPE_LOG_WARN("the lobby could not be prepared: {}",
                    ran.ok() ? outcome.message() : ran.message());
        return;
    }

    g_lobby_root     = root;
    g_lobby_controls = controls;
    int watched = 0;
    for (const unreal::LobbyControl& control : g_lobby_controls) {
        if (control.widget != 0 && unreal::AlsoWatchWidget(control.widget).ok()) {
            ++watched;
        }
    }
    MPE_LOG_INFO("lobby prepared and hidden; {} of {} button(s) live, opening is now a "
                "visibility change",
                watched, g_lobby_controls.size());
}

/// Takes the lobby down and gives the frontend back.
void OnLeaveLobby() {
    // The session goes too, not just the screen.
    //
    // Hiding the lobby used to be all this did, which left whoever pressed BACK still in
    // the session they were in. A guest who backed out was still occupying a slot in
    // somebody else's lobby, still on their roster, and had no way to tell: their own
    // screen was gone. A host who backed out left a session advertised that they were no
    // longer watching.
    //
    // Leaving is what BACK means from a multiplayer screen. Opening it again hosts a fresh
    // session, which is the state a player expects to return to.
    {
        std::lock_guard lock(g_state_mutex);
        if (g_state && g_state->manager &&
            g_state->manager->Phase() != lobby::LobbyPhase::Idle) {
            MPE_LOG_INFO("leaving the session on the way out of the lobby");
            g_state->manager->LeaveSession();
        }
    }

    // The buttons are kept watched and the controls kept mapped: the lobby is hidden, not
    // destroyed, so its widgets are the same ones when it is shown again. Dropping them
    // here would make every button dead the second time the screen was opened.
    unreal::LobbyUIContext ui = g_lobby_ui;
    if (!g_lobby_ui_ready || !unreal::BindLobbyMenu(g_live_menu, ui).ok()) {
        return;
    }
    (void)unreal::RunOnGameThread([&]() { unreal::ShowLobbyUI(ui, false); }, 10000);
    MPE_LOG_INFO("multiplayer lobby hidden");
}

/// Asks Steam what is out there and reports what came back.
///
/// Nothing is invented here. An empty list is shown empty, because a browser that lists
/// servers which do not exist is worse than one that lists none.
void RefreshServerList() {
    const std::vector<unreal::ServerEntry> found = DiscoveredServers();
    MPE_LOG_INFO("server browser refreshed: {} server(s) found", found.size());
}

/// Starts the match the lobby is configured for.
///
/// The lobby comes down first. Leaving it up means the new level loads behind a menu that
/// is still holding input focus, which looks exactly like a hang.
void OnStartMatch() {
    CaptureServerName();
    PublishSelectedMatchSettings();

    MPE_LOG_INFO("starting a {} match on {} (friendly fire {})", g_lobby.mode,
                g_lobby.scenario, g_lobby.friendly_fire ? "on" : "off");

    // Everybody, or nobody.
    //
    // This used to leave the lobby and begin the campaign here, which started a match on
    // this machine and told no one: a client watched the host disappear and stayed sitting
    // in a lobby that no longer had anyone in it.
    //
    // The lobby owns the sequence instead. It counts down out loud so every peer sees the
    // same number, broadcasts the launch with the scenario and a shared seed, waits for
    // every peer to report it has loaded, and only then releases them. This machine begins
    // its own campaign through exactly the same path as everyone else, when the lobby says
    // to, not before.
    Result started = Result::Success();
    {
        std::lock_guard lock(g_state_mutex);
        if (!g_state || !g_state->manager) {
            MPE_LOG_WARN("cannot start a match: networking is unavailable");
            return;
        }
        started = g_state->manager->StartCountdown();
    }

    if (!started.ok()) {
        // Said on screen rather than only to the log. The usual reason is being alone, and
        // a player who presses START in an empty lobby deserves to be told that rather
        // than watching nothing happen.
        MPE_LOG_WARN("the match did not start: {}", started.message());
        ShowSessionNotice("CANNOT START", std::string(started.message()));
    }
}

/// Joins the server selected in the browser.
void OnJoinMatch() {
    // The listings, not the filtered view, because the row that was chosen was chosen out
    // of the filtered list and has to be translated back to a Steam lobby id.
    const std::vector<steam::LobbyListing> listings = steam::BrowseLobbies();

    std::vector<steam::LobbyListing> visible;
    for (const steam::LobbyListing& listing : listings) {
        unreal::ServerEntry entry;
        entry.name     = listing.name;
        entry.mode     = listing.mode;
        entry.map      = listing.map;
        entry.players  = listing.members;
        entry.capacity = listing.capacity > 0 ? listing.capacity : 10;
        entry.ping     = listing.ping_milliseconds;
        if (g_server_filter.Accepts(entry)) {
            visible.push_back(listing);
        }
    }

    if (visible.empty()) {
        MPE_LOG_WARN("nothing to join: the browser is showing no servers");
        return;
    }

    const std::size_t row = (g_selected_server >= 0 &&
                             static_cast<std::size_t>(g_selected_server) < visible.size())
                                ? static_cast<std::size_t>(g_selected_server)
                                : 0;
    const steam::LobbyListing& target = visible[row];

    MPE_LOG_INFO("joining '{}' ({} on {}), lobby {}, {}/{} players, {} ms", target.name,
                target.mode, target.map, target.id, target.members, target.capacity,
                target.ping_milliseconds);

    // Leaving and joining under one lock, not three.
    //
    // Taking it separately for the check, the leave and the join leaves gaps another
    // thread can change the session in, and the worst of those lands between leaving this
    // machine's own session and entering somebody else's, which is the moment there is no
    // session at all.
    Result joined = Result::Success();
    {
        std::lock_guard lock(g_state_mutex);
        if (!g_state || !g_state->manager) {
            MPE_LOG_WARN("cannot join {}: networking is unavailable", target.name);
            return;
        }

        // The session this player is sitting in is left first, automatically.
        //
        // Opening the multiplayer screen hosts a session, so by the time anybody reaches
        // the browser they are always hosting one. Refusing to join while hosting
        // therefore refused every join there is, and told the player to leave a session
        // they never knowingly started. Leaving it here is what accepting an invitation
        // already does.
        if (g_state->manager->Phase() != lobby::LobbyPhase::Idle) {
            MPE_LOG_INFO("leaving this machine's own session first");
            g_state->manager->LeaveSession();
        }
        joined = g_state->manager->JoinSession(target.id);
    }
    if (!joined.ok()) {
        MPE_LOG_ERROR("could not join lobby {}: {}", target.id, joined.message());
        return;
    }

    // The lobby screen stays up. Joining is asynchronous, and the phase line in the status
    // panel is what reports how it is going; closing the screen here would leave the player
    // watching the main menu with no idea whether anything was happening.
    g_screen = MultiplayerScreen::Home;
    SwitchLobbyTab(false);
}

void MaintainMainMenuButton() {
    // The menu instance last decorated. A different address means a freshly built menu,
    // which needs the entry again.
    static std::uintptr_t s_decorated_menu = 0;
    static auto           s_last_check     = std::chrono::steady_clock::time_point{};

    // Once a second, and not more often, however tempting it is.
    //
    // Establishing that the menu is not there yet costs a pass over the whole object
    // array, resolving a name for every object in it. Polling that four times a second to
    // shave a few hundred milliseconds off when the entry appears was measured against
    // this game and was catastrophic: the process was already saturating four cores
    // compiling shaders, and adding four full scans a second on top pushed the time to
    // reach the main menu from around twenty five seconds to over five minutes.
    //
    // The entry cannot appear before the menu does, and the menu cannot appear until the
    // game has finished the work this would have been stealing time from. A second of
    // latency after that is invisible; making the game load slower to remove it is not.
    const auto now = std::chrono::steady_clock::now();
    if (now - s_last_check < std::chrono::seconds(1)) {
        return;
    }
    s_last_check = now;

    // Work out how to call functions before the menu turns up, not after.
    //
    // Detection needs a reasonably populated graph: with only a few thousand objects there
    // are too few distinct classes to tell an inherited virtual from an overridden one, and
    // guessing there would mean calling the wrong function. So it is attempted each tick
    // once the graph is substantial, and it caches on the first success, which puts it in
    // place well before the menu exists.
    {
        std::lock_guard lock(g_state_mutex);
        if (!g_state || !g_state->objects.has_value()) {
            return;
        }
        // A high bar deliberately: detection compares virtual tables across unrelated
        // classes, and a thin graph does not contain enough of them to be conclusive.
        if (g_state->objects->Count() > 45000) {
            unreal::CallLayout layout;
            (void)unreal::DetectCallLayout(*g_state->objects, layout);

            // Prepared before the menu exists, not when it appears.
            //
            // Doing this when the menu turned up meant the menu was drawn and then sat
            // there without a MULTIPLAYER entry until the work finished, and a press during
            // that window did nothing. None of it depends on the menu, so it happens as
            // soon as the object graph is populated, which is well before the frontend is
            // shown. A failure is not fatal here: the entry is still added, and pressing it
            // reports why rather than silently doing nothing.
            if (!g_lobby_ui_ready) {
                if (const Result statics =
                        unreal::ResolveLobbyStatics(*g_state->objects, g_lobby_ui);
                    statics.ok()) {
                    // Where the text field keeps its font and colour, read from the class
                    // rather than assumed. FEditableTextStyle derives from a polymorphic
                    // base, so its first property sits one pointer past the start of the
                    // style; anything not found stays zero and is simply not written.
                    if (g_state->reflection.has_value() && g_lobby_ui.editable_class != 0) {
                        const auto style = g_state->reflection->FindProperty(
                            g_lobby_ui.editable_class, "WidgetStyle");
                        if (style.ok()) {
                            const std::uintptr_t base =
                                static_cast<std::uintptr_t>(style.value().offset);
                            g_lobby_ui.editable_font_offset = base + sizeof(void*);
                            g_lobby_ui.editable_colour_offset =
                                g_lobby_ui.editable_font_offset + 0x48 + 0x8;
                            MPE_LOG_INFO("editable text style at +0x{:X}: font +0x{:X}, "
                                        "colour +0x{:X}",
                                        base, g_lobby_ui.editable_font_offset,
                                        g_lobby_ui.editable_colour_offset);
                        } else {
                            MPE_LOG_WARN("the text field's style was not found; it will keep "
                                        "its default appearance");
                        }
                        const auto hint = g_state->reflection->FindProperty(
                            g_lobby_ui.editable_class, "HintText");
                        if (hint.ok()) {
                            g_lobby_ui.editable_hint_offset =
                                static_cast<std::uintptr_t>(hint.value().offset);
                        }
                    }
                    g_lobby_ui_ready = true;
                    MPE_LOG_INFO("lobby handles cached; opening the lobby is now instant");
                } else {
                    static bool s_complained = false;
                    if (!s_complained) {
                        s_complained = true;
                        MPE_LOG_WARN("the lobby cannot be prepared: {}", statics.message());
                    }
                }
            }
        }
    }

    // Confirm the known menu is still there with a single read, not a scan.
    //
    // Scanning the whole object array on a short timer kept the process permanently busy,
    // which had a consequence well beyond wasted cycles: the startup step that waits for
    // the game to go idle never saw it go idle, so the reflection layout detection that
    // step gates never ran, and every property offset stayed at its default guess. A
    // background task that is merely expensive can break something that looks unrelated.
    if (s_decorated_menu != 0) {
        std::uintptr_t still_there = 0;
        std::lock_guard lock(g_state_mutex);
        if (g_state && g_state->objects.has_value()) {
            still_there = g_state->objects->ClassOf(s_decorated_menu);
        }
        if (still_there != 0) {
            return; // Same menu, nothing to do, nothing scanned.
        }
        s_decorated_menu = 0;
        // The lobby must not attach to a menu that has gone away. It builds perfectly
        // against a dead one and draws nothing, which looks exactly like a broken button.
        g_live_menu = 0;
        // The widgets beside the old menu went with it. Cleared so the next menu collects
        // its own, rather than folding away addresses that no longer belong to anything.
        g_lobby_ui.also_fold.clear();
    }

    // Finding the menu is a plain memory scan, so the cheap check happens every time and
    // the expensive part, which needs the game thread, only runs when there is work.
    std::uintptr_t menu = 0;
    {
        std::lock_guard lock(g_state_mutex);
        if (!g_state || !g_state->objects.has_value()) {
            return;
        }

        // Nothing new registered means nothing new to find.
        //
        // Establishing that the menu does not exist yet costs a pass over the whole object
        // array, and the game can spend minutes compiling shaders before the frontend is
        // built. That was a hundred and eighty full scans during the slowest part of the
        // game's startup, each one taking the process address space lock repeatedly, to
        // learn nothing each time.
        //
        // The count is a single read. A menu cannot appear without the count going up, so
        // an unchanged count means the scan can be skipped. Not skipped forever, because
        // an object destroyed and another created between two polls leaves the count where
        // it was, so every fifth poll looks anyway and the worst case is five seconds.
        static std::uint32_t s_last_count   = 0;
        static int           s_skipped_scans = 0;
        const std::uint32_t  count          = g_state->objects->Count();
        if (count == s_last_count && s_skipped_scans < 4) {
            ++s_skipped_scans;
            return;
        }
        s_skipped_scans = 0;
        s_last_count    = count;
        // One pass that resolves a name only when it can possibly matter.
        //
        // Both the menu and the panels beside it are collected here on purpose.
        // Deferring the panels to a later poll so this could stop early looked like a
        // clean saving and was not: the function returns early once the menu is
        // decorated, so the later poll never came, the fold list stayed empty, and the
        // fireteam panel sat drawn over the lobby.
        //
        // What made the pass expensive was never the walking. Every object had two
        // FNames turned into strings so that a handful of names could be compared
        // against them, which is a hundred thousand lookups and allocations for fifty
        // thousand objects.
        //
        // There are only a few hundred distinct classes among those objects, so each
        // class is judged once by name and the verdict kept against its FName index.
        // After that an object costs one integer hash lookup, and its own name is
        // resolved only when its class is one of the few that can hold something
        // wanted. The judging still uses real resolved names rather than a name pool
        // search, because a bounded search silently finds nothing here and once
        // removed the menu entry entirely.
        enum class Kind : std::uint8_t {
            Ignore,
            Menu,
            FoldsWithMenu,
            Function,
            ClassObject,
            WidgetLibrary,
            TextLibrary,
            Controller,
        };
        static std::unordered_map<std::uint32_t, Kind> s_class_kind;

        // Kept across polls, so a piece found while the game was still loading is not
        // looked for again once the menu appears.
        static unreal::MenuButtonPlan s_pieces;

        std::vector<std::uintptr_t> beside;
        const unreal::NamePool&     names = *g_state->names;

        g_state->objects->ForEachRaw([&](const unreal::ObjectArray::RawObject& object) {
            Kind       kind  = Kind::Ignore;
            const auto known = s_class_kind.find(object.class_name_index);
            if (known != s_class_kind.end()) {
                kind = known->second;
            } else {
                const Expected<std::string> cls = names.Resolve(object.class_name_index);
                if (cls.ok()) {
                    const std::string& text = cls.value();
                    if (text == "WBP_MainMenu_C") {
                        kind = Kind::Menu;
                    } else if (text.rfind("WBP_Squad", 0) == 0 ||
                               text == "WBP_MeteoriteUILayout_C" ||
                               text.rfind("WBP_MeteoriteBoundActionBar", 0) == 0) {
                        kind = Kind::FoldsWithMenu;
                    } else if (text == "Function") {
                        kind = Kind::Function;
                    } else if (text == "WidgetBlueprintLibrary") {
                        kind = Kind::WidgetLibrary;
                    } else if (text == "KismetTextLibrary") {
                        kind = Kind::TextLibrary;
                    } else if (text.find("PlayerController") != std::string::npos &&
                               text.find("Component") == std::string::npos) {
                        kind = Kind::Controller;
                    } else if (text.find("Class") != std::string::npos) {
                        kind = Kind::ClassObject;
                    }
                }
                s_class_kind.emplace(object.class_name_index, kind);
            }

            if (kind == Kind::Ignore) {
                return true;
            }

            // Functions and blueprint classes are numerous, so they stop being examined
            // the moment what was wanted among them has been found.
            switch (kind) {
                case Kind::Function:
                    if (s_pieces.create_function != 0 && s_pieces.add_child_function != 0 &&
                        s_pieces.remove_child_function != 0 &&
                        s_pieces.convert_function != 0) {
                        return true;
                    }
                    break;
                case Kind::ClassObject:
                    if (s_pieces.button_class != 0) {
                        return true;
                    }
                    break;
                case Kind::WidgetLibrary:
                    if (s_pieces.widget_library != 0) {
                        return true;
                    }
                    break;
                case Kind::TextLibrary:
                    if (s_pieces.text_library != 0) {
                        return true;
                    }
                    break;
                case Kind::Controller:
                    if (s_pieces.controller != 0) {
                        return true;
                    }
                    break;
                default:
                    break;
            }

            const Expected<std::string> own = names.Resolve(object.name_index);
            if (!own.ok()) {
                return true;
            }
            const std::string& text       = own.value();
            const bool         is_default = text.rfind("Default__", 0) == 0;

            switch (kind) {
                case Kind::Menu:
                    if (!is_default) {
                        menu = object.address;
                    }
                    break;
                case Kind::FoldsWithMenu:
                    // The frontend's layout owns the fireteam area and the action bar,
                    // and the squad rows are their own widgets, so each is folded in its
                    // own right. Collapsing the menu alone left them drawn and audible.
                    if (!is_default) {
                        beside.push_back(object.address);
                    }
                    break;
                case Kind::Function:
                    if (text == "Create") {
                        s_pieces.create_function = object.address;
                    } else if (text == "AddChild") {
                        s_pieces.add_child_function = object.address;
                    } else if (text == "RemoveChild") {
                        s_pieces.remove_child_function = object.address;
                    } else if (text == "Conv_StringToText") {
                        s_pieces.convert_function = object.address;
                    }
                    break;
                case Kind::ClassObject:
                    if (text == "WBP_MeteoriteStandaloneButtonDefault_C") {
                        s_pieces.button_class = object.address;
                    }
                    break;
                case Kind::WidgetLibrary:
                    if (text == "Default__WidgetBlueprintLibrary") {
                        s_pieces.widget_library = object.address;
                    }
                    break;
                case Kind::TextLibrary:
                    if (text == "Default__KismetTextLibrary") {
                        s_pieces.text_library = object.address;
                    }
                    break;
                case Kind::Controller:
                    if (!is_default && text.find("_GEN_VARIABLE") == std::string::npos) {
                        s_pieces.controller = object.address;
                    }
                    break;
                case Kind::Ignore:
                    break;
            }
            return true;
        });

        const unreal::MenuButtonPlan pieces = s_pieces;
        g_lobby_ui.also_fold = beside;
        unreal::SeedMenuButtonPlan(pieces);
    }

    // When the menu first appeared, so the delay a player actually sees can be a measured
    // number rather than an impression. Everything before this is the game's own loading;
    // everything after it is the mod's.
    static std::chrono::steady_clock::time_point s_menu_first_seen{};
    if (menu != 0 && s_menu_first_seen == std::chrono::steady_clock::time_point{}) {
        s_menu_first_seen = now;
        MPE_LOG_INFO("main menu 0x{:X} exists; placing the multiplayer entry", menu);
    }

    if (menu == 0 || menu == s_decorated_menu) {
        return;
    }

    // Everything is resolved here, on this thread, before the game thread is involved.
    // Doing these lookups inside the job meant the game sat blocked through a scan of the
    // whole object array, which showed up as a multi second freeze the moment the menu
    // appeared. Now the game thread only makes the calls.
    unreal::CallLayout    layout;
    unreal::MenuButtonPlan plan;
    {
        std::lock_guard lock(g_state_mutex);
        if (!g_state || !g_state->objects.has_value()) {
            return;
        }
        if (!unreal::DetectCallLayout(*g_state->objects, layout).ok()) {
            return;
        }
        // Deliberately not warmed here.
        //
        // The button class and the player controller do not exist until the frontend is
        // built, which is the same moment the menu appears, so an attempt before that
        // cannot succeed and costs a full pass over the object array to fail. Tried once
        // per poll it was a scan a second for the whole of the game's loading.
        //
        // It is resolved on the one pass below instead, which has to happen anyway.
        // One retry, only for the font.
        //
        // The handles are resolved before the frontend is built, which is what makes the
        // entry appear with the rest of the menu rather than long after it. The typeface is
        // the one thing that benefits from waiting: it is read from the game's own text
        // blocks, and there are far more of those once a menu is on screen.
        if (g_lobby_ui_ready && !g_lobby_ui.has_font) {
            (void)unreal::ResolveLobbyStatics(*g_state->objects, g_lobby_ui);
        }
        // The menu address is handed over rather than searched for again: the pass above
        // just found it, and repeating that scan here was most of the delay between the
        // menu being drawn and the entry appearing on it.
        if (const Result resolved =
                unreal::ResolveMenuButtonPlan(*g_state->objects, menu, plan);
            !resolved.ok()) {
            // Logged once per menu rather than every tick: a silent return here meant the
            // entry simply never appeared with no indication of why.
            static std::uintptr_t s_complained_about = 0;
            if (s_complained_about != menu) {
                s_complained_about = menu;
                MPE_LOG_WARN("menu found at 0x{:X} but the entry cannot be built: {}", menu,
                            resolved.message());
            }
            return;
        }
    }

    std::uintptr_t button  = 0;
    Result         outcome = Result::Success();
    const Result   ran     = unreal::RunOnGameThread(
        [&]() { outcome = unreal::ApplyMenuButtonPlan(plan, "MULTIPLAYER", button); }, 20000);

    if (ran.ok() && outcome.ok()) {
        s_decorated_menu     = menu;
        // The click attaches to this exact instance rather than searching for a menu of
        // its own. Two independent searches can disagree, and attaching to the loser
        // produces a lobby that builds without error and is never drawn.
        g_live_menu          = menu;
        g_multiplayer_button = button;
        g_screen             = MultiplayerScreen::Home;
        MPE_LOG_INFO("multiplayer entry added to main menu 0x{:X}, {} ms after the menu "
                    "appeared",
                    menu,
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - s_menu_first_seen)
                        .count());

        // Start listening on the new button straight away. The previous one went away with
        // the previous menu, so the watch has to follow the current widget.
        // The menu outlives every button on it, so it is what the job pump hooks. Without
        // this, each job armed and disarmed a breakpoint across every thread, which is why
        // opening the lobby took seconds instead of a frame.
        if (const Result pump = unreal::InstallGameThreadPump(menu); !pump.ok()) {
            MPE_LOG_WARN("game thread pump unavailable: {}", pump.message());
        }

        unreal::StopWatchingWidgetEvents();
        if (const Result watching = unreal::WatchWidgetEvents(button); !watching.ok()) {
            MPE_LOG_WARN("could not watch the multiplayer button: {}", watching.message());
        } else {
            // HandleButtonClicked is the framework's own click, and it arrives exactly once
            // per press. It was chosen by watching what the button actually emits rather
            // than by guessing a name: a real click produces twenty three distinct events,
            // most of them hover and focus noise, and several look like plausible
            // candidates. Resolved by name here so it holds across runs.
            std::lock_guard lock(g_state_mutex);
            if (g_state && g_state->objects.has_value()) {
                // Found once and kept, with a single read to prove it is still there.
                //
                // FindFunction is a pass over the whole object array, and this ran on every
                // menu, holding the state lock for around a second while the player looked
                // at a MULTIPLAYER entry that did not answer a click yet. The address of a
                // UFunction does not change once its class is loaded, so the scan is worth
                // doing exactly once; ClassOf is a single guarded read and catches the case
                // where it somehow did.
                static std::uintptr_t s_click_function = 0;
                if (s_click_function != 0 &&
                    g_state->objects->ClassOf(s_click_function) == 0) {
                    s_click_function = 0;
                }
                if (s_click_function == 0) {
                    s_click_function =
                        unreal::FindFunction(*g_state->objects, "HandleButtonClicked");
                }
                const std::uintptr_t click = s_click_function;
                if (click != 0) {
                    unreal::SetWidgetClickEvent(click);
                    MPE_LOG_INFO("multiplayer button click bound to HandleButtonClicked "
                                "0x{:X}",
                                click);
                } else {
                    MPE_LOG_WARN("HandleButtonClicked not found; the button will not respond");
                }
            }
        }
    }
}

// --- The loading screen ------------------------------------------------------
//
// WHAT THE PERCENTAGE MEANS
//
// Every number this reports is measured. Nothing on this screen is a curve chosen to look
// like progress, because a player reads a percentage as a promise and a made up one is
// worse than none at all.
//
// The wait is divided into bands, and each band is filled from something real:
//
//   JOINING LOBBY       0..30   which of three connection steps has completed
//   STARTING SESSION   30..55   how much of the countdown has elapsed
//   LOADING MAP        55..75   nothing; the engine reports no fraction, so this one is
//                               honestly indeterminate and says so
//   WAITING FOR PLAYERS 75..100 how many peers have reported their load finished
//
// The bands are what keep the bar moving forward rather than restarting at every stage, and
// they are why the screen can be truthful about the one step it cannot measure without the
// whole thing looking broken.
//
// The step nobody can measure is the engine's own load. Its campaign entry point commits to
// the load and returns, reporting nothing further; there is no fraction to read. That is
// exactly the case an indeterminate bar exists for, so the bar sweeps, the large readout
// becomes the elapsed time, and the line underneath says what the game is doing. The screen
// is never silent and never claims to know something it does not.

/// When the current wait started, for the elapsed clock.
std::chrono::steady_clock::time_point g_loading_since{};
unreal::LoadingStage                  g_loading_stage = unreal::LoadingStage::None;
std::uint32_t                         g_loading_frame = 0;

/// Where in the list of lines this session starts.
///
/// Seeded once from the clock rather than reset per wait, so two players joining the same
/// lobby do not read the same line at the same moment, and so the first thing anybody sees
/// is not the same line every launch.
std::uint32_t g_loading_line_seed = static_cast<std::uint32_t>(
    std::chrono::steady_clock::now().time_since_epoch().count());

/// Builds the loading screen once, and again if its menu was collected under it.
void PrepareLoadingOverlay() {
    if (!g_lobby_ui_ready || g_live_menu == 0) {
        return;
    }
    if (unreal::LoadingOverlayIsBuilt()) {
        std::lock_guard lock(g_state_mutex);
        if (!g_state || !g_state->objects.has_value()) {
            return;
        }
        if (g_state->objects->ClassOf(unreal::LoadingOverlayWidget()) != 0) {
            return; // Still there.
        }
        MPE_LOG_INFO("the loading overlay was collected with its menu; building it again");
        unreal::ForgetLoadingOverlay();
    }

    unreal::LobbyUIContext ui = g_lobby_ui;
    if (!unreal::BindLobbyMenu(g_live_menu, ui).ok()) {
        return;
    }
    Result outcome = Result::Success();
    (void)unreal::RunOnGameThread([&]() { outcome = unreal::BuildLoadingOverlay(ui); }, 10000);
    if (!outcome.ok()) {
        static bool s_complained = false;
        if (!s_complained) {
            s_complained = true;
            MPE_LOG_WARN("the loading overlay could not be built: {}", outcome.message());
        }
    }
}

/// Makes the loading screen's CANCEL answer the mouse.
///
/// Separate from the lobby's own controls because the two are rebuilt on different
/// schedules: forgetting the watched widgets before a lobby rebuild drops this one too, so
/// it is put back rather than registered once and assumed.
void RegisterLoadingCancel() {
    const std::uintptr_t cancel = unreal::LoadingCancelButton();
    if (cancel == 0) {
        return;
    }
    for (const unreal::LobbyControl& control : g_lobby_controls) {
        if (control.widget == cancel) {
            return;
        }
    }
    if (!unreal::AlsoWatchWidget(cancel).ok()) {
        return;
    }
    g_lobby_controls.push_back({cancel, unreal::LobbyAction::CancelLoading, 0});
}

/// Works out what the loading screen should say, from the session and nothing else.
[[nodiscard]] bool DescribeLoading(unreal::LoadingView& out_view) {
    lobby::LobbyPhase phase = lobby::LobbyPhase::Idle;
    std::uint8_t      countdown = 0;
    std::size_t       loaded = 0;
    std::size_t       total = 0;
    std::string       still_loading;
    std::uint8_t      countdown_total = 5;
    {
        std::lock_guard lock(g_state_mutex);
        if (!g_state || !g_state->manager) {
            return false;
        }
        const lobby::LobbySnapshot& snapshot = g_state->manager->Snapshot();
        phase     = snapshot.phase;
        countdown = snapshot.countdown_seconds;
        total     = snapshot.players.size();
        for (const lobby::PlayerSlot& player : snapshot.players) {
            if (player.load_progress >= 1.0F) {
                ++loaded;
            } else if (still_loading.empty()) {
                still_loading = player.display_name;
            }
        }
    }

    // Joining is three connection steps, in order, and each one is a thing that either has
    // happened or has not. That is a real fraction over three, not a guess.
    const auto joining = [&out_view](int step, std::string_view detail) {
        out_view.stage   = unreal::LoadingStage::JoiningLobby;
        out_view.percent = lobby::LoadingPercent(lobby::LoadingStep::JoiningLobby, step, 3);
        out_view.detail  = detail;
    };

    switch (phase) {
        case lobby::LobbyPhase::Joining:
            joining(1, "Entering the Steam lobby.");
            break;
        case lobby::LobbyPhase::Connecting:
            joining(2, "Opening a relay connection to the host. No ports are involved; "
                       "Steam routes this.");
            break;
        case lobby::LobbyPhase::Handshaking:
            joining(3, "Waiting for the host to accept and send the roster.");
            break;

        case lobby::LobbyPhase::Countdown: {
            // A real fraction: the countdown is a timer this machine can see running.
            const int remaining = countdown;
            const int elapsed   = countdown_total > remaining ? countdown_total - remaining : 0;
            out_view.stage      = unreal::LoadingStage::StartingSession;
            out_view.percent    = lobby::LoadingPercent(lobby::LoadingStep::StartingSession,
                                                        elapsed, countdown_total);
            out_view.detail     = remaining > 0
                                      ? std::format("Everyone starts together in {}.", remaining)
                                      : std::string("Telling every player to load.");
            break;
        }

        case lobby::LobbyPhase::Loading:
            if (total > 0 && loaded < total) {
                // Also a real fraction, and the one that actually decides when the match
                // begins: nobody is released until every machine has reported.
                out_view.stage   = unreal::LoadingStage::WaitingForPlayers;
                out_view.percent = lobby::LoadingPercent(lobby::LoadingStep::WaitingForPlayers,
                                                         static_cast<int>(loaded),
                                                         static_cast<int>(total));
                out_view.detail =
                    still_loading.empty()
                        ? std::format("{} of {} players have finished loading.", loaded, total)
                        : std::format("{} of {} players have finished loading. Waiting on {}.",
                                      loaded, total,
                                      text::CleanDisplayName(still_loading, 24));
            } else {
                // The one step with no fraction to report. Said plainly rather than
                // decorated with a number nobody can stand behind.
                out_view.stage         = unreal::LoadingStage::LoadingMap;
                out_view.percent = lobby::LoadingPercent(lobby::LoadingStep::LoadingMap, 0, 0);
                out_view.indeterminate = true;
                out_view.detail        = "Building shaders and streaming the level. The first "
                                         "run on a map is the slowest one.";
            }
            // Cancelling is withheld here rather than offered, for a while. The engine has
            // already been told to load, so leaving now ends the session while the map
            // carries on arriving, which is a worse place to be than waiting a few more
            // seconds. RefreshLoadingScreen gives it back once waiting has stopped being
            // the likely explanation.
            out_view.cancellable = false;
            break;

        default:
            return false; // Nothing worth covering the screen for.
    }
    return true;
}

/// Drives the loading screen. Called from the tick at the animation rate.
void RefreshLoadingScreen() {
    if (!g_lobby_ui_ready || !unreal::LoadingOverlayIsBuilt()) {
        return;
    }

    unreal::LoadingView view;
    const bool          wanted = DescribeLoading(view);
    const auto          now    = std::chrono::steady_clock::now();

    if (!wanted) {
        if (unreal::LoadingOverlayIsOpen()) {
            unreal::LobbyUIContext ui = g_lobby_ui;
            if (unreal::BindLobbyMenu(g_live_menu, ui).ok()) {
                (void)unreal::RunOnGameThread(
                    [&]() { unreal::ShowLoadingOverlay(ui, false); }, 5000);
            }
            MPE_LOG_INFO("loading screen closed after {}s",
                        std::chrono::duration_cast<std::chrono::seconds>(now - g_loading_since)
                            .count());
        }
        g_loading_stage = unreal::LoadingStage::None;
        return;
    }

    // The clock restarts when the stage does, because a player watching WAITING FOR PLAYERS
    // wants to know how long that has been going, not how long ago they pressed a button.
    const bool opening = !unreal::LoadingOverlayIsOpen();
    if (opening || view.stage != g_loading_stage) {
        g_loading_since = now;
        g_loading_stage = view.stage;
    }
    view.elapsed_seconds = static_cast<int>(
        std::chrono::duration_cast<std::chrono::seconds>(now - g_loading_since).count());
    view.frame = ++g_loading_frame;

    // A modal with no exit is a trap, and this one covers the whole screen and takes every
    // click. Loading withholds cancelling because leaving mid load is genuinely worse than
    // waiting, but only while waiting is still the likely explanation. Past half a minute it
    // is not, and being stuck behind a screen that cannot be dismissed is the worse of the
    // two outcomes by a wide margin.
    constexpr int kCancelUnlocksAfterSeconds = 30;
    if (!view.cancellable && view.elapsed_seconds >= kCancelUnlocksAfterSeconds) {
        view.cancellable = true;
    }

    // The line under the title is flavour, not narration.
    //
    // It used to describe the step, which the title already names, the bar already
    // measures and the clock already times. Four lines saying the same thing is three too
    // many. Something to read while waiting is worth more, and a line that changes every
    // few seconds is also the clearest possible signal that the mod has not stopped.
    //
    // Except when something is actually wrong. A wait that has gone on long enough to be
    // worth explaining gets the line back, because at that point it is the most useful
    // thing on the screen.
    if (view.elapsed_seconds < kCancelUnlocksAfterSeconds) {
        view.detail = unreal::LoadingLine(g_loading_line_seed + view.frame / 40U);
    }

    unreal::LobbyUIContext ui = g_lobby_ui;
    if (!unreal::BindLobbyMenu(g_live_menu, ui).ok()) {
        return;
    }
    // A quarter of a second, not five.
    //
    // This runs twelve and a half times a second on the same thread that counts the
    // countdown down and reads button presses. A five second deadline on a screen update
    // means one unlucky frame costs five seconds of everything else, and the countdown
    // advances by however much real time each tick observed, clamped to a second: enough
    // stalled ticks in a row and a five second countdown takes a minute.
    //
    // Nothing here is worth waiting for. A dropped animation frame is invisible; a tick
    // that does not happen is not.
    (void)unreal::RunOnGameThread(
        [&]() {
            unreal::SetLoadingView(ui, view);
            if (opening) {
                unreal::ShowLoadingOverlay(ui, true);
            }
        },
        250);
    if (opening) {
        MPE_LOG_INFO("loading screen open: stage {}, {}%", static_cast<int>(view.stage),
                    view.percent);
    }
}

/// Abandons whatever the loading screen is waiting for.
void OnCancelLoading() {
    MPE_LOG_INFO("the player cancelled the wait");
    {
        std::lock_guard lock(g_state_mutex);
        if (g_state && g_state->manager) {
            g_state->manager->LeaveSession();
        }
    }
    ShowSessionNotice("CANCELLED", "You left before it finished. Nothing was started.");
    EnsureSessionHosted();
}

/// Polls every watched field and logs transitions.
///
/// Runs at 1 Hz from the tick loop. Every entry is revalidated before it is read, because
/// a guarded read alone cannot tell a live value from a dead object's leftover bytes.
void PollWatchedFields() {
    std::lock_guard lock(g_state_mutex);
    if (!g_state || g_state->watched.empty()) {
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    if (now - g_state->last_watch_poll < std::chrono::seconds(1)) {
        return;
    }
    g_state->last_watch_poll = now;

    for (ModState::WatchedField& field : g_state->watched) {
        if (!field.valid || !RevalidateWatch(field)) {
            continue;
        }
        std::uint8_t raw = 0;
        if (!unreal::memory::GuardedRead(field.address, &raw, sizeof(raw))) {
            MPE_LOG_INFO("watch: {} at 0x{:X} became unreadable; the object was probably "
                        "collected",
                        field.label, field.address);
            field.valid = false;
            continue;
        }
        const bool value = (raw != 0);

        // Hold friendly fire on if enforcement is enabled.
        //
        // Only fields whose name says friendly fire are touched, so enabling this cannot
        // reach anything else being watched.
        if (g_enforce_friendly_fire.load(std::memory_order_acquire) && !value &&
            field.label.find("riendlyFire") != std::string::npos) {
            const std::uint8_t on = 1;
            if (unreal::memory::GuardedWrite(field.address, &on, sizeof(on))) {
                MPE_LOG_INFO("pvp: re-enabled friendly fire on {} (0x{:X})", field.label,
                            field.address);
                field.last_value = true;
                continue;
            }
            MPE_LOG_WARN("pvp: could not write friendly fire on {} (0x{:X})", field.label,
                        field.address);
        }

        if (value != field.last_value) {
            MPE_LOG_INFO("watch: {} changed {} -> {} (0x{:X})", field.label,
                        field.last_value ? "true" : "false", value ? "true" : "false",
                        field.address);
            field.last_value = value;
        }
    }
}
void ResolveLiveInstances(const std::unordered_map<std::string, unreal::ObjectInfo>& located,
                          const unreal::PropertyInfo& friendly_fire,
                          std::uintptr_t friendly_fire_owner);
void SweepForCombatFields(const std::unordered_map<std::string, unreal::ObjectInfo>& located,
                          const unreal::PropertyInfo& friendly_fire,
                          const std::vector<unreal::StructUsage>& known_usages);

/// Blocks until the game has finished starting, or the timeout expires.
///
/// WHY THIS GATE EXISTS
///
/// Scanning the executable's data for UE globals means hundreds of thousands of memory
/// probes, and VirtualQuery contends on the process address space lock. Running that
/// while the loader is mapping gigabytes of assets starved the game's own startup: it
/// sat on its splash screen and never reached the main menu, intermittently, roughly
/// half the time.
///
/// The gate is the game's own window. While it is still loading the only visible
/// top level window belongs to the splash screen; once the engine is up it creates its
/// real window with a title. Waiting for that costs nothing and removes the contention
/// entirely, because by then the heavy loading is done.
[[nodiscard]] bool HasGameWindow() {
    struct Search {
        DWORD process_id{0};
        bool  found{false};
    };

    {
        Search search;
        search.process_id = ::GetCurrentProcessId();

        ::EnumWindows(
            [](HWND window, LPARAM parameter) -> BOOL {
                auto* state = reinterpret_cast<Search*>(parameter);

                DWORD owner = 0;
                ::GetWindowThreadProcessId(window, &owner);
                if (owner != state->process_id || ::IsWindowVisible(window) == FALSE) {
                    return TRUE;
                }

                wchar_t class_name[64] = {};
                ::GetClassNameW(window, class_name, static_cast<int>(std::size(class_name)));
                if (::wcscmp(class_name, L"SplashScreenClass") == 0) {
                    return TRUE; // Still loading.
                }

                // A real window with a title means the engine reached its main loop.
                if (::GetWindowTextLengthW(window) > 0) {
                    state->found = true;
                    return FALSE;
                }
                return TRUE;
            },
            reinterpret_cast<LPARAM>(&search));

        return search.found;
    }
}

/// Locates the UE5 FName pool and proves it works, then commits it under the lock.
///
/// Verification is deliberately concrete: block 0 of the pool always begins with the
/// engine's intrinsic names, so dumping them shows unambiguously whether the location
/// is right. A wrong location produces garbage, not plausible names.
void ResolveUnrealReflection() {
    // Nothing is searched for twice.
    //
    // The fast path reads both globals straight out of the instructions that reference
    // them, and it succeeds in about two seconds. This function then located the same two
    // things again by scanning: half a million name slots and a sweep for the object
    // array, taking around fifteen seconds and overwriting a pool and an array that were
    // already correct and already in use.
    //
    // The waste was the smaller half of the problem. That scanning probes hundreds of
    // thousands of addresses and takes the process address space lock to do it, competing
    // with the game's own asset loader at exactly the moment the game is loading, which is
    // the contention this file already documents as the way the mod breaks somebody's
    // startup. Doing it for an answer we have is indefensible.
    {
        std::lock_guard lock(g_state_mutex);
        if (g_state && g_state->names.has_value() && g_state->objects.has_value()) {
            MPE_LOG_INFO("UE reflection was already resolved by the fast path; skipping the "
                        "search entirely");
            g_reflection_was_fast = true;
        }
    }
    if (g_reflection_was_fast) {
        DetectPropertyLayout();
        return;
    }

    Expected<unreal::NamePool> pool = unreal::NamePool::Locate();
    if (!pool.ok()) {
        MPE_LOG_ERROR("UE reflection unavailable: {}", pool.message());
        MPE_LOG_ERROR("the UE side game engine variant cannot be reached without it");
        return;
    }

    // Proof of correctness, in the log where a user can see it.
    const std::vector<unreal::NameEntry> sample = pool.value().DumpBlock(0, 24);
    MPE_LOG_INFO("FName pool verification, first {} name(s) of block 0:", sample.size());
    std::string line;
    for (std::size_t i = 0; i < sample.size(); ++i) {
        line += sample[i].text;
        if (i + 1 < sample.size()) {
            line += ", ";
        }
    }
    MPE_LOG_INFO("  {}", line);

    // Look up the names that matter for making a campaign scenario play as a versus
    // match. Finding these confirms the reflected surface is reachable by name.
    for (const char* wanted : {"bFriendlyFireEnabled", "bFriendlyFire",
                               "BlamGameEngineSocialOptions", "BlamGameEnginePlayerTraits",
                               "BlamGameEngineBaseVariantStorage", "BlamPlayerRespawn",
                               "BlamNetworkGameStateComponent"}) {
        const Expected<std::uint32_t> index = pool.value().FindIndexOf(wanted, 8);
        if (index.ok()) {
            MPE_LOG_INFO("  resolved '{}' to FName index {}", wanted, index.value());
        } else {
            MPE_LOG_WARN("  '{}' not found in the searched blocks: {}", wanted, index.message());
        }
    }

    // Commit the pool first: the object array holds a pointer to it, so the pool must
    // already be in its final home before the array is located against it.
    {
        std::lock_guard lock(g_state_mutex);
        if (!g_state) {
            return;
        }
        g_state->names = std::move(pool).value();
    }

    ResolveUnrealObjects();
}

/// Locates the UE global object array and proves it by resolving live object names.
///
/// Verification is concrete rather than assumed: a wrong location cannot produce
/// objects whose own name and whose class name both resolve through the name pool to
/// plausible identifiers. That is the test, and it is why the locator scores candidates
/// instead of accepting the first structurally valid one.
/// The structs whose field offsets the mod actually reads.
constexpr const char* kWantedStructs[] = {
    "BlamGameEngineBaseVariantStorage", "BlamGameEngineSocialOptions",
    "BlamGameEnginePlayerTraits",       "BlamGameEngineCampaignVariantStorage",
    "BlamScenarioGameOptions",          "BlamGameEngineTimer",
    "BlamPlayerRespawn",                "BlamSocialOptionsFlags",
};

/// Works out where a UProperty keeps its offset, size and next pointer on this build.
///
/// Split out of the locating pass so both routes into it share one implementation. The
/// fast path already has the pool and the array and needs only this, and doing it here
/// rather than duplicating it is what makes skipping the search safe.
///
/// The documented UE5 layout is wrong on this build: reading BlamScenarioGameOptions with
/// it reports size 0 and no fields even though the struct name resolves correctly. So the
/// chain is detected from live structs and then validated, because a wrong offset quietly
/// pointing at the wrong field is the worst outcome available.
void DetectPropertyLayout() {
    const unreal::NamePool*    names   = nullptr;
    const unreal::ObjectArray* objects = nullptr;
    {
        std::lock_guard lock(g_state_mutex);
        if (!g_state || !g_state->names.has_value() || !g_state->objects.has_value()) {
            return;
        }
        // Both live in g_state for the process lifetime, so pointers taken under the lock
        // stay valid once it is released, which keeps the tick thread unblocked.
        names   = &*g_state->names;
        objects = &*g_state->objects;
    }

    // The types have to exist before their layout can be read from them. UE registers
    // UObjects progressively, so this is a condition rather than a delay: an early look
    // found none of them purely because the game had not reached them yet.
    std::unordered_map<std::string, unreal::ObjectInfo> located;
    (void)pacing::WaitFor(
        "waiting for the target structs to be registered",
        [&]() {
            located.clear();
            objects->ForEach([&](const unreal::ObjectInfo& object) {
                for (const char* wanted : kWantedStructs) {
                    if (object.name == wanted && !located.contains(object.name)) {
                        located.emplace(object.name, object);
                    }
                }
                return located.size() < std::size(kWantedStructs);
            });
            return located.size() >= std::size(kWantedStructs);
        },
        []() { return !g_running_or_starting.load(std::memory_order_acquire); },
        std::chrono::seconds(1), std::chrono::seconds(3));

    MPE_LOG_INFO("located {} of {} target struct(s) among {} object(s)", located.size(),
                std::size(kWantedStructs), objects->Count());
    if (located.empty()) {
        MPE_LOG_ERROR("no target struct appeared; the game may not have finished starting");
        return;
    }

    unreal::Reflection          reflection(*names);
    std::vector<std::uintptr_t> candidates;
    candidates.reserve(located.size());
    for (const auto& [name, object] : located) {
        candidates.push_back(object.address);
    }

    const unreal::ReflectionLayout detected = reflection.DetectLayout(candidates);
    if (detected.detected) {
        reflection.SetLayout(detected);
        MPE_LOG_INFO("  UE layout detected: {}", detected.Describe());
    } else {
        MPE_LOG_WARN("  UE layout detection found no property chain; trying documented offsets");
    }

    bool layout_ok = false;
    for (const auto& [name, object] : located) {
        std::string  report;
        const Result verified = reflection.VerifyLayout(object.address, report);
        MPE_LOG_INFO("  layout check: {}", report);
        if (verified.ok()) {
            layout_ok = true;
            break;
        }
    }
    if (!layout_ok) {
        MPE_LOG_ERROR("  UE property layout could not be validated against any target struct");
        MPE_LOG_ERROR("  offsets will not be trusted; reflection reads are disabled");
        if (!candidates.empty()) {
            MPE_LOG_INFO("{}", reflection.ProbeStructLayout(candidates.front()));
        }
        return;
    }

    {
        std::lock_guard lock(g_state_mutex);
        if (g_state) {
            g_state->reflection = reflection;
        }
    }
    PublishEngineReflection(reflection);
}

void ResolveUnrealObjects() {
    // The name pool lives in g_state for the process lifetime, so a raw pointer taken
    // under the lock stays valid. Taking it this way lets the heavy scanning below run
    // without holding the lock, which matters because it can take tens of seconds and
    // the tick thread would otherwise be blocked for all of it.
    const unreal::NamePool* names = nullptr;
    {
        std::lock_guard lock(g_state_mutex);
        if (!g_state || !g_state->names.has_value()) {
            return;
        }
        names = &*g_state->names;
    }

    Expected<unreal::ObjectArray> array = unreal::ObjectArray::Locate(*names);
    if (!array.ok()) {
        MPE_LOG_ERROR("GUObjectArray unavailable: {}", array.message());
        MPE_LOG_ERROR("live UE objects cannot be reached, so the game engine variant cannot be "
                     "read or modified");
        return;
    }
    unreal::ObjectArray objects = std::move(array).value();

    // Wait for the game to register the types we need.
    //
    // UE creates UObjects progressively during startup, so scanning at a fixed moment
    // is simply wrong: an early scan found 0 of 8 target structs purely because the
    // game had not got to them yet. Polling until they exist is the correct behaviour,
    // and the interval is deliberately generous so this does not hammer the memory
    // manager while the game is loading.
    static constexpr const char* kWanted[] = {
        "BlamGameEngineBaseVariantStorage", "BlamGameEngineSocialOptions",
        "BlamGameEnginePlayerTraits",       "BlamGameEngineCampaignVariantStorage",
        "BlamScenarioGameOptions",          "BlamGameEngineTimer",
        "BlamPlayerRespawn",                "BlamSocialOptionsFlags",
    };

    // Wait for the object graph to stop growing.
    //
    // Expressed as a condition, not a duration: the count must hold steady across
    // consecutive polls. A machine that registers objects slowly simply satisfies it
    // later, instead of a fixed deadline expiring mid registration and leaving half the
    // types missing, which is what a 240 second limit did.
    std::uint32_t previous_count = 0;
    std::uint32_t stable_polls   = 0;
    (void)pacing::WaitFor(
        "waiting for the UE object graph to settle",
        [&]() {
            const std::uint32_t count = objects.Count();
            if (count > 20000 && count == previous_count) {
                ++stable_polls;
            } else {
                stable_polls = 0;
            }
            previous_count = count;
            return stable_polls >= 3;
        },
        []() { return !g_running_or_starting.load(std::memory_order_acquire); },
        std::chrono::seconds(2), std::chrono::seconds(2));

    std::unordered_map<std::string, unreal::ObjectInfo> located;
    objects.ForEach([&](const unreal::ObjectInfo& object) {
        for (const char* wanted : kWanted) {
            if (object.name == wanted && !located.contains(object.name)) {
                located.emplace(object.name, object);
            }
        }
        return located.size() < std::size(kWanted);
    });

    MPE_LOG_INFO("located {} of {} target struct(s) among {} object(s)", located.size(),
                std::size(kWanted), objects.Count());
    if (located.empty()) {
        MPE_LOG_ERROR("no target struct appeared; the game may not have finished starting");
        return;
    }

    {
        std::lock_guard lock(g_state_mutex);
        if (!g_state) {
            return;
        }
        g_state->objects = objects;
    }
    PublishEngineObjects(objects);

    // Proof, in the log: the first objects in a UE process are always engine
    // intrinsics, so their names and classes are recognizable at a glance.
    MPE_LOG_INFO("UE object array verification, first 12 object(s):");
    std::uint32_t shown = 0;
    objects.ForEach(
        [&](const unreal::ObjectInfo& object) {
            MPE_LOG_INFO("  [{:>6}] {:<40} class={}", object.index, object.name,
                        object.class_name);
            return ++shown < 12;
        },
        64);

    // Shared with the fast path rather than written twice.
    DetectPropertyLayout();

    unreal::Reflection reflection(*names);
    {
        std::lock_guard lock(g_state_mutex);
        if (!g_state || !g_state->reflection.has_value()) {
            return;
        }
        reflection = *g_state->reflection;
    }

    // Dump the fields. This is the payoff: named fields with real offsets.
    for (const char* wanted : kWanted) {
        const auto it = located.find(wanted);
        if (it == located.end()) {
            MPE_LOG_WARN("  '{}' was not found in the object array", wanted);
            continue;
        }

        const Expected<unreal::StructInfo> info = reflection.ReadStruct(it->second.address);
        if (!info.ok()) {
            MPE_LOG_WARN("  '{}' could not be read: {}", wanted, info.message());
            continue;
        }

        MPE_LOG_INFO("struct {} at 0x{:X}: size {}, {} field(s)", info.value().name,
                    info.value().address, info.value().properties_size,
                    info.value().properties.size());
        for (const unreal::PropertyInfo& property : info.value().properties) {
            MPE_LOG_INFO("    +0x{:03X}  {:<12} {:<44} size {}", property.offset,
                        property.type_name, property.name, property.TotalSize());
        }
    }

    // The specific field this whole chain exists to reach.
    unreal::PropertyInfo friendly_fire{};
    std::uintptr_t       friendly_fire_owner = 0;
    for (const auto& [name, object] : located) {
        for (const unreal::PropertyInfo& property :
             reflection.FindPropertiesContaining(object.address, "riendly")) {
            MPE_LOG_INFO("field located: {}::{} at +0x{:X} type {} size {}", name, property.name,
                        property.offset, property.type_name, property.TotalSize());
            // Prefer the one on BlamScenarioGameOptions: that struct also carries
            // difficulty, skulls and insertion point, so it is the per match options
            // block rather than a nested flags helper.
            if (name == "BlamScenarioGameOptions") {
                friendly_fire       = property;
                friendly_fire_owner = object.address;
            } else if (friendly_fire_owner == 0) {
                friendly_fire       = property;
                friendly_fire_owner = object.address;
            }
        }
    }

    ResolveLiveInstances(located, friendly_fire, friendly_fire_owner);
}

/// Finds a live instance of the struct holding bFriendlyFireEnabled, then reads it.
///
/// A ScriptStruct has no entry in the object array, so an instance can only be reached
/// through an object that embeds it. That means three steps: detect where a
/// StructProperty records its inner type, find the classes with a field of that type,
/// then find live objects of those classes. The instance is the object plus the field
/// offset.
void ResolveLiveInstances(
    const std::unordered_map<std::string, unreal::ObjectInfo>& located,
    const unreal::PropertyInfo& friendly_fire, std::uintptr_t friendly_fire_owner) {
    unreal::Reflection  reflection{*[]() -> const unreal::NamePool* {
        std::lock_guard lock(g_state_mutex);
        return (g_state && g_state->names.has_value()) ? &*g_state->names : nullptr;
    }()};
    unreal::ObjectArray objects = *[]() -> const unreal::ObjectArray* {
        std::lock_guard lock(g_state_mutex);
        return (g_state && g_state->objects.has_value()) ? &*g_state->objects : nullptr;
    }();
    {
        std::lock_guard lock(g_state_mutex);
        if (g_state && g_state->reflection.has_value()) { reflection = *g_state->reflection; }
    }

    if (friendly_fire_owner == 0 || friendly_fire.name.empty()) {
        MPE_LOG_WARN("no friendly fire field was located; cannot look for an instance");
        return;
    }

    // Refine the two offsets a single struct could not pin down.
    std::vector<std::uintptr_t> addresses;
    addresses.reserve(located.size());
    for (const auto& [name, object] : located) {
        addresses.push_back(object.address);
    }

    unreal::ReflectionLayout layout = reflection.Layout();

    if (const std::size_t size_offset = reflection.DetectPropertiesSizeOffset(addresses);
        size_offset != 0) {
        MPE_LOG_INFO("properties_size refined to +0x{:X} using {} struct(s)", size_offset,
                    addresses.size());
        layout.properties_size_offset = size_offset;
    } else {
        MPE_LOG_WARN("properties_size could not be refined; struct sizes stay unreliable");
    }

    if (const std::size_t inner = reflection.DetectStructPropertyInnerOffset(addresses, addresses);
        inner != 0) {
        MPE_LOG_INFO("FStructProperty::Struct detected at +0x{:X}", inner);
        layout.struct_property_inner_offset   = inner;
        layout.struct_property_inner_detected = true;
    } else {
        MPE_LOG_WARN("FStructProperty::Struct offset not detected; embedded struct search "
                    "cannot run");
    }
    reflection.SetLayout(layout);

    // Commit the refined layout back to shared state.
    //
    // Without this the detection is stranded on a local copy: a later pass reading
    // g_state->reflection would have struct_property_inner_detected false and conclude
    // that nothing embeds anything, which is exactly the wrong answer this produced.
    {
        std::lock_guard lock(g_state_mutex);
        if (g_state && g_state->reflection.has_value()) {
            g_state->reflection->SetLayout(layout);
        }
    }

    if (!layout.struct_property_inner_detected) {
        return;
    }

    // Find every class or struct with a field whose type is our target struct.
    std::vector<unreal::StructUsage> usages;
    objects.ForEach([&](const unreal::ObjectInfo& object) {
        // Only types carry properties. Filtering here is what keeps this from being a
        // property walk over all 52698 objects.
        if (object.class_name != "Class" && object.class_name != "ScriptStruct" &&
            object.class_name != "BlueprintGeneratedClass") {
            return true;
        }
        for (const unreal::PropertyInfo& property : reflection.ReadProperties(object.address)) {
            if (property.type_name != "StructProperty") {
                continue;
            }
            if (reflection.ResolveStructPropertyInner(property.address) != friendly_fire_owner) {
                continue;
            }
            usages.push_back(unreal::StructUsage{object.address, object.name, object.class_name,
                                                 property.name, property.offset});
        }
        return usages.size() < 32;
    });

    MPE_LOG_INFO("the target struct is embedded in {} type(s):", usages.size());
    for (const unreal::StructUsage& usage : usages) {
        MPE_LOG_INFO("  {} ({})::{} at +0x{:X}", usage.owner_name, usage.owner_class_name,
                    usage.property_name, usage.property_offset);
    }

    // For each owning type, look for live objects of it and read the field.
    std::size_t instances_read = 0;
    for (const unreal::StructUsage& usage : usages) {
        const std::vector<unreal::ObjectInfo> instances =
            objects.FindInstancesOfClassAddress(usage.owner_address, 8);
        if (instances.empty()) {
            MPE_LOG_INFO("  no live instance of {} right now", usage.owner_name);
            continue;
        }
        for (const unreal::ObjectInfo& instance : instances) {
            const std::uintptr_t struct_instance =
                instance.address + static_cast<std::uintptr_t>(usage.property_offset);
            const Expected<bool> value =
                reflection.ReadBoolField(struct_instance, friendly_fire);
            if (!value.ok()) {
                MPE_LOG_WARN("  {} at 0x{:X}: could not read {}: {}", instance.name,
                            instance.address, friendly_fire.name, value.message());
                continue;
            }
            ++instances_read;
            const std::uintptr_t field_address =
                struct_instance + static_cast<std::uintptr_t>(friendly_fire.offset);
            MPE_LOG_INFO("  LIVE VALUE {}.{}.{} = {} (instance 0x{:X}, field 0x{:X})",
                        instance.name, usage.property_name, friendly_fire.name,
                        value.value() ? "true" : "false", instance.address, field_address);

            RegisterWatch(std::format("{}.{}.{}", instance.name, usage.property_name,
                                      friendly_fire.name),
                          instance.address, usage.owner_address,
                          static_cast<std::uint32_t>(field_address - instance.address),
                          value.value());
        }
    }

    if (instances_read == 0) {
        MPE_LOG_INFO("no live instance was readable at the main menu, which is expected: the "
                    "per match options struct is populated when a match is set up");
    }

    SweepForCombatFields(located, friendly_fire, usages);
}

/// Answers the question the previous step could not: is this field the one that
/// matters, and does anything else in the game hold a similar one.
///
/// BlamMetaDataSaveGame is a save game record, so its copy of the options may be a
/// serialization artifact rather than what the simulation consults. Three checks:
///
///   1. Sweep every reflected type for any field whose name suggests friendly fire,
///      betrayal or team damage. If a game mode or session type has its own, that is
///      the one worth writing.
///   2. Find every type that embeds any of the variant structs, not just the scenario
///      options, so the whole ownership graph is visible.
///   3. Exercise the write path on a live instance, confirming the store lands and can
///      be restored. This validates the mechanism without claiming it changes play.
void SweepForCombatFields(
    const std::unordered_map<std::string, unreal::ObjectInfo>& located,
    const unreal::PropertyInfo& friendly_fire,
    const std::vector<unreal::StructUsage>& known_usages) {
    if (!g_state || !g_state->reflection.has_value() || !g_state->objects.has_value()) {
        return;
    }
    const unreal::Reflection&  reflection = *g_state->reflection;
    const unreal::ObjectArray& objects    = *g_state->objects;

    // --- 1. Global sweep for combat rule fields ---------------------------
    static constexpr const char* kFragments[] = {"riendly", "etray", "eamKill", "eamDamage",
                                                 "llowDamage", "vP", "ersusMode"};

    struct Hit {
        std::string owner;
        std::string owner_kind;
        std::string field;
        std::string type;
        std::int32_t offset{0};
    };
    std::vector<Hit> hits;
    std::size_t      types_scanned = 0;

    objects.ForEach([&](const unreal::ObjectInfo& object) {
        if (object.class_name != "Class" && object.class_name != "ScriptStruct" &&
            object.class_name != "BlueprintGeneratedClass") {
            return true;
        }
        ++types_scanned;
        for (const unreal::PropertyInfo& property : reflection.ReadProperties(object.address)) {
            for (const char* fragment : kFragments) {
                if (property.name.find(fragment) == std::string::npos) {
                    continue;
                }
                hits.push_back(Hit{object.name, object.class_name, property.name,
                                   property.type_name, property.offset});
                break;
            }
        }
        return hits.size() < 64;
    });

    MPE_LOG_INFO("combat rule sweep: scanned {} reflected type(s), found {} candidate field(s)",
                types_scanned, hits.size());
    for (const Hit& hit : hits) {
        MPE_LOG_INFO("  {} ({})::{} at +0x{:X} type {}", hit.owner, hit.owner_kind, hit.field,
                    hit.offset, hit.type);
    }

    // Any Blueprint class carrying a friendly fire flag is a stronger candidate than a
    // save game record, because a setup parameters blueprint is what a match is
    // actually configured from. Read the live value from every instance of each.
    for (const Hit& hit : hits) {
        if (hit.owner_kind != "BlueprintGeneratedClass" ||
            hit.field.find("riendly") == std::string::npos) {
            continue;
        }

        // Find the class object itself, then its instances.
        std::uintptr_t class_address = 0;
        objects.ForEach([&](const unreal::ObjectInfo& object) {
            if (object.name == hit.owner) {
                class_address = object.address;
                return false;
            }
            return true;
        });
        if (class_address == 0) {
            continue;
        }

        const std::vector<unreal::ObjectInfo> instances =
            objects.FindInstancesOfClassAddress(class_address, 8);
        MPE_LOG_INFO("  {} has {} live instance(s)", hit.owner, instances.size());

        unreal::PropertyInfo field;
        field.name         = hit.field;
        field.type_name    = hit.type;
        field.offset       = hit.offset;
        field.element_size = 1;

        for (const unreal::ObjectInfo& instance : instances) {
            const Expected<bool> value = reflection.ReadBoolField(instance.address, field);
            if (!value.ok()) {
                continue;
            }
            MPE_LOG_INFO("    LIVE {}::{} = {} (instance 0x{:X})", instance.name, hit.field,
                        value.value() ? "true" : "false", instance.address);

            RegisterWatch(std::format("{}::{}", instance.name, hit.field), instance.address,
                          class_address, static_cast<std::uint32_t>(hit.offset), value.value());
        }
    }

    // --- 2. Ownership graph for every variant struct, in ONE pass ----------
    //
    // The previous version walked all 52698 objects once per target struct, eight full
    // passes over the whole graph. Building a lookup from struct address to name and
    // testing every StructProperty against it does the same work in a single pass.
    std::unordered_map<std::uintptr_t, std::string> targets;
    for (const auto& [name, object] : located) {
        targets.emplace(object.address, name);
    }

    std::unordered_map<std::string, std::vector<unreal::StructUsage>> owners_by_target;
    objects.ForEach([&](const unreal::ObjectInfo& candidate) {
        if (candidate.class_name != "Class" && candidate.class_name != "ScriptStruct" &&
            candidate.class_name != "BlueprintGeneratedClass") {
            return true;
        }
        for (const unreal::PropertyInfo& property : reflection.ReadProperties(candidate.address)) {
            if (property.type_name != "StructProperty") {
                continue;
            }
            const auto it = targets.find(reflection.ResolveStructPropertyInner(property.address));
            if (it == targets.end()) {
                continue;
            }
            owners_by_target[it->second].push_back(
                unreal::StructUsage{candidate.address, candidate.name, candidate.class_name,
                                    property.name, property.offset});
        }
        return true;
    });

    for (const auto& [name, object] : located) {
        const auto it = owners_by_target.find(name);
        if (it == owners_by_target.end() || it->second.empty()) {
            MPE_LOG_INFO("  {} is embedded by nothing reflected", name);
            continue;
        }
        for (const unreal::StructUsage& owner : it->second) {
            MPE_LOG_INFO("  {} embedded by {} ({})::{} at +0x{:X}", name, owner.owner_name,
                        owner.owner_class_name, owner.property_name, owner.property_offset);
        }
    }

    // --- 3. No automatic writes -------------------------------------------
    //
    // Startup does not modify game state, deliberately.
    //
    // An earlier version wrote to a live frontend object during startup and left the
    // change applied. That is not something a mod should do on its own: the field's
    // consumers are not understood, the value was left altered without anyone asking,
    // and it coincided with the game failing to start. Discovery reads; changing the
    // game happens only when someone explicitly calls MPE_SetFriendlyFire.
    //
    // The write mechanism itself is already proven: an earlier run toggled a live
    // instance and read the new value back before restoring it. Repeating that on every
    // launch buys nothing and risks the run.
    (void)known_usages;
    (void)friendly_fire;
    MPE_LOG_INFO("discovery complete; no game state was modified. Use MPE_SetFriendlyFire to "
                "change it deliberately.");
}

/// True when a marker file of this name sits next to the mod.
///
/// A kill switch that needs no rebuild and no command line. If something this mod does
/// stops the game from starting, the fix has to be available to someone who only has the
/// files, so creating an empty file is the whole procedure.
[[nodiscard]] bool DisableFlagPresent(const wchar_t* name) {
    std::error_code error;
    return std::filesystem::exists(DataDirectory() / name, error);
}

void Initialize() {
    // Debug by default, and a file next to the mod turns it up further.
    //
    // The parts that are hardest to fix are the ones that need two people in two copies of
    // the game at the same time, and a report of one of those is only as good as what was
    // written down while it happened. Info alone left out the phase by phase detail of a
    // join, which is exactly the part nobody can reconstruct afterwards. Dropping to Debug
    // costs a larger file and nothing else that matters here.
    //
    // MultiplayerEvolved/trace.on adds every packet and every reflection lookup, which is
    // only worth having when a specific question needs it.
    const log::Level level =
        DisableFlagPresent(L"trace.on") ? log::Level::Trace : log::Level::Debug;
    log::Initialize(DataDirectory() / "MultiplayerEvolved.log", level);
    MPE_LOG_INFO("MultiplayerEvolved {} starting, logging at {}", MPE_VERSION_STRING,
                log::ToString(level));
    MPE_LOG_INFO("game build: {}", GameBuildString());
    MPE_LOG_INFO("data directory: {}", DataDirectory().string());

    // The FName adapter is written in early, but never called here.
    //
    // Calling it at startup crashed the game, and the reason is visible in the routine it
    // forwards to: it opens with a guard on a static flag, which is a one time
    // initialisation of the name pool. Invoking it before the engine has run that
    // initialisation is asking a subsystem to do work before it exists.
    //
    // Writing the adapter is inert on its own. It only fills padding, and nothing executes
    // it until something calls it, so installation can happen at any time while
    // verification has to wait for the engine. Use the 'fname' command once the game is at
    // the menu to exercise it.
    if (!DisableFlagPresent(L"fe_no_fname")) {
        unreal::TrampolineInfo trampoline;
        if (const Result installed = unreal::InstallFNameTrampoline(trampoline);
            !installed.ok()) {
            MPE_LOG_WARN("FName adapter not installed: {}", installed.message());
        }
    } else {
        MPE_LOG_INFO("FName adapter skipped: fe_no_fname is present");
    }

    auto state = std::make_unique<ModState>();

    // Networking first, deliberately.
    //
    // It does not depend on the engine, and engine binding polls for up to two
    // minutes waiting for the simulation module to appear. Doing the engine first
    // meant Steam was not initialized until that poll finished, so on any build where
    // the module never appears the lobby was dead for two minutes for no reason.
    // Order now reflects the dependency: Steam, then engine.
    const std::string network_failure = BringUpNetworking(*state);
    if (!network_failure.empty()) {
        MPE_LOG_ERROR("{}", network_failure);
        MPE_LOG_ERROR("the mod is loaded but cannot host or join. The game is unaffected.");
    }

    // The engine control is created once, up front, and never replaced.
    //
    // LobbyManager holds a reference to it, so swapping the object later would leave
    // that reference dangling. Binding status is reported through the log and through
    // the globals surface instead, both of which can appear later without
    // invalidating anything already handed out.
    // The campaign path, not the inert one.
    //
    // Every launch this project has ever made went through the engine's own reflected
    // campaign entry point, and it works. The inert control was installed regardless, so
    // the lobby's launch sequence, which drives everything through this interface, refused
    // at its first step: no countdown, no LaunchNow, and a client left in the lobby while
    // the host loaded alone.
    //
    // The reflection stays on the Unreal side. This hands the engine control a callable
    // and nothing else, so Engine/ still knows nothing about object arrays or the game
    // thread.
    // Posted, and taking nothing that the tick holds.
    //
    // This runs from inside LobbyManager::Tick, which the tick loop calls while holding
    // g_state_mutex, so the two things this must not do are take that lock and wait.
    //
    // It did both. The job reached back for g_state_mutex on the game thread while the tick
    // loop held it, so the game thread blocked on the lobby and the lobby blocked on the
    // game thread: sixty seconds of frozen game, and then a wait that gave up and returned
    // while the job was still holding references into the frame it had just left. The job
    // wrote its result into those bytes once the lock finally came free, which is the
    // unhandled exception both machines died with.
    //
    // The object array and the reflection now come from the engine view, which has its own
    // lock and is never held across a tick, and the campaign call is posted rather than
    // waited on. Beginning a match is a load: nothing useful can be reported back from it
    // synchronously, and blocking the lobby for the duration stops the keepalives and every
    // screen update at precisely the moment the other machines are watching for them.
    state->engine = std::make_unique<engine::CampaignEngineControl>(
        [](std::string_view scenario, bool friendly_fire) -> Result {
            std::optional<unreal::ObjectArray> objects_copy;
            std::optional<unreal::Reflection>  reflection_copy;
            if (!TakeEngineView(objects_copy, reflection_copy)) {
                return Result::Fail(ErrorCode::InvalidState, "UE reflection is not ready yet");
            }

            // Everything the job needs is copied into it. Nothing is captured by reference,
            // because nobody is waiting and there is no frame left to refer to.
            const std::string wanted(scenario);
            return unreal::PostToGameThread(
                [objects_copy, reflection_copy, wanted, friendly_fire]() {
                    const Result began =
                        unreal::BeginCampaign(*objects_copy, *reflection_copy, wanted,
                                              kDefaultCampaignAsset, friendly_fire);
                    if (!began.ok()) {
                        MPE_LOG_ERROR("beginning the campaign failed: {}", began.message());
                        ReportLaunchFailure(began.message());
                    }
                });
        });

    // The lobby manager needs both a backend and a transport. Without them the mod
    // stays loaded and the globals surface still works, which is why this is not a
    // hard failure.
    if (network_failure.empty()) {
        state->manager = std::make_unique<lobby::LobbyManager>(
            *state->backend, *state->transport, *state->engine, state->sink);
    } else {
        MPE_LOG_WARN("no lobby manager: networking is unavailable");
    }

    // Published before the engine poll, so the Steam surface and the lobby are usable
    // immediately rather than only after it completes. Publishing late was a real bug:
    // every export reported InvalidState for the whole two minute poll.
    {
        std::lock_guard lock(g_state_mutex);
        g_state = std::move(state);
    }

    // Reflection, immediately after publishing and before any of the waits below.
    //
    // The menu entry has to exist the moment the menu does. The searching path could not
    // manage that: waiting for the simulation module, waiting for loading to settle and
    // then scanning the heap came to most of a minute, by which point the menu had been on
    // screen long enough for anything added to it to look like an afterthought.
    //
    // Reading both globals from instructions that reference them needs only the static
    // image, so it runs now and costs a moment. If it does not work out, the slower search
    // further down still runs and nothing is lost but the head start.
    g_reflection_was_fast = TryFastReflection();
    if (g_reflection_was_fast) {
        MPE_LOG_INFO("UE reflection ready early; the menu entry can be in place before the "
                    "menu is first drawn");

        // Work out how to call functions now rather than when the menu first appears.
        //
        // Detection compares virtual tables across unrelated classes, which the partly
        // built graph already supports. Leaving it until the menu turns up meant the entry
        // arrived a couple of seconds after the menu had been drawn, which is exactly the
        // late arrival worth avoiding. If there are not yet enough distinct classes, the
        // menu watcher retries later and nothing is lost.
        unreal::CallLayout layout;
        std::lock_guard    lock(g_state_mutex);
        if (g_state && g_state->objects.has_value()) {
            if (unreal::DetectCallLayout(*g_state->objects, layout).ok()) {
                MPE_LOG_INFO("call layout ready early too; the entry can go in on the first "
                            "menu tick");
            }
        }
    }

    g_ready.store(true, std::memory_order_release);
    g_running.store(true, std::memory_order_release);
    g_tick_thread = std::thread(&TickLoop);

    MPE_LOG_INFO("MultiplayerEvolved ready ({})",
                network_failure.empty() ? "lobby available" : "lobby unavailable");

    // Asked once, in the background, and applied at the next start. Nothing waits on it.
    {
        std::lock_guard lock(g_version_mutex);
        // Trailing separator, because the staged file is written next to the mod itself.
        g_binaries_directory = (std::filesystem::path(ExecutableDirectory()) / L"").wstring();
    }
    StartUpdateCheck();

    // Engine binding last. Waiting for the module is lock free; only the commit takes
    // the lock, and it must happen with the ModuleImage already in its final home
    // because SymbolRegistry stores a pointer to it.
    ResolveEngineBinding();

    // The UE5 side, which is where the game engine variant, player traits and social
    // options live. Locating the name pool is the prerequisite for reaching any of it.
    //
    // Gated on the game being up: scanning during startup contends with the asset
    // loader badly enough to leave the game stuck on its splash screen.
    // THE STARTUP CRASH AND ITS FIX
    //
    // This scanning probes hundreds of thousands of addresses in the executable's data.
    // Run during startup it contended with the game's asset loader badly enough that the
    // game intermittently never left its splash screen, sitting at 377 MB with 12 s of
    // CPU. Three defects caused it, all fixed:
    //
    //   1. Reads were unguarded. The header claimed a structured exception handler
    //      protected them; it did not exist. GuardedRead now wraps every access in
    //      __try/__except, so a page freed by the loader between the check and the read
    //      returns false instead of faulting inside the game.
    //
    //   2. The scan made a syscall per candidate. Block pointers are scattered heap
    //      addresses, so each readability check missed the region cache and became a
    //      real VirtualQuery, around 500000 of them, each taking the process address
    //      space lock. A cheap arithmetic pre filter now rejects garbage first.
    //
    //   3. It ran at the worst possible moment. The gate below waits for the game's own
    //      window, which only appears once the engine is up and the heavy loading is
    //      done, then pauses briefly so the first frames are not competing either.
    const auto aborted = []() { return !g_running_or_starting.load(std::memory_order_acquire); };

    // Two gates, both observations rather than durations.
    //
    // The window appearing means the engine reached its main loop. Waiting for the
    // process to then go quiet means the asset streaming that follows has finished too.
    // On a fast machine both pass in seconds; on a slow one they take as long as they
    // take, and neither can expire and leave the scan running at the wrong moment.
    // Both gates exist to keep the heap scan away from the asset loader. When there is no
    // heap scan to keep away, waiting for them buys nothing and costs about twenty
    // seconds, which is twenty seconds of the mod being unusable for no reason.
    //
    // The fast path resolves the pool and the array from the static image in about two
    // seconds and touches almost nothing while doing it. All that is left afterwards is
    // reading a property chain out of a handful of live structs, which is cheap enough
    // that scheduling it around the loader would be superstition.
    const bool need_the_search = !g_reflection_was_fast;
    if (need_the_search) {
        if (!pacing::WaitFor("waiting for the game window", &HasGameWindow, aborted)) {
            MPE_LOG_WARN("the game window never appeared; skipping UE reflection");
            return;
        }
        MPE_LOG_INFO("game window is up; waiting for loading to settle before scanning");

        if (!pacing::WaitForQuiet("waiting for the game to finish loading", aborted)) {
            MPE_LOG_WARN("the game never went quiet; skipping UE reflection rather than "
                        "competing with a machine that is still working");
            return;
        }
        MPE_LOG_INFO("the game is idle; starting UE reflection");
    }

    ResolveUnrealReflection();
}

void Shutdown() {
    g_running_or_starting.store(false, std::memory_order_release);
    g_running.store(false, std::memory_order_release);

    // Before the join, not after.
    //
    // The tick thread can be sitting in a game thread dispatch, and a job whose deadline has
    // passed while it is already running is waited on rather than abandoned, because
    // abandoning it hands a running job a stack frame that has gone. During teardown the
    // game may never run another frame, so that wait would never end and the join would
    // never return. Cancelling the queue first releases the waiter.
    unreal::ShutdownGameThreadDispatch();
    if (g_tick_thread.joinable()) {
        g_tick_thread.join();
    }
    g_ready.store(false, std::memory_order_release);

    {
        std::lock_guard lock(g_state_mutex);
        if (g_state && g_state->manager) {
            // Leaves the lobby and closes every connection so peers see a clean
            // departure rather than a timeout.
            g_state->manager->LeaveSession();
        }
        g_state.reset();
    }

    // Released after the objects that use it, so no callback can be dispatched
    // into freed memory.
    debugtrap::Shutdown();

    steam::Shutdown();

    MPE_LOG_INFO("MultiplayerEvolved stopped");
    log::Shutdown();
}

DWORD WINAPI InitializeThread(LPVOID) {
    Initialize();
    return 0;
}

/// Runs an action against the lobby manager under the state lock, translating the
/// result into the C ABI convention: 0 on success, negative ErrorCode on failure.
template <typename Action>
[[nodiscard]] int WithManager(Action&& action) {
    std::lock_guard lock(g_state_mutex);
    if (!g_state || !g_state->manager) {
        return -static_cast<int>(ErrorCode::InvalidState);
    }
    const Result result = action(*g_state->manager);
    return result.ok() ? 0 : -static_cast<int>(result.code());
}

} // namespace
} // namespace mpe

// ---------------------------------------------------------------------------
// Public C API
// ---------------------------------------------------------------------------
//
// Convention: 0 means success, a negative value is the negated mpe::ErrorCode.
// MPE_LastErrorMessage returns the text for the most recent failure.

extern "C" {

/// True once the mod finished starting up and can accept commands.
__declspec(dllexport) int MPE_IsReady() {
    return mpe::g_ready.load(std::memory_order_acquire) ? 1 : 0;
}

__declspec(dllexport) const char* MPE_Version() {
    return MPE_VERSION_STRING;
}

/// Hosts a session. mode is a canonical mode name such as "capture_the_flag".
/// map_path may be null or empty for the base scenario with no custom layout.
__declspec(dllexport) int MPE_HostSession(const char* mode, const char* scenario,
                                         const char* map_path, int max_players,
                                         int friends_only) {
    if (mode == nullptr || scenario == nullptr) {
        return -static_cast<int>(mpe::ErrorCode::InvalidArgument);
    }

    mpe::engine::GameMode parsed_mode{};
    if (!mpe::engine::ParseGameMode(mode, parsed_mode)) {
        return -static_cast<int>(mpe::ErrorCode::InvalidArgument);
    }

    mpe::lobby::HostOptions options;
    options.visibility = (friends_only != 0) ? mpe::lobby::LobbyVisibility::FriendsOnly
                                             : mpe::lobby::LobbyVisibility::Public;
    options.max_players = static_cast<std::uint32_t>(max_players > 0 ? max_players : 8);
    options.settings.mode     = parsed_mode;
    options.settings.scenario = scenario;
    if (map_path != nullptr) {
        options.map_variant_path = map_path;
    }

    return mpe::WithManager([&](mpe::lobby::LobbyManager& manager) {
        return manager.HostSession(options);
    });
}

__declspec(dllexport) int MPE_JoinSession(unsigned long long lobby_id) {
    return mpe::WithManager([&](mpe::lobby::LobbyManager& manager) {
        return manager.JoinSession(lobby_id);
    });
}

__declspec(dllexport) int MPE_LeaveSession() {
    std::lock_guard lock(mpe::g_state_mutex);
    if (!mpe::g_state || !mpe::g_state->manager) {
        return -static_cast<int>(mpe::ErrorCode::InvalidState);
    }
    mpe::g_state->manager->LeaveSession();
    return 0;
}

__declspec(dllexport) int MPE_StartMatch() {
    return mpe::WithManager(
        [](mpe::lobby::LobbyManager& manager) { return manager.StartCountdown(); });
}

__declspec(dllexport) int MPE_OpenInviteOverlay() {
    return mpe::WithManager(
        [](mpe::lobby::LobbyManager& manager) { return manager.OpenInviteOverlay(); });
}

__declspec(dllexport) int MPE_SelectMap(const char* map_path) {
    if (map_path == nullptr) {
        return -static_cast<int>(mpe::ErrorCode::InvalidArgument);
    }
    return mpe::WithManager([&](mpe::lobby::LobbyManager& manager) {
        return manager.SelectMapVariant(map_path);
    });
}

__declspec(dllexport) int MPE_SendChat(const char* text) {
    if (text == nullptr) {
        return -static_cast<int>(mpe::ErrorCode::InvalidArgument);
    }
    return mpe::WithManager(
        [&](mpe::lobby::LobbyManager& manager) { return manager.SendChat(text); });
}

/// Current phase as a stable lowercase identifier, for example "in_lobby".
/// Never null. The returned pointer is valid until the next call.
__declspec(dllexport) const char* MPE_Phase() {
    static thread_local std::string phase;
    std::lock_guard lock(mpe::g_state_mutex);
    if (!mpe::g_state || !mpe::g_state->manager) {
        phase = "unavailable";
        return phase.c_str();
    }
    phase = mpe::lobby::ToString(mpe::g_state->manager->Phase());
    return phase.c_str();
}

/// Writes a value into a stride 0x18 descriptor record by name. Returns 0 on success,
/// or a negated mpe::ErrorCode.
///
/// This does exactly what it says and no more. A successful return means the memory was
/// written and can be read back. It does NOT mean the engine observed the change:
/// cross reference analysis found zero references from .text to any record field or any
/// table base, so this data appears to be residual rather than live. See
/// docs/04-ENGINE-BINDING.md.
///
/// It refuses stride 0x10 string id records, so asking it to enable something like
/// "forge_main_menu_palettes" fails with an explanation rather than appearing to work.
__declspec(dllexport) int MPE_SetGlobal(const char* name, unsigned long long value) {
    if (name == nullptr) {
        return -static_cast<int>(mpe::ErrorCode::InvalidArgument);
    }
    std::lock_guard lock(mpe::g_state_mutex);
    if (!mpe::g_state || !mpe::g_state->globals.has_value()) {
        return -static_cast<int>(mpe::ErrorCode::InvalidState);
    }
    const mpe::Result result = mpe::g_state->globals->SetNumber(name, value);
    return result.ok() ? 0 : -static_cast<int>(result.code());
}

/// Reads a writable engine global by name. Returns 0 on success and writes through
/// out_value, which must not be null.
__declspec(dllexport) int MPE_GetGlobal(const char* name, unsigned long long* out_value) {
    if (name == nullptr || out_value == nullptr) {
        return -static_cast<int>(mpe::ErrorCode::InvalidArgument);
    }
    std::lock_guard lock(mpe::g_state_mutex);
    if (!mpe::g_state || !mpe::g_state->globals.has_value()) {
        return -static_cast<int>(mpe::ErrorCode::InvalidState);
    }
    const auto value = mpe::g_state->globals->GetNumber(name);
    if (!value.ok()) {
        return -static_cast<int>(value.code());
    }
    *out_value = value.value();
    return 0;
}

/// Writes every global whose name contains the filter into the log, with its type
/// and current value. Pass an empty string for all of them.
///
/// This is how a player or contributor finds what is available without needing the
/// probe tools or a copy of the symbol dump.
__declspec(dllexport) int MPE_LogGlobals(const char* name_contains, int max_results) {
    std::lock_guard lock(mpe::g_state_mutex);
    if (!mpe::g_state || !mpe::g_state->globals.has_value()) {
        return -static_cast<int>(mpe::ErrorCode::InvalidState);
    }

    const std::string filter(name_contains != nullptr ? name_contains : "");
    const auto listed = mpe::g_state->globals->List(
        filter, max_results > 0 ? static_cast<std::size_t>(max_results) : 64);

    mpe::log::Write(mpe::log::Level::Info, "Mod",
                   std::format("{} global(s) matching '{}':", listed.size(), filter));
    for (const auto& info : listed) {
        mpe::log::Write(mpe::log::Level::Info, "Mod",
                       std::format("  {:<64} type={} value=0x{:X}{}", info.name, info.type,
                                   info.value, info.writable ? "" : "  (read only string id)"));
    }
    return static_cast<int>(listed.size());
}

/// Creates a real Steam lobby, publishes join metadata, reads it back, then leaves.
///
/// This exercises the entire metadata plane end to end against the live Steam
/// backend: CreateLobby, the LobbyCreated call result, the callback ABI, metadata
/// publish and read, and LeaveLobby. It deliberately drives ILobbyBackend directly
/// rather than LobbyManager::HostSession, because HostSession is gated on the engine
/// binding and this test is about the lobby layer, not the engine.
///
/// Returns 0 on success, or a negated mpe::ErrorCode. Blocks for up to
/// timeout_seconds while polling, so call it from a worker thread, not a render
/// thread.
__declspec(dllexport) int MPE_LobbySelfTest(int timeout_seconds) {
    using namespace std::chrono;

    // Minimal observer that records the outcome. Declared locally because it exists
    // only for the duration of this test.
    class TestObserver final : public mpe::lobby::ILobbyBackendObserver {
    public:
        bool             created{false};
        bool             failed{false};
        mpe::lobby::LobbyId lobby{0};
        std::string      detail;

        void OnLobbyCreated(mpe::lobby::LobbyId id) override {
            created = true;
            lobby   = id;
        }
        void OnLobbyCreateFailed(const mpe::Error& error) override {
            failed = true;
            detail = error.message;
        }
        void OnLobbyEntered(mpe::lobby::LobbyId id, bool) override { lobby = id; }
        void OnLobbyEnterFailed(const mpe::Error& error) override {
            failed = true;
            detail = error.message;
        }
        void OnMemberJoined(const mpe::lobby::LobbyMember&) override {}
        void OnMemberLeft(mpe::lobby::PlatformId, bool) override {}
        void OnLobbyDataChanged(mpe::lobby::LobbyId) override {}
        void OnMemberDataChanged(mpe::lobby::PlatformId) override {}
        void OnJoinRequested(mpe::lobby::LobbyId, mpe::lobby::PlatformId) override {}
    };

    mpe::lobby::ILobbyBackend* backend = nullptr;
    {
        std::lock_guard lock(mpe::g_state_mutex);
        if (!mpe::g_state || !mpe::g_state->backend) {
            mpe::log::Write(mpe::log::Level::Error, "Mod",
                           "lobby self test: the Steam backend is not available");
            return -static_cast<int>(mpe::ErrorCode::SteamUnavailable);
        }
        backend = mpe::g_state->backend.get();
    }

    mpe::log::Write(mpe::log::Level::Info, "Mod", "lobby self test: creating a lobby");

    const mpe::Result created =
        backend->Create(mpe::lobby::LobbyVisibility::FriendsOnly, 8);
    if (!created.ok()) {
        mpe::log::Write(mpe::log::Level::Error, "Mod",
                       std::format("lobby self test: Create failed: {}", created.message()));
        return -static_cast<int>(created.code());
    }

    // Poll for the asynchronous result. Steam callbacks are dispatched by whichever
    // thread pumps them, and Poll drains our queue on this one.
    TestObserver observer;
    const auto   deadline = steady_clock::now() + seconds(timeout_seconds > 0 ? timeout_seconds : 15);
    while (steady_clock::now() < deadline && !observer.created && !observer.failed) {
        // When this process owns the Steam API it also owns the callback pump.
        // Without this the CreateLobby call result never fires and the test times out,
        // which is exactly what happened before it was added.
        mpe::steam::RunCallbacks();
        backend->Poll(observer);
        std::this_thread::sleep_for(milliseconds(100));
    }

    if (observer.failed) {
        mpe::log::Write(mpe::log::Level::Error, "Mod",
                       std::format("lobby self test: FAILED: {}", observer.detail));
        return -static_cast<int>(mpe::ErrorCode::LobbyUnavailable);
    }
    if (!observer.created) {
        mpe::log::Write(mpe::log::Level::Error, "Mod",
                       "lobby self test: timed out waiting for the lobby to be created");
        backend->Leave();
        return -static_cast<int>(mpe::ErrorCode::Timeout);
    }

    mpe::log::Write(mpe::log::Level::Info, "Mod",
                   std::format("lobby self test: lobby {} created, owner={}", observer.lobby,
                               backend->IsOwner()));

    // Read back the metadata a joining client would evaluate. This is the part that
    // proves a friend could actually find and validate this lobby.
    int failures = 0;
    for (const char* key : {mpe::lobby::keys::kProtocolVersion, mpe::lobby::keys::kGameBuild,
                            mpe::lobby::keys::kHostId}) {
        const auto value = backend->GetLobbyData(key);
        if (value.ok()) {
            mpe::log::Write(mpe::log::Level::Info, "Mod",
                           std::format("lobby self test:   {} = {}", key, value.value()));
        } else {
            ++failures;
            mpe::log::Write(mpe::log::Level::Error, "Mod",
                           std::format("lobby self test:   {} MISSING: {}", key,
                                       value.message()));
        }
    }

    // Member data still has to round trip, because the roster's display names travel that
    // way before a transport connection exists. The key used to be a ready flag, which no
    // longer exists, so the probe writes its own.
    if (const mpe::Result member = backend->SetMemberData("mpe.probe", "1"); member.ok()) {
        const auto local = backend->LocalId();
        if (local.ok()) {
            const auto read_back =
                backend->GetMemberData(local.value(), "mpe.probe");
            mpe::log::Write(mpe::log::Level::Info, "Mod",
                           std::format("lobby self test:   member data round trip: {}",
                                       read_back.ok() ? read_back.value() : "MISSING"));
            if (!read_back.ok()) {
                ++failures;
            }
        }
    } else {
        ++failures;
    }

    mpe::log::Write(mpe::log::Level::Info, "Mod", "lobby self test: leaving the lobby");
    backend->Leave();

    if (failures > 0) {
        mpe::log::Write(mpe::log::Level::Error, "Mod",
                       std::format("lobby self test: completed with {} failure(s)", failures));
        return -static_cast<int>(mpe::ErrorCode::LobbyUnavailable);
    }

    mpe::log::Write(mpe::log::Level::Info, "Mod", "lobby self test: PASSED");
    return 0;
}

/// Opens the Steam invite overlay for the current lobby. Exposed separately from the
/// lobby manager so the metadata plane can be driven without the engine.
__declspec(dllexport) int MPE_TestInviteOverlay() {
    std::lock_guard lock(mpe::g_state_mutex);
    if (!mpe::g_state || !mpe::g_state->backend) {
        return -static_cast<int>(mpe::ErrorCode::SteamUnavailable);
    }
    const mpe::Result result = mpe::g_state->backend->OpenInviteOverlay();
    return result.ok() ? 0 : -static_cast<int>(result.code());
}

/// Sets every located copy of the friendly fire flag. Returns how many were written.
///
/// Several independent copies exist. Which one a match actually consults is not yet
/// established, so this writes all of them rather than pretending to know.
__declspec(dllexport) int MPE_SetFriendlyFire(int enabled) {
    std::lock_guard lock(mpe::g_state_mutex);
    if (!mpe::g_state) {
        return -static_cast<int>(mpe::ErrorCode::InvalidState);
    }

    const std::uint8_t byte = (enabled != 0) ? 1u : 0u;
    int written = 0;
    for (mpe::ModState::WatchedField& field : mpe::g_state->watched) {
        if (!field.valid) {
            continue;
        }
        if (mpe::unreal::memory::WriteBytes(field.address, &byte, sizeof(byte))) {
            // Updated so the watcher reports the game overwriting us, not our own store.
            field.last_value = (byte != 0);
            ++written;
            mpe::log::Write(mpe::log::Level::Info, "Mod",
                           std::format("set {} = {}", field.label, byte != 0));
        }
    }
    return written;
}

/// Logs the current value of every located copy. Returns how many were readable.
__declspec(dllexport) int MPE_LogFriendlyFire() {
    std::lock_guard lock(mpe::g_state_mutex);
    if (!mpe::g_state) {
        return -static_cast<int>(mpe::ErrorCode::InvalidState);
    }
    int readable = 0;
    for (const mpe::ModState::WatchedField& field : mpe::g_state->watched) {
        std::uint8_t raw = 0;
        if (field.valid && mpe::unreal::memory::GuardedRead(field.address, &raw, sizeof(raw))) {
            ++readable;
            mpe::log::Write(mpe::log::Level::Info, "Mod",
                           std::format("  {} = {} (0x{:X})", field.label, raw != 0,
                                       field.address));
        }
    }
    return readable;
}

// Defined below; MPE_Command dispatches to it.
__declspec(dllexport) int MPE_DumpDiagnostics();

/// Runs one text command. The universal control channel.
///
/// WHY A TEXT COMMAND RATHER THAN MORE EXPORTS
///
/// The mod runs inside the game, and there is no in game UI to drive it. The practical
/// way to reach it from outside is CreateRemoteThread, which can only call a function
/// taking a single pointer sized argument. A text command fits that exactly: the caller
/// writes a string into the game's address space and starts a thread at this function.
///
/// One entry point therefore covers every operation, present and future, without adding
/// an export per feature or needing a shim to marshal multiple arguments.
///
/// Commands:
///   ff status            log every friendly fire copy and its value
///   ff on | ff off       set every located copy
///   diag                 log the symbol discovery report
///   globals <substring>  log matching engine globals
///   watch                log the current watch list
///
/// Returns 0 on success, or a negated mpe::ErrorCode.
__declspec(dllexport) int MPE_Command(const char* command_line) {
    if (command_line == nullptr) {
        return -static_cast<int>(mpe::ErrorCode::InvalidArgument);
    }

    // Copied immediately: the memory belongs to the caller's remote allocation and may
    // be freed the moment the thread returns.
    std::string command;
    try {
        command.assign(command_line);
    } catch (...) {
        return -static_cast<int>(mpe::ErrorCode::InvalidArgument);
    }
    if (command.size() > 512) {
        command.resize(512);
    }

    mpe::log::Write(mpe::log::Level::Info, "Mod", std::format("command: '{}'", command));

    const auto starts_with = [&command](std::string_view prefix) {
        return command.rfind(prefix, 0) == 0;
    };

    if (starts_with("ff status")) {
        return MPE_LogFriendlyFire();
    }
    if (starts_with("ff on")) {
        return MPE_SetFriendlyFire(1);
    }
    if (starts_with("ff off")) {
        return MPE_SetFriendlyFire(0);
    }
    if (starts_with("diag")) {
        return MPE_DumpDiagnostics();
    }
    if (starts_with("globals")) {
        const std::size_t space = command.find(' ');
        const std::string filter =
            (space == std::string::npos) ? std::string{} : command.substr(space + 1);
        return MPE_LogGlobals(filter.c_str(), 64);
    }
    if (starts_with("lobbytest")) {
        return MPE_LobbySelfTest(20);
    }
    if (starts_with("host")) {
        // Defaults chosen so the command is useful with no arguments. The scenario is a
        // campaign level because those are the only levels this build ships, and
        // friends_only keeps a test host off the public list.
        return MPE_HostSession("slayer", "a30", "/Game/Levels/Halo1/Solo/A30/A30", 4, 1);
    }
    if (starts_with("leave")) {
        return MPE_LeaveSession();
    }
    if (starts_with("invite")) {
        return MPE_OpenInviteOverlay();
    }
    if (starts_with("events")) {
        // Names every event seen on the multiplayer button, and marks the most recent one.
        // Click the button, then run this: whatever appears at the end is the click.
        std::lock_guard lock(mpe::g_state_mutex);
        if (!mpe::g_state || !mpe::g_state->objects.has_value()) {
            return -static_cast<int>(mpe::ErrorCode::InvalidState);
        }
        const std::vector<std::uintptr_t> seen = mpe::unreal::SeenWidgetEvents();
        const std::uintptr_t              last = mpe::unreal::LastWidgetEvent();

        mpe::log::Write(mpe::log::Level::Info, "Mod",
                       std::format("{} distinct event(s) seen on the watched widget",
                                   seen.size()));
        for (const std::uintptr_t function : seen) {
            std::string name = "?";
            mpe::g_state->objects->ForEach([&](const mpe::unreal::ObjectInfo& object) {
                if (object.address != function) {
                    return true;
                }
                name = object.name;
                return false;
            });
            mpe::log::Write(mpe::log::Level::Info, "Mod",
                           std::format("  0x{:X}  {}{}", function, name,
                                       function == last ? "   <- most recent" : ""));
        }
        return static_cast<int>(seen.size());
    }
    if (starts_with("uiprobe")) {
        // Draws one large red rectangle and nothing else, to establish whether a widget
        // created by this mod can be drawn at all. A whole screen that builds without error
        // and shows nothing cannot distinguish a layout mistake from a mounting mistake;
        // this can.
        mpe::unreal::LobbyUIContext ui;
        mpe::Result                 outcome = mpe::Result::Success();
        {
            std::lock_guard lock(mpe::g_state_mutex);
            if (!mpe::g_state || !mpe::g_state->objects.has_value()) {
                return -static_cast<int>(mpe::ErrorCode::InvalidState);
            }
            if (const mpe::Result resolved =
                    mpe::unreal::ResolveLobbyUI(*mpe::g_state->objects, ui);
                !resolved.ok()) {
                mpe::log::Write(mpe::log::Level::Error, "Mod",
                               std::format("probe unavailable: {}", resolved.message()));
                return -static_cast<int>(mpe::ErrorCode::InvalidState);
            }
        }
        const mpe::Result ran = mpe::unreal::RunOnGameThread(
            [&]() { outcome = mpe::unreal::ProbeLobbyUI(ui); }, 20000);
        if (!ran.ok() || !outcome.ok()) {
            mpe::log::Write(mpe::log::Level::Error, "Mod",
                           std::format("probe failed: {}",
                                       ran.ok() ? outcome.message() : ran.message()));
            return -static_cast<int>(mpe::ErrorCode::InvalidState);
        }
        return 0;
    }
    if (starts_with("mpbutton")) {
        mpe::Result     outcome = mpe::Result::Success();
        std::uintptr_t button  = 0;
        const mpe::Result ran = mpe::unreal::RunOnGameThread(
            [&]() {
                std::lock_guard lock(mpe::g_state_mutex);
                if (!mpe::g_state || !mpe::g_state->objects.has_value()) {
                    outcome = mpe::Result::Fail(mpe::ErrorCode::InvalidState, "not ready");
                    return;
                }
                outcome = mpe::unreal::AddMainMenuButton(*mpe::g_state->objects, "MULTIPLAYER",
                                                        button);
            },
            20000);
        if (!ran.ok()) {
            mpe::log::Write(mpe::log::Level::Error, "Mod",
                           std::format("could not reach the game thread: {}", ran.message()));
            return -static_cast<int>(mpe::ErrorCode::Timeout);
        }
        if (!outcome.ok()) {
            mpe::log::Write(mpe::log::Level::Error, "Mod",
                           std::format("mpbutton failed: {}", outcome.message()));
            return -static_cast<int>(mpe::ErrorCode::InvalidState);
        }
        return 0;
    }
    if (starts_with("menu")) {
        // Reports the live main menu and what its button container and existing entries
        // actually are. Adding an entry means creating one of the same class the game
        // already uses, so that class has to be read rather than guessed.
        //   +0x560 MainButtonContainer   +0x568 PlayCoop   +0x508 CampaignMenuButton
        std::lock_guard lock(mpe::g_state_mutex);
        if (!mpe::g_state || !mpe::g_state->objects.has_value()) {
            return -static_cast<int>(mpe::ErrorCode::InvalidState);
        }
        const mpe::unreal::ObjectArray& objects = *mpe::g_state->objects;

        std::uintptr_t menu = 0;
        objects.ForEach([&](const mpe::unreal::ObjectInfo& object) {
            if (object.name.rfind("Default__", 0) == 0 ||
                object.class_name != "WBP_MainMenu_C") {
                return true;
            }
            menu = object.address;
            return false;
        });
        if (menu == 0) {
            mpe::log::Write(mpe::log::Level::Warn, "Mod", "no live WBP_MainMenu_C");
            return -static_cast<int>(mpe::ErrorCode::InvalidState);
        }
        mpe::log::Write(mpe::log::Level::Info, "Mod",
                       std::format("main menu at 0x{:X}", menu));

        struct Slot { const char* label; std::uintptr_t offset; };
        static constexpr Slot kSlots[] = {
            {"MainButtonContainer", 0x560}, {"PlayCoop", 0x568},
            {"CampaignMenuButton", 0x508},  {"QuitButton", 0x570},
        };
        for (const Slot& slot : kSlots) {
            std::uintptr_t pointer = 0;
            if (!mpe::unreal::memory::GuardedRead(menu + slot.offset, &pointer,
                                                 sizeof(pointer)) ||
                pointer == 0) {
                mpe::log::Write(mpe::log::Level::Warn, "Mod",
                               std::format("  {:<22} unreadable", slot.label));
                continue;
            }
            std::string class_name = "?";
            std::string object_name = "?";
            objects.ForEach([&](const mpe::unreal::ObjectInfo& candidate) {
                if (candidate.address != pointer) {
                    return true;
                }
                class_name  = candidate.class_name;
                object_name = candidate.name;
                return false;
            });
            mpe::log::Write(mpe::log::Level::Info, "Mod",
                           std::format("  {:<22} 0x{:X}  {} : {}", slot.label, pointer,
                                       object_name, class_name));
        }
        return 0;
    }
    if (starts_with("ui ")) {
        // "ui show <WidgetClass>" / "ui hide"
        //
        // The widget is constructed by the engine and added to the viewport, so it is real
        // in game UI drawn and styled by the game, not an overlay painted on top.
        static std::uintptr_t s_open_widget = 0;

        const std::string verb = command.substr(std::strlen("ui "));
        mpe::Result        outcome = mpe::Result::Success();
        std::uintptr_t    created = 0;

        const mpe::Result ran = mpe::unreal::RunOnGameThread(
            [&]() {
                std::lock_guard lock(mpe::g_state_mutex);
                if (!mpe::g_state || !mpe::g_state->objects.has_value()) {
                    outcome = mpe::Result::Fail(mpe::ErrorCode::InvalidState, "not ready");
                    return;
                }
                if (verb.rfind("show ", 0) == 0) {
                    outcome = mpe::unreal::ShowWidget(*mpe::g_state->objects,
                                                     verb.substr(5), created);
                } else if (verb.rfind("hide", 0) == 0) {
                    outcome = mpe::unreal::HideWidget(*mpe::g_state->objects, s_open_widget);
                } else {
                    outcome = mpe::Result::Fail(mpe::ErrorCode::InvalidArgument,
                                               "use: ui show <WidgetClass> | ui hide");
                }
            },
            20000);

        if (!ran.ok()) {
            mpe::log::Write(mpe::log::Level::Error, "Mod",
                           std::format("could not reach the game thread: {}", ran.message()));
            return -static_cast<int>(mpe::ErrorCode::Timeout);
        }
        if (!outcome.ok()) {
            mpe::log::Write(mpe::log::Level::Error, "Mod",
                           std::format("ui {} failed: {}", verb, outcome.message()));
            return -static_cast<int>(mpe::ErrorCode::InvalidState);
        }
        if (created != 0) {
            s_open_widget = created;
        }
        return 0;
    }
    if (starts_with("traits")) {
        // Reads, and optionally sets, the per player traits carried by the live campaign
        // variant. These are plain struct fields, so they are written directly rather than
        // through a function call.
        //
        // Layout, all taken from the reflected property offsets rather than assumed:
        //   BlamGameEngineCampaignVariant +0x28 CampaignVariantStorage
        //   CampaignVariantStorage        +0x0C PerPlayerTraits .. +0x40 PerPlayerXuidMapping
        // That span is 0x34 bytes and BlamGameEnginePlayerTraits is 0xD, so it holds four
        // entries, one per fireteam slot.
        //   BlamGameEnginePlayerTraits  +0x0 Vitality  +0x4 Weapons  +0x9 Movement
        //   Vitality: DamageResistance, BodyRecharge, ShieldRecharge, Deathless
        //   Weapons:  Damage, MeleeDamage, RechargingGrenades, WeaponPickup, InfiniteAmmo
        //   Movement: Speed, Gravity
        constexpr std::uintptr_t kTraitsBase   = 0x28 + 0x0C;
        constexpr std::size_t    kTraitStride  = 0x0D;
        constexpr int            kPlayerSlots  = 4;

        struct Field {
            const char*   name;
            std::uintptr_t offset;
        };
        static constexpr Field kFields[] = {
            {"damageresist", 0x00}, {"bodyrecharge", 0x01}, {"shieldrecharge", 0x02},
            {"deathless", 0x03},    {"damage", 0x04},       {"meleedamage", 0x05},
            {"grenades", 0x06},     {"weaponpickup", 0x07}, {"infiniteammo", 0x08},
            {"speed", 0x09},        {"gravity", 0x0A},
        };

        std::lock_guard lock(mpe::g_state_mutex);
        if (!mpe::g_state || !mpe::g_state->objects.has_value()) {
            return -static_cast<int>(mpe::ErrorCode::InvalidState);
        }

        std::uintptr_t variant = 0;
        mpe::g_state->objects->ForEach([&](const mpe::unreal::ObjectInfo& object) {
            if (object.name.rfind("Default__", 0) == 0 ||
                object.class_name != "BlamGameEngineCampaignVariant") {
                return true;
            }
            variant = object.address;
            return false;
        });
        if (variant == 0) {
            mpe::log::Write(mpe::log::Level::Warn, "Mod",
                           "no live BlamGameEngineCampaignVariant; start a mission first");
            return -static_cast<int>(mpe::ErrorCode::InvalidState);
        }

        // Apply any "name=value" pairs given, to every player slot.
        const std::string arguments =
            command.size() > 6 ? command.substr(6) : std::string{};
        int applied = 0;
        for (const Field& field : kFields) {
            const std::size_t at = arguments.find(std::string(field.name) + "=");
            if (at == std::string::npos) {
                continue;
            }
            const int value = std::atoi(arguments.c_str() + at + std::strlen(field.name) + 1);
            if (value < 0 || value > 255) {
                continue;
            }
            const auto byte = static_cast<std::uint8_t>(value);
            for (int slot = 0; slot < kPlayerSlots; ++slot) {
                const std::uintptr_t address =
                    variant + kTraitsBase + (slot * kTraitStride) + field.offset;
                if (mpe::unreal::memory::GuardedWrite(address, &byte, sizeof(byte))) {
                    ++applied;
                }
            }
            mpe::log::Write(mpe::log::Level::Info, "Mod",
                           std::format("trait {} = {} on all {} slot(s)", field.name, value,
                                       kPlayerSlots));
        }

        // Report the resulting values so a change is visible rather than assumed.
        mpe::log::Write(mpe::log::Level::Info, "Mod",
                       std::format("traits on variant 0x{:X} ({} write(s) applied)", variant,
                                   applied));
        for (int slot = 0; slot < kPlayerSlots; ++slot) {
            std::string line = std::format("  player {}:", slot + 1);
            for (const Field& field : kFields) {
                std::uint8_t value = 0;
                const std::uintptr_t address =
                    variant + kTraitsBase + (slot * kTraitStride) + field.offset;
                if (!mpe::unreal::memory::GuardedRead(address, &value, sizeof(value))) {
                    value = 0xFF;
                }
                line += std::format(" {}={}", field.name, value);
            }
            mpe::log::Write(mpe::log::Level::Info, "Mod", line);
        }
        return applied;
    }
    if (starts_with("mp ")) {
        // The lobby surface. These are the game's own calls, so they behave exactly as the
        // menu does, which is why they are preferred over anything hand rolled.
        const std::string verb = command.substr(std::strlen("mp "));

        mpe::Result outcome = mpe::Result::Success();
        int        players = 0;

        const mpe::Result ran = mpe::unreal::RunOnGameThread([&]() {
            std::lock_guard lock(mpe::g_state_mutex);
            if (!mpe::g_state || !mpe::g_state->objects.has_value()) {
                outcome = mpe::Result::Fail(mpe::ErrorCode::InvalidState, "not ready");
                return;
            }
            const mpe::unreal::ObjectArray& objects = *mpe::g_state->objects;
            if (verb.rfind("open", 0) == 0) {
                outcome = mpe::unreal::CallSimple(objects, "MeteoriteLobbyNotifier",
                                                 "BeginAllowInvites");
            } else if (verb.rfind("close", 0) == 0) {
                outcome = mpe::unreal::CallSimple(objects, "MeteoriteLobbyNotifier",
                                                 "EndAllowInvites");
            } else if (verb.rfind("players", 0) == 0) {
                outcome = mpe::unreal::CallReturningInt(objects, "MeteoriteSquadLobbyViewModel",
                                                       "GetNumSquadMembers", players);
            } else {
                outcome = mpe::Result::Fail(mpe::ErrorCode::InvalidArgument,
                                           "use: mp open | mp close | mp players");
            }
        });

        if (!ran.ok()) {
            mpe::log::Write(mpe::log::Level::Error, "Mod",
                           std::format("could not reach the game thread: {}", ran.message()));
            return -static_cast<int>(mpe::ErrorCode::Timeout);
        }
        if (!outcome.ok()) {
            mpe::log::Write(mpe::log::Level::Error, "Mod",
                           std::format("mp {} failed: {}", verb, outcome.message()));
            return -static_cast<int>(mpe::ErrorCode::InvalidState);
        }
        return players;
    }
    if (starts_with("campaign ")) {
        // "campaign <scenario> [ff] [difficulty]"
        //
        // ff turns friendly fire on in the options the match starts with, which is what
        // makes players able to damage each other. difficulty is 0 to 3.
        std::string argument = command.substr(std::strlen("campaign "));
        std::string scenario = argument;
        std::string asset    = "DA_FirstPlayableCampaign";
        bool        friendly_fire = false;
        int         difficulty    = -1;

        if (const std::size_t space = argument.find(' '); space != std::string::npos) {
            scenario                 = argument.substr(0, space);
            const std::string rest   = argument.substr(space + 1);
            if (rest.find("ff") != std::string::npos) {
                friendly_fire = true;
            }
            for (const char character : rest) {
                if (character >= '0' && character <= '3') {
                    difficulty = character - '0';
                    break;
                }
            }
        }
        mpe::Result        outcome  = mpe::Result::Success();
        const mpe::Result  ran      = mpe::unreal::RunOnGameThread(
            [&]() {
                std::lock_guard lock(mpe::g_state_mutex);
                if (!mpe::g_state || !mpe::g_state->objects.has_value() ||
                    !mpe::g_state->reflection.has_value()) {
                    outcome = mpe::Result::Fail(mpe::ErrorCode::InvalidState, "not ready");
                    return;
                }
                outcome = mpe::unreal::BeginCampaign(*mpe::g_state->objects,
                                                    *mpe::g_state->reflection, scenario,
                                                    asset, friendly_fire, difficulty);
            },
            60000);
        if (!ran.ok()) {
            mpe::log::Write(mpe::log::Level::Error, "Mod",
                           std::format("could not reach the game thread: {}", ran.message()));
            return -static_cast<int>(mpe::ErrorCode::Timeout);
        }
        if (!outcome.ok()) {
            mpe::log::Write(mpe::log::Level::Error, "Mod",
                           std::format("campaign start failed: {}", outcome.message()));
            return -static_cast<int>(mpe::ErrorCode::InvalidState);
        }
        return 0;
    }
    if (starts_with("funcs ")) {
        // Lists reflected functions whose name contains a fragment, with their owner.
        //
        // ClientTravel completes but the transition afterwards faults in the game's own
        // code, which means level loading here does not go through Unreal's travel at all.
        // Finding the call the game makes for itself needs a search over function names,
        // not class names.
        const std::string fragment = command.substr(std::strlen("funcs "));

        std::lock_guard lock(mpe::g_state_mutex);
        if (!mpe::g_state || !mpe::g_state->objects.has_value()) {
            return -static_cast<int>(mpe::ErrorCode::InvalidState);
        }

        int matched = 0;
        mpe::g_state->objects->ForEach([&](const mpe::unreal::ObjectInfo& object) {
            if (object.class_name != "Function" && object.class_name != "DelegateFunction") {
                return true;
            }
            if (object.name.find(fragment) == std::string::npos) {
                return true;
            }
            mpe::log::Write(mpe::log::Level::Info, "Mod",
                           std::format("  {:<44} {}", object.name,
                                       mpe::g_state->objects->BuildPath(object)));
            return ++matched < 80;
        });
        mpe::log::Write(mpe::log::Level::Info, "Mod",
                       std::format("'{}' matched {} function(s)", fragment, matched));
        return matched;
    }
    if (starts_with("pe detect")) {
        std::lock_guard lock(mpe::g_state_mutex);
        if (!mpe::g_state || !mpe::g_state->objects.has_value()) {
            return -static_cast<int>(mpe::ErrorCode::InvalidState);
        }
        mpe::unreal::CallLayout layout;
        const mpe::Result detected =
            mpe::unreal::DetectCallLayout(*mpe::g_state->objects, layout);
        if (!detected.ok()) {
            mpe::log::Write(mpe::log::Level::Error, "Mod",
                           std::format("ProcessEvent detection failed: {}",
                                       detected.message()));
            return -static_cast<int>(mpe::ErrorCode::InvalidState);
        }
        return layout.vtable_slot;
    }
    if (starts_with("gt test")) {
        // Proves the game thread can be reached at all, before anything is called on it.
        // The job records the thread it ran on, which must differ from the caller's.
        const DWORD caller = ::GetCurrentThreadId();
        DWORD       ran_on = 0;
        const mpe::Result ran = mpe::unreal::RunOnGameThread([&ran_on]() {
            ran_on = ::GetCurrentThreadId();
        });
        if (!ran.ok()) {
            mpe::log::Write(mpe::log::Level::Error, "Mod",
                           std::format("game thread dispatch failed: {}", ran.message()));
            return -static_cast<int>(mpe::ErrorCode::Timeout);
        }
        mpe::log::Write(mpe::log::Level::Info, "Mod",
                       std::format("game thread dispatch OK: job ran on thread {}, caller "
                                   "was {}",
                                   ran_on, caller));
        return static_cast<int>(ran_on);
    }
    if (starts_with("exec ")) {
        const std::string commandText = command.substr(std::strlen("exec "));
        mpe::Result        outcome = mpe::Result::Success();
        const mpe::Result  ran = mpe::unreal::RunOnGameThread([&]() {
            std::lock_guard lock(mpe::g_state_mutex);
            if (!mpe::g_state || !mpe::g_state->objects.has_value()) {
                outcome = mpe::Result::Fail(mpe::ErrorCode::InvalidState, "no object array");
                return;
            }
            outcome = mpe::unreal::ExecuteConsoleCommand(*mpe::g_state->objects, commandText);
        });
        if (!ran.ok()) {
            mpe::log::Write(mpe::log::Level::Error, "Mod",
                           std::format("could not reach the game thread: {}", ran.message()));
            return -static_cast<int>(mpe::ErrorCode::Timeout);
        }
        if (!outcome.ok()) {
            mpe::log::Write(mpe::log::Level::Error, "Mod",
                           std::format("command failed: {}", outcome.message()));
            return -static_cast<int>(mpe::ErrorCode::InvalidState);
        }
        return 0;
    }
    if (starts_with("travel ") || starts_with("host listen") || starts_with("join ")) {
        // One code path for hosting and joining, because ClientTravel does both: a URL
        // ending in ?listen starts a server, and a URL that is an address joins one.
        std::string url;
        if (starts_with("host listen")) {
            url = "/Game/Levels/Halo1/Solo/A30/A30?listen";
        } else if (starts_with("join ")) {
            url = command.substr(std::strlen("join "));
        } else {
            url = command.substr(std::strlen("travel "));
        }

        mpe::Result       outcome = mpe::Result::Success();
        const mpe::Result ran = mpe::unreal::RunOnGameThread([&]() {
            std::lock_guard lock(mpe::g_state_mutex);
            if (!mpe::g_state || !mpe::g_state->objects.has_value()) {
                outcome = mpe::Result::Fail(mpe::ErrorCode::InvalidState, "no object array");
                return;
            }
            outcome = mpe::unreal::Travel(*mpe::g_state->objects, url);
        },
        // Travel tears down a world and loads another, which takes far longer than a
        // normal call. Five seconds reported a timeout for work that had in fact started
        // correctly, which is worse than useless because it looks like a failure.
        60000);
        if (!ran.ok()) {
            mpe::log::Write(mpe::log::Level::Error, "Mod",
                           std::format("could not reach the game thread: {}", ran.message()));
            return -static_cast<int>(mpe::ErrorCode::Timeout);
        }
        if (!outcome.ok()) {
            mpe::log::Write(mpe::log::Level::Error, "Mod",
                           std::format("travel failed: {}", outcome.message()));
            return -static_cast<int>(mpe::ErrorCode::InvalidState);
        }
        return 0;
    }
    if (starts_with("vtable ")) {
        // Dumps a live object's virtual table as module offsets.
        //
        // ProcessEvent is a UObject virtual and calling it is what makes every other piece
        // of this possible, but its slot index is not something to assume: guessing an
        // address from a plausible looking pattern has already produced two silent wrong
        // answers on this build. Dumping the table lets the slot be identified from
        // evidence, by comparing entries across unrelated classes and checking which are
        // shared, and by function size from the exception directory.
        const std::string wanted = command.substr(std::strlen("vtable "));

        std::lock_guard lock(mpe::g_state_mutex);
        if (!mpe::g_state || !mpe::g_state->objects.has_value()) {
            return -static_cast<int>(mpe::ErrorCode::InvalidState);
        }

        const std::vector<mpe::unreal::ObjectInfo> found =
            mpe::g_state->objects->FindByName(wanted, 8);
        const mpe::unreal::ObjectInfo* instance = nullptr;
        for (const mpe::unreal::ObjectInfo& candidate : found) {
            // Class objects and defaults share a vtable with the class machinery rather
            // than with instances, so a real instance is what is wanted here.
            if (candidate.class_name != "Class" &&
                candidate.name.rfind("Default__", 0) != 0) {
                instance = &candidate;
                break;
            }
        }
        if (instance == nullptr) {
            mpe::log::Write(mpe::log::Level::Warn, "Mod",
                           std::format("no live non default instance named '{}'", wanted));
            return 0;
        }

        std::uintptr_t table = 0;
        if (!mpe::unreal::memory::GuardedRead(instance->address, &table, sizeof(table)) ||
            table == 0) {
            mpe::log::Write(mpe::log::Level::Warn, "Mod", "could not read the virtual table");
            return 0;
        }

        const auto base = reinterpret_cast<std::uintptr_t>(::GetModuleHandleW(nullptr));
        mpe::log::Write(mpe::log::Level::Info, "Mod",
                       std::format("{} ({}) at 0x{:X}, vtable 0x{:X} (RVA 0x{:X})",
                                   instance->name, instance->class_name, instance->address,
                                   table, table - base));

        int printed = 0;
        for (int slot = 0; slot < 96; ++slot) {
            std::uintptr_t entry = 0;
            if (!mpe::unreal::memory::GuardedRead(table + slot * sizeof(entry), &entry,
                                                 sizeof(entry))) {
                break;
            }
            if (entry == 0) {
                break;
            }
            mpe::log::Write(mpe::log::Level::Info, "Mod",
                           std::format("  [{:>3}] 0x{:X}  RVA 0x{:X}", slot, entry,
                                       entry - base));
            ++printed;
        }
        return printed;
    }
    if (starts_with("props ")) {
        // Dumps the reflected properties of a class by name.
        //
        // Needed to answer whether a game mode can be built on what already exists. A
        // player state that already carries a score and a kill count, and already
        // replicates them, is most of a scoreboard; one that does not means that state has
        // to be invented and replicated by hand.
        const std::string wanted = command.substr(std::strlen("props "));

        std::lock_guard lock(mpe::g_state_mutex);
        if (!mpe::g_state || !mpe::g_state->objects.has_value() ||
            !mpe::g_state->reflection.has_value()) {
            return -static_cast<int>(mpe::ErrorCode::InvalidState);
        }

        const std::vector<mpe::unreal::ObjectInfo> found =
            mpe::g_state->objects->FindByName(wanted, 4);
        if (found.empty()) {
            mpe::log::Write(mpe::log::Level::Warn, "Mod",
                           std::format("no object named '{}'", wanted));
            return 0;
        }

        int total = 0;
        for (const mpe::unreal::ObjectInfo& object : found) {
            const std::vector<mpe::unreal::PropertyInfo> properties =
                mpe::g_state->reflection->ReadProperties(object.address);
            mpe::log::Write(mpe::log::Level::Info, "Mod",
                           std::format("{} ({}) has {} property(ies):", object.name,
                                       object.class_name, properties.size()));
            for (const mpe::unreal::PropertyInfo& property : properties) {
                // A StructProperty only reports its own class, which says nothing about
                // what is inside it. Resolving the inner type turns a wall of
                // "StructProperty" into a readable tree, which is what makes nested
                // settings like player traits explorable at all.
                std::string detail = property.type_name;
                if (property.type_name == "StructProperty") {
                    const std::uintptr_t inner =
                        mpe::g_state->reflection->ResolveStructPropertyInner(property.address);
                    if (inner != 0) {
                        std::string inner_name;
                        mpe::g_state->objects->ForEach(
                            [&](const mpe::unreal::ObjectInfo& candidate) {
                                if (candidate.address != inner) {
                                    return true;
                                }
                                inner_name = candidate.name;
                                return false;
                            });
                        detail = inner_name.empty()
                                     ? std::format("StructProperty -> 0x{:X}", inner)
                                     : std::format("StructProperty -> {}", inner_name);
                    }
                }
                mpe::log::Write(mpe::log::Level::Info, "Mod",
                               std::format("  +0x{:<4X} {:<40} {}", property.offset,
                                           property.name, detail));
                ++total;
            }
        }
        return total;
    }
    if (starts_with("pvp on")) {
        mpe::g_enforce_friendly_fire.store(true, std::memory_order_release);
        const int applied = MPE_SetFriendlyFire(1);
        mpe::log::Write(mpe::log::Level::Info, "Mod",
                       std::format("pvp: friendly fire enabled on {} field(s) and will be "
                                   "held on through level loads",
                                   applied));
        return applied;
    }
    if (starts_with("pvp off")) {
        mpe::g_enforce_friendly_fire.store(false, std::memory_order_release);
        const int applied = MPE_SetFriendlyFire(0);
        mpe::log::Write(mpe::log::Level::Info, "Mod", "pvp: friendly fire disabled");
        return applied;
    }
    if (starts_with("pvp status")) {
        mpe::log::Write(mpe::log::Level::Info, "Mod",
                       std::format("pvp: enforcement is {}",
                                   mpe::g_enforce_friendly_fire.load(std::memory_order_acquire)
                                       ? "ON"
                                       : "off"));
        return MPE_LogFriendlyFire();
    }
    if (starts_with("fname")) {
        mpe::unreal::TrampolineInfo info;
        const mpe::Result installed = mpe::unreal::InstallFNameTrampoline(info);
        if (!installed.ok()) {
            mpe::log::Write(mpe::log::Level::Error, "Mod",
                           std::format("FName adapter unavailable: {}", installed.message()));
            return -static_cast<int>(mpe::ErrorCode::InvalidState);
        }
        mpe::log::Write(mpe::log::Level::Info, "Mod",
                       std::format("FName adapter at 0x{:X} (RVA 0x{:X})", info.address,
                                   info.module_offset));

        // The name to resolve is an argument, because the default is a bad test.
        //
        // "None" is index 0, and 0 is also what a call that silently did nothing returns.
        // A correct answer and a total failure are indistinguishable, so proving the
        // adapter works needs a name whose index is not zero and is already known from the
        // pool walk.
        std::wstring wanted = L"None";
        if (const std::size_t space = command.find(' '); space != std::string::npos) {
            wanted = mpe::text::WidenUtf8(command.substr(space + 1));
        }

        std::uint32_t    index = 0;
        const mpe::Result tested = mpe::unreal::TestFNameTrampoline(wanted.c_str(), index);
        if (!tested.ok()) {
            mpe::log::Write(mpe::log::Level::Error, "Mod",
                           std::format("FName self test failed: {}", tested.message()));
            return -static_cast<int>(mpe::ErrorCode::InvalidState);
        }
        std::string narrow;
        for (const wchar_t character : wanted) {
            narrow.push_back(static_cast<char>(character));
        }
        mpe::log::Write(mpe::log::Level::Info, "Mod",
                       std::format("FName(\"{}\") resolved to index {}{}", narrow, index,
                                   index == 0 ? "  <- zero, which is both NAME_None and "
                                                "what a failed call returns"
                                              : "  <- non zero, the adapter really ran"));
        return static_cast<int>(index);
    }
    if (starts_with("objects")) {
        // Lists live objects whose class name contains a fragment. This is the general
        // purpose lens on the object graph, and the basis of the session capture below.
        const std::size_t space = command.find(' ');
        if (space == std::string::npos) {
            mpe::log::Write(mpe::log::Level::Warn, "Mod", "usage: objects <class name fragment>");
            return -static_cast<int>(mpe::ErrorCode::InvalidArgument);
        }
        return mpe::LogObjectsMatching(command.substr(space + 1), 400);
    }
    if (starts_with("net")) {
        // Captures the networking object graph in one pass.
        //
        // The game ships working co-op, so a session already stands up a net driver,
        // connections and remote player controllers. Enumerating them during a live
        // session is what shows which transport the game actually uses and what a
        // session is made of. Doing this in solo first gives the baseline to diff
        // against, because most of these classes exist with zero instances until a
        // session starts.
        static constexpr const char* kNetFragments[] = {
            "NetDriver",     "NetConnection", "OnlineSession", "GameSession",
            "PlayerState",   "PlayerController", "GameMode",   "GameState",
            "Party",         "Lobby",         "Matchmaking",   "Session",
            "Replication",   "Steam",         "Fireteam",      "Peer",
        };

        int total = 0;
        mpe::log::Write(mpe::log::Level::Info, "Mod",
                       "=== networking object graph ===================================");
        for (const char* fragment : kNetFragments) {
            total += mpe::LogObjectsMatching(fragment, 24);
        }
        mpe::log::Write(mpe::log::Level::Info, "Mod",
                       std::format("=== {} networking object(s) total ===", total));
        return total;
    }
    if (starts_with("trace ff")) {
        // Arms a hardware watchpoint on every located friendly fire copy, up to the
        // hardware limit of four. This is what identifies the consumer: whichever
        // instruction reads the byte gets recorded with its module and offset.
        std::lock_guard lock(mpe::g_state_mutex);
        if (!mpe::g_state) {
            return -static_cast<int>(mpe::ErrorCode::InvalidState);
        }
        mpe::debugtrap::DisarmAll();
        mpe::debugtrap::ClearHits();

        int armed = 0;
        for (const mpe::ModState::WatchedField& field : mpe::g_state->watched) {
            if (!field.valid || armed >= 4) {
                continue;
            }
            // Class default objects are templates that nothing plays from, so the four
            // scarce slots go to real instances.
            if (field.label.find("Default__") != std::string::npos) {
                continue;
            }
            const mpe::Result armed_ok =
                mpe::debugtrap::Arm(field.address, 1, mpe::debugtrap::Condition::ReadWrite,
                                   field.label);
            if (armed_ok.ok()) {
                ++armed;
                mpe::log::Write(mpe::log::Level::Info, "Mod",
                               std::format("tracing {} at 0x{:X}", field.label, field.address));
            } else {
                mpe::log::Write(mpe::log::Level::Warn, "Mod",
                               std::format("could not trace {}: {}", field.label,
                                           armed_ok.message()));
            }
        }
        mpe::log::Write(mpe::log::Level::Info, "Mod",
                       std::format("{} watchpoint(s) armed. Start a mission, then run "
                                   "'trace hits'.",
                                   armed));
        return armed;
    }
    if (starts_with("trace exec ")) {
        // Arms an execution breakpoint on a module offset, given as hex.
        //
        // This is how a candidate function is identified rather than assumed. A byte
        // pattern can only say that some bytes matched; arming the address and counting
        // how often it is reached says what the function actually is. Something called
        // once per frame on one thread is a tick. Something never reached is not.
        const std::string argument = command.substr(std::strlen("trace exec "));

        std::uintptr_t offset = 0;
        try {
            offset = static_cast<std::uintptr_t>(std::stoull(argument, nullptr, 16));
        } catch (const std::exception&) {
            mpe::log::Write(mpe::log::Level::Warn, "Mod",
                           std::format("'{}' is not a hex module offset", argument));
            return -static_cast<int>(mpe::ErrorCode::InvalidArgument);
        }

        const auto base = reinterpret_cast<std::uintptr_t>(::GetModuleHandleW(nullptr));
        const std::uintptr_t address = base + offset;

        const mpe::Result armed = mpe::debugtrap::Arm(
            address, 1, mpe::debugtrap::Condition::Execute, std::format("exec+0x{:X}", offset));
        if (!armed.ok()) {
            mpe::log::Write(mpe::log::Level::Error, "Mod",
                           std::format("could not arm execution trap: {}", armed.message()));
            return -static_cast<int>(mpe::ErrorCode::InvalidState);
        }
        mpe::log::Write(mpe::log::Level::Info, "Mod",
                       std::format("execution trap armed on RVA 0x{:X} (0x{:X}); run "
                                   "'trace hits' after a few seconds to see the call rate",
                                   offset, address));
        return 0;
    }
    if (starts_with("trace hits")) {
        const std::vector<mpe::debugtrap::Hit> hits = mpe::debugtrap::Hits();
        mpe::log::Write(mpe::log::Level::Info, "Mod",
                       std::format("{} distinct instruction(s) touched the watched byte(s)",
                                   hits.size()));
        for (const mpe::debugtrap::Hit& hit : hits) {
            mpe::log::Write(mpe::log::Level::Info, "Mod",
                           std::format("  {}+0x{:X} hit {} time(s) on address 0x{:X} "
                                       "(thread {}, rip 0x{:X})",
                                       hit.module_name, hit.module_offset, hit.count,
                                       hit.address, hit.thread_id, hit.instruction));
        }
        return static_cast<int>(hits.size());
    }
    if (starts_with("trace off")) {
        mpe::debugtrap::DisarmAll();
        return 0;
    }
    if (starts_with("watch")) {
        std::lock_guard lock(mpe::g_state_mutex);
        if (!mpe::g_state) {
            return -static_cast<int>(mpe::ErrorCode::InvalidState);
        }
        mpe::log::Write(mpe::log::Level::Info, "Mod",
                       std::format("{} field(s) watched", mpe::g_state->watched.size()));
        for (const mpe::ModState::WatchedField& field : mpe::g_state->watched) {
            mpe::log::Write(mpe::log::Level::Info, "Mod",
                           std::format("  {} at 0x{:X} = {} {}", field.label, field.address,
                                       field.last_value, field.valid ? "" : "(stale)"));
        }
        return static_cast<int>(mpe::g_state->watched.size());
    }

    mpe::log::Write(mpe::log::Level::Warn, "Mod", std::format("unknown command: '{}'", command));
    return -static_cast<int>(mpe::ErrorCode::NotFound);
}

/// Writes the symbol discovery report into the log. The single most useful thing
/// a user can do when asked to help diagnose a new game build.
__declspec(dllexport) int MPE_DumpDiagnostics() {
    std::lock_guard lock(mpe::g_state_mutex);
    if (!mpe::g_state) {
        return -static_cast<int>(mpe::ErrorCode::InvalidState);
    }
    if (!mpe::g_state->symbols.has_value()) {
        mpe::log::Write(mpe::log::Level::Error, "Mod",
                       "symbol discovery did not complete, so there is no report");
        return -static_cast<int>(mpe::ErrorCode::SymbolNotResolved);
    }
    mpe::log::Write(mpe::log::Level::Info, "Mod", mpe::g_state->symbols->BuildDiscoveryReport());
    return 0;
}

} // extern "C"

// ---------------------------------------------------------------------------
// Module lifecycle
// ---------------------------------------------------------------------------

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID reserved) {
    switch (reason) {
        case DLL_PROCESS_ATTACH: {
            mpe::g_self_module = module;
            ::DisableThreadLibraryCalls(module);

            // Startup polls for the engine and for Steam, so it cannot run under
            // the loader lock.
            const HANDLE thread =
                ::CreateThread(nullptr, 0, &mpe::InitializeThread, nullptr, 0, nullptr);
            if (thread != nullptr) {
                ::CloseHandle(thread);
            }
            break;
        }

        case DLL_PROCESS_DETACH:
            // On process termination Windows has already stopped other threads, so
            // joining ours would deadlock. Only clean up on an explicit unload.
            if (reserved == nullptr) {
                mpe::Shutdown();
            }
            break;

        default:
            break;
    }
    return TRUE;
}







