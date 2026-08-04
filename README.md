# Halo Multiplayer Evolved

Competitive multiplayer for Halo: Campaign Evolved, over Steam, with no port
forwarding.

The remake shipped without the multiplayer that made the original worth playing.
This adds it back: the same maps, the same modes, the same feel. Not a
reinterpretation, and not custom content. The multiplayer the game should have had.

---

## For people who just want to play

You need the game on Steam, and a friend who also has this mod. Both of you install
it the same way.

1. Download the latest release.
2. Drop `version.dll`, `ForgeEvolved.dll` and the `ForgeEvolved` folder into:
   ```
   <game folder>/Meteorite/Binaries/Win64/
   ```
3. Start the game normally.

That is the entire install. No launcher, no injector, no admin rights, nothing to
configure. The game already loads a file called `VERSION.dll` at startup, so putting
ours there means it comes along for the ride. To uninstall, delete those three
things.

Once you are in, there is a **MULTIPLAYER** entry at the top of the main menu. It
opens a lobby where you pick a mode and a map, invite people into your session, and
start the match. Empty player slots are buttons; clicking one opens Steam's invite
window pointed at *your match*, so whoever accepts lands in your game.

**Is it safe for my install?** The mod never writes to your game files. If anything
it needs is missing it stays loaded and does nothing, and says why in
`ForgeEvolved.log`. It cannot half-work.

---

## Why this is possible at all

This is the part worth understanding, because it explains why this is a normal
amount of work rather than an impossible amount.

**The game's gameplay does not run on Unreal.** Unreal Engine 5 is the shell: menus,
rendering, input. The actual simulation is `HaloSimulation_tag_release.dll`, a Blam
engine of roughly the Reach or Halo 4 era. That shipped binary still contains the
multiplayer gametypes, a complete host-and-client replication layer, and the
networked map variant pipeline. None of it was removed. It was left in and not
wired up.

What is missing is a way to *reach* it: a transport and a session driver that work
on Steam. The shell binds its networking to PlayFab Party and uses Steam only for
presence, so there is no path from "two people on Steam" to "two people in a Blam
match".

So this mod supplies exactly that and nothing more:

- a transport, `ISteamNetworkingSockets` over the Steam Datagram Relay, which is
  why nobody has to forward a port
- a lobby and session driver
- an in-game menu to drive it

Everything the engine already knows how to do, the engine keeps doing. We are not
reimplementing Halo. We are plugging in the cable.

Full evidence and reasoning: [docs/00-ARCHITECTURE.md](docs/00-ARCHITECTURE.md).

---

## Status

Honest state, not aspirations. "Verified in game" means it was compiled, run, and
watched working. Anything not yet exercised says so.

### Works, confirmed in game

| | |
| --- | --- |
| Loads into the running game without crashing | Verified in game |
| Steam API bound to the game's own `steam_api64.dll` | Verified in game |
| Steam interfaces and callback ABI registration | Verified in game |
| Symbol discovery against the live relocated Blam module | Verified in game, 5/5 required |
| Steam lobby create, metadata, member data, leave | Verified against live Steam |
| **MULTIPLAYER entry in the real main menu** | Verified in game |
| **Full lobby screen: tabs, teams, settings, server browser** | Verified in game |
| Mode and map selection | Verified in game |
| Server browser filters (mode, slots, ping) | Verified in game |
| Starting a match from the lobby | Verified in game |
| Friendly fire toggle that survives a level load | Verified in game |
| Session hosted as a public, discoverable Steam lobby | Verified in game |
| Update check against GitHub releases | Verified in game |

### Built, not yet proven with two people

| | |
| --- | --- |
| Relay transport, listen server over SDR | Initializes; no peer test yet |
| Wire protocol with role and phase authorization | Compiles; unit level only |
| Lobby state machine, roster, ready, synchronized launch | Compiles; not yet exercised |
| Invite overlay pointed at the session | Implemented; not yet confirmed end to end |

### Not done

| | |
| --- | --- |
| **A second player actually joining** | The one test that matters. Never run. |
| Competitive scoring (Slayer kills, CTF captures) | The shipped binary has no reachable gametype code yet |
| Downloading and applying an update | Only the check exists |
| Original multiplayer maps | Blood Gulch and friends are not in this build of the game |

Startup, from `ForgeEvolved.log`:

