# How two players end up in one match

Measured on CU3 against the running game, 2026-08-08.

## The finding that decides the approach

The game's own Blam network components expose these, through UE reflection:

| Class | Function | What it means |
| --- | --- | --- |
| `BlamNetworkPlayerStateComponent` | `ServerSetBlamEndpointIds` | A Server RPC |
| `BlamNetworkPlayerStateComponent` | `ServerSetPrimaryPlayerId` | A Server RPC |
| `BlamNetworkPlayerStateComponent` | `OnRep_EndpointId` | A replicated property |
| `BlamNetworkPlayerStateComponent` | `OnRep_EndpointGeneration` | A replicated property |
| `BlamNetworkGameStateComponent` | `OnRep_bSessionRunning` | A replicated property |
| `BlamOnlineSessionSubsystem` | `IsReadyToPlay` | |

A `Server` prefix on a UFunction is not a naming convention, it is a declaration: the
function is invoked on a client and executed on the server. `OnRep_` is the same kind of
statement about a property. Their presence means this build carries a live UE replication
layer that already understands Blam endpoints, and that the simulation is already written
to run with more than one of them.

So making two players see each other is not a matter of building replication. It is a
matter of getting both machines into one UE networked session and letting what is already
there do its work. That is the same shape as the launch problem, which was solved by
calling the game's own campaign entry point rather than reimplementing a level load.

## What does not work, and why

`Travel` with a `?listen` URL is refused. It needs a gameplay player controller, and asking
for one from the front end finds only the frontend controller, which is correct: travelling
with no world loaded is what crashed earlier builds.

Issuing it after a scenario has loaded does not reach the engine at all. Nothing is logged,
because nothing ran.

## The obstacle in front of everything else

**Loading a scenario destroys the mod's game thread channel.**

Queued work runs from a widget's event path, and the widget carrying it belongs to the front
end. Beginning a campaign takes the front end away, so from the moment a map is loaded there
is no route onto the game thread and every queued job waits out its deadline.

Nothing that has to happen inside a match can be built until that is fixed, because nothing
inside a match can currently be executed at all. It is the next piece of work, and it comes
before the session work rather than after it.

## Prior art

`devnull9090/mjolnir-core` reaches the same `open <package>?listen` idea and marks it
experimental, with no recorded result. Its verified CU3 world package paths are useful and
match what this project uses:

```
/Game/Levels/Halo1/Solo/A30/A30      and the same shape for A15 A50 B30 B40 C10 C20 C45 D20 D40
/Game/Levels/Halo1/Solo/Extra/E10/E10  E20 E30
```

Its notes also record that `HaloSimulation_tag_release.dll` exports one function,
`CreateBlamEngineShell`, imports WS2_32, and contains the complete original mode set:
Slayer, CTF, Oddball, King of the Hill, Infection, Juggernaut, Assault and Territories, with
their option and trait blocks. Treat the rest of that repository's conclusions as unverified
until measured here; the parts quoted above were checked against this build.
