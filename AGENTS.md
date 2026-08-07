# AGENTS.md

Halo Multiplayer Evolved is a C++20 mod for Halo: Campaign Evolved (Steam 2806050, UE 5.5.4).
It injects via a `version.dll` proxy, drives the game's own UMG widgets, and runs sessions
over Steam's relay. No engine source, no SDK, no package manager.

## Commands

```bash
build.bat                       # release build into build\
build.bat debug                 # symbols, no optimisation
build.bat install               # build, then copy into the game folder
build.bat package               # build, then make the release archive in build\
tools\protocol_check\build.bat  # wire protocol checks, ~1 second
tools\session_check\build.bat   # a host and a client against each other, ~1 second
tools\steam_check\build.bat     # discovery against the real Steam client, ~15 seconds
```

`build.bat install` refuses while the game is running, because Windows will not replace a
mapped DLL. Close the game first. It prints the size and modified time of what it copied.

There is no test framework. All three checkers are plain console programs that return
non-zero on failure.

- `protocol_check` covers the message acceptability matrix and the packet bodies. Add cases
  when you touch `src/Net/PacketProtocol.cpp`.
- `session_check` runs a real host and a real client against each other over a loopback
  transport. Add cases when you touch `src/Lobby/LobbyManager.cpp`, and especially when the
  change only shows itself with two people in a session: that is the class of bug that
  otherwise costs two machines, two players and a log from each to find.
- `steam_check` hosts a real lobby and searches for it. It needs the Steam client running
  and the game closed, because Steam rate limits lobby creation and refuses while the game
  holds a session. A refusal is reported as SKIPPED, not as a failure: it proves nothing
  either way.

Its loopback transport keeps a separate queue per channel and lets a case choose the drain
order. Use it. Separate channels are separate lanes with no ordering between them, and
assuming otherwise is what made the first client that ever reached a host disconnect it.

## Verifying a change

The build succeeding proves nothing about what is on disk or in the game.

1. `build.bat install`, and read the printed size and timestamp.
2. Confirm a string only the new build contains is actually in the installed DLL.
3. Read `Meteorite/Binaries/Win64/MultiplayerEvolved/MultiplayerEvolved.log`. The previous
   run is kept beside it as `MultiplayerEvolved.previous.log`.

Do not launch the game to test unless you were asked to. Say the build is ready instead.

## Releasing

Order matters. `main` advertising a version whose tag does not exist is a broken download
for every player.

1. Build, install, verify the installed bytes.
2. `build.bat package` for the archive. Never assemble it by hand: doing that shipped four
   releases whose zip held the two DLLs and nothing else, because the `MultiplayerEvolved`
   folder was created empty and the archiver dropped it. The install instructions told
   players to copy three things when only two existed, and a fresh install got no symbol
   descriptor. The package step stages `data/`, checks the descriptor is in it, and lists
   the archive contents.
3. `gh release create vX.Y.Z --target main` with the three assets.
4. Download the published asset back and hash-match it against the installed file. Check
   the zip's contents too, not just its size.
5. Only then merge the README and `kModVersion` bump.

Bump `kProtocolVersion` in `src/Net/PacketProtocol.h` for any change to an existing message
layout **or** to how a message is handled. The browse marker carries that version, so
incompatible builds stop listing each other instead of failing mid-handshake.

## Git

- One logical change per commit. Split by hunk rather than batching unrelated work.
- Push after every commit.
- `main` is protected. Open a PR from `dev`.
- Conventional messages: `feat:`, `fix:`, `perf:`, `docs:`, `test:`.
- The body says what was broken and why the fix is the right one, not what the diff shows.
- Update the README only when setup, usage or dependencies actually change.
- No AI-assistant attribution anywhere: not in messages, trailers, PR bodies, or comments.

## House rules that are load-bearing

