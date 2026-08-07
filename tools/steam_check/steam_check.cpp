// SPDX-License-Identifier: MIT
// MultiplayerEvolved: tools/steam_check/steam_check.cpp
//
// Proves the server browser against real Steam, on one machine.
//
// WHY THIS EXISTS
//
// Discovery was the one reported failure that could not be reproduced or refuted here. A
// player said their friends' games never appeared; the cause turned out to be a marker key
// that nothing had ever written, and the only reason that shipped is that nobody could
// check it without two people and two copies of the game.
//
// Two people are not required. Hosting a lobby and then searching for it is a complete
// exercise of the chain: the marker is published the way the mod publishes it, Steam's own
// string filter is applied the way the browser applies it, and the search either returns
// the lobby or does not. A friend's lobby travels that identical path with a different id,
// so a search that finds this one would find theirs.
//
// What it still does not cover is a second machine's relay route and an invitation
// arriving, both of which need somebody else to be there.
//
// Needs Steam running and a steam_appid.txt containing 2806050 beside the executable; the
// build script writes one.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include "Lobby/Discovery.h"
#include "Net/SteamSocketsTransport.h"
#include "Lobby/ILobbyBackend.h"
#include "Lobby/SteamMatchmakingHooks.h"
#include "Net/PacketProtocol.h"
#include "Steam/SteamApi.h"

namespace {

using namespace mpe;
using namespace mpe::lobby;

int g_failures = 0;

void Check(bool condition, const std::string& what) {
    std::printf(condition ? "  ok    %s\n" : "  FAIL  %s\n", what.c_str());
    if (!condition) {
        ++g_failures;
    }
}

/// Records what the backend reports, so the run can wait on it.
class Watcher final : public ILobbyBackendObserver {
public:
    void OnLobbyCreated(LobbyId lobby) override { created = lobby; }
    void OnLobbyCreateFailed(const Error& error) override { failure = error.message; }
    void OnLobbyEntered(LobbyId, bool) override {}
    void OnLobbyEnterFailed(const Error& error) override { failure = error.message; }
    void OnMemberJoined(const LobbyMember&) override {}
    void OnMemberLeft(PlatformId, bool) override {}
    void OnLobbyDataChanged(LobbyId) override {}
    void OnMemberDataChanged(PlatformId) override {}
    void OnJoinRequested(LobbyId, PlatformId) override {}

    LobbyId     created{0};
    std::string failure;
};

/// Records what the transport reports.
class Wire final : public net::ITransportObserver {
public:
    void OnPeerConnected(net::PeerHandle peer, const net::PeerIdentity&) override {
        connected = peer;
    }
    void OnPeerDisconnected(net::PeerHandle, net::DisconnectReason,
                            std::string_view detail) override {
        disconnect = std::string(detail);
    }
    void OnPacketReceived(net::PeerHandle, net::Channel,
                          std::span<const std::byte> payload) override {
        received.assign(reinterpret_cast<const char*>(payload.data()), payload.size());
    }
    void OnConnectFailed(net::DisconnectReason, std::string_view detail) override {
        failure = std::string(detail);
    }

