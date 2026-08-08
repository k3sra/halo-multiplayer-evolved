# What the game already ships

Everything here was read out of the running game rather than inferred. The commands
that produce it are listed at the end, so any of it can be re-checked on a new build
instead of trusted.

---

## The engine layout, measured

Every offset the reflection reader uses on build
`2026.07.25.1112544.4-Rel-i343-Meteorite-2607-CU3` (UE 5.5.4):

| Structure | Member | Offset |
| --- | --- | --- |
| `UObject` | `ClassPrivate` | `+0x10` |
| | `NamePrivate` | `+0x18` |
| | `OuterPrivate` | `+0x20` |
| | size | `0x28` |
| `UField` | `Next` | `+0x28` |
| | size | `0x30` |
| `UStruct` | `SuperStruct` | `+0x40` |
| | `Children` | `+0x48` |
| | `ChildProperties` | `+0x50` |
| | `PropertiesSize` | `+0x58` |
| | size | `0xB0` |
| `UClass` | size | `0x200` |
| `FField` | `ClassPrivate` | `+0x08` |
| | `Owner` | `+0x10` |
| | `Next` | `+0x18` |
| | `NamePrivate` | `+0x20` |
| | `FlagsPrivate` | `+0x28` |
| `FProperty` | `ArrayDim` | `+0x30` |
| | `ElementSize` | `+0x34` |
| | `PropertyFlags` | `+0x38` |
| | `RepIndex` | `+0x40` |
| | `Offset_Internal` | `+0x44` |
| | `RepNotifyFunc` | `+0x68` |
| | size | `0x70` |
| `UFunction` | `FunctionFlags` | `+0xB0` |
| | `NumParms` | `+0xB4` |
| | `ParmsSize` | `+0xB6` |

Three of these are not what the documented UE5 layout says, and each cost time.

`UStruct::SuperStruct` sits at `+0x40` rather than `+0x30` because `UStruct` privately
inherits `FStructBaseChain`, whose two members take sixteen bytes between `UField` and
the first member anybody writes down. Reading the super pointer at `+0x30` walked into
that chain, which is why inherited fields came back as nonsense.

`FField::Next` is at `+0x18` and `NamePrivate` at `+0x20`, eight bytes earlier than the
usual figures, because `FFieldVariant` is a tagged pointer here rather than a pointer
plus a padded bool.

`FProperty::RepNotifyFunc` comes *after* the four in-memory link pointers, not before
them. Reading it where the older layout puts it produced a pointer, which resolves to no
name, which reads exactly like "this property has no notify handler". Every replicated
field looked unreplicated.

Nothing above is assumed at runtime. `UObject` is 0x28 bytes and `UField` is 0x30 on any
64-bit build and both are ordinary `UClass` objects in the object array, so the slot
holding `PropertiesSize` is the one reading those two values, and the slot holding
`SuperStruct` is the one where `Field` points at `Object`. The rest follows from a
property chain whose nodes name themselves: every `FProperty` subclass is called
`<Something>Property`.

---

## The net driver

```
NetDriverDefinitions = [{
    DefName                 = GameNetDriver
    DriverClassName         = OnlineSubsystemPlayFab.PlayFabNetDriver
    DriverClassNameFallback = OnlineSubsystemUtils.IpNetDriver
    MaxChannelsOverride     = -1
}]
```

**The game's networking is PlayFab Party, not Steam.** `USteamNetDriver`,
`USteamNetConnection` and `/Script/OnlineSubsystemSteam` are all compiled into the
binary, and none of them is what the engine builds. `PartyWin.dll` and
`PlayFabMultiplayerWin.dll` ship next to the executable, which agrees.

A `GameNetDriver` has really run on this machine: the saved
`Windows/Engine.ini` carries

```ini
[GameNetDriver StatelessConnectHandlerComponent]
CachedClientID=2
```

which only the stateless connect handshake writes, and only after a client connect.

This rules out one plan and confirms another. `open steam.<id>` cannot work, because no
Steam net driver is registered. Driving the game's own session can, because the game's
own session already works.

---

## How two players end up in one world

