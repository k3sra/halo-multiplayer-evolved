// SPDX-License-Identifier: MIT
// ForgeEvolved: ModMain.cpp
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
// The exported FE_* functions are the surface a UI layer, a script host or a
// community front end drives. A flat C ABI is used deliberately: it is callable
// from any language, and it keeps the C++ types out of the boundary so a UI built
// against one mod version keeps working against the next.

#define FE_LOG_CATEGORY "Mod"

#include "Blam/DebugGlobals.h"
#include "Blam/ModuleImage.h"
#include "Blam/SymbolRegistry.h"
#include "Core/GameBuild.h"
#include "Core/Log.h"
#include "Core/Pacing.h"
#include "Debug/AccessTrap.h"
#include "Unreal/FNameTrampoline.h"
#include "Unreal/GameThread.h"
#include "Unreal/LobbyUI.h"
#include "Update/UpdateCheck.h"
#include "Engine/InertEngineControl.h"
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
#include <chrono>
#include <filesystem>
#include <format>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <string>
#include <thread>
#include <vector>

#include "Unreal/ProcessMemory.h"

namespace fe {
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
        FE_LOG_INFO("lobby phase {} -> {}", lobby::ToString(previous), lobby::ToString(current));
    }

    void OnSnapshotChanged(const lobby::LobbySnapshot& snapshot) override {
        // Logged at trace: this fires on every roster or progress change and would
        // drown the log at a higher level.
        FE_LOG_TRACE("snapshot: phase={} players={} ready={} countdown={} map={:.0f}%",
                     lobby::ToString(snapshot.phase), snapshot.players.size(),
                     snapshot.ReadyCount(), snapshot.countdown_seconds,
                     snapshot.map_transfer_progress * 100.0f);
    }

    void OnChatMessage(lobby::PlatformId author, std::string_view display_name,
                       std::string_view text) override {
        FE_LOG_INFO("chat {}: {}", display_name.empty() ? std::to_string(author)
                                                        : std::string(display_name),
                    text);
    }

    void OnError(const Error& error) override {
        FE_LOG_ERROR("lobby error [{}]: {}", ToString(error.code), error.message);
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
void SelectLobbyMap(int map_index);
void EnsureSessionHosted();
void InviteToSession(int team);
void ApplyServerFilter();
void PublishSessionDetails();
void CaptureServerName();
void OpenSessionInvite();
void RefreshLobbyStatus();
void PrepareLobby();

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

/// True while a slot has been pressed but the session is not yet ready to be invited to.
///
/// Creating a Steam lobby is asynchronous, so the first press always arrives too early.
/// Remembering it means the player's click is honoured a moment later rather than lost.
bool g_invite_pending = false;

/// This build's version, compared against the newest GitHub release to decide whether the
/// status panel should tell the player to update.
constexpr const char* kModVersion = "0.1.0";

/// The newest version seen on GitHub, empty until a check has succeeded.
///
/// Written by a one shot background thread and read by the status panel, so it is guarded
/// rather than shared raw: the two run on different threads from the moment the check
/// starts.
std::mutex  g_version_mutex;
std::string g_latest_version;

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
            FE_LOG_INFO("update check skipped: {}", release.message());
            return;
        }
        std::lock_guard lock(g_version_mutex);
        g_latest_version = release.value().version;
        if (update::IsNewer(g_latest_version, kModVersion)) {
            FE_LOG_INFO("an update is available: {} (this build is {})", g_latest_version,
                        kModVersion);
        } else {
            FE_LOG_INFO("this build is current ({})", kModVersion);
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
    return std::filesystem::path(ExecutableDirectory()) / "ForgeEvolved";
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
            FE_LOG_WARN("using built in symbol defaults: {}", loaded.message());
        }
    } else {
        FE_LOG_INFO("no descriptor at {}, using built in defaults", descriptor.string());
    }

    if (config.game_build != GameBuildString()) {
        FE_LOG_WARN("the symbol descriptor targets build '{}' but this game is '{}'; discovery "
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

    FE_LOG_INFO("engine binding resolved: {} symbol(s) available", state.symbols->Count());

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
    FE_LOG_INFO("controllable surface: {} writable global(s), {} read only string id(s)",
                writable, string_ids);

    // Self test the write path against a debug visualization boolean, chosen because
    // toggling it has no gameplay effect even if something goes wrong. It writes,
    // reads back, and restores. Knowing the record layout is right beats assuming it.
    if (const Result verified = state.globals->VerifyWritePath("debug_damage_verbose");
        !verified.ok()) {
        FE_LOG_ERROR("global write path self test FAILED: {}", verified.message());
        FE_LOG_ERROR("globals will be readable but writes are not trusted on this build");
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
        FE_LOG_ERROR("{}", image.message());
        FE_LOG_ERROR("the engine surface is unavailable. The lobby and Steam layers are "
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
        FE_LOG_ERROR("{}", failure);
        FE_LOG_ERROR("report this log along with the game version so a symbol descriptor can "
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
        FE_LOG_INFO("the host application did not initialize Steam within {} s; this process "
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
        PrepareLobby();

        // Kept current while a session is up: the name, mode and map a player picks have to
        // reach the lobby's metadata or nobody browsing can tell one game from another.
        PublishSessionDetails();

        // A slot pressed before the lobby finished being created is honoured here, once it
        // exists, rather than being dropped.
        if (g_invite_pending) {
            OpenSessionInvite();
        }

        RefreshLobbyStatus();

        // Phase changes are logged, so a session that never becomes joinable says why
        // instead of simply never appearing.
        {
            static lobby::LobbyPhase s_phase = lobby::LobbyPhase::Idle;
            std::lock_guard          lock(g_state_mutex);
            if (g_state && g_state->manager) {
                const lobby::LobbyPhase now = g_state->manager->Phase();
                if (now != s_phase) {
                    s_phase = now;
                    FE_LOG_INFO("session phase is now {} (lobby {}, error '{}')",
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
                    FE_LOG_INFO("MULTIPLAYER clicked");
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
                    g_lobby.mode = "CAPTURE THE FLAG";
                    SelectLobbyMode(false);
                    break;
                case unreal::LobbyAction::SelectSlayer:
                    g_lobby.mode = "SLAYER";
                    SelectLobbyMode(true);
                    break;
                case unreal::LobbyAction::StartMatch:
                    OnStartMatch();
                    break;
                case unreal::LobbyAction::JoinMatch:
                    OnJoinMatch();
                    break;
                case unreal::LobbyAction::Back:
                    OnLeaveLobby();
                    break;
                case unreal::LobbyAction::InviteRed:
                    InviteToSession(0);
                    break;
                case unreal::LobbyAction::InviteBlue:
                    InviteToSession(1);
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
                    SelectLobbyMap(pressed_index);
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
            FE_LOG_WARN("a hardware trap exceeded its recording budget and is being "
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
        FE_LOG_INFO("watch: {} retired; its object was collected and no distinct {} instance "
                    "of the class remains",
                    field.label, want_default ? "default" : "live");
        field.valid = false;
        return false;
    }

    FE_LOG_INFO("watch: {} rebound from a collected object at 0x{:X} to 0x{:X}", field.label,
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
        FE_LOG_WARN("the object array is not available yet");
        return 0;
    }

    const std::vector<unreal::ObjectInfo> found =
        g_state->objects->FindByClassNameContains(fragment, limit);
    if (found.empty()) {
        return 0;
    }

    FE_LOG_INFO("'{}' matched {} live object(s):", fragment, found.size());
    for (const unreal::ObjectInfo& object : found) {
        FE_LOG_INFO("  {} : {} @ 0x{:X}", object.name, object.class_name, object.address);
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
        FE_LOG_INFO("fast reflection unavailable; falling back to the search");
        return false;
    }

    Expected<unreal::NamePool> pool = unreal::NamePool::FromBlocks(pool_address);
    if (!pool.ok()) {
        FE_LOG_WARN("fast pool rejected: {}", pool.error().message);
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
        FE_LOG_WARN("fast object array rejected: {}", objects.error().message);
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
        FE_LOG_WARN("multiplayer was pressed before the lobby was ready to open");
        return;
    }

    unreal::LobbyUIContext ui = g_lobby_ui;
    if (const Result bound = unreal::BindLobbyMenu(g_live_menu, ui); !bound.ok()) {
        FE_LOG_WARN("the lobby cannot attach to menu 0x{:X}: {}", g_live_menu,
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
        FE_LOG_INFO("multiplayer lobby shown");
        return;
    }

    // The host always occupies a slot, so the lobby is never shown empty.
    if (g_lobby.players.empty()) {
        g_lobby.AddPlayer(SteamPlayerName(), true);
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
        FE_LOG_WARN("could not open the multiplayer lobby: {}",
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
    FE_LOG_INFO("{} of {} lobby button(s) are live", watched, g_lobby_controls.size());

    // Read back a second later what size the layout actually gave the screen. A lobby that
    // reports its real size and still looks empty is a different fault from one the layout
    // collapsed to nothing, and from a blank screen the two look identical.
    g_lobby_measure_at = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    FE_LOG_INFO("multiplayer lobby open (root 0x{:X})", lobby_root);
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

/// Takes whatever the player typed into the server name field.
///
/// Read on demand rather than tracked as it is typed: the field owns its own text, so
/// asking it at the moment the name matters is both simpler and always current.
void CaptureServerName() {
    if (!g_lobby_ui_ready) {
        return;
    }
    unreal::LobbyUIContext ui = g_lobby_ui;
    if (!unreal::BindLobbyMenu(g_live_menu, ui).ok()) {
        return;
    }
    std::string typed;
    (void)unreal::RunOnGameThread([&]() { typed = unreal::ReadServerName(ui); }, 5000);
    if (!typed.empty()) {
        g_lobby.server_name = typed;
        FE_LOG_INFO("server name is '{}'", g_lobby.server_name);
    }
}

/// Makes sure a real, joinable session exists behind the lobby screen.
///
/// The screen on its own is only a picture: without a session there is nothing for anyone
/// to join and nothing for an invite to point at. Hosting is idempotent here, so opening
/// the lobby twice does not create two sessions.
void EnsureSessionHosted() {
    std::lock_guard lock(g_state_mutex);
    if (!g_state || !g_state->manager) {
        FE_LOG_WARN("no session can be hosted: networking is unavailable");
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

    if (const Result hosted = g_state->manager->HostSession(options); !hosted.ok()) {
        FE_LOG_WARN("the multiplayer session could not be hosted: {}", hosted.message());
        return;
    }
    FE_LOG_INFO("multiplayer session hosted for up to {} players",
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
    lobby::LobbyId lobby = 0;
    {
        std::lock_guard lock(g_state_mutex);
        if (!g_state || !g_state->manager) {
            FE_LOG_WARN("cannot invite: networking is unavailable");
            g_invite_pending = false;
            return;
        }
        if (g_state->manager->Phase() != lobby::LobbyPhase::Hosting) {
            return; // Not yet; the poll loop will try again.
        }
        lobby = g_state->manager->Snapshot().lobby_id;
    }
    if (lobby == 0) {
        return;
    }

    g_invite_pending = false;
    PublishSessionDetails();
    steam::ActivateGameOverlayInviteDialog(lobby);
    FE_LOG_INFO("invite overlay opened for session {}", lobby);
}

void InviteToSession(int team) {
    FE_LOG_INFO("inviting a player to the {} team of this multiplayer session",
                team == 0 ? "red" : "blue");
    EnsureSessionHosted();
    g_invite_pending = true;
    OpenSessionInvite();
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
        if (g_state->manager->Phase() != lobby::LobbyPhase::Hosting) {
            return;
        }
        lobby = g_state->manager->Snapshot().lobby_id;
    }
    if (lobby == 0) {
        return;
    }

    const std::string name =
        g_lobby.server_name.empty() ? std::format("{}'s game", SteamPlayerName())
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
    FE_LOG_INFO("session advertised as '{}' ({} on {})", name, g_lobby.mode,
                g_lobby.scenario);
}

/// Keeps the status panel honest about the network, the session and the build.
///
/// Rewritten in place on a timer rather than rebuilt, and only while the lobby is actually
/// on screen, so it costs nothing when nobody is looking at it.
void RefreshLobbyStatus() {
    static auto s_last = std::chrono::steady_clock::time_point{};

    if (!g_lobby_ui_ready || !unreal::LobbyIsBuilt() || g_lobby_root == 0) {
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (now - s_last < std::chrono::milliseconds(500)) {
        return;
    }
    s_last = now;

    unreal::LobbyStatus status;
    status.online = steam::IsInitialized() && steam::IsLoggedOn();

    {
        std::lock_guard lock(g_state_mutex);
        if (g_state && g_state->manager) {
            const lobby::LobbySnapshot& snapshot = g_state->manager->Snapshot();
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
                    status.session = "JOINING";
                    break;
                default:
                    status.session = snapshot.last_error.empty()
                                         ? "BUSY"
                                         : std::format("ERROR: {}", snapshot.last_error);
                    break;
            }
        } else {
            status.session = "UNAVAILABLE";
        }
    }
    if (!status.online) {
        status.session   = "OFFLINE";
        status.invitable = false;
    }

    std::string latest;
    {
        std::lock_guard lock(g_version_mutex);
        latest = g_latest_version;
    }
    status.update_available = !latest.empty() && update::IsNewer(latest, kModVersion);
    status.version = status.update_available
                         ? std::format("v{}  UPDATE {} READY", kModVersion, latest)
                         : std::format("v{}  UP TO DATE", kModVersion);

    unreal::LobbyUIContext ui = g_lobby_ui;
    if (!unreal::BindLobbyMenu(g_live_menu, ui).ok()) {
        return;
    }
    (void)unreal::RunOnGameThread([&]() { unreal::SetLobbyStatus(ui, status); }, 5000);
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
    FE_LOG_INFO("server filter applied: {} of {} server(s) match (mode '{}', slots {}, "
                "ping {})",
                matching.size(), DiscoveredServers().size(),
                g_server_filter.mode.empty() ? "any" : g_server_filter.mode,
                g_server_filter.slots, g_server_filter.max_ping);
}

/// Marks the chosen mode on screen without rebuilding.
void SelectLobbyMode(bool slayer) {
    FE_LOG_INFO("mode is now {}", g_lobby.mode);
    if (!g_lobby_ui_ready) {
        return;
    }
    unreal::LobbyUIContext ui = g_lobby_ui;
    if (!unreal::BindLobbyMenu(g_live_menu, ui).ok()) {
        return;
    }
    (void)unreal::RunOnGameThread([&]() { unreal::SetLobbyMode(ui, slayer); }, 5000);
}

/// Chooses the scenario a match will be played on, and marks it on screen.
void SelectLobbyMap(int map_index) {
    if (map_index < 0 || map_index >= static_cast<int>(std::size(unreal::kLobbyMaps))) {
        return;
    }
    g_lobby.scenario = unreal::kLobbyMaps[map_index].scenario;
    FE_LOG_INFO("map is now {}", g_lobby.scenario);

    if (!g_lobby_ui_ready) {
        return;
    }
    unreal::LobbyUIContext ui = g_lobby_ui;
    if (!unreal::BindLobbyMenu(g_live_menu, ui).ok()) {
        return;
    }
    (void)unreal::RunOnGameThread([&]() { unreal::SetLobbyMap(ui, map_index); }, 5000);
}

/// Builds the whole lobby ahead of time and leaves it hidden.
///
/// This is what makes opening it instant. Building the screen creates around a hundred
/// widgets and duplicates a widget tree for every one of the frontend's buttons, and doing
/// that when the player presses MULTIPLAYER is work they sit and wait through. Done here it
/// happens while they are still looking at the main menu, and pressing the entry then costs
/// one visibility change.
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
        FE_LOG_WARN("the lobby could not be prepared: {}",
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
    FE_LOG_INFO("lobby prepared and hidden; {} of {} button(s) live, opening is now a "
                "visibility change",
                watched, g_lobby_controls.size());
}

/// Takes the lobby down and gives the frontend back.
void OnLeaveLobby() {
    // The buttons are kept watched and the controls kept mapped: the lobby is hidden, not
    // destroyed, so its widgets are the same ones when it is shown again. Dropping them
    // here would make every button dead the second time the screen was opened.
    unreal::LobbyUIContext ui = g_lobby_ui;
    if (!g_lobby_ui_ready || !unreal::BindLobbyMenu(g_live_menu, ui).ok()) {
        return;
    }
    (void)unreal::RunOnGameThread([&]() { unreal::ShowLobbyUI(ui, false); }, 10000);
    FE_LOG_INFO("multiplayer lobby hidden");
}

/// Asks Steam what is out there and reports what came back.
///
/// Nothing is invented here. An empty list is shown empty, because a browser that lists
/// servers which do not exist is worse than one that lists none.
void RefreshServerList() {
    const std::vector<unreal::ServerEntry> found = DiscoveredServers();
    FE_LOG_INFO("server browser refreshed: {} server(s) found", found.size());
}

/// Starts the match the lobby is configured for.
///
/// The lobby comes down first. Leaving it up means the new level loads behind a menu that
/// is still holding input focus, which looks exactly like a hang.
void OnStartMatch() {
    CaptureServerName();
    const std::string scenario = g_lobby.scenario;
    const bool        friendly_fire = g_lobby.friendly_fire;
    FE_LOG_INFO("starting a {} match on {} (friendly fire {})", g_lobby.mode, scenario,
                friendly_fire ? "on" : "off");

    OnLeaveLobby();

    Result       outcome = Result::Success();
    const Result ran     = unreal::RunOnGameThread(
        [&]() {
            std::lock_guard lock(g_state_mutex);
            if (!g_state || !g_state->objects.has_value() ||
                !g_state->reflection.has_value()) {
                outcome = Result::Fail(ErrorCode::InvalidState, "reflection is not ready");
                return;
            }
            outcome = unreal::BeginCampaign(*g_state->objects, *g_state->reflection,
                                            scenario, kDefaultCampaignAsset, friendly_fire);
        },
        60000);

    if (!ran.ok() || !outcome.ok()) {
        FE_LOG_WARN("the match did not start: {}",
                    ran.ok() ? outcome.message() : ran.message());
    }
}

/// Joins the server selected in the browser.
void OnJoinMatch() {
    const std::vector<unreal::ServerEntry> servers = DiscoveredServers();
    if (servers.empty()) {
        FE_LOG_WARN("nothing to join: the browser found no servers");
        return;
    }
    const unreal::ServerEntry& target = servers.front();
    FE_LOG_INFO("joining {} ({} on {})", target.name, target.mode, target.map);
    OnLeaveLobby();
}

void MaintainMainMenuButton() {
    // The menu instance last decorated. A different address means a freshly built menu,
    // which needs the entry again.
    static std::uintptr_t s_decorated_menu = 0;
    static auto           s_last_check     = std::chrono::steady_clock::time_point{};

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
                            FE_LOG_INFO("editable text style at +0x{:X}: font +0x{:X}, "
                                        "colour +0x{:X}",
                                        base, g_lobby_ui.editable_font_offset,
                                        g_lobby_ui.editable_colour_offset);
                        } else {
                            FE_LOG_WARN("the text field's style was not found; it will keep "
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
                    FE_LOG_INFO("lobby handles cached; opening the lobby is now instant");
                } else {
                    static bool s_complained = false;
                    if (!s_complained) {
                        s_complained = true;
                        FE_LOG_WARN("the lobby cannot be prepared: {}", statics.message());
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
    }

    // Finding the menu is a plain memory scan, so the cheap check happens every time and
    // the expensive part, which needs the game thread, only runs when there is work.
    std::uintptr_t menu = 0;
    {
        std::lock_guard lock(g_state_mutex);
        if (!g_state || !g_state->objects.has_value()) {
            return;
        }
        // The menu, and the panels the frontend draws beside it rather than inside it.
        //
        // The fireteam list survives collapsing the menu, so it is not part of it. It is
        // gathered here, in the pass that is already being made, rather than in a scan of
        // its own when the lobby opens.
        std::vector<std::uintptr_t> beside;
        g_state->objects->ForEach([&](const unreal::ObjectInfo& object) {
            if (object.name.rfind("Default__", 0) == 0) {
                return true;
            }
            if (object.class_name == "WBP_MainMenu_C") {
                menu = object.address;
            } else if (object.class_name.rfind("WBP_Squad", 0) == 0 ||
                       object.class_name == "WBP_MeteoriteUILayout_C" ||
                       object.class_name.rfind("WBP_MeteoriteBoundActionBar", 0) == 0) {
                // The frontend's layout, not just the squad panel.
                //
                // Folding every squad widget still left the fireteam audible, so what
                // answers the mouse is not a squad widget at all: it is the layout the
                // frontend wraps around the menu, which owns the fireteam area and the
                // action bar. Collapsing the menu removed the menu's sounds because it
                // removed the whole widget that owned them, and this is the same move
                // applied to the thing that owns the rest.
                // The whole family, not just the panel.
                //
                // Collapsing WBP_SquadWidget_C alone made the fireteam disappear and left
                // it audible: hovering where it had been still played its sounds. A
                // collapsed widget cannot be hit tested, so the rows that answered the
                // mouse were never inside the widget that was collapsed. The list entries
                // are their own widgets and have to be folded in their own right.
                beside.push_back(object.address);
            }
            return true;
        });
        g_lobby_ui.also_fold = beside;
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
        // One retry, only for the font.
        //
        // The handles are resolved before the frontend is built, which is what makes the
        // entry appear with the rest of the menu rather than long after it. The typeface is
        // the one thing that benefits from waiting: it is read from the game's own text
        // blocks, and there are far more of those once a menu is on screen.
        if (g_lobby_ui_ready && !g_lobby_ui.has_font) {
            (void)unreal::ResolveLobbyStatics(*g_state->objects, g_lobby_ui);
        }
        if (const Result resolved = unreal::ResolveMenuButtonPlan(*g_state->objects, plan);
            !resolved.ok()) {
            // Logged once per menu rather than every tick: a silent return here meant the
            // entry simply never appeared with no indication of why.
            static std::uintptr_t s_complained_about = 0;
            if (s_complained_about != menu) {
                s_complained_about = menu;
                FE_LOG_WARN("menu found at 0x{:X} but the entry cannot be built: {}", menu,
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
        FE_LOG_INFO("multiplayer entry added to main menu 0x{:X}", menu);

        // Start listening on the new button straight away. The previous one went away with
        // the previous menu, so the watch has to follow the current widget.
        // The menu outlives every button on it, so it is what the job pump hooks. Without
        // this, each job armed and disarmed a breakpoint across every thread, which is why
        // opening the lobby took seconds instead of a frame.
        if (const Result pump = unreal::InstallGameThreadPump(menu); !pump.ok()) {
            FE_LOG_WARN("game thread pump unavailable: {}", pump.message());
        }

        unreal::StopWatchingWidgetEvents();
        if (const Result watching = unreal::WatchWidgetEvents(button); !watching.ok()) {
            FE_LOG_WARN("could not watch the multiplayer button: {}", watching.message());
        } else {
            // HandleButtonClicked is the framework's own click, and it arrives exactly once
            // per press. It was chosen by watching what the button actually emits rather
            // than by guessing a name: a real click produces twenty three distinct events,
            // most of them hover and focus noise, and several look like plausible
            // candidates. Resolved by name here so it holds across runs.
            std::lock_guard lock(g_state_mutex);
            if (g_state && g_state->objects.has_value()) {
                const std::uintptr_t click =
                    unreal::FindFunction(*g_state->objects, "HandleButtonClicked");
                if (click != 0) {
                    unreal::SetWidgetClickEvent(click);
                    FE_LOG_INFO("multiplayer button click bound to HandleButtonClicked "
                                "0x{:X}",
                                click);
                } else {
                    FE_LOG_WARN("HandleButtonClicked not found; the button will not respond");
                }
            }
        }
    }
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
            FE_LOG_INFO("watch: {} at 0x{:X} became unreadable; the object was probably "
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
                FE_LOG_INFO("pvp: re-enabled friendly fire on {} (0x{:X})", field.label,
                            field.address);
                field.last_value = true;
                continue;
            }
            FE_LOG_WARN("pvp: could not write friendly fire on {} (0x{:X})", field.label,
                        field.address);
        }

        if (value != field.last_value) {
            FE_LOG_INFO("watch: {} changed {} -> {} (0x{:X})", field.label,
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
    Expected<unreal::NamePool> pool = unreal::NamePool::Locate();
    if (!pool.ok()) {
        FE_LOG_ERROR("UE reflection unavailable: {}", pool.message());
        FE_LOG_ERROR("the UE side game engine variant cannot be reached without it");
        return;
    }

    // Proof of correctness, in the log where a user can see it.
    const std::vector<unreal::NameEntry> sample = pool.value().DumpBlock(0, 24);
    FE_LOG_INFO("FName pool verification, first {} name(s) of block 0:", sample.size());
    std::string line;
    for (std::size_t i = 0; i < sample.size(); ++i) {
        line += sample[i].text;
        if (i + 1 < sample.size()) {
            line += ", ";
        }
    }
    FE_LOG_INFO("  {}", line);

    // Look up the names that matter for making a campaign scenario play as a versus
    // match. Finding these confirms the reflected surface is reachable by name.
    for (const char* wanted : {"bFriendlyFireEnabled", "bFriendlyFire",
                               "BlamGameEngineSocialOptions", "BlamGameEnginePlayerTraits",
                               "BlamGameEngineBaseVariantStorage", "BlamPlayerRespawn",
                               "BlamNetworkGameStateComponent"}) {
        const Expected<std::uint32_t> index = pool.value().FindIndexOf(wanted, 8);
        if (index.ok()) {
            FE_LOG_INFO("  resolved '{}' to FName index {}", wanted, index.value());
        } else {
            FE_LOG_WARN("  '{}' not found in the searched blocks: {}", wanted, index.message());
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
        FE_LOG_ERROR("GUObjectArray unavailable: {}", array.message());
        FE_LOG_ERROR("live UE objects cannot be reached, so the game engine variant cannot be "
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

    FE_LOG_INFO("located {} of {} target struct(s) among {} object(s)", located.size(),
                std::size(kWanted), objects.Count());
    if (located.empty()) {
        FE_LOG_ERROR("no target struct appeared; the game may not have finished starting");
        return;
    }

    {
        std::lock_guard lock(g_state_mutex);
        if (!g_state) {
            return;
        }
        g_state->objects = objects;
    }

    // Proof, in the log: the first objects in a UE process are always engine
    // intrinsics, so their names and classes are recognizable at a glance.
    FE_LOG_INFO("UE object array verification, first 12 object(s):");
    std::uint32_t shown = 0;
    objects.ForEach(
        [&](const unreal::ObjectInfo& object) {
            FE_LOG_INFO("  [{:>6}] {:<40} class={}", object.index, object.name,
                        object.class_name);
            return ++shown < 12;
        },
        64);

    unreal::Reflection reflection(*names);

    // Detect the real offsets rather than trusting the documented ones. The documented
    // UE5 layout is wrong on this build: reading BlamScenarioGameOptions with it
    // reported size 0 and no fields even though the struct name resolved correctly.
    std::vector<std::uintptr_t> candidates;
    candidates.reserve(located.size());
    for (const auto& [name, object] : located) {
        candidates.push_back(object.address);
    }

    const unreal::ReflectionLayout detected = reflection.DetectLayout(candidates);
    if (detected.detected) {
        reflection.SetLayout(detected);
        FE_LOG_INFO("  UE layout detected: {}", detected.Describe());
    } else {
        FE_LOG_WARN("  UE layout detection found no property chain; trying documented offsets");
    }

    // Validate whatever layout we ended up with. A struct reporting fields outside its
    // own size means an offset is wrong, and a wrong offset silently pointing at the
    // wrong field is the worst possible outcome.
    bool layout_ok = false;
    for (const auto& [name, object] : located) {
        std::string report;
        const Result verified = reflection.VerifyLayout(object.address, report);
        FE_LOG_INFO("  layout check: {}", report);
        if (verified.ok()) {
            layout_ok = true;
            break;
        }
    }

    if (!layout_ok) {
        FE_LOG_ERROR("  UE property layout could not be validated against any target struct");
        FE_LOG_ERROR("  offsets will not be trusted; reflection reads are disabled");
        // Emit the raw data needed to work out the layout by hand, for the first
        // candidate. This is the artifact that makes the next attempt cheap.
        if (!candidates.empty()) {
            FE_LOG_INFO("{}", reflection.ProbeStructLayout(candidates.front()));
        }
        return;
    }

    {
        std::lock_guard lock(g_state_mutex);
        if (!g_state) {
            return;
        }
        g_state->reflection = reflection;
    }

    // Dump the fields. This is the payoff: named fields with real offsets.
    for (const char* wanted : kWanted) {
        const auto it = located.find(wanted);
        if (it == located.end()) {
            FE_LOG_WARN("  '{}' was not found in the object array", wanted);
            continue;
        }

        const Expected<unreal::StructInfo> info = reflection.ReadStruct(it->second.address);
        if (!info.ok()) {
            FE_LOG_WARN("  '{}' could not be read: {}", wanted, info.message());
            continue;
        }

        FE_LOG_INFO("struct {} at 0x{:X}: size {}, {} field(s)", info.value().name,
                    info.value().address, info.value().properties_size,
                    info.value().properties.size());
        for (const unreal::PropertyInfo& property : info.value().properties) {
            FE_LOG_INFO("    +0x{:03X}  {:<12} {:<44} size {}", property.offset,
                        property.type_name, property.name, property.TotalSize());
        }
    }

    // The specific field this whole chain exists to reach.
    unreal::PropertyInfo friendly_fire{};
    std::uintptr_t       friendly_fire_owner = 0;
    for (const auto& [name, object] : located) {
        for (const unreal::PropertyInfo& property :
             reflection.FindPropertiesContaining(object.address, "riendly")) {
            FE_LOG_INFO("field located: {}::{} at +0x{:X} type {} size {}", name, property.name,
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
        FE_LOG_WARN("no friendly fire field was located; cannot look for an instance");
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
        FE_LOG_INFO("properties_size refined to +0x{:X} using {} struct(s)", size_offset,
                    addresses.size());
        layout.properties_size_offset = size_offset;
    } else {
        FE_LOG_WARN("properties_size could not be refined; struct sizes stay unreliable");
    }

    if (const std::size_t inner = reflection.DetectStructPropertyInnerOffset(addresses, addresses);
        inner != 0) {
        FE_LOG_INFO("FStructProperty::Struct detected at +0x{:X}", inner);
        layout.struct_property_inner_offset   = inner;
        layout.struct_property_inner_detected = true;
    } else {
        FE_LOG_WARN("FStructProperty::Struct offset not detected; embedded struct search "
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

    FE_LOG_INFO("the target struct is embedded in {} type(s):", usages.size());
    for (const unreal::StructUsage& usage : usages) {
        FE_LOG_INFO("  {} ({})::{} at +0x{:X}", usage.owner_name, usage.owner_class_name,
                    usage.property_name, usage.property_offset);
    }

    // For each owning type, look for live objects of it and read the field.
    std::size_t instances_read = 0;
    for (const unreal::StructUsage& usage : usages) {
        const std::vector<unreal::ObjectInfo> instances =
            objects.FindInstancesOfClassAddress(usage.owner_address, 8);
        if (instances.empty()) {
            FE_LOG_INFO("  no live instance of {} right now", usage.owner_name);
            continue;
        }
        for (const unreal::ObjectInfo& instance : instances) {
            const std::uintptr_t struct_instance =
                instance.address + static_cast<std::uintptr_t>(usage.property_offset);
            const Expected<bool> value =
                reflection.ReadBoolField(struct_instance, friendly_fire);
            if (!value.ok()) {
                FE_LOG_WARN("  {} at 0x{:X}: could not read {}: {}", instance.name,
                            instance.address, friendly_fire.name, value.message());
                continue;
            }
            ++instances_read;
            const std::uintptr_t field_address =
                struct_instance + static_cast<std::uintptr_t>(friendly_fire.offset);
            FE_LOG_INFO("  LIVE VALUE {}.{}.{} = {} (instance 0x{:X}, field 0x{:X})",
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
        FE_LOG_INFO("no live instance was readable at the main menu, which is expected: the "
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

    FE_LOG_INFO("combat rule sweep: scanned {} reflected type(s), found {} candidate field(s)",
                types_scanned, hits.size());
    for (const Hit& hit : hits) {
        FE_LOG_INFO("  {} ({})::{} at +0x{:X} type {}", hit.owner, hit.owner_kind, hit.field,
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
        FE_LOG_INFO("  {} has {} live instance(s)", hit.owner, instances.size());

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
            FE_LOG_INFO("    LIVE {}::{} = {} (instance 0x{:X})", instance.name, hit.field,
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
            FE_LOG_INFO("  {} is embedded by nothing reflected", name);
            continue;
        }
        for (const unreal::StructUsage& owner : it->second) {
            FE_LOG_INFO("  {} embedded by {} ({})::{} at +0x{:X}", name, owner.owner_name,
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
    // game happens only when someone explicitly calls FE_SetFriendlyFire.
    //
    // The write mechanism itself is already proven: an earlier run toggled a live
    // instance and read the new value back before restoring it. Repeating that on every
    // launch buys nothing and risks the run.
    (void)known_usages;
    (void)friendly_fire;
    FE_LOG_INFO("discovery complete; no game state was modified. Use FE_SetFriendlyFire to "
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
    log::Initialize(DataDirectory() / "ForgeEvolved.log", log::Level::Info);
    FE_LOG_INFO("ForgeEvolved {} starting", FE_VERSION_STRING);
    FE_LOG_INFO("game build: {}", GameBuildString());
    FE_LOG_INFO("data directory: {}", DataDirectory().string());

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
            FE_LOG_WARN("FName adapter not installed: {}", installed.message());
        }
    } else {
        FE_LOG_INFO("FName adapter skipped: fe_no_fname is present");
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
        FE_LOG_ERROR("{}", network_failure);
        FE_LOG_ERROR("the mod is loaded but cannot host or join. The game is unaffected.");
    }

    // The engine control is created once, up front, and never replaced.
    //
    // LobbyManager holds a reference to it, so swapping the object later would leave
    // that reference dangling. Binding status is reported through the log and through
    // the globals surface instead, both of which can appear later without
    // invalidating anything already handed out.
    state->engine = std::make_unique<engine::InertEngineControl>(
        "the engine command binding for this game build is not resolved: the command "
        "descriptors were located but their call ABI has not been derived, so hosting a "
        "match is refused rather than guessed at");

    // The lobby manager needs both a backend and a transport. Without them the mod
    // stays loaded and the globals surface still works, which is why this is not a
    // hard failure.
    if (network_failure.empty()) {
        state->manager = std::make_unique<lobby::LobbyManager>(
            *state->backend, *state->transport, *state->engine, state->sink);
    } else {
        FE_LOG_WARN("no lobby manager: networking is unavailable");
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
    if (TryFastReflection()) {
        FE_LOG_INFO("UE reflection ready early; the menu entry can be in place before the "
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
                FE_LOG_INFO("call layout ready early too; the entry can go in on the first "
                            "menu tick");
            }
        }
    }

    g_ready.store(true, std::memory_order_release);
    g_running.store(true, std::memory_order_release);
    g_tick_thread = std::thread(&TickLoop);

    FE_LOG_INFO("ForgeEvolved ready ({})",
                network_failure.empty() ? "lobby available" : "lobby unavailable");

    // Asked once, in the background. The answer is only used to tell the player their copy
    // is behind, so nothing waits on it.
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
    if (!pacing::WaitFor("waiting for the game window", &HasGameWindow, aborted)) {
        FE_LOG_WARN("the game window never appeared; skipping UE reflection");
        return;
    }
    FE_LOG_INFO("game window is up; waiting for loading to settle before scanning");

    if (!pacing::WaitForQuiet("waiting for the game to finish loading", aborted)) {
        FE_LOG_WARN("the game never went quiet; skipping UE reflection rather than "
                    "competing with a machine that is still working");
        return;
    }

    FE_LOG_INFO("the game is idle; starting UE reflection");
    ResolveUnrealReflection();
}

void Shutdown() {
    g_running_or_starting.store(false, std::memory_order_release);
    g_running.store(false, std::memory_order_release);
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

    FE_LOG_INFO("ForgeEvolved stopped");
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
} // namespace fe

// ---------------------------------------------------------------------------
// Public C API
// ---------------------------------------------------------------------------
//
// Convention: 0 means success, a negative value is the negated fe::ErrorCode.
// FE_LastErrorMessage returns the text for the most recent failure.

extern "C" {

/// True once the mod finished starting up and can accept commands.
__declspec(dllexport) int FE_IsReady() {
    return fe::g_ready.load(std::memory_order_acquire) ? 1 : 0;
}

__declspec(dllexport) const char* FE_Version() {
    return FE_VERSION_STRING;
}

/// Hosts a session. mode is a canonical mode name such as "capture_the_flag".
/// map_path may be null or empty for the base scenario with no custom layout.
__declspec(dllexport) int FE_HostSession(const char* mode, const char* scenario,
                                         const char* map_path, int max_players,
                                         int friends_only) {
    if (mode == nullptr || scenario == nullptr) {
        return -static_cast<int>(fe::ErrorCode::InvalidArgument);
    }

    fe::engine::GameMode parsed_mode{};
    if (!fe::engine::ParseGameMode(mode, parsed_mode)) {
        return -static_cast<int>(fe::ErrorCode::InvalidArgument);
    }

    fe::lobby::HostOptions options;
    options.visibility = (friends_only != 0) ? fe::lobby::LobbyVisibility::FriendsOnly
                                             : fe::lobby::LobbyVisibility::Public;
    options.max_players = static_cast<std::uint32_t>(max_players > 0 ? max_players : 8);
    options.settings.mode     = parsed_mode;
    options.settings.scenario = scenario;
    if (map_path != nullptr) {
        options.map_variant_path = map_path;
    }

    return fe::WithManager([&](fe::lobby::LobbyManager& manager) {
        return manager.HostSession(options);
    });
}

__declspec(dllexport) int FE_JoinSession(unsigned long long lobby_id) {
    return fe::WithManager([&](fe::lobby::LobbyManager& manager) {
        return manager.JoinSession(lobby_id);
    });
}

__declspec(dllexport) int FE_LeaveSession() {
    std::lock_guard lock(fe::g_state_mutex);
    if (!fe::g_state || !fe::g_state->manager) {
        return -static_cast<int>(fe::ErrorCode::InvalidState);
    }
    fe::g_state->manager->LeaveSession();
    return 0;
}

__declspec(dllexport) int FE_SetReady(int ready) {
    return fe::WithManager([&](fe::lobby::LobbyManager& manager) {
        return manager.SetLocalReady(ready != 0);
    });
}

__declspec(dllexport) int FE_StartMatch() {
    return fe::WithManager(
        [](fe::lobby::LobbyManager& manager) { return manager.StartCountdown(); });
}

__declspec(dllexport) int FE_OpenInviteOverlay() {
    return fe::WithManager(
        [](fe::lobby::LobbyManager& manager) { return manager.OpenInviteOverlay(); });
}

__declspec(dllexport) int FE_SelectMap(const char* map_path) {
    if (map_path == nullptr) {
        return -static_cast<int>(fe::ErrorCode::InvalidArgument);
    }
    return fe::WithManager([&](fe::lobby::LobbyManager& manager) {
        return manager.SelectMapVariant(map_path);
    });
}

__declspec(dllexport) int FE_SendChat(const char* text) {
    if (text == nullptr) {
        return -static_cast<int>(fe::ErrorCode::InvalidArgument);
    }
    return fe::WithManager(
        [&](fe::lobby::LobbyManager& manager) { return manager.SendChat(text); });
}

/// Current phase as a stable lowercase identifier, for example "in_lobby".
/// Never null. The returned pointer is valid until the next call.
__declspec(dllexport) const char* FE_Phase() {
    static thread_local std::string phase;
    std::lock_guard lock(fe::g_state_mutex);
    if (!fe::g_state || !fe::g_state->manager) {
        phase = "unavailable";
        return phase.c_str();
    }
    phase = fe::lobby::ToString(fe::g_state->manager->Phase());
    return phase.c_str();
}

/// Writes a value into a stride 0x18 descriptor record by name. Returns 0 on success,
/// or a negated fe::ErrorCode.
///
/// This does exactly what it says and no more. A successful return means the memory was
/// written and can be read back. It does NOT mean the engine observed the change:
/// cross reference analysis found zero references from .text to any record field or any
/// table base, so this data appears to be residual rather than live. See
/// docs/04-ENGINE-BINDING.md.
///
/// It refuses stride 0x10 string id records, so asking it to enable something like
/// "forge_main_menu_palettes" fails with an explanation rather than appearing to work.
__declspec(dllexport) int FE_SetGlobal(const char* name, unsigned long long value) {
    if (name == nullptr) {
        return -static_cast<int>(fe::ErrorCode::InvalidArgument);
    }
    std::lock_guard lock(fe::g_state_mutex);
    if (!fe::g_state || !fe::g_state->globals.has_value()) {
        return -static_cast<int>(fe::ErrorCode::InvalidState);
    }
    const fe::Result result = fe::g_state->globals->SetNumber(name, value);
    return result.ok() ? 0 : -static_cast<int>(result.code());
}

/// Reads a writable engine global by name. Returns 0 on success and writes through
/// out_value, which must not be null.
__declspec(dllexport) int FE_GetGlobal(const char* name, unsigned long long* out_value) {
    if (name == nullptr || out_value == nullptr) {
        return -static_cast<int>(fe::ErrorCode::InvalidArgument);
    }
    std::lock_guard lock(fe::g_state_mutex);
    if (!fe::g_state || !fe::g_state->globals.has_value()) {
        return -static_cast<int>(fe::ErrorCode::InvalidState);
    }
    const auto value = fe::g_state->globals->GetNumber(name);
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
__declspec(dllexport) int FE_LogGlobals(const char* name_contains, int max_results) {
    std::lock_guard lock(fe::g_state_mutex);
    if (!fe::g_state || !fe::g_state->globals.has_value()) {
        return -static_cast<int>(fe::ErrorCode::InvalidState);
    }

    const std::string filter(name_contains != nullptr ? name_contains : "");
    const auto listed = fe::g_state->globals->List(
        filter, max_results > 0 ? static_cast<std::size_t>(max_results) : 64);

    fe::log::Write(fe::log::Level::Info, "Mod",
                   std::format("{} global(s) matching '{}':", listed.size(), filter));
    for (const auto& info : listed) {
        fe::log::Write(fe::log::Level::Info, "Mod",
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
/// Returns 0 on success, or a negated fe::ErrorCode. Blocks for up to
/// timeout_seconds while polling, so call it from a worker thread, not a render
/// thread.
__declspec(dllexport) int FE_LobbySelfTest(int timeout_seconds) {
    using namespace std::chrono;

    // Minimal observer that records the outcome. Declared locally because it exists
    // only for the duration of this test.
    class TestObserver final : public fe::lobby::ILobbyBackendObserver {
    public:
        bool             created{false};
        bool             failed{false};
        fe::lobby::LobbyId lobby{0};
        std::string      detail;

        void OnLobbyCreated(fe::lobby::LobbyId id) override {
            created = true;
            lobby   = id;
        }
        void OnLobbyCreateFailed(const fe::Error& error) override {
            failed = true;
            detail = error.message;
        }
        void OnLobbyEntered(fe::lobby::LobbyId id, bool) override { lobby = id; }
        void OnLobbyEnterFailed(const fe::Error& error) override {
            failed = true;
            detail = error.message;
        }
        void OnMemberJoined(const fe::lobby::LobbyMember&) override {}
        void OnMemberLeft(fe::lobby::PlatformId, bool) override {}
        void OnLobbyDataChanged(fe::lobby::LobbyId) override {}
        void OnMemberDataChanged(fe::lobby::PlatformId) override {}
        void OnJoinRequested(fe::lobby::LobbyId, fe::lobby::PlatformId) override {}
    };

    fe::lobby::ILobbyBackend* backend = nullptr;
    {
        std::lock_guard lock(fe::g_state_mutex);
        if (!fe::g_state || !fe::g_state->backend) {
            fe::log::Write(fe::log::Level::Error, "Mod",
                           "lobby self test: the Steam backend is not available");
            return -static_cast<int>(fe::ErrorCode::SteamUnavailable);
        }
        backend = fe::g_state->backend.get();
    }

    fe::log::Write(fe::log::Level::Info, "Mod", "lobby self test: creating a lobby");

    const fe::Result created =
        backend->Create(fe::lobby::LobbyVisibility::FriendsOnly, 8);
    if (!created.ok()) {
        fe::log::Write(fe::log::Level::Error, "Mod",
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
        fe::steam::RunCallbacks();
        backend->Poll(observer);
        std::this_thread::sleep_for(milliseconds(100));
    }

    if (observer.failed) {
        fe::log::Write(fe::log::Level::Error, "Mod",
                       std::format("lobby self test: FAILED: {}", observer.detail));
        return -static_cast<int>(fe::ErrorCode::LobbyUnavailable);
    }
    if (!observer.created) {
        fe::log::Write(fe::log::Level::Error, "Mod",
                       "lobby self test: timed out waiting for the lobby to be created");
        backend->Leave();
        return -static_cast<int>(fe::ErrorCode::Timeout);
    }

    fe::log::Write(fe::log::Level::Info, "Mod",
                   std::format("lobby self test: lobby {} created, owner={}", observer.lobby,
                               backend->IsOwner()));

    // Read back the metadata a joining client would evaluate. This is the part that
    // proves a friend could actually find and validate this lobby.
    int failures = 0;
    for (const char* key : {fe::lobby::keys::kProtocolVersion, fe::lobby::keys::kGameBuild,
                            fe::lobby::keys::kHostId}) {
        const auto value = backend->GetLobbyData(key);
        if (value.ok()) {
            fe::log::Write(fe::log::Level::Info, "Mod",
                           std::format("lobby self test:   {} = {}", key, value.value()));
        } else {
            ++failures;
            fe::log::Write(fe::log::Level::Error, "Mod",
                           std::format("lobby self test:   {} MISSING: {}", key,
                                       value.message()));
        }
    }

    // Member data is the path a ready flag travels before a transport connection
    // exists, so it is worth proving too.
    if (const fe::Result member = backend->SetMemberData(fe::lobby::keys::kMemberReady, "1");
        member.ok()) {
        const auto local = backend->LocalId();
        if (local.ok()) {
            const auto read_back =
                backend->GetMemberData(local.value(), fe::lobby::keys::kMemberReady);
            fe::log::Write(fe::log::Level::Info, "Mod",
                           std::format("lobby self test:   member ready flag round trip: {}",
                                       read_back.ok() ? read_back.value() : "MISSING"));
            if (!read_back.ok()) {
                ++failures;
            }
        }
    } else {
        ++failures;
    }

    fe::log::Write(fe::log::Level::Info, "Mod", "lobby self test: leaving the lobby");
    backend->Leave();

    if (failures > 0) {
        fe::log::Write(fe::log::Level::Error, "Mod",
                       std::format("lobby self test: completed with {} failure(s)", failures));
        return -static_cast<int>(fe::ErrorCode::LobbyUnavailable);
    }

    fe::log::Write(fe::log::Level::Info, "Mod", "lobby self test: PASSED");
    return 0;
}

/// Opens the Steam invite overlay for the current lobby. Exposed separately from the
/// lobby manager so the metadata plane can be driven without the engine.
__declspec(dllexport) int FE_TestInviteOverlay() {
    std::lock_guard lock(fe::g_state_mutex);
    if (!fe::g_state || !fe::g_state->backend) {
        return -static_cast<int>(fe::ErrorCode::SteamUnavailable);
    }
    const fe::Result result = fe::g_state->backend->OpenInviteOverlay();
    return result.ok() ? 0 : -static_cast<int>(result.code());
}

/// Sets every located copy of the friendly fire flag. Returns how many were written.
///
/// Several independent copies exist. Which one a match actually consults is not yet
/// established, so this writes all of them rather than pretending to know.
__declspec(dllexport) int FE_SetFriendlyFire(int enabled) {
    std::lock_guard lock(fe::g_state_mutex);
    if (!fe::g_state) {
        return -static_cast<int>(fe::ErrorCode::InvalidState);
    }

    const std::uint8_t byte = (enabled != 0) ? 1u : 0u;
    int written = 0;
    for (fe::ModState::WatchedField& field : fe::g_state->watched) {
        if (!field.valid) {
            continue;
        }
        if (fe::unreal::memory::WriteBytes(field.address, &byte, sizeof(byte))) {
            // Updated so the watcher reports the game overwriting us, not our own store.
            field.last_value = (byte != 0);
            ++written;
            fe::log::Write(fe::log::Level::Info, "Mod",
                           std::format("set {} = {}", field.label, byte != 0));
        }
    }
    return written;
}

/// Logs the current value of every located copy. Returns how many were readable.
__declspec(dllexport) int FE_LogFriendlyFire() {
    std::lock_guard lock(fe::g_state_mutex);
    if (!fe::g_state) {
        return -static_cast<int>(fe::ErrorCode::InvalidState);
    }
    int readable = 0;
    for (const fe::ModState::WatchedField& field : fe::g_state->watched) {
        std::uint8_t raw = 0;
        if (field.valid && fe::unreal::memory::GuardedRead(field.address, &raw, sizeof(raw))) {
            ++readable;
            fe::log::Write(fe::log::Level::Info, "Mod",
                           std::format("  {} = {} (0x{:X})", field.label, raw != 0,
                                       field.address));
        }
    }
    return readable;
}

// Defined below; FE_Command dispatches to it.
__declspec(dllexport) int FE_DumpDiagnostics();

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
/// Returns 0 on success, or a negated fe::ErrorCode.
__declspec(dllexport) int FE_Command(const char* command_line) {
    if (command_line == nullptr) {
        return -static_cast<int>(fe::ErrorCode::InvalidArgument);
    }

    // Copied immediately: the memory belongs to the caller's remote allocation and may
    // be freed the moment the thread returns.
    std::string command;
    try {
        command.assign(command_line);
    } catch (...) {
        return -static_cast<int>(fe::ErrorCode::InvalidArgument);
    }
    if (command.size() > 512) {
        command.resize(512);
    }

    fe::log::Write(fe::log::Level::Info, "Mod", std::format("command: '{}'", command));

    const auto starts_with = [&command](std::string_view prefix) {
        return command.rfind(prefix, 0) == 0;
    };

    if (starts_with("ff status")) {
        return FE_LogFriendlyFire();
    }
    if (starts_with("ff on")) {
        return FE_SetFriendlyFire(1);
    }
    if (starts_with("ff off")) {
        return FE_SetFriendlyFire(0);
    }
    if (starts_with("diag")) {
        return FE_DumpDiagnostics();
    }
    if (starts_with("globals")) {
        const std::size_t space = command.find(' ');
        const std::string filter =
            (space == std::string::npos) ? std::string{} : command.substr(space + 1);
        return FE_LogGlobals(filter.c_str(), 64);
    }
    if (starts_with("lobbytest")) {
        return FE_LobbySelfTest(20);
    }
    if (starts_with("host")) {
        // Defaults chosen so the command is useful with no arguments. The scenario is a
        // campaign level because those are the only levels this build ships, and
        // friends_only keeps a test host off the public list.
        return FE_HostSession("slayer", "a30", "/Game/Levels/Halo1/Solo/A30/A30", 4, 1);
    }
    if (starts_with("leave")) {
        return FE_LeaveSession();
    }
    if (starts_with("invite")) {
        return FE_OpenInviteOverlay();
    }
    if (starts_with("events")) {
        // Names every event seen on the multiplayer button, and marks the most recent one.
        // Click the button, then run this: whatever appears at the end is the click.
        std::lock_guard lock(fe::g_state_mutex);
        if (!fe::g_state || !fe::g_state->objects.has_value()) {
            return -static_cast<int>(fe::ErrorCode::InvalidState);
        }
        const std::vector<std::uintptr_t> seen = fe::unreal::SeenWidgetEvents();
        const std::uintptr_t              last = fe::unreal::LastWidgetEvent();

        fe::log::Write(fe::log::Level::Info, "Mod",
                       std::format("{} distinct event(s) seen on the watched widget",
                                   seen.size()));
        for (const std::uintptr_t function : seen) {
            std::string name = "?";
            fe::g_state->objects->ForEach([&](const fe::unreal::ObjectInfo& object) {
                if (object.address != function) {
                    return true;
                }
                name = object.name;
                return false;
            });
            fe::log::Write(fe::log::Level::Info, "Mod",
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
        fe::unreal::LobbyUIContext ui;
        fe::Result                 outcome = fe::Result::Success();
        {
            std::lock_guard lock(fe::g_state_mutex);
            if (!fe::g_state || !fe::g_state->objects.has_value()) {
                return -static_cast<int>(fe::ErrorCode::InvalidState);
            }
            if (const fe::Result resolved =
                    fe::unreal::ResolveLobbyUI(*fe::g_state->objects, ui);
                !resolved.ok()) {
                fe::log::Write(fe::log::Level::Error, "Mod",
                               std::format("probe unavailable: {}", resolved.message()));
                return -static_cast<int>(fe::ErrorCode::InvalidState);
            }
        }
        const fe::Result ran = fe::unreal::RunOnGameThread(
            [&]() { outcome = fe::unreal::ProbeLobbyUI(ui); }, 20000);
        if (!ran.ok() || !outcome.ok()) {
            fe::log::Write(fe::log::Level::Error, "Mod",
                           std::format("probe failed: {}",
                                       ran.ok() ? outcome.message() : ran.message()));
            return -static_cast<int>(fe::ErrorCode::InvalidState);
        }
        return 0;
    }
    if (starts_with("mpbutton")) {
        fe::Result     outcome = fe::Result::Success();
        std::uintptr_t button  = 0;
        const fe::Result ran = fe::unreal::RunOnGameThread(
            [&]() {
                std::lock_guard lock(fe::g_state_mutex);
                if (!fe::g_state || !fe::g_state->objects.has_value()) {
                    outcome = fe::Result::Fail(fe::ErrorCode::InvalidState, "not ready");
                    return;
                }
                outcome = fe::unreal::AddMainMenuButton(*fe::g_state->objects, "MULTIPLAYER",
                                                        button);
            },
            20000);
        if (!ran.ok()) {
            fe::log::Write(fe::log::Level::Error, "Mod",
                           std::format("could not reach the game thread: {}", ran.message()));
            return -static_cast<int>(fe::ErrorCode::Timeout);
        }
        if (!outcome.ok()) {
            fe::log::Write(fe::log::Level::Error, "Mod",
                           std::format("mpbutton failed: {}", outcome.message()));
            return -static_cast<int>(fe::ErrorCode::InvalidState);
        }
        return 0;
    }
    if (starts_with("menu")) {
        // Reports the live main menu and what its button container and existing entries
        // actually are. Adding an entry means creating one of the same class the game
        // already uses, so that class has to be read rather than guessed.
        //   +0x560 MainButtonContainer   +0x568 PlayCoop   +0x508 CampaignMenuButton
        std::lock_guard lock(fe::g_state_mutex);
        if (!fe::g_state || !fe::g_state->objects.has_value()) {
            return -static_cast<int>(fe::ErrorCode::InvalidState);
        }
        const fe::unreal::ObjectArray& objects = *fe::g_state->objects;

        std::uintptr_t menu = 0;
        objects.ForEach([&](const fe::unreal::ObjectInfo& object) {
            if (object.name.rfind("Default__", 0) == 0 ||
                object.class_name != "WBP_MainMenu_C") {
                return true;
            }
            menu = object.address;
            return false;
        });
        if (menu == 0) {
            fe::log::Write(fe::log::Level::Warn, "Mod", "no live WBP_MainMenu_C");
            return -static_cast<int>(fe::ErrorCode::InvalidState);
        }
        fe::log::Write(fe::log::Level::Info, "Mod",
                       std::format("main menu at 0x{:X}", menu));

        struct Slot { const char* label; std::uintptr_t offset; };
        static constexpr Slot kSlots[] = {
            {"MainButtonContainer", 0x560}, {"PlayCoop", 0x568},
            {"CampaignMenuButton", 0x508},  {"QuitButton", 0x570},
        };
        for (const Slot& slot : kSlots) {
            std::uintptr_t pointer = 0;
            if (!fe::unreal::memory::GuardedRead(menu + slot.offset, &pointer,
                                                 sizeof(pointer)) ||
                pointer == 0) {
                fe::log::Write(fe::log::Level::Warn, "Mod",
                               std::format("  {:<22} unreadable", slot.label));
                continue;
            }
            std::string class_name = "?";
            std::string object_name = "?";
            objects.ForEach([&](const fe::unreal::ObjectInfo& candidate) {
                if (candidate.address != pointer) {
                    return true;
                }
                class_name  = candidate.class_name;
                object_name = candidate.name;
                return false;
            });
            fe::log::Write(fe::log::Level::Info, "Mod",
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
        fe::Result        outcome = fe::Result::Success();
        std::uintptr_t    created = 0;

        const fe::Result ran = fe::unreal::RunOnGameThread(
            [&]() {
                std::lock_guard lock(fe::g_state_mutex);
                if (!fe::g_state || !fe::g_state->objects.has_value()) {
                    outcome = fe::Result::Fail(fe::ErrorCode::InvalidState, "not ready");
                    return;
                }
                if (verb.rfind("show ", 0) == 0) {
                    outcome = fe::unreal::ShowWidget(*fe::g_state->objects,
                                                     verb.substr(5), created);
                } else if (verb.rfind("hide", 0) == 0) {
                    outcome = fe::unreal::HideWidget(*fe::g_state->objects, s_open_widget);
                } else {
                    outcome = fe::Result::Fail(fe::ErrorCode::InvalidArgument,
                                               "use: ui show <WidgetClass> | ui hide");
                }
            },
            20000);

        if (!ran.ok()) {
            fe::log::Write(fe::log::Level::Error, "Mod",
                           std::format("could not reach the game thread: {}", ran.message()));
            return -static_cast<int>(fe::ErrorCode::Timeout);
        }
        if (!outcome.ok()) {
            fe::log::Write(fe::log::Level::Error, "Mod",
                           std::format("ui {} failed: {}", verb, outcome.message()));
            return -static_cast<int>(fe::ErrorCode::InvalidState);
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

        std::lock_guard lock(fe::g_state_mutex);
        if (!fe::g_state || !fe::g_state->objects.has_value()) {
            return -static_cast<int>(fe::ErrorCode::InvalidState);
        }

        std::uintptr_t variant = 0;
        fe::g_state->objects->ForEach([&](const fe::unreal::ObjectInfo& object) {
            if (object.name.rfind("Default__", 0) == 0 ||
                object.class_name != "BlamGameEngineCampaignVariant") {
                return true;
            }
            variant = object.address;
            return false;
        });
        if (variant == 0) {
            fe::log::Write(fe::log::Level::Warn, "Mod",
                           "no live BlamGameEngineCampaignVariant; start a mission first");
            return -static_cast<int>(fe::ErrorCode::InvalidState);
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
                if (fe::unreal::memory::GuardedWrite(address, &byte, sizeof(byte))) {
                    ++applied;
                }
            }
            fe::log::Write(fe::log::Level::Info, "Mod",
                           std::format("trait {} = {} on all {} slot(s)", field.name, value,
                                       kPlayerSlots));
        }

        // Report the resulting values so a change is visible rather than assumed.
        fe::log::Write(fe::log::Level::Info, "Mod",
                       std::format("traits on variant 0x{:X} ({} write(s) applied)", variant,
                                   applied));
        for (int slot = 0; slot < kPlayerSlots; ++slot) {
            std::string line = std::format("  player {}:", slot + 1);
            for (const Field& field : kFields) {
                std::uint8_t value = 0;
                const std::uintptr_t address =
                    variant + kTraitsBase + (slot * kTraitStride) + field.offset;
                if (!fe::unreal::memory::GuardedRead(address, &value, sizeof(value))) {
                    value = 0xFF;
                }
                line += std::format(" {}={}", field.name, value);
            }
            fe::log::Write(fe::log::Level::Info, "Mod", line);
        }
        return applied;
    }
    if (starts_with("mp ")) {
        // The lobby surface. These are the game's own calls, so they behave exactly as the
        // menu does, which is why they are preferred over anything hand rolled.
        const std::string verb = command.substr(std::strlen("mp "));

        fe::Result outcome = fe::Result::Success();
        int        players = 0;

        const fe::Result ran = fe::unreal::RunOnGameThread([&]() {
            std::lock_guard lock(fe::g_state_mutex);
            if (!fe::g_state || !fe::g_state->objects.has_value()) {
                outcome = fe::Result::Fail(fe::ErrorCode::InvalidState, "not ready");
                return;
            }
            const fe::unreal::ObjectArray& objects = *fe::g_state->objects;
            if (verb.rfind("open", 0) == 0) {
                outcome = fe::unreal::CallSimple(objects, "MeteoriteLobbyNotifier",
                                                 "BeginAllowInvites");
            } else if (verb.rfind("close", 0) == 0) {
                outcome = fe::unreal::CallSimple(objects, "MeteoriteLobbyNotifier",
                                                 "EndAllowInvites");
            } else if (verb.rfind("players", 0) == 0) {
                outcome = fe::unreal::CallReturningInt(objects, "MeteoriteSquadLobbyViewModel",
                                                       "GetNumSquadMembers", players);
            } else {
                outcome = fe::Result::Fail(fe::ErrorCode::InvalidArgument,
                                           "use: mp open | mp close | mp players");
            }
        });

        if (!ran.ok()) {
            fe::log::Write(fe::log::Level::Error, "Mod",
                           std::format("could not reach the game thread: {}", ran.message()));
            return -static_cast<int>(fe::ErrorCode::Timeout);
        }
        if (!outcome.ok()) {
            fe::log::Write(fe::log::Level::Error, "Mod",
                           std::format("mp {} failed: {}", verb, outcome.message()));
            return -static_cast<int>(fe::ErrorCode::InvalidState);
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
        fe::Result        outcome  = fe::Result::Success();
        const fe::Result  ran      = fe::unreal::RunOnGameThread(
            [&]() {
                std::lock_guard lock(fe::g_state_mutex);
                if (!fe::g_state || !fe::g_state->objects.has_value() ||
                    !fe::g_state->reflection.has_value()) {
                    outcome = fe::Result::Fail(fe::ErrorCode::InvalidState, "not ready");
                    return;
                }
                outcome = fe::unreal::BeginCampaign(*fe::g_state->objects,
                                                    *fe::g_state->reflection, scenario,
                                                    asset, friendly_fire, difficulty);
            },
            60000);
        if (!ran.ok()) {
            fe::log::Write(fe::log::Level::Error, "Mod",
                           std::format("could not reach the game thread: {}", ran.message()));
            return -static_cast<int>(fe::ErrorCode::Timeout);
        }
        if (!outcome.ok()) {
            fe::log::Write(fe::log::Level::Error, "Mod",
                           std::format("campaign start failed: {}", outcome.message()));
            return -static_cast<int>(fe::ErrorCode::InvalidState);
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

        std::lock_guard lock(fe::g_state_mutex);
        if (!fe::g_state || !fe::g_state->objects.has_value()) {
            return -static_cast<int>(fe::ErrorCode::InvalidState);
        }

        int matched = 0;
        fe::g_state->objects->ForEach([&](const fe::unreal::ObjectInfo& object) {
            if (object.class_name != "Function" && object.class_name != "DelegateFunction") {
                return true;
            }
            if (object.name.find(fragment) == std::string::npos) {
                return true;
            }
            fe::log::Write(fe::log::Level::Info, "Mod",
                           std::format("  {:<44} {}", object.name,
                                       fe::g_state->objects->BuildPath(object)));
            return ++matched < 80;
        });
        fe::log::Write(fe::log::Level::Info, "Mod",
                       std::format("'{}' matched {} function(s)", fragment, matched));
        return matched;
    }
    if (starts_with("pe detect")) {
        std::lock_guard lock(fe::g_state_mutex);
        if (!fe::g_state || !fe::g_state->objects.has_value()) {
            return -static_cast<int>(fe::ErrorCode::InvalidState);
        }
        fe::unreal::CallLayout layout;
        const fe::Result detected =
            fe::unreal::DetectCallLayout(*fe::g_state->objects, layout);
        if (!detected.ok()) {
            fe::log::Write(fe::log::Level::Error, "Mod",
                           std::format("ProcessEvent detection failed: {}",
                                       detected.message()));
            return -static_cast<int>(fe::ErrorCode::InvalidState);
        }
        return layout.vtable_slot;
    }
    if (starts_with("gt test")) {
        // Proves the game thread can be reached at all, before anything is called on it.
        // The job records the thread it ran on, which must differ from the caller's.
        const DWORD caller = ::GetCurrentThreadId();
        DWORD       ran_on = 0;
        const fe::Result ran = fe::unreal::RunOnGameThread([&ran_on]() {
            ran_on = ::GetCurrentThreadId();
        });
        if (!ran.ok()) {
            fe::log::Write(fe::log::Level::Error, "Mod",
                           std::format("game thread dispatch failed: {}", ran.message()));
            return -static_cast<int>(fe::ErrorCode::Timeout);
        }
        fe::log::Write(fe::log::Level::Info, "Mod",
                       std::format("game thread dispatch OK: job ran on thread {}, caller "
                                   "was {}",
                                   ran_on, caller));
        return static_cast<int>(ran_on);
    }
    if (starts_with("exec ")) {
        const std::string commandText = command.substr(std::strlen("exec "));
        fe::Result        outcome = fe::Result::Success();
        const fe::Result  ran = fe::unreal::RunOnGameThread([&]() {
            std::lock_guard lock(fe::g_state_mutex);
            if (!fe::g_state || !fe::g_state->objects.has_value()) {
                outcome = fe::Result::Fail(fe::ErrorCode::InvalidState, "no object array");
                return;
            }
            outcome = fe::unreal::ExecuteConsoleCommand(*fe::g_state->objects, commandText);
        });
        if (!ran.ok()) {
            fe::log::Write(fe::log::Level::Error, "Mod",
                           std::format("could not reach the game thread: {}", ran.message()));
            return -static_cast<int>(fe::ErrorCode::Timeout);
        }
        if (!outcome.ok()) {
            fe::log::Write(fe::log::Level::Error, "Mod",
                           std::format("command failed: {}", outcome.message()));
            return -static_cast<int>(fe::ErrorCode::InvalidState);
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

        fe::Result       outcome = fe::Result::Success();
        const fe::Result ran = fe::unreal::RunOnGameThread([&]() {
            std::lock_guard lock(fe::g_state_mutex);
            if (!fe::g_state || !fe::g_state->objects.has_value()) {
                outcome = fe::Result::Fail(fe::ErrorCode::InvalidState, "no object array");
                return;
            }
            outcome = fe::unreal::Travel(*fe::g_state->objects, url);
        },
        // Travel tears down a world and loads another, which takes far longer than a
        // normal call. Five seconds reported a timeout for work that had in fact started
        // correctly, which is worse than useless because it looks like a failure.
        60000);
        if (!ran.ok()) {
            fe::log::Write(fe::log::Level::Error, "Mod",
                           std::format("could not reach the game thread: {}", ran.message()));
            return -static_cast<int>(fe::ErrorCode::Timeout);
        }
        if (!outcome.ok()) {
            fe::log::Write(fe::log::Level::Error, "Mod",
                           std::format("travel failed: {}", outcome.message()));
            return -static_cast<int>(fe::ErrorCode::InvalidState);
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

        std::lock_guard lock(fe::g_state_mutex);
        if (!fe::g_state || !fe::g_state->objects.has_value()) {
            return -static_cast<int>(fe::ErrorCode::InvalidState);
        }

        const std::vector<fe::unreal::ObjectInfo> found =
            fe::g_state->objects->FindByName(wanted, 8);
        const fe::unreal::ObjectInfo* instance = nullptr;
        for (const fe::unreal::ObjectInfo& candidate : found) {
            // Class objects and defaults share a vtable with the class machinery rather
            // than with instances, so a real instance is what is wanted here.
            if (candidate.class_name != "Class" &&
                candidate.name.rfind("Default__", 0) != 0) {
                instance = &candidate;
                break;
            }
        }
        if (instance == nullptr) {
            fe::log::Write(fe::log::Level::Warn, "Mod",
                           std::format("no live non default instance named '{}'", wanted));
            return 0;
        }

        std::uintptr_t table = 0;
        if (!fe::unreal::memory::GuardedRead(instance->address, &table, sizeof(table)) ||
            table == 0) {
            fe::log::Write(fe::log::Level::Warn, "Mod", "could not read the virtual table");
            return 0;
        }

        const auto base = reinterpret_cast<std::uintptr_t>(::GetModuleHandleW(nullptr));
        fe::log::Write(fe::log::Level::Info, "Mod",
                       std::format("{} ({}) at 0x{:X}, vtable 0x{:X} (RVA 0x{:X})",
                                   instance->name, instance->class_name, instance->address,
                                   table, table - base));

        int printed = 0;
        for (int slot = 0; slot < 96; ++slot) {
            std::uintptr_t entry = 0;
            if (!fe::unreal::memory::GuardedRead(table + slot * sizeof(entry), &entry,
                                                 sizeof(entry))) {
                break;
            }
            if (entry == 0) {
                break;
            }
            fe::log::Write(fe::log::Level::Info, "Mod",
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

        std::lock_guard lock(fe::g_state_mutex);
        if (!fe::g_state || !fe::g_state->objects.has_value() ||
            !fe::g_state->reflection.has_value()) {
            return -static_cast<int>(fe::ErrorCode::InvalidState);
        }

        const std::vector<fe::unreal::ObjectInfo> found =
            fe::g_state->objects->FindByName(wanted, 4);
        if (found.empty()) {
            fe::log::Write(fe::log::Level::Warn, "Mod",
                           std::format("no object named '{}'", wanted));
            return 0;
        }

        int total = 0;
        for (const fe::unreal::ObjectInfo& object : found) {
            const std::vector<fe::unreal::PropertyInfo> properties =
                fe::g_state->reflection->ReadProperties(object.address);
            fe::log::Write(fe::log::Level::Info, "Mod",
                           std::format("{} ({}) has {} property(ies):", object.name,
                                       object.class_name, properties.size()));
            for (const fe::unreal::PropertyInfo& property : properties) {
                // A StructProperty only reports its own class, which says nothing about
                // what is inside it. Resolving the inner type turns a wall of
                // "StructProperty" into a readable tree, which is what makes nested
                // settings like player traits explorable at all.
                std::string detail = property.type_name;
                if (property.type_name == "StructProperty") {
                    const std::uintptr_t inner =
                        fe::g_state->reflection->ResolveStructPropertyInner(property.address);
                    if (inner != 0) {
                        std::string inner_name;
                        fe::g_state->objects->ForEach(
                            [&](const fe::unreal::ObjectInfo& candidate) {
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
                fe::log::Write(fe::log::Level::Info, "Mod",
                               std::format("  +0x{:<4X} {:<40} {}", property.offset,
                                           property.name, detail));
                ++total;
            }
        }
        return total;
    }
    if (starts_with("pvp on")) {
        fe::g_enforce_friendly_fire.store(true, std::memory_order_release);
        const int applied = FE_SetFriendlyFire(1);
        fe::log::Write(fe::log::Level::Info, "Mod",
                       std::format("pvp: friendly fire enabled on {} field(s) and will be "
                                   "held on through level loads",
                                   applied));
        return applied;
    }
    if (starts_with("pvp off")) {
        fe::g_enforce_friendly_fire.store(false, std::memory_order_release);
        const int applied = FE_SetFriendlyFire(0);
        fe::log::Write(fe::log::Level::Info, "Mod", "pvp: friendly fire disabled");
        return applied;
    }
    if (starts_with("pvp status")) {
        fe::log::Write(fe::log::Level::Info, "Mod",
                       std::format("pvp: enforcement is {}",
                                   fe::g_enforce_friendly_fire.load(std::memory_order_acquire)
                                       ? "ON"
                                       : "off"));
        return FE_LogFriendlyFire();
    }
    if (starts_with("fname")) {
        fe::unreal::TrampolineInfo info;
        const fe::Result installed = fe::unreal::InstallFNameTrampoline(info);
        if (!installed.ok()) {
            fe::log::Write(fe::log::Level::Error, "Mod",
                           std::format("FName adapter unavailable: {}", installed.message()));
            return -static_cast<int>(fe::ErrorCode::InvalidState);
        }
        fe::log::Write(fe::log::Level::Info, "Mod",
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
            const std::string argument = command.substr(space + 1);
            wanted.clear();
            for (const char character : argument) {
                wanted.push_back(static_cast<wchar_t>(character));
            }
        }

        std::uint32_t    index = 0;
        const fe::Result tested = fe::unreal::TestFNameTrampoline(wanted.c_str(), index);
        if (!tested.ok()) {
            fe::log::Write(fe::log::Level::Error, "Mod",
                           std::format("FName self test failed: {}", tested.message()));
            return -static_cast<int>(fe::ErrorCode::InvalidState);
        }
        std::string narrow;
        for (const wchar_t character : wanted) {
            narrow.push_back(static_cast<char>(character));
        }
        fe::log::Write(fe::log::Level::Info, "Mod",
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
            fe::log::Write(fe::log::Level::Warn, "Mod", "usage: objects <class name fragment>");
            return -static_cast<int>(fe::ErrorCode::InvalidArgument);
        }
        return fe::LogObjectsMatching(command.substr(space + 1), 400);
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
        fe::log::Write(fe::log::Level::Info, "Mod",
                       "=== networking object graph ===================================");
        for (const char* fragment : kNetFragments) {
            total += fe::LogObjectsMatching(fragment, 24);
        }
        fe::log::Write(fe::log::Level::Info, "Mod",
                       std::format("=== {} networking object(s) total ===", total));
        return total;
    }
    if (starts_with("trace ff")) {
        // Arms a hardware watchpoint on every located friendly fire copy, up to the
        // hardware limit of four. This is what identifies the consumer: whichever
        // instruction reads the byte gets recorded with its module and offset.
        std::lock_guard lock(fe::g_state_mutex);
        if (!fe::g_state) {
            return -static_cast<int>(fe::ErrorCode::InvalidState);
        }
        fe::debugtrap::DisarmAll();
        fe::debugtrap::ClearHits();

        int armed = 0;
        for (const fe::ModState::WatchedField& field : fe::g_state->watched) {
            if (!field.valid || armed >= 4) {
                continue;
            }
            // Class default objects are templates that nothing plays from, so the four
            // scarce slots go to real instances.
            if (field.label.find("Default__") != std::string::npos) {
                continue;
            }
            const fe::Result armed_ok =
                fe::debugtrap::Arm(field.address, 1, fe::debugtrap::Condition::ReadWrite,
                                   field.label);
            if (armed_ok.ok()) {
                ++armed;
                fe::log::Write(fe::log::Level::Info, "Mod",
                               std::format("tracing {} at 0x{:X}", field.label, field.address));
            } else {
                fe::log::Write(fe::log::Level::Warn, "Mod",
                               std::format("could not trace {}: {}", field.label,
                                           armed_ok.message()));
            }
        }
        fe::log::Write(fe::log::Level::Info, "Mod",
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
            fe::log::Write(fe::log::Level::Warn, "Mod",
                           std::format("'{}' is not a hex module offset", argument));
            return -static_cast<int>(fe::ErrorCode::InvalidArgument);
        }

        const auto base = reinterpret_cast<std::uintptr_t>(::GetModuleHandleW(nullptr));
        const std::uintptr_t address = base + offset;

        const fe::Result armed = fe::debugtrap::Arm(
            address, 1, fe::debugtrap::Condition::Execute, std::format("exec+0x{:X}", offset));
        if (!armed.ok()) {
            fe::log::Write(fe::log::Level::Error, "Mod",
                           std::format("could not arm execution trap: {}", armed.message()));
            return -static_cast<int>(fe::ErrorCode::InvalidState);
        }
        fe::log::Write(fe::log::Level::Info, "Mod",
                       std::format("execution trap armed on RVA 0x{:X} (0x{:X}); run "
                                   "'trace hits' after a few seconds to see the call rate",
                                   offset, address));
        return 0;
    }
    if (starts_with("trace hits")) {
        const std::vector<fe::debugtrap::Hit> hits = fe::debugtrap::Hits();
        fe::log::Write(fe::log::Level::Info, "Mod",
                       std::format("{} distinct instruction(s) touched the watched byte(s)",
                                   hits.size()));
        for (const fe::debugtrap::Hit& hit : hits) {
            fe::log::Write(fe::log::Level::Info, "Mod",
                           std::format("  {}+0x{:X} hit {} time(s) on address 0x{:X} "
                                       "(thread {}, rip 0x{:X})",
                                       hit.module_name, hit.module_offset, hit.count,
                                       hit.address, hit.thread_id, hit.instruction));
        }
        return static_cast<int>(hits.size());
    }
    if (starts_with("trace off")) {
        fe::debugtrap::DisarmAll();
        return 0;
    }
    if (starts_with("watch")) {
        std::lock_guard lock(fe::g_state_mutex);
        if (!fe::g_state) {
            return -static_cast<int>(fe::ErrorCode::InvalidState);
        }
        fe::log::Write(fe::log::Level::Info, "Mod",
                       std::format("{} field(s) watched", fe::g_state->watched.size()));
        for (const fe::ModState::WatchedField& field : fe::g_state->watched) {
            fe::log::Write(fe::log::Level::Info, "Mod",
                           std::format("  {} at 0x{:X} = {} {}", field.label, field.address,
                                       field.last_value, field.valid ? "" : "(stale)"));
        }
        return static_cast<int>(fe::g_state->watched.size());
    }

    fe::log::Write(fe::log::Level::Warn, "Mod", std::format("unknown command: '{}'", command));
    return -static_cast<int>(fe::ErrorCode::NotFound);
}

/// Writes the symbol discovery report into the log. The single most useful thing
/// a user can do when asked to help diagnose a new game build.
__declspec(dllexport) int FE_DumpDiagnostics() {
    std::lock_guard lock(fe::g_state_mutex);
    if (!fe::g_state) {
        return -static_cast<int>(fe::ErrorCode::InvalidState);
    }
    if (!fe::g_state->symbols.has_value()) {
        fe::log::Write(fe::log::Level::Error, "Mod",
                       "symbol discovery did not complete, so there is no report");
        return -static_cast<int>(fe::ErrorCode::SymbolNotResolved);
    }
    fe::log::Write(fe::log::Level::Info, "Mod", fe::g_state->symbols->BuildDiscoveryReport());
    return 0;
}

} // extern "C"

// ---------------------------------------------------------------------------
// Module lifecycle
// ---------------------------------------------------------------------------

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID reserved) {
    switch (reason) {
        case DLL_PROCESS_ATTACH: {
            fe::g_self_module = module;
            ::DisableThreadLibraryCalls(module);

            // Startup polls for the engine and for Steam, so it cannot run under
            // the loader lock.
            const HANDLE thread =
                ::CreateThread(nullptr, 0, &fe::InitializeThread, nullptr, 0, nullptr);
            if (thread != nullptr) {
                ::CloseHandle(thread);
            }
            break;
        }

        case DLL_PROCESS_DETACH:
            // On process termination Windows has already stopped other threads, so
            // joining ours would deadlock. Only clean up on an explicit unload.
            if (reserved == nullptr) {
                fe::Shutdown();
            }
            break;

        default:
            break;
    }
    return TRUE;
}