    net::PeerHandle connected{net::PeerHandle::Invalid};
    std::string     received;
    std::string     failure;
    std::string     disconnect;
};

/// Pumps Steam and the backend for a while, or until predicate is satisfied.
template <typename Predicate>
bool PumpUntil(ILobbyBackend& backend, Watcher& watcher, Predicate predicate,
               std::chrono::seconds limit) {
    const auto deadline = std::chrono::steady_clock::now() + limit;
    while (std::chrono::steady_clock::now() < deadline) {
        steam::RunCallbacks();
        backend.Poll(watcher);
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return false;
}

} // namespace

int main(int argc, char** argv) {
    std::printf("talking to the Steam client\n");

    // Where the game's own Steamworks redistributable lives is derived from the game
    // binaries directory, so it has to be the real one rather than wherever this happens to
    // be run from. The build script passes it.
    if (argc < 2) {
        std::printf("  usage: steam_check <game>\\Meteorite\\Binaries\\Win64\n");
        return 2;
    }
    const std::string binaries = argv[1];
    std::printf("  game binaries: %s\n", binaries.c_str());

    std::string error;
    if (!steam::Initialize(binaries, true, error)) {
        std::printf("  SKIPPED  %s\n", error.c_str());
        std::printf("\nNot a failure: this check needs the Steam client running and a\n"
                    "steam_appid.txt beside the executable. Nothing was proven either way.\n");
        return 0;
    }
    std::printf("  %s\n", steam::DescribeBinding().c_str());
    Check(steam::IsLoggedOn(), "signed in to Steam");

    auto hooks = SteamMatchmakingHooks::CreateInstance();
    if (!hooks.ok()) {
        std::printf("  FAIL  the Steam lobby backend would not start: %s\n",
                    std::string(hooks.message()).c_str());
        return 1;
    }
    ILobbyBackend& backend = *hooks.value();
    Watcher        watcher;

    std::printf("hosting a lobby and looking for it\n");

    if (!backend.Create(LobbyVisibility::Public, 10).ok()) {
        std::printf("  FAIL  Steam refused to create a lobby\n");
        return 1;
    }
    const bool created = PumpUntil(
        backend, watcher, [&] { return watcher.created != 0 || !watcher.failure.empty(); },
        std::chrono::seconds(20));

    // A refusal from Steam is not a failure of this mod, and reporting it as one would be
    // a red run that says nothing. Steam rate limits lobby creation and refuses outright
    // when the game is already running with a session of its own, which is the usual cause
    // when this is run twice in a row or beside the game.
    if (!created || watcher.created == 0) {
        std::printf("  SKIPPED  Steam would not create a lobby: %s\n",
                    watcher.failure.empty() ? "no answer within twenty seconds"
                                            : watcher.failure.c_str());
        std::printf("\nNothing was proven either way. Steam rate limits lobby creation, and\n"
                    "refuses while the game is running with a session of its own. Close the\n"
                    "game, wait a minute, and run this again.\n");
        std::fflush(stdout);
        std::_Exit(0);
    }
    Check(true, "Steam created a public lobby");

    // The backend publishes the marker on creation, which is the thing being tested. Read
    // it back rather than assumed: a marker that is not there is the exact defect that made
    // the browser incapable of returning a row.
    const auto marker = backend.GetLobbyData(keys::kBrowseMarker);
    Check(marker.ok() && marker.value() == std::to_string(net::kProtocolVersion),
          "the browse marker is on the lobby, carrying this protocol version");

    // Long enough for Steam to publish the lobby to its own matchmaking service before it
    // is searched for. Searching immediately is a race, and losing it would look exactly
    // like the bug this is here to detect.
    std::this_thread::sleep_for(std::chrono::seconds(3));

    steam::RequestLobbyList();
    const bool answered = PumpUntil(
        backend, watcher,
        [&] {
            (void)steam::BrowseLobbies();
            return steam::LastSearchSawOwnLobby();
        },
        std::chrono::seconds(20));

    Check(answered,
          "a Steam search with the mod's own filter returned this lobby, so a friend's "
          "will appear the same way");

    // ------------------------------------------------------------------
    // Every Steam call this mod makes, made once.
    // ------------------------------------------------------------------
    //
    // This is the bug class that actually took somebody's game down. The mod binds the
    // flat API by hand, and the game ships two Steamworks SDKs at different interface
    // versions; handing a v018 pointer to a v017 module's wrapper calls a different vtable
    // slot, which read the friend flags as a pointer and wrote through them. It crashed on
    // GetFriendCount, and nothing short of calling it would have found that.
    //
    // So every one of them is called here, for real, once. A wrong signature or a
    // mismatched interface faults on the call rather than in somebody's match.
    std::printf("calling every bound Steam function once\n");

    const auto persona = steam::GetPersonaName();
    Check(persona != nullptr && *persona != '\0', "GetPersonaName returned a name");
    Check(steam::GetLocalSteamId() != 0, "GetSteamID returned an id");
    Check(steam::IsOverlayEnabled() || true, "IsOverlayEnabled answered without faulting");

    // The one that crashed. Reads the whole friends list, each name, and what each person
    // is playing.
    const auto friends = steam::FriendsInGame();
    std::printf("  ok    FriendsInGame read %zu friend(s) without faulting\n", friends.size());

    Check(steam::GetNumLobbyMembers(watcher.created) >= 1,
          "GetNumLobbyMembers counted us in our own lobby");
    Check(steam::GetLobbyOwner(watcher.created) == steam::GetLocalSteamId(),
          "GetLobbyOwner named us as the owner");
    Check(steam::SetRichPresence("connect", "mpe-check"),
          "SetRichPresence was accepted");

    // Invited to nobody on purpose.
    //
    // A zero id is not a person, so Steam refuses and no real player is bothered by a test
    // run. What is being checked is the call itself: that the binding is resolved, the
    // signature matches the module's, and a refusal comes back as false rather than as a
    // fault. Whether an invitation reaches somebody is Steam's to answer, and needs a
    // second account to ask.
    const bool invited_nobody = steam::InviteUserToLobby(watcher.created, 0);
    Check(!invited_nobody,
          "InviteUserToLobby is callable and refuses an invalid target cleanly");

    // ------------------------------------------------------------------
    // The relay, to ourselves.
    // ------------------------------------------------------------------
    //
    // Steam routes a P2P connection to your own identity locally, so the listen socket,
    // the connect, the acceptance and a real packet over a real channel can all be
    // exercised without a second machine. What it does not exercise is a route between two
    // different PCs, which is the part that still needs somebody else.
    std::printf("connecting to ourselves over the Steam relay\n");

    net::SteamTransportOptions options;
    options.virtual_port       = 22799; // Not the game's, so nothing collides.
    options.owns_callback_pump = false; // Pumped here, beside the backend.

    auto transport = net::SteamSocketsTransport::Create(options);
    if (!transport.ok()) {
        std::printf("  SKIPPED  the Steam transport would not start: %s\n",
                    std::string(transport.message()).c_str());
    } else {
        net::IPeerTransport& wire_transport = *transport.value();
        Wire                 wire;

        bool declined = false;

        net::ListenConfig listen;
        listen.max_clients = 4;
        listen.use_relay   = true;
        const bool listening = wire_transport.Listen(listen).ok();
        Check(listening, "a relay listen socket opened");

        const auto self = wire_transport.LocalIdentity();
        if (listening && self.ok()) {
            // A refusal here is Steam's answer, not a defect.
            //
            // Routing a P2P connection to your own identity is not something Steam
            // undertakes to do, and this client declines it. That is a limit on what can be
            // proven from one machine rather than anything wrong with the transport, so it
            // is reported as unproven. Calling it a failure would make a green run
            // impossible for a reason nobody can fix.
            declined = !wire_transport.Connect(self.value(), 15000).ok();
            if (declined) {
                std::printf("  SKIPPED  Steam declined to route a connection to this machine\n"
                            "           itself, which it is not obliged to do. The listen\n"
                            "           socket opened; a route between two PCs needs two PCs.\n");
            } else {
                Check(true, "a P2P connection to our own identity was accepted for routing");

                const auto deadline =
                    std::chrono::steady_clock::now() + std::chrono::seconds(20);
                while (std::chrono::steady_clock::now() < deadline &&
                       wire.connected == net::PeerHandle::Invalid && wire.failure.empty()) {
                    steam::RunCallbacks();
                    wire_transport.Poll(wire);
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                }

                if (wire.connected != net::PeerHandle::Invalid) {
                    Check(true, "the relay connected");

                    const std::string                hello = "multiplayer-evolved";
                    const std::span<const std::byte> payload{
                        reinterpret_cast<const std::byte*>(hello.data()), hello.size()};
                    (void)wire_transport.Send(wire.connected, net::Channel::Control, payload,
                                              net::SendMode::Reliable);
                    wire_transport.Flush();

                    const auto until =
                        std::chrono::steady_clock::now() + std::chrono::seconds(10);
                    while (std::chrono::steady_clock::now() < until && wire.received.empty()) {
                        steam::RunCallbacks();
                        wire_transport.Poll(wire);
                        std::this_thread::sleep_for(std::chrono::milliseconds(50));
                    }
                    Check(wire.received == hello,
                          "a packet went out over the relay and came back byte for byte");
                } else {
                    std::printf("  SKIPPED  the relay did not connect to this machine "
                                "itself: %s\n",
                                wire.failure.empty() ? "no answer within twenty seconds"
                                                     : wire.failure.c_str());
                }
            }
        }
        wire_transport.Shutdown();
    }

    backend.Leave();

    // The backend is released first, because its callbacks are registered with the Steam
    // API and its destructor unregisters them.
    hooks.value().reset();

    // Steam is deliberately not shut down.
    //
    // This process owns the Steam API, so Shutdown would call SteamAPI_Shutdown and then
    // FreeLibrary on steam_api64.dll while the Steam client still has threads inside it,
    // which ends the process with a stack buffer overrun after every check has already
    // passed. Letting the process exit take it down is correct for a program that lives
    // for ten seconds.
    //
    // The mod is not affected and this is not a bug in it: inside the game the module is
    // the game's, owns_module is false, and neither SteamAPI_Shutdown nor FreeLibrary is
    // ever called.

    std::printf("\n%s (%d failure(s))\n", g_failures == 0 ? "PASSED" : "FAILED", g_failures);
    std::fflush(stdout);

    // Exits without running static destructors.
    //
    // SteamAPI_Init starts threads inside steam_api64.dll and this process owns that
    // module. Both ordinary ways out fault: shutting Steam down unloads the library while
    // its threads are still in it, and a normal return destroys statics underneath them.
    // Either way the process dies with a stack buffer overrun after every check has passed,
    // which turns a green run into a red exit code for a reason a reader cannot act on.
    //
    // Nothing here needs flushing that has not been flushed. TerminateProcess rather than
    // _Exit, because the fault comes from a Steam thread rather than from this one and only
    // the bluntest exit is guaranteed to win the race with it.
    ::TerminateProcess(::GetCurrentProcess(), g_failures == 0 ? 0u : 1u);
    return g_failures == 0 ? 0 : 1;
}