| Rule | Why |
| --- | --- |
| Comments explain **why**, never what | The what is in the line below it |
| Plain prose, no em-dashes | Matches every existing comment and message |
| A refusal must say what to do instead | A silent `return` becomes a bug report months later |
| Log the reason, not just the failure | Most of this project was debugged from its own log |

## Engine facts that cost real time to learn

Each one broke something and was fixed once. Do not re-derive them.

- **`FVector2D` is two doubles**, not floats. Passing floats builds a whole screen that
  draws nothing. See `Vector2` in `src/Unreal/LobbyUI.cpp`.
- **Writing a UPROPERTY only works before Slate builds the widget.** After that, call the
  engine's own setter. `Builder::SetTextLive` in `src/Unreal/LobbyUI.cpp` is the pattern.
- **`ESlateVisibility::Visible` on a container swallows clicks** before its children see
  them. Containers want `SelfHitTestInvisible` (4). This killed the main menu twice.
- **Take Steam interfaces from the module's own accessor export**, never by asking the
  client for a version name. The game ships two Steamworks SDKs at different versions; a
  mismatch calls the wrong vtable slot. See `AcquireInterface` in `src/Steam/SteamApi.cpp`.
- **Channels are independently ordered.** A message sent later can arrive first, so never
  treat an out-of-phase packet as a protocol violation worth disconnecting over.
- **A design point is two screen pixels at 4K.** The lobby is authored at 1920x1080 and
  scaled.
- **`NamePool::FindIndexOf` searches a bounded number of blocks and this pool has 141.**
  It returns nothing for a name that lives past the limit, with no error. Filtering the
  object array on an index from it found zero objects forever and removed the MULTIPLAYER
  entry entirely. Filter on something already known to be correct.

## Performance: what not to do

The mod shares a process with a game that saturates every core while loading.

- **Never scan the object array on a fast timer.** `ObjectArray::ForEach` walks ~50,000
  entries and takes the process address-space lock repeatedly. Polling it four times a
  second instead of once pushed time-to-main-menu from 25 seconds to over five minutes.
- **Cache anything whose address does not change.** UFunction addresses, widget classes and
  the menu button plan are resolved once and kept; only the live menu instance varies.
- **Turning FNames into strings is what a scan actually costs**, not the walking.
  `ForEach` resolves two per object, so a pass is ~100,000 lookups and allocations. Use
  `ForEachRaw`, judge each *class* once and keep the verdict against its FName index, and
  resolve an object's own name only when its class can hold something you want.
- **Gate a scan on `Count()` changing.** It is a single read, and nothing new can appear
  without it moving.
- Prefer the fast path in `TryFastReflection` (reads globals out of instructions, ~2s) over
  searching. The search exists as a fallback and should stay one.

## Layout

| Path | What lives there |
| --- | --- |
| `src/ModMain.cpp` | Orchestration, tick loop, lobby state, exports |
| `src/Unreal/` | Reflection, object array, game-thread calls, the lobby UI |
| `src/Steam/` | Hand-bound Steam flat API, no SDK |
| `src/Net/`, `src/Lobby/` | Wire protocol, transport, session state machine |
| `src/Blam/` | The shipped simulation module and its symbols |
| `loader/` | The `version.dll` proxy and the update swap |
| `docs/` | [Architecture](docs/00-ARCHITECTURE.md), [Engine binding](docs/04-ENGINE-BINDING.md), [Map format](docs/02-MAP-FORMAT.md), [Packaging](docs/03-PACKAGING-AND-NEXUS.md) |

## Keeping this file honest

This file is read in full at the start of every agent session, so every line costs
attention on every task. Earn the space.

- Add a rule when something breaks **twice**, or when it cost more than an hour once.
- Write the rule as the shortest sentence that would have prevented it, and point at the
  code with a path rather than pasting the code.
- Delete a rule the moment it stops being true. A stale rule is worse than a missing one,
  because it will be believed.
- Depth goes in `docs/`. This file stays a map, not the territory.
- Update it in the same commit as the change that made it true.