```
[Mod]          game build: 2026.07.25.1112544.4-Rel-i343-Meteorite-2607-CU3
[Blam.Module]  attached to HaloSimulation_tag_release.dll at 0x7FFE312F0000
[Blam.Symbols] indexed 3649 record(s) across 3 table(s); 5/5 required
[Steam.Api]    net_sockets='SteamNetworkingSockets012' networking_ready=true
[Net.Steam]    steam transport ready (virtual port 22701, relay warm up requested)
[Mod]          lobby handles cached; opening the lobby is now instant
[Mod]          multiplayer entry added to main menu
```

---

## Roadmap

In dependency order. Each one is blocked by the one above it.

1. **Two players in one lobby.** Everything else is guesswork until this happens
   once.
2. **A match both players are in.** Synchronized launch already exists in code and
   has never been run against a real peer.
3. **Competitive scoring.** The engine's gametype code is present in the binary but
   nothing reachable registers it. This is cross-reference work over 7.9 MB of
   `.text`, not more of the code already here.
4. **The original maps.** Blood Gulch, Sidewinder, Hang 'Em High. Not shipped with
   this build of the game, so they have to come from somewhere.
5. **Self-updating installs**, so a protocol change does not silently break
   everyone's session.

---

## Building it yourself

You need Visual Studio 2022 with the C++ desktop workload. That is all — no
Steamworks SDK, no vcpkg, no package manager. The mod binds to the copy of
`steam_api64.dll` the game already ships, by hand.

```bash
build.bat
```

To build and drop it straight into the game:

```bash
build.bat install
```

Output lands in `build/`. The install step copies `ForgeEvolved.dll`, `version.dll`
and the `data/` folder into the game's `Win64` directory.

---

## Layout

```
src/
├── Core/      Result, logging, hashing, bounds-checked serialization
├── Blam/      Module attach, pattern and string scanning, symbol discovery
├── Engine/    IEngineControl, the seam between the mod and the game
├── Net/       Transport interface, wire protocol, Steam relay transport
├── Lobby/     Platform facade, Steam matchmaking hooks, lobby state machine
├── Unreal/    UE5 reflection, game-thread calls, the in-game lobby UI
├── Update/    Release check against GitHub
├── Map/       Map model, JSON and canonical binary, transactional injector
└── ModMain.cpp  Wiring, tick loop, public C API

loader/        The version.dll proxy
data/          Symbol descriptors, shipped with releases
docs/          Architecture, engine binding, packaging
```

---

## Design rules

These are not style preferences. Each exists because its absence caused a specific
failure.

**No hardcoded addresses, ever.** A game update shifts every address, and a
hardcoded offset turns a working mod into a crash on patch day. Everything is found
at runtime by anchoring on the engine's own name strings, which are stable precisely
because the engine depends on them.

**Fail closed.** Discovery either fully succeeds and validates, or the mod stays
loaded and completely inert with the reason in the log. A half-resolved binding that
guesses at a missing entry is how mods corrupt saves.

**No exceptions.** The Blam DLL is built without exception support across its ABI
boundary, and unwinding through engine frames corrupts its state. Every fallible
operation returns `Result` or `Expected<T>`, both `[[nodiscard]]`, so a dropped
failure is a compile warning.

**All remote input is hostile.** `ByteReader` cannot over-read. `DecodePacket`
rejects malformed frames. `IsMessageAcceptable` checks a message against the local
role and phase before the body is parsed, so a client cannot start matches on the
host. Map payloads are hash-verified before parsing.

**Every observer callback arrives on the tick thread.** Steam callbacks fire on the
game's thread, so both Steam integrations copy the event into a queue and do nothing
else. That is why `LobbyManager` contains no locks, and a lock appearing in it means
this contract has been broken somewhere below.

**Three interfaces, not two.** `ILobbyBackend`, `IPeerTransport` and `IEngineControl`
exist so a dedicated server swaps one implementation, a game patch touches one, and
the lobby state machine can be tested with no game and no Steam client.

---

## Contributing

Roughly in order of how self-contained they are:

1. **A symbol descriptor for a new game build.** Data only, no code. See
   `data/symbols/` and the notes inside the existing descriptor.
2. **Testing with two people.** Genuinely the most valuable thing anyone can do
   right now.
3. **Cross-reference work to reach the gametype registrations.** Disassembler and
   debugger work. `PatternScanner::FindRipRelativeReferences` is the starting point
   and is already written.
4. **A Vortex game extension**, so installing becomes one click.

Match the surrounding code. Comments explain why a decision was made, not what a
line does.

---

## Credits

The README structure here was inspired by
[mjolnir-core](https://github.com/devnull9090/mjolnir-core), which is doing parallel
work on the same game and got there first on several of the engine findings.

## Licence

MIT. See LICENSE.

Not affiliated with Microsoft, 343 Industries or Valve. Halo is a trademark of
Microsoft.