`BlamNetworkPlayerStateComponent` is a `PlayerStateComponent`, so one exists per player
and it replicates through the player state that owns it:

```
BlamNetworkPlayerStateComponent <- PlayerStateComponent <- GameFrameworkComponent
                                <- ActorComponent <- Object

  fn  ServerSetBlamEndpointIds
  fn  ServerSetPrimaryPlayerId
  fn  OnRep_EndpointId
  fn  OnRep_EndpointGeneration

  +0xA8  PrimaryPlayerId                 FUniqueNetIdRepl  replicated
  +0xD8  BlamNetworkInChannelEndpointId  uint16            replicated, OnRep_EndpointId
  +0xDA  BlamNetworkOutOfBandEndpointId  uint16            replicated, OnRep_EndpointId
  +0xDC  BlamEndpointGeneration          uint8             replicated, OnRep_EndpointGeneration
```

`BlamNetworkGameStateComponent` carries one flag for the whole session:

```
  +0xA8  bSessionRunning  bool  replicated, OnRep_bSessionRunning
```

So the division of labour is explicit. **UE replication carries identity, not
simulation.** A client's Blam endpoint ids arrive over UE replication, and the Blam
engine's own in-channel and out-of-band networking carries the game itself. That is why
loading the same scenario at the same moment produces two people who cannot see each
other: nothing has assigned either of them an endpoint, so the Blam simulation has no
peer to talk to.

---

## The co-op entry points

These are reflected `UFunction`s, which means they can be called through `ProcessEvent`
on the game thread with no patching. Signatures read from `UFunction::ParmsSize` and the
function's own child properties.

| Function | Owner | Parameters |
| --- | --- | --- |
| `BeginAllowInvites` | `MeteoriteLobbyNotifier` | none |
| `EndAllowInvites` | `MeteoriteLobbyNotifier` | none |
| `AcceptInvite` | `MeteoriteLobbyNotifier` | `UMeteoriteInviteToastInitData*` |
| `InviteFriend` | `MeteoriteProfileTrayWidgetBase` | none |
| `JoinFriend` | `MeteoriteProfileTrayWidgetBase` | none |
| `CanShowInviteFriend` | `MeteoriteProfileTrayWidgetBase` | returns `bool` |
| `CanShowJoinFriend` | `MeteoriteProfileTrayWidgetBase` | returns an enum |
| `InviteOtherPlayer` | `MeteoriteProfilePuckBase` | |
| `LeaveFireteam` | `MeteoritePlayerViewModel` | |
| `KickFromFireteam` | `MeteoritePlayerViewModel` | |

All are `Native|BlueprintCallable`. A live `MeteoriteLobbyNotifier` exists while the
main menu is up, so `BeginAllowInvites` is reachable from the moment the game starts.

---

## What reflects nothing

Worth writing down so it is not searched for twice.

`BlamNetworkSessionGameInstanceSubsystem` and `BlamOnlineSessionSubsystem` are
`GameInstanceSubsystem`s with **no reflected fields and no reflected functions**, apart
from `IsReadyToPlay` on the second. Whatever they do is native, so reaching them means
their virtual tables rather than reflection.

`BlamMultiplayerGlobalsTagDataAsset` derives from `BlamTagDataAssetBase`, whose only
fields are `CookedAssetsReferencedByTag` and `BinaryBlobSize`. **The multiplayer globals
are a Blam tag binary blob, not UE properties.** Reading the shipped mode list means
parsing that blob out of a loaded instance, not walking reflection.

`BlamNetworkPlayerControllerComponent` reflects nothing of its own.

---

## Re-checking any of this

Every table above comes from one of these, run against the live game:

```
layout                          the detected offsets, with UObject and UField read back
dump <class>                    ancestry, functions, fields, replicated fields
values <class>                  live field values on an instance or class default
sig <function>                  full signature, flags and frame size
field <class> <property>        hex dump of one FProperty, for settling a member's offset
raw <class> [bytes]             annotated hex dump of an instance, for native-only types
netdrivers                      NetDriverDefinitions and the live networking objects
funcs <fragment>                every reflected function whose name matches
objects <class fragment>        live objects by class
```
