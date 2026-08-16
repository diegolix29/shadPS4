<!--
SPDX-FileCopyrightText: Copyright 2026 Wozzardman
SPDX-License-Identifier: GPL-2.0-or-later
-->

# Bloodborne seamless co-op reverse engineering

## Scope and executable identity

The current analysis targets the European GOTY executable CUSA03173, app
version 01.09. The analyzed `eboot.bin` SHA-256 is:

```text
d65f0b4f01d59166aed16f8604196d8b7dd805abbf0758b356e8f1354c9429f9
```

Runtime hooks are additionally protected by exact instruction signatures. A
version label alone is not accepted as proof that an offset is compatible.

## Responsibility boundary

- shadNet owns summon advertisement storage, search-response normalization,
  search/claim delivery, relaxed discovery filters, and retaining an active
  pairing across ordinary removal messages.
- shadPS4 owns HTTP routing plus PS4 NP matching, signaling, P2P transport, and
  the versioned host-placement header used only when retail's summon claim has
  no destination field.
- Bloodborne owns bell eligibility, summon lifecycle transitions, character
  insertion, summon-point selection, and warp/map reload behavior.

shadNet no longer adds a private `SeamlessWarp` field, and shadPS4 no longer
parses one. That metadata was not consumed by Bloodborne and could not invoke a
native warp.

The captured `SummonData` blob is 224 bytes. In the current capture its four
little-endian floats at offsets `0x5C`, `0x60`, `0x64`, and `0x68` are
`143.97487`, `-116.94293`, `-87.73028`, and `1.8583716`, matching the exact
player position plus heading behind the rounded JSON `PosX/PosY/PosZ` values.
This is another reason shadNet preserves game-owned `SummonData` bytes. The one
confirmed exception is the response-only available-result count at SummonData
offset `0x79`, described below.

The live cross-map claim corrected an earlier assumption about `HostData`.
Bloodborne 1.09's native `SummonDataSummonRequest` serializer at `0x01E98650`
writes only `CharaId`, `TargetUserId`, `TargetCharaId`, `SessionId`, and
`UserId`. The captured request contained exactly those fields, the executable
has no resolved `HostData` string, and the responder consequently received no
host map or transform through the retail WebAPI message. shadNet's old
`HostData` test fixture was synthetic rather than evidence of this game path.

Seamless mode now bridges only that missing information in the existing HTTP
exchange. The host-side `SummonBuild.Entry` hook caches the same exact map,
area, position, and heading it wrote into Bloodborne's prepared descriptor.
shadPS4 adds a bounded version-1 `X-ShadPS4-Bloodborne-Host-Placement` header
to the native `/summon_messenger/request`; shadNet stores it with the claim and
returns it on the responder's native `/summon_messenger/create` response. The
header contains a packed map, four IEEE-754 bit patterns, and signed SOS area;
it does not alter the game's JSON schema. The guest validates and caches the
header, then the post-copy game hook supplies it to the native placement
setters. An independent create/request/create probe confirmed byte-for-byte
header relay through shadNet.

## Confirmed native paths

The event registration table resolves these game wrappers:

| Event | 01.09 wrapper | Observed behavior |
| --- | ---: | --- |
| `SetSosSignWarp` | `0x0132E260` | Sets global state byte `+0x1520` |
| `SetSummonedPos` | `0x01332BB0` | Sets global state byte `+0x20` |
| `SetSosSignPos` | `0x01333BA0` | Copies player position/orientation/map into `+0x14A0/+0x14B0/+0x14C0` |
| `SummonedMapReload` | `0x01336B90` | Dispatches native reload function `0x0131E5C0` |

`PlayerWarpTool` registers five named commands in both result-returning and
void forms. The result-returning wrappers are `0x01FB2BE0` through
`0x01FB2DE0`; they dispatch by name through `0x00FBE4A0`. The void wrappers are
`0x01FB2E60` through `0x01FB3060` and dispatch through `0x00FBE660`.

Those wrappers are generic command adapters, not the warp implementations. The
registration code binds the names to these concrete executors:

| Command | 01.09 executor |
| --- | ---: |
| `RemoteWarpPlayer` | `0x0154D180` |
| `RemoteResetPlayer` | `0x0154D5E0` |
| `GetPlayerInfo` | `0x0154D7A0` |
| `RemoteWarpPlayerWorldPos` | `0x0154DC50` |
| `RemoteWarpPlayerMapStudioPos` | `0x0154E0B0` |

`RemoteWarpPlayer` parses `map0` through `map3`, `igPosX/Y/Z`, `degX/Y`, and
`cDegX/Y`, then calls `0x0154EA30` with position, player orientation, camera
orientation, and packed map ID pointers. `0x0154EA30` compares the requested
map to the current map. Its same-map path reaches the local placement helper
`0x0154B110`; its cross-map path configures the built-in
`SprjEzSelectBot.PlayerWarp` flow so Bloodborne performs its own map load.
`0x0154B110` accepts the world/player manager, map ID, target position, and
target orientation and invokes the game's world-loading and player-placement
virtual methods.

The word `Remote` here refers to the debug command interface. These executors
warp the player local to the process and are not evidence of a network guest
warp RPC. They are a viable fallback only if invoked inside the guest process
with a host transform. The native summon insertion path remains the preferred
seamless-co-op path.

The summon-request state callbacks are `0x014B47C0`, `0x014B4890`,
`0x014B48C0`, and `0x014B4AE0`. Multiplayer insertion callbacks span
`0x01E4FCF0` through `0x01E50D70`. These are runtime trace points, not emulator
implementations of the underlying gameplay.

The native summon placement data flow is now concrete:

1. `0x0156E7A0` arms a received placement record at global state
   `+0x1620/+0x1630/+0x1640/+0x1644/+0x1648`; `0x0156E6E0` conditionally
   replaces it when the stored matching key agrees.
2. `CSMultiPlayerIns::StartMultiPlayNotifyWait` copies the received position,
   orientation, and map from `+0x1620/+0x1630/+0x1644` into both placement
   banks at `+0x14A0/+0x14B0/+0x14C0` and
   `+0x14D0/+0x14E0/+0x14C4`.
3. The selector at `0x01332BC0` uses `+0x14F0` when the warp byte at `+0x1520`
   is set; otherwise it follows the current/fallback map path through
   `+0x1528`.
4. `SummonedMapReload` reaches `0x0131E5C0`, which serializes game-owned map
   reload state into its internal buffers before advancing multiplayer state.

This is stronger than an emulator-side coordinate write: Bloodborne already
receives the remote transform, copies it during insertion, selects its own map,
and owns the reload. Paired captures established host/guest ordering and showed
that retail same-map insertion never sets `+0x14F0/+0x1520`. Seamless mode now
uses a guarded post-copy hook to invoke the game's forced-placement setters,
map selector, named `SummonedMapReload` wrapper, and stage-transition routine
only for a cross-map summoned client. Live paired captures have validated the
transported placement and map transition. They also established that the
native multiplayer serializer preserves the joined room when it runs before
the transition; the remaining validation is the corrected ordering that
reapplies the transported destination after serialization.

### Ordinary stage travel

Ordinary lantern/headstone travel is related to summon placement but is not the
same operation. The native stage-warp registration and instruction dispatch
resolve the following path:

| Operation | 01.09 handler | Native behavior |
| --- | ---: | --- |
| `WarpNextStage` | `0x0132E010` | Packs four map components into global state `+0x0C`, stores the warp point at `+0x10`, then requests a stage transition |
| `WarpNextStage_Bonfire` | `0x0132E050` | Derives area/block from a respawn-point ID, stores that map and the full ID, then requests a stage transition |
| WarpParam/respawn-point request | `0x013CDF30` | Looks up the supplied WarpParam ID and fills the map/mode fields at `+0x0C/+0x1538/+0x153C` |
| Shared stage-transition request | `0x013CDE30` | Arms byte `+0x08` and advances the game-owned load/respawn flow |
| Stage-load descriptor construction | `0x01944D76` | Copies the selected respawn-point and auxiliary IDs into a game-owned load descriptor |
| Request acknowledgement | `0x0194100A` | Tests the consume condition immediately before the game clears the armed request byte |
| Respawn-point resource resolution | `0x01938F65` | Selects the task's respawn-point ID for the native map-resource lookup |
| Respawn transform resolution | `0x0154AFC0` | Resolves a respawn-point ID to position and three-component orientation from loaded map data |
| Resolved placement application | `0x01939C80` | Copies the task's resolved position and facing into the live player/world state |

In a verified seamless session inside the defeated Great Bridge boss arena, the
host no longer received the lantern interaction prompt while the guest remained
connected. Common event `7200` does not prohibit a multiplayer host: it waits for
lamp flag `72410100` and then invokes the native respawn-point warp. The missing
prompt therefore occurs earlier than the event instruction.

The event interpreter now gives an exact native registration path. Event group
`2009` dispatches command `[05]` to `0x017C60D6`, where it reads all six
`Register Healing Fountain` operands and calls `0x0133B030`. That native accepts
the event flag, entity ID, reaction distance and angle, initial sword count, and
sword level; it registers an action object through `0x01327650` and enqueues an
`OnEvent_Bonfire` record. The read-only `HealingFountain.Register.Native`
observer is installed at this actual native entry and records those six values.
The former observer at `0x017C67C0` was removed because that address is a record
lookup/removal loop, not the registration call, and its first pointer could not
honestly be described as an interaction vtable.

RTTI/state-step registration also resolves
`CSChairMessengerRespawnPointNotifyStep::STEP_Init` at `0x01E5C6C0`,
`STEP_Update` at `0x01E5C890`, and `STEP_Finish` at `0x01E5CAA0`. Despite its
proximity to respawn-point behavior, this is not the local lantern-prompt task.
Update calls `0x01EA06D0`, which constructs the
`/api_ChairMessRespawnPointNotice` WebAPI request using the selected respawn
point. Its `+0x9F6/+0x9F8/+0xA50` checks are therefore online-notification
availability gates, not evidence for the missing interaction prompt. Read-only
Init and Update observers remain useful for identifying that request, but no
seamless patch changes these fields. The game's own network-debug formatter
names `+0x9F5/+0x9F6` LAN connect/disconnect and `+0x9F7/+0x9F8`
sign-in/sign-out; `+0x9F6/+0x9F8` are latched from the online-state object at
global `0x056C7048`, while `+0xA50` is asserted by a separate player-state path
around `0x0193A3E0`.

The `OnEvent_BonfireRespawn` callback at `0x0138CA20` only stores its event
object in globals, and the `forReturnToSummonedCoordPosAng` xref at
`0x019C9B20` only registers a named parameter. Neither is the runtime warp or
the missing prompt gate.

Following the relocated vtable of the action object created by `0x01327650`
reaches its update at `0x012F5C40`. The update calculates four independent
availability bytes at object `+0x48..+0x4B`; the final action enable passed to
`0x0146E250` is true only when all four are zero. The exact multiplayer byte is
`+0x49`: `0x012F8315` tests whether the local session has more than one player
and combines that result with pending matchmaking state before storing the byte
at `0x012F8323`. This is a native reason the host's lantern prompt disappears
after a guest joins.

The experimental seamless hook at `0x012F836E` runs after all four bytes are
calculated and before the first availability decision. It clears only `+0x49`,
and only when the local player has SpEffect `9001`, which common event `9190`
assigns when the player is simultaneously multiplayer and host. A guest does
not have effect `9001`, and the special-map, missing-entity, and remaining
native gate bytes at `+0x48/+0x4A/+0x4B` are left intact.

The successful two-client cross-map session disproved the earlier conclusion
that this hook restores the host prompt. Host B repeatedly reported
`host_effect_9001=true`, `result=applied`, and all four bytes zero after the
hook, but neither client received a lantern interaction prompt. Clearing
`+0x49` is therefore a confirmed local availability change, not a complete
host-only lantern bypass. A second action-selection or presentation gate still
needs to be resolved. The hook does not force a progression flag, change the
shared session manager, or expose the action to a guest, but it must not be
described as functional yet.

Common event `7200` itself remains network-synchronized; it does not issue
`Set Network Sync State (Disabled)`. Instruction 0 ends the event when the
local process is already a multiplayer client, then the surviving event waits
for the lamp interaction flag and calls `Warp Player to Respawn Point` with the
instance's destination RespawnParam. A same-map responder normally initialized
that event before becoming a client, so its task may still be waiting when the
host activates the lamp. A guest that loads the map while already joined may
instead end the newly initialized event at instruction 0. Once the remaining
host prompt gate is resolved, a paired run can distinguish two outcomes without
inventing a destination message: either Bloodborne's synchronized event
naturally calls the bonfire warp in both processes, or only the host reaches it
and the guest needs a scoped way to retain or replay event `7200`. Stage-warp
captures include the current map and host effect `9001` so the two outcomes
will be unambiguous.

The installed `common.emevd.dcx` independently confirms the decoded script and
provides a byte-level boundary for a fallback. Its DFLT payload begins at DCX
offset `0x4C` and inflates to a `0x11950`-byte EVD. Event `7200` is event record
31 at EVD `0x660`; its 11 instruction records begin at EVD `0x7D70`.
Instruction record zero is bank `1003`, command `6`, with four argument bytes
at EVD `0xF55C`: `00 01 00 00`, meaning execution end type `End` and desired
multiplayer state `Client`. A future fallback can therefore target this one
event instance or its one instruction with exact structural checks. It must
not globally change command `1003[6]`, because the map scripts use the same
client guard for many unrelated bosses, objects, and progression events.

The named `SummonReloadStart` callbacks also do not provide a host-travel
transport. `OnBeJoinStart_White` and the other join variants enqueue event
`4059` with that name. The handler at `0x01389440` conditionally calls
`0x0131E5C0`; `ForceSummonReloadStart` at `0x01389620` sets an additional
manager byte before calling the same function. They lead back to the existing
guest-side `SummonedMapReload.Native` serializer and contain no destination
map, RespawnParam, or remote-player warp call.

The event interpreter's `Warp Player` instruction (`2003[14]`) writes the same
map and warp-point fields before calling `0x013CDE30`. Its `Warp Player to
Respawn Point` instruction (`2003[49]`) calls `0x013CDF30`, matching the decoded
event-script definitions. Runtime observers now cover all four handlers and
record named map components, WarpParam/respawn-point IDs, the constructed load
descriptor, and the transition state.

At `0x01944B33`, the descriptor builder copies global mode `+0x1538` to
descriptor `+0x98`. A nonzero mode selects request type/subtype `9/2`, then
`0x01944D76` copies global `+0x153C` and `+0x1540` to descriptor
`+0x80/+0x84`. The low value is the exact WarpParam/respawn-point ID. This is
the native handoff from menu/event selection into Bloodborne's stage-load
request, before the game resolves the final spawn transform from its params.

The descriptor continues through a fully game-owned load path. `0x01928680`
copies it into the stage manager, and `0x01937570` constructs a stage task whose
map, request type/subtype, respawn-point ID, auxiliary ID, and mode are at
`+0x58/+0xA4/+0xA8/+0xD0/+0xD4/+0xE8`. For request type 9, `0x01938F65`
validates the nonnegative ID and feeds a generated respawn-point key through the
current map's resource lookup. Static control flow near `0x019423A0` can call
`0x0154AFC0` with a map resource, respawn-point ID, and task `+0xF0/+0x100` as
output buffers; that resolver searches loaded map entries and copies position
and orientation. `0x01939C80` is a related candidate placement/update routine.
Runtime lantern and headstone travel has not called the resolver and has not
called the candidate placement routine after the destination lookup, so neither
may yet be described as the normal travel placement path. These observers do
not change game state.

The labeled lantern/headstone capture
`cusa03173-109-p10072-1786666830025.jsonl` produced:

| Observed route/destination | WarpParam ID | Map | Mode | Auxiliary ID | Settled SOS area |
| --- | ---: | --- | ---: | ---: | ---: |
| Great Bridge | `2412952` | `m24_01_00_00` | 1 | `101163` | `-241109` |
| Great Bridge to Hunter's Dream | `2102950` | `m21_00_00_00` | 2 | `101200` | `210000` |
| Central Yharnam | `2412951` | `m24_01_00_00` | 1 | `101163` | `241010` |
| Awaken Above Ground - Hypogean Gaol | `2802952` | `m28_00_00_00` | 1 | `101163` | `280040` |
| Hypogean Gaol lantern to Hunter's Dream | `2102952` | `m21_00_00_00` | 2 | `101200` | `210000` |

Great Bridge and Central Yharnam demonstrate that the map ID alone is
insufficient: both load `m24_01_00_00`, while adjacent WarpParam IDs choose
different spawn points. Normal world destinations used mode 1; Hunter's Dream
used mode 2. Hunter's Dream also used different WarpParam IDs for arrivals from
Great Bridge and Hypogean Gaol, so the destination map and mode still do not
identify the exact arrival point. Every observed menu travel and initial save
load entered through the WarpParam handler and shared transition. None reached
the summon-placement, `SummonedMapReload`, or `PlayerWarpTool` observers.

Two follow-up captures establish the early transition timing:

- `cusa03173-109-p13425-1786671208910.jsonl` requested Dream WarpParam
  `2102952`. The request was acknowledged 533 ms later, the descriptor followed
  at 658 ms, and SOS area `210000` first appeared at 4,268 ms.
- `cusa03173-109-p14739-1786674685820.jsonl` requested Hypogean Gaol WarpParam
  `2802952`. The acknowledgement occurred at 534 ms, the descriptor at 642 ms,
  and SOS area `280040` first appeared at 6,618 ms.

In both cases the acknowledgement saw `request_active=1` and the descriptor,
108-125 ms later, saw it cleared. The clear therefore means that the stage
request was consumed; it is not load or placement completion.

The post-observer capture `cusa03173-109-p33923-1786679493594.jsonl` recorded
two more complete native requests:

- WarpParam `2102952` resolved `m21_00_00_00` in mode 2. Acknowledgement was at
  533 ms, descriptor construction at 654 ms, and map-resource resolution at
  1,033 ms.
- WarpParam `2412952` resolved Great Bridge `m24_01_00_00` in mode 1. Its
  corresponding timings were 533 ms, 644 ms, and 1,033 ms.

Neither request hit `0x0154AFC0` or reached `0x01939C80` after resource
resolution. The latter observer fired 64 times only during the capture's
initial map bootstrap, on a separate type/subtype `5/1` task for
`m28_00_00_00`. Ordinary menu travel therefore confirms the fixed WarpParam and
map-resource portions of the native path, but its final transform handoff
remains elsewhere. The accidental Bell use earlier in this capture is a
separate item/SOS sequence and does not overlap either travel request.

A single-client lantern run can therefore establish which entry route the menu
uses, the destination encoding, the native transition ordering, and the fixed
respawn-point resource. It cannot establish how Bloodborne chooses a remote
guest or whether refreshed `HostData`/received-placement state moves that
guest; those questions still require a paired two-client capture. Ordinary
travel is a strong native cross-map loading primitive, but a host's arbitrary
live position has no WarpParam ID. The likely seamless composition is to use
the stage path for a required guest map load, then let the received-summon
placement path apply the host transform after load.

## SOS area state and bell dispatch

The local SOS validity updater starts at `0x01308DC0`. A live trace resolved its
player-object vtable slot `+0x628` to `0x018FE8F0`. That leaf accessor returns bit
4 of the byte at player state `+0x52C`. The updater uses the bit as a bank
selector: clear reads the signed SOS area code at `+0x278`, set reads `+0x27C`,
and the selected code is stored in the condition object at `+0x20`. The game
compares the previous and current values after grouping them by division by ten
before running its native SOS transition handling. These values are area state,
not a boolean bell-validity result.

A labeled live run produced this sequence:

- Active gameplay used SOS area `241040`.
- A map transition used the negative sentinel `-241109`.
- Hunter's Dream settled on SOS area `210000`.
- Sinister Resonant Bell created `SummonType: 2` in area `241040`.
- Small Resonant Bell created `SummonType: 0` in area `241040`.
- Both creates received `ResKind: 0` from shadNet, and each silence/transition
  produced a normal remove request.
- A Beckoning Bell attempt in Hunter's Dream produced no summon HTTP request.

The last result places that restriction inside Bloodborne before its proprietary
HTTP client. It is not a shadNet discovery filter and should not be bypassed by
manufacturing a request in either shadNet or shadPS4.

A second controlled run used an allowed Beckoning Bell, warped to Hunter's
Dream, and then attempted the same bell again. The allowed attempt reached
`POST /summon_messenger/get` with `AreaId: 385875968`, `AreaRegionId: 230100`,
`MatchingLevel: 46`, `SummonType: 0`, and the game-owned player transform. The
Hunter's Dream attempt settled on SOS area `210000` and generated no HTTP
request. None of the five `OnEvent_Call_*SOS` or `Call_*Sos` dispatch observers
ran for the rejected attempt. The restriction is therefore earlier than the
SOS event wrappers as well as earlier than networking.

The executable's action-name table is recovered from SELF relocation records.
`UseItem` occupies runtime slot `0x053343B0` and is ordinal 7 among the action
name strings. The formatter walks from a preceding non-name slot, which made an
initial index-8 interpretation ambiguous. The
`SprjChrActionFlagModule` constructor at `0x01A0F7D0` installs vtable
`0x05334350`; its transition updater is `0x01A0F910`. At updater entry, module
`+0x10` is the current action mask and `+0x18` is the previous mask. The updater
derives newly pressed and released masks at `+0x20/+0x28`.

Captures `cusa03173-109-p81584-1786503127695.jsonl` and
`cusa03173-109-p84475-1786503825005.jsonl` identify `UseItem` empirically as
action bit `0x80`. An allowed Beckoning Bell press raised `0x80` at
`0x01A0F910` and reached `/summon_messenger/get` about 803 ms later. The
rejected Hunter's Dream press raised the same bit but produced no later SOS
request. The second capture includes two title-screen detours and still cleanly
reproduces this boundary. The restriction is downstream of input/action
construction and upstream of SOS request construction. `SprjPlaylog_ChrUseItem`
was also investigated and ruled out as a gate: it is a type name in the playlog
registry.

The first player-side consumer is in the update function at `0x018F4FF0`.
`0x018F7516` tests newly pressed action bit `0x80`, calls the player's vtable
method at `+0x1C8` to obtain inventory state, and passes inventory `+0x1D0` to
`0x014D1EF0`. That function resolves the selected goods ID and stores it at
player `+0x478`. Both the allowed and rejected Beckoning attempts reach this
point with goods ID `200` and item state zero. The later `+0x1C0` vtable call
previously labeled as a start gate belongs to a separate secondary action path;
neither Beckoning attempt reaches it, and its concrete function only checks a
player field at `+0x3B8`. It is not the bell eligibility test.

The native goods application function at `0x018F9720` receives the player,
goods ID, and use argument. It calls the shared goods/effect executor at
`0x018C9160`, which resolves the `EquipParamGoods` row through `0x01F1E3D0` and
submits the resulting action through `0x01FF0FA0`. Read-only observers now cover
both entries, the executor result at `0x018F97A0`, and the central goods lookup.
The lookup observer is restricted to multiplayer and control goods and records
the guest return address and most recently observed SOS area. An
allowed/rejected caller-set comparison can therefore locate the first
game-owned path that inspects goods `200`, including availability checks that
occur before native application begins.

The inventory UI greys out Beckoning Bell in Hunter's Dream, so the game has
already rejected the item before `UseItem` dispatch. A failed quick-tool press
is only a timing marker; it is not expected to expose the predicate itself.
The relevant comparison is the goods-availability query while the item is
selectable versus greyed out. The latest trace shows the observed SOS area move
from `230100` to `210000` after the Hunter's Dream warp. The lookup trace now
associates each caller with that area so the assignment made during map loading
can be distinguished from the later bell-specific read that controls UI
availability.

A live one-way boundary test isolated the availability transition without a
warp or reload. On the grey side the selected SOS field was `-280050`; crossing
into the allowed side changed it to `230100`. Goods ID `200` was polled every
two seconds by the same availability caller at `0x0157F200` on both sides. That
function special-cases Beckoning Bell at `0x0157F855`, selects player field
`+0x278` or `+0x27C`, and performs an unsigned `area > 999999` rejection at
`0x0157F8C8`. Consequently every negative SOS area is rejected before the
native area validator runs.

The following native restriction query at `0x0131D7B0` already takes the
absolute value of the area code and checks Bloodborne's area/event tables. For
`280050`, the table resolves to event flag `2800`; the m28 scripts set that flag
during map-state initialization and boss completion. The patched run reached
this query for `-280050` and confirmed that flag as the next active blocker.

With seamless co-op enabled, shadPS4 now changes eight game-owned Bell and
status decisions:

| Decision | 01.09 site | Seamless behavior |
| --- | ---: | --- |
| Beckoning inventory range | `0x0157F8C8` | unsigned `ja` becomes signed `jg` |
| Beckoning inventory area flag | `0x0157F960` | Bell-only blocked result becomes allowed |
| Small/Sinister inventory result | `0x0157F6D1` | goods-specific result register becomes allowed |
| Small/Sinister availability return | `0x0157F6D3` | skips remaining predicates through the checked epilogue |
| active-Bell range | `0x015068BB` | unsigned `ja` becomes signed `jg` |
| active-Bell area flag | `0x015068FC` | selects normal active indicator state |
| responder search range | `0x0191A8C3` | preserves the native table result for signed negative areas |
| SOS status area result | `0x018700D3` | area-derived restriction enum becomes allowed |

The first pair makes the Beckoning Bell usable. Small Resonant and Sinister
Resonant share an earlier goods-specific branch at `0x0157F686`. Responder Bell
availability is polled continuously. In the
Hunter's Dream live trace, goods `205` briefly returned allowed while the
player was loading, then returned blocked after the multiplayer effect mask
changed from `0x202205` to `0x202207`; the SOS area remained `210000`. This
proves that bypassing the map-area query and common call alone is insufficient.
Seamless mode now sets `cl` to one at `0x0157F6D1` and jumps from `0x0157F6D3`
to the function's normal stack-canary/return epilogue. Those sites are reached
only by the goods `205`/`225` branch, so the full availability bypass applies
only to Small Resonant and Sinister bells. Unrelated inventory items do not
inherit it, and item execution remains native. The active-Bell pair changes the periodic
active-Bell updater at `0x01506820`: an allowed search receives effect `9003`,
while a blocked search receives effect `9004`, which is the Bell X shown by the
HUD. Positive out-of-range codes remain rejected at both range checks.

Responder search has a second area predicate at `0x0191A750`, shared by four
callers: Small/Sinister inventory availability, the active responder indicator,
candidate processing, and the responder advertisement builder. It parses the
absolute area into Bloodborne's native region table, then performs a final
unsigned comparison against `1000000`. The defeated boss arena's `-241109`
area resolves to an allowed table entry (`-1`) but fails solely because the
negative value is unsigned-greater than the limit. At `0x0191A8C3`, seamless
mode replaces the carry reduction with a signed `jge` failure path and otherwise
copies the native table result. Positive values at or above the limit, invalid
table states, and missing player/world state remain rejected.

The shared status producer at `0x0186FE40` reads the selected SOS area and calls
`0x0131D7B0`. Its original reduction at `0x018700D3` turns either a blocked
area/event-table result or the sign bit of a negative area into boolean one;
that boolean becomes restriction enum `3` in four status fields. The seamless
patch changes only this area-derived boolean to zero. Independent session and
multiplayer state remains part of the final enum calculation. All eight changes
are restricted to CUSA03173 01.09 and require exact byte signatures.

Capture `cusa03173-109-p130208-1786590194704.jsonl` confirmed the first pair at
the original negative area `-280050`: the native availability result was zero,
the inventory enabled the Beckoning Bell, and goods `200` reached both
`0x018F9720` and the shared executor at `0x018C9160` with use argument `17`.
This is a successful UI/item dispatch, but it is not yet a successful summon
advertisement. The game-owned `CSRequestGetSosStep` vector remained empty, the
request builder did not run, and no `/summon_messenger/get` exchange was
captured during the following twenty seconds. The earlier known-good `230100`
capture instead changed that vector from zero elements to one before entering
the request builder.

Capture `cusa03173-109-p132506-1786590938774.jsonl` resolved that distinction.
The shared executor succeeded, effect `9000` with special state `189` became
active, and the HUD showed the Bell X, but the SOS-type vector stayed empty.
Capture `cusa03173-109-p135289-1786591752765.jsonl` then confirmed the active
indicator patches: the negative side held effects `9000` and `9003`, never
`9004`, while its SOS-type vector was still empty. Crossing to area `230100`
left `9003` active and immediately populated the vector with summon type `0`.

Read-only checkpoints at `0x0187239E` and `0x01E5F970` isolated the remaining
gate. With the Bell active, `event_bits` remained `0x21` on both sides. The
status producer returned `(3,3)` for its primary and secondary status on
`-280050` and `(0,0)` on `230100`. The caller at `0x01872360` only forwarded
Beckoning event code `7` in the latter case; `0x01E5F970` maps that event to
summon type `0` using the game's own table.

Capture `cusa03173-109-p137094-1786592261144.jsonl` confirms the complete native
path after the SOS-status patch. Without crossing from `-280050`, the game
forwarded event code `7`, populated summon type `[0]`, entered the request
builder, and selected signed area `-280050`. HTTP capture
`summon-1786592298655-p137094-1.txt` received status 200 and `ResKind: 0`;
Bloodborne itself normalized the wire request to `AreaRegionId: 280050`.
Neither shadNet nor shadPS4's HTTP routing needs to spoof the area for this
case.

The rejected relative-branch result observer was replaced with a safe
checkpoint at the shared executor epilogue `0x018C9364`. Additional read-only
capture records goods submission, the executor allow bit, multiplayer effects,
native SOS status, insertion event codes, and the SOS-type vector.

Read-only checkpoints at `0x0157F8BA` and `0x0157F962` record the selected area
and native blocked flag, respectively. An initial observer at `0x0157F913` was
removed because it covered a relative short jump, which cannot be copied
verbatim into the current guest-hook trampoline. The hook installer now rejects
relative and RIP-relative instruction spans during installation.

Capture `cusa03173-109-p79559-1786502645177.jsonl` independently reproduced
the network boundary. The allowed attempt generated exactly one
`/summon_messenger/get` request using area region `230100`; after the player
settled in Hunter's Dream at `210000`, the rejected attempt generated no second
request. The trace exited without an access violation. It produced no action
entries solely because the original `0x100` filter was wrong, not because the
action updater failed to execute.

The host search state machine is `CSRequestGetSosStep`:

| Step | 01.09 function |
| --- | ---: |
| `STEP_Init` | `0x01E5FAF0` |
| `STEP_Update_forSeamless` | `0x01E5FBF0` |
| request builder | `0x01E5FCE0` |
| `STEP_Update_forWide` | `0x01E603B0` |
| `STEP_Finish` | `0x01E603C0` |

The wide update is an empty return in this executable. The request builder uses
the player vtable method at `+0x628` to select signed area field `+0x278` or
`+0x27C`, loads the selected value at `0x01E60270`, and passes it into the
native get-list request at `0x01E60320`. The successful negative-area capture
shows that the native request path normalizes the wire `AreaRegionId`, so an
emulator-layer area spoof is unnecessary. Starting this state machine from
shadPS4 would also skip game lifecycle setup.

### Paired discovery and native handoff

The first controlled two-client run used separate shadPS4 profiles, P2P ports,
cache directories, and shadNet users. Client A (user 2465) rang Beckoning and
client B (user 2767) rang Small Resonant in natural SOS area `241040`. B
repeatedly completed `POST /summon_messenger/create`; A repeatedly completed
`POST /summon_messenger/get`. The then-current shadNet build returned B's
advertisement to A with `ResKind: 0`, the requested top-level search location,
and the original opaque `SummonData` intact.

Bloodborne did not reject that response at the HTTP boundary. A entered the
candidate decoder at `0x014BA980`, passed its initial native validation at
`0x014BAA5C`, and submitted the constructed candidate at `0x014BABCB` twelve
times. It never reached `/summon_messenger/request`, never populated the
`CSRequestSummon` request pointer at object `+0xE0`, and never issued an NP room
create, join, or signaling operation. Discovery, authentication, payload
transport, and the shadNet create/get route therefore worked; the stalled
boundary is Bloodborne's native transition from an accepted candidate to a
summon request.

Static control flow narrows that transition further:

- `0x014B8D10` inserts a constructed candidate into the summon manager's
  primary list at `+0x30` and registers its observer through `0x014C0190`.
- The manager update at `0x014B5E10` filters primary candidates against local
  and manager identity, area, session, and duplicate state. A passing candidate
  is converted into a selector record in the pending list at `+0x1A8`.
- `SosStatus.Update` at `0x01872360` scans the pending list at `0x01873269`. It
  tests each item's state byte and intersects the candidate role's metadata
  mask with the status producer's current selection mask.
- A match at `0x018732DD` marks the pending item selected, prepares its native
  request descriptor through `0x01874710`, and queues a non-null result through
  `0x01875320`. Once the queued status and global multiplayer state permit it,
  `0x01873797` calls `0x014BAEC0`.
- `0x014BAEC0` is the only identified writer of the summon manager request
  handle at `+0x138`; that handle is what allows `CSRequestSummon.Init` to
  advance beyond its idle path.

Offline decoding of all twelve advertisements identified the immediate
handoff blocker. The sender writes zero at raw SummonData offset `0x79`.
`0x014BABA5` copies storage `+0x7D` (raw `+0x79`) into candidate `+0xED`.
The manager can merge multiple observations of the same generation by adding
this byte at `0x014B8F2E`, then consumes one available result while building a
pending record at `0x014B71B2`. A zero count takes the rejection branch. Every
advertisement shadNet returned to A still contained zero, so none could enter
the pending list.

shadNet now applies the missing search-response normalization: when a
version-3 SummonData payload is exactly 224 bytes and its available-result
count is zero, the returned copy uses count one. The stored advertisement and
every other byte remain unchanged; already nonzero, malformed, or unknown
payloads are preserved. The broker test covers both zero normalization and
nonzero preservation. This is server emulation of the native response contract,
not an emulator-side summon or lifecycle bypass.

The second controlled run validated that normalization. A received B's role-7
advertisement, entered `0x014BA980`, passed every observed native filter, consumed
the normalized count at `0x014B71B2`, and reached both pending insertion at
`0x014B750B` and selection match at `0x018732DD`. The live operands also confirm
that the local identity is `wozzardman`, the candidate identity is `Andrews`, the
candidate and manager area are both `241040`, the role-selection mask is `0x80`,
and the selection producer supplied `0xffffffff`.

The match did not reach `0x01875320`, `/summon_messenger/request`, or an NP room
operation. At `0x018732DD`, the selected list item contains two objects: its raw
candidate at `+0x00` and the prepared request descriptor at `+0x08`. The native
request builder `0x01874710` consumes the latter and returned null in this run.
The new trace follows its role-delay, session, control, global-capacity,
role-capacity, and role-rule stages, then records its rejection or accepted
return. This is now the narrowest known blocker.

The candidate constructor at `0x014BAA78` copies the native 16-byte identity from
decoded SummonData `+0x44`, which is the payload's padded online-name field. It
copies the top-level `UserId` and `CharaId` into candidate `+0x14/+0x18` instead.
B's captured candidate identity is therefore `Andrews`, even though both copied
saves advertise the same `CharaId` sentinel `0x8000000000000000`. The shared
character sentinel is not the value used by this native self check.

The game parameters identify the multiplayer goods and their native effects:

| Good | ID | SpEffect | Special state |
| --- | ---: | ---: | ---: |
| Beckoning Bell | `200` | `9000` | `189` |
| Small Resonant Bell | `205` | `9005` | `188` |
| Sinister Resonant Bell | `225` | `9025` | `190` |
| Silencing Blank | `111` | `7` | n/a |

`SpecialState` is the 16-bit field at byte offset `0x156` of
`SP_EFFECT_PARAM_ST`. Executable branches directly recognize states 188-190.
The active-Bell updater additionally manages indicator effects `9003` and
`9004`; these effects describe search availability and do not replace the
underlying Bell effect or the native SOS event transition.

Related native condition/update functions are:

| Path | 01.09 function |
| --- | ---: |
| `IsValidSos` condition dispatch (event ID `0x2C`) | `0x013091B0` |
| `Condition:SelfSos` | `0x0130B030` |
| `Condition:SOS` | `0x0130C590` |
| `Condition:SosMenu` | `0x0130CDE0` |
| `Call_WhiteSos` (game event `0x222EA`) | `0x01389AB0` |
| `Call_BlackSos` (game event `0x222EB`) | `0x01389AF0` |
| `OnEvent_Call_SOS` native dispatch | `0x01389A20` |
| `OnEvent_Call_BlackSOS` native dispatch | `0x01389A50` |
| `OnMatchingCheck` | `0x013808B0` |
| `OnMatchingError` | `0x013809F0` |

`Call_WhiteSos` dispatches game event `0x222EA` through the generic event path at
`0x01317110`. `OnEvent_Call_SOS` reaches the native SOS handler at `0x0132EAD0`
with SOS type 1; the black and dragonewt variants pass types 2 and 3. Byte-verified
observers cover those dispatches. The controlled comparisons placed the
original rejection before all of them; the seamless patches therefore change
the confirmed inventory, indicator, and SOS-status decisions without replacing
these event wrappers.

## Event-script findings

The decoded scripts from
[BloodborneEventScripts](https://github.com/HotPocketRemix/BloodborneEventScripts),
reviewed at commit `23260ea96546f2cb693f821beb392378e77b94ba`, establish the
script/executable boundary:

- Common event 9190 sets or clears multiplayer-host effect 9001 after the
  session state already changes.
- Events 9191 and 9192 maintain map/event flags 6500-6503 and 6400-6403.
- Event 9240 sets or clears invasion-allowed effect 9020 from boss and event
  state.
- Map NPC-summon events require the host to already have effect 9000.

These scripts consume multiplayer state; they do not implement the immediate
bell-use rejection observed in Hunter's Dream. That restriction remains in the
executable item/action path. They also gate bosses, multiplayer effects, and
NPC summon signs after the relevant state exists; they do not implement the
player-to-player candidate, request, NP room, or insertion lifecycle. The
native executable path remains the correct integration point for player
summoning.

The event instruction definitions also expose Bloodborne's native same-map
placement operations: `Warp Character and Set Floor` (`2004[40]`), `Issue
Short-range Warp Request` (`2004[41]`), and `Warp Character and Copy Floor`
(`2004[42]`). The Character command dispatcher is `0x017BA6A0`; it decrements
the instruction ID and dispatches through the jump table at `0x017BCEB8`.
The relevant 01.09 handlers and native helpers are now resolved:

| Instruction | Handler | Native operation |
| --- | ---: | --- |
| `2004[3]` Character Warp Request | `0x017BA853` | routes by target type |
| `2004[40]` Warp Character and Set Floor | `0x017BC18A` | sets floor, then routes by target type |
| `2004[41]` Issue Short-range Warp Request | `0x017BC284` | routes by target type |
| `2004[42]` Warp Character and Copy Floor | `0x017BC2F6` | copies floor, then routes by target type |

The target type is `0` for an object/dummy poly, `1` for an area entity, and
`2` for a character/dummy poly. The resulting helper ABIs are:

| Helper | Arguments |
| ---: | --- |
| `0x013CB3D0` | source character ID, target area-entity ID, command-variant argument |
| `0x013CB870` | source character ID, target object ID, dummy-poly ID, command-variant argument |
| `0x013CCBD0` | target character ID, target dummy-poly ID, source character ID |
| `0x018BBA70` | character object, floor object |

The ordinary commands pass zero and the short-range command passes one as the
variant argument to the area/object helpers. In 01.09 neither helper consumes
that incoming argument, so the two variants currently resolve and apply the
same local placement. The area helper resolves a map entity and invokes the
source character's native placement method. The object helper resolves a
map-object dummy poly. The character helper derives a target-relative transform
and applies it to the source character. The set/copy-floor handlers call
`0x018BBA70` before placement so collision and floor ownership remain
game-managed.

These are local game operations, not a hidden network warp RPC. They are useful
inside the guest process after Bloodborne has created the multiplayer session;
for example, a guest-side event can warp local player entity `10000` to a
host-derived character or map marker. They do not replace the native summon
request, NP room, insertion, received-placement, or map-load lifecycle. The
trace now observes all four helpers so a successful paired run can establish
which operation retail summoning already uses before any hook invokes one.

## Runtime capture

Set this environment variable before launching shadPS4:

```text
SHADPS4_BLOODBORNE_RE_TRACE=1
```

For the two-client comparison, enable all three shadPS4 flags in both client
processes:

```text
SHADPS4_BLOODBORNE_RE_TRACE=1
SHADPS4_CAPTURE_BLOODBORNE_SUMMON=1
SHADPS4_BLOODBORNE_SEAMLESS_COOP=1
```

Two clients on one machine must use different local profiles and UDP ports.
The CLI `--user-id` option selects a profile only for that process and does not
rewrite the default in `users.json`; set `SHADPS4_P2P_PORT` to a different port
for each process. Use a different existing directory with `--cache-dir` for
each process because the archived pipeline cache is not safe for concurrent
writers. Both profiles need distinct shadNet accounts and separate save
directories.

Run shadNet with `BloodborneSeamlessCoop=true` or
`SHADNET_BLOODBORNE_SEAMLESS_COOP=1`. Trace headers and capture filenames carry
the host process ID, and every JSONL entry carries wall-clock milliseconds, so
two local clients can write to the same capture directory without ambiguity.

On CUSA03173 01.09, shadPS4 installs byte-verified observers and writes JSONL
under `captures/bloodborne-re/`. The capture records native SOS area codes and
the resolved area-method address, bell-messenger stages, `CSRequestGetSos`
startup and selected request area, changed action masks, white/black SOS
dispatch, matching events, accepted candidates, observer registration,
pending-list selection and request-handle allocation, summon-state transitions,
received and copied placement banks, forced-map selection, summon-point queries,
map reloads, generic `PlayerWarpTool` dispatch, concrete executors, decoded
arguments to `0x0154EA30`/`0x0154B110`, and the resolved `2004` Character area,
object, character-relative, and floor operations. It also records `WarpNextStage`,
`WarpNextStage_Bonfire`, WarpParam/respawn-point lookup, the shared native
stage-transition request, stage-load descriptor construction, request
acknowledgement, respawn-point resource resolution, the native respawn transform
resolver candidate, and placement/update candidate. Calls are observed without
changing return values or replacing game behavior.

The single-client restriction and destination comparisons are complete. A
negative-area Beckoning ring now uses the normal Bell effect, active indicator,
SOS event, request builder, and HTTP path. Small Resonant and Sinister now pass
their shared native inventory-area verdict while retaining later common
restrictions. Live paired runs confirmed Small Resonant item execution and
effect `9005` in both naturally allowed and originally blocked regions. The
blocked boss-arena retry confirmed that the signed-range patch for the shared
responder predicate restores native advertisement construction in area
`-241109`.
The named warp routes establish ordinary request encoding, acknowledgement
timing, and destination resource lookup. Ordinary lantern/headstone request
type `9/2` did not hit the candidate transform resolver or candidate placement
routine after lookup, so more unpaired menu travel will not resolve the
remaining guest-placement questions.

The paired living-boss control run completed on 2026-08-14. A rang Beckoning
and polled area `230100`; B advertised Small Resonant role 7 in the same area.
A inserted B into the pending list and reached every builder marker:
`RoleDelayPassed`, `SessionStatePassed`, `ControlAndWorldStatePassed`,
`GlobalCapacityPassed`, `RoleCapacityPassed`, and `RoleRules`.
`SummonBuild.Return` returned B's prepared request object instead of null, and
`SummonSelection.QueueInsert` queued it.

The subsequent lifecycle remained native end to end. A posted
`/summon_messenger/request`, called `sceNpMatching2CreateJoinRoom`, and created
room 1. B called `sceNpMatching2JoinRoom`. Both peers reached signaling states
`ESTABLISHED` and `MUTUAL_ACTIVATED`, after which B deleted its responder
advertisement. Bloodborne displayed its discovery and summon notifications,
both characters became visible, and movement replicated in both directions.
B's trace recorded the native area placement call with source entity `2300204`
and target area entity `2302300`; no emulator-defined warp message was needed.
The host then entered the living boss fog, the guest followed, and both clients
transitioned into the encounter normally. Character movement, boss state, and
combat remained synchronized until the clients were intentionally terminated.

The initial completed-boss comparison returned null between
`SummonBuild.SessionStatePassed` and `ControlAndWorldStatePassed`. Inside that
block, `0x018749E1` calls the native area/event-state validator at `0x0131D7B0`;
the branch at `0x018749E8` rejects a nonzero result. The relevant map script
sets event flag 2410 when its boss-completion flags are on, accounting for the
rejection observed in the dead-boss region. Seamless mode now NOPs only this
byte-verified rejection branch. The following negative-area, role, population,
capacity, and session-rule checks remained native at that stage of the
investigation.

A clean server and two-client retry confirmed the completed-boss patch. B
advertised, A requested it, B joined A's NP room, signaling activated, the
guest ran `SummonedMapReload.Native`, and native placement completed. The user
confirmed both characters were loaded, visible, and synchronized. One client
later encountered the recurring `GXRenderThread` access violation at
`0x80263BA08`; its signature predates this summon path and occurred after the
co-op lifecycle had completed.

The next paired test moved both clients into the defeated boss arena whose
selected SOS area is `-241109`. A's Beckoning search continued polling, while
B's Small Resonant item execution returned success and installed effect `9005`.
B never entered `/summon_messenger/create`, so the restriction was downstream
of item use and upstream of HTTP or shadNet.

The retry with the shared responder range patch confirmed that boundary. Both
trace headers reported `responder_search_negative_area_patch=true`; B posted
its role-7 advertisement with area `-241109`, and A received Andrews. A passed
the native identity, session, area, duplicate, and freshness filters, inserted
the pending candidate, and matched it. The request builder then passed
`RoleDelayPassed` and `SessionStatePassed` but returned null before
`ControlAndWorldStatePassed`.

The next instruction after the already-patched completed-boss event-state
branch is a separate sign-only rejection. At `0x018749F0`, `js 0x01874B36`
rejects the native table event ID when it is negative. Area `-241109` resolves
to the valid table sentinel `-1`, so seamless mode now NOPs only this
byte-verified branch. The subsequent role, population, capacity, and
session-rule checks remain unchanged.

The clean retry confirmed the negative-event patch. A reached every remaining
builder marker, `SummonBuild.Return` returned the prepared request for area
`-241109`, and the normal request handle and queue were created. A posted
`/summon_messenger/request` and called `sceNpMatching2CreateJoinRoom`; B called
`sceNpMatching2JoinRoom`. Both signaling connections reached `ESTABLISHED` and
`MUTUAL_ACTIVATED`, B ran `SummonedMapReload.Native`, and native placement used
source entity `2410019` and target area entity `2412010`. The user confirmed
that both characters were visible and movement remained synchronized inside
the normally unsummonable defeated-boss arena.

The first true cross-map run started A in that Great Bridge area at `-241109`
and B in Hunter's Dream at `210000`. B successfully executed Small Resonant and
advertised role 7. shadNet returned it to A with the requester's top-level
location, and A decoded, accepted, submitted, and inserted the advertisement
into its primary candidate list. The candidate repeatedly passed the identity
and session checkpoints but stopped before `SummonCandidate.AreaGatePassed`;
no pending record, summon request, NP room, or map reload was created.

The intervening native block performs a second location check that is not a
top-level HTTP field comparison. It reduces the manager area at root `+0xA80`
and decoded candidate area at `+0x98` by signed division by ten, compares the
two at `0x014B7148`, then rejects a mismatch with `jne` at `0x014B714A`. Live
operands retained the real areas `-241109` and `210000`, proving that this is
the first cross-map candidate restriction. Seamless mode now NOPs only the
byte-verified mismatch branch. It does not rewrite the opaque SummonData or
replace the later request, placement, and reload calls.

The restarted run confirmed that patch. With manager area `-241109`, candidate
area `210000`, and manager flags `+0xB14 == 0`, A reached
`SummonCandidate.AreaGatePassed`, consumed the normalized availability count,
inserted the pending record, matched it, and entered `SummonBuild.Entry`. The
prepared request retained B's real area `210000`. The builder passed its delay
and session-state stages but returned null before
`SummonBuild.ControlAndWorldStatePassed`.

The builder repeats the same signed area-group comparison later in its control
block. It compares the current world area and prepared candidate area at
`0x01874B77`; the original `je` at `0x01874B79` continues only when they match,
while a mismatch enters the null-result path. Seamless mode now changes that
byte-verified conditional jump to an unconditional jump to the same normal
continuation. The subsequent control, role, capacity, population, and session
rules remain native.

The next restarted run confirmed every remaining network stage across the map
boundary. A built and queued the request, created the NP room, and claimed B's
advertisement. B joined the room, both signaling connections became mutual,
and B ran `CSMultiPlayerIns.PlacementCopy` and `SummonedMapReload.Native`. The
session was genuinely active, but B remained in Hunter's Dream instead of
loading Great Bridge.

The placement records explain the split result. B's received record at global
`+0x1620` still contained its advertised Hunter's Dream transform, packed map
`0x15000000`, and area `210000`. `CSMultiPlayerIns.PlacementCopy` copied that
record into the normal SOS and summoned-placement banks. The separate selected
summoned-map slot at `+0x14F0` remained `0xffffffff`, and the selector byte at
`+0x1520` remained zero. Bloodborne therefore had a connected player but no
Great Bridge destination to select for the guest reload.

The HTTP capture shows why shadNet cannot derive the missing placement from the
normal claim. `/summon_messenger/request` includes the two users, characters,
and session ID, but no host map or transform. The responder's opaque SummonData
does include a placement record at payload offsets `+0x58` through `+0x6C`, and
that original responder record is what survived into the prepared request.
Changing only the response's top-level area and position cannot change the
later native multiplayer placement payload.

`SummonSelection.QueueInsert` at `0x01875320` copies the complete 0xA8-byte
prepared descriptor into Bloodborne's own request queue. Seamless mode now
hooks `SummonBuild.Entry` at `0x01874710` and, only when the prepared map differs
from the current host map, replaces the descriptor's map, position, heading,
and SOS area with the host's live values before that native copy. The source is
the same game-owned transform chain used by `SetSosSignPos`: position at
transform `+0x1E0`, heading at `+0x1D4`, and the current packed map resolved
from the active map-list entry at global `0x0553B148`. That map lookup is a
nested container: the active index is at list `+0x20`, list `+0x10` points to
the array owner, its count is at `+0x18`, its entries pointer is at `+0x20`,
and each entry is 0xA0 bytes with the packed map at `+0x08`. The SOS area comes
from manager root `+0xA80`. Same-map requests are left intact, and invalid
pointers, mappings, maps, or non-finite transforms reject the rewrite without
partially changing the descriptor.

Static disassembly also establishes what the guest does with a corrected
record. `CSMultiPlayerIns.StartNotifyWait` copies received position,
orientation, and map at global `+0x1620/+0x1630/+0x1644` into both the ordinary
SOS bank `+0x14A0/+0x14B0/+0x14C0` and summoned-placement bank
`+0x14D0/+0x14E0/+0x14C4`. It does not set forced map `+0x14F0` or selector byte
`+0x1520`. `SummonedPlacement.SelectMap` uses `+0x14F0` only when `+0x1520` is
set; otherwise it resolves the normal/default map. None of
`CSMultiPlayerIns.StartNotifyWait`, `StartWait`, `FirstSyncWait`, `Create`, or
`CreateWait` calls the selector or the stage transition. This is expected for
retail matchmaking, where the area gates guarantee that the summoned client
has already loaded the destination map.

Seamless mode now hooks the common post-copy point at `0x01E4FF3A`. The hook is
functional only for a multiplayer insert object whose role field at `+0xE8`
is zero, the summoned-client role observed in the paired capture. It also
requires a valid received placement, a valid transported host map when the
versioned header is present, a map different from the currently loaded map,
finite placement vectors, writable global state, and no existing stage
transition or forced warp. The transported transform replaces the retail
responder-sign copy in the summoned-placement bank immediately before the
native setters run. Same-map and host-side insert paths return without
modifying state, and a create response without the header clears stale cached
transport state.

For a qualifying cross-map guest, the handoff uses Bloodborne's native setters
at `0x0156CF20`, `0x0156CF40`, and `0x0156CF10` to copy the summoned position,
orientation, and map into forced slots `+0x1500/+0x1510/+0x14F0`. It invokes
the native warp setter at `0x0156CF60`, calls the selector at `0x01332BC0` with
warp-info ID `-1`, verifies that the selected map at global `+0x0C` equals the
received map, and finally calls the normal stage-transition routine at
`0x013CDE30`. The handoff arms the destination but does not serialize the
session yet. Repeated executions of the game's insert loop copy the responder's
old-map transform back into the summoned placement bank, so every qualifying
post-copy callback now restores the transported host transform before applying
its transition-pending guards. The descriptor-finalization hook described
below dispatches the reload only after the game has captured the target map.
This reuses the game-owned placement, reload, and transition path rather than
implementing a custom emulator teleport. The post-copy trace records every
guard result, whether placement was refreshed, and the native selector and
transition results under `cross_map_guest_handoff`.

The selector's disassembly removes one ABI ambiguity: `0x01332BC0` ignores its
first argument and preserves only `esi` as the warp-info ID. When forced-warp
byte `+0x1520` is set, it selects map `+0x14F0` directly and writes it to
selected map `+0x0C`. Calling it as `(0, -1)` therefore matches the native
forced path and does not omit a context object.

The other direct selector/transition users explain why their extra manager
writes must not be copied into the summon handoff. Their embedded game names
are `HostDead_1` (`0x01381A60`), `SoloPlayDeath_2` (`0x01381DE0`),
`PartyGhostDeath_2` (`0x013824C0`), `PlayerKill_4030_1` (`0x01383A00`),
`BlockClear2_1` (`0x01385470`), `BlockClear2_3` (`0x01385930`),
`OnReviveMagic_1` (`0x01389E80`), `OnLeave_Limit` (`0x0138A4A0`), and
`Failed_BossAreaMission_LeaveMap` (`0x0138B4E0`). `SetSelfBloodMapUid` at
`0x0132E170` also calls the selector but does not start a transition. These are
death, revival, progression, and forced-leave paths; their additional state is
specific to those operations. The common native core is precisely the forced
placement setters, selector, and stage transition currently used by the hook.

The two failed cross-map captures provide end-to-end controls. Before the host
rewrite, A queued B's Hunter's Dream descriptor and B later copied and selected
the same `0x15000000`. After the host rewrite, A's trace showed an applied
`0x18010000` Great Bridge descriptor in `SummonBuild` and
`SummonSelection.QueueInsert`, but B still copied `0x15000000` from its own
sign state. The raw claim then proved why: `0x01E98650` did not serialize the
rewritten descriptor or any host placement. The versioned header now carries
the already game-owned host rewrite across exactly that missing boundary; it
does not replace matchmaking, signaling, player insertion, selection, or map
loading.

The first live transported-placement run reached
`cross_map_guest_handoff.result=applied` with `transported_host=true`, selected
map `0x18010000`, and transition result 1. B loaded Great Bridge at A's exact
captured transform `[-132.559601, -27.0188828, 55.6097145]`. That validates the
host rewrite, shadNet relay, guest setters, selector, and native stage warp.
The clients were not visible because Bloodborne left the NP room during the
reload and A continued issuing the same summon claim.

Three temporary Matching2 leave experiments were used only to isolate that
lifecycle failure. Returning success while retaining membership, omitting the
callback, and returning an aborted callback each failed differently: either B
remained on the loading screen or loaded without A completing the summon and
without mutual visibility. Those experiments are removed. The current code
does not intercept `sceNpMatching2LeaveRoom`.

The next clean run called `ForceSummonReloadStart` at `0x01389620` before the
stage transition. The handoff recorded `summon_reload_started=true`, but the
nested `SummonedMapReload.Native` observer never fired. Disassembly explains
the result: the force handler still gates its tail-call to `0x0131E5C0` on the
game's current multiplayer state, and the post-copy hook executes before that
gate reaches the required state. B loaded the transported map, left room 1
about 25 seconds after joining, and the clients again remained invisible.
The next builds called the unconditional named wrapper at `0x01336B90`. A
clean run with that wrapper after successful placement selection reached
`SummonedMapReload.Native`, and room 1 remained joined for more than 40
seconds instead of B leaving after the reload. The serializer also restored
the staged destination to B's current `0x15000000` map, however, so
`Warp.RespawnPlacement.Apply` placed B back in Hunter's Dream. Calling the
wrapper before the forced setters, after the setters but before the stage
transition, and after the stage transition all produced the same Dream task.
The third trace is particularly conclusive: `Warp.StageTransition` entered
with global map `0x18010000`, then `SummonedMapReload.Native` ran, and the
eventual placement task contained `0x15000000`. Immediate call ordering cannot
separate room retention from stale current-map serialization.

Static analysis isolated the final state transition in `0x0131E5C0`: after
building its three reload buffers, it sets the object at global `0x0553D6D0`
offset `+0x84` to state 3. The game also exposes the native one-argument state
transition at `0x0178D9A0`, whose complete body performs that same assignment.
A live build called this small setter before `Warp.StageTransition`. It kept B
in room 1 and the transition accepted `0x18010000`, but it prevented the game
from constructing a Great Bridge placement task. The stage request was
acknowledged while B's active map stayed `0x15000000`, so the deferred reload
correctly remained `waiting_for_target_map` and B stayed in Hunter's Dream.
State 3 must not be entered before the destination descriptor exists.

The next live build moved the small state setter to the common descriptor
finalization instruction at `0x01944F23`. That ordering succeeded in creating
a `0x18010000` task: `r9+0x08` matched Great Bridge, the session state changed
from 2 to 3, and `Warp.RespawnPlacement.Apply` later consumed the Great Bridge
map. Two independent failures remained. The insert loop had overwritten the
transported transform after the first handoff, so the task resolved B's Dream
coordinates `[-10.8132086, -6.98065758, -21.0132046]` on the Great Bridge map
and produced a grey/out-of-bounds scene. B also called
`sceNpMatching2LeaveRoom` roughly five seconds after descriptor construction,
just before placement and the deferred serializer, so room 1 was lost.

The current build still waits for a completed target descriptor at
`0x01944F23`, but dispatches the game's named `SummonedMapReload` wrapper at
that boundary before applying the small state setter. The next live trace
confirmed that the insert-loop refresh worked and that the wrapper ran twice
before placement, but the reload object remained in state 2 at the return from
each wrapper call. The Great Bridge task nevertheless resolved A's exact host
coordinates `[-132.559601, -27.0188828, 55.6097145]`, proving that the grey
screen placement bug is fixed. B still called `sceNpMatching2LeaveRoom` just
before placement because state 3 had not been established.

The current ordering therefore runs the complete named serializer after the
target descriptor exists, then uses the tiny native `0x0178D9A0` setter if the
serializer has not yet advanced the reload object from state 2 to 3. Only
after both operations does it restore the transported host transform to both
the summoned and forced placement banks. This combines the buffer construction
that previously retained a room with the correctly delayed state transition;
the descriptor already exists, so state 3 can no longer suppress its
construction. The old `SosStatus.Update` dispatch remains only as a guarded
fallback if the descriptor-bound sequence fails. Both functional hook sites
and all invoked game calls are byte-signature checked. This exact ordering has
now been live validated through exact host placement, but Bloodborne still
left the room immediately before placement.

### Multiplayer room lifecycle

The HLE boundary identified the imported `sceNpMatching2LeaveRoom` return at
`0x00CC790F`, but that call is only the final mechanism. Static recovery now
separates the policy owner, matching wrapper, matching controller, and generic
task scheduler.

`CSMultiPlayMan` is the game-level policy owner. The singleton pointer is at
`0x05540290`; the object is allocated as `0x308` bytes and constructed by
`0x01ECE640` with vptr `0x05351020`. Its important fields are:

| Offset | Recovered role |
| ---: | --- |
| `+0x10` | Owning game/world object |
| `+0x18` | Embedded synchronized matching-controller wrapper; its first qword is the controller pointer |
| `+0x38` | Request data used by the join flow |
| `+0x118` | Multiplayer-active/notification byte |
| `+0x11C` | Request deadline or duration value |
| `+0x120` | Auxiliary multiplayer phase/result |
| `+0x124` | `CSMultiPlayMan` lifecycle state |
| `+0x138` and later | Member, event, and notification containers |
| `+0x280` | Asynchronous leave-completion task |

The lifecycle state at `+0x124` has two three-state operation groups:

| Value | Meaning established from writers and consumers |
| ---: | --- |
| `0` | Idle/reset |
| `1` | Create/join-room flow starting |
| `2` | Create/join-room flow failed |
| `3` | Create/join-room flow established |
| `4` | Join-existing-room flow starting |
| `5` | Join-existing-room flow failed |
| `6` | Join-existing-room flow established |
| `7` | Leave in progress |
| `8` | Queried by `0x01ED2300`; no direct writer has been recovered |

`0x01ECFCD0` builds the room attributes, sets state 1, and submits matching
task type 7. `0x01ED0670` sets state 4 and submits matching task type 8 using
the request object at `+0x38`. The common update at `0x01ED0B20` consumes
matching events and advances the successful paths to states 3 and 6. The
state predicates at `0x01ED00B0` through `0x01ED2300` are simple comparisons,
not operations that create, join, leave, or warp a room.

The matching controller behind the wrapper is a distinct `0x460`-byte object.
Its constructor is `0x00CC3370`, factory is `0x00CBF7D0`, primary vptr is
the relocated table at `0x052C6710`, and its secondary
reference-count/interface subobject begins at `+0x68` with the relocated table
at `0x052C6900`. The base controller owns the generic scheduler
at `+0x10`, lifecycle state at `+0x118`, current time at `+0x58`, member vector
at `+0x80/+0x88`, context ID at `+0x368`, and locks at `+0x3D0/+0x3E8`. The
derived NP matching controller adds the room ID at `+0x400`, request ID at
`+0x430`, and the asynchronous request reference at `+0x458`.

The controller states are `0` idle, `1` starting, `2/3` the two established
room modes, and `4` stopping. `0x00C98070` constructs stop task type `0x0C`
at task `+0x24`, records its reason at task `+0x218`, submits it through the
controller scheduler, and sets controller operation `+0x3C0` to 3. Its callback
`0x00C98110` changes the controller to state 4 and schedules virtual slot
`+0x78` for established mode 3 or slot `+0x80` for mode 2. Both paths converge
on the same orderly teardown:

1. `0x00C9B6E0` sends internal event `0x8001000100000000` to each member.
2. `0x00C9B960` waits for the member vector to empty and schedules virtual slot `+0x88`.
3. `0x00CC5390` unregisters room callbacks and schedules `0x00CC53C0`.
4. `0x00CC53F0` calls the native leave wrapper with room ID `+0x400` and stores its request ID at `+0x430`.
5. `0x00CC54F0` waits for completion, clears the room ID, and schedules `0x00C9BBE0`.
6. `0x00C9BBE0` returns the controller to idle and completes the original stop task.

NP room callbacks can also initiate this chain. The controller callback at
`0x00CC3DA0` maps event `0x1101` to member joined, `0x1102` to member left,
`0x1103` to kicked out, `0x1104` to room destroyed, and `0x1105` to room owner
changed. Kicked-out and room-destroyed events enqueue stop tasks with their own
reasons. They are not the source of the observed cross-map leave: shadNet saw
the guest voluntarily submit `sceNpMatching2LeaveRoom`.

The voluntary game-level stop is `CSMultiPlayMan::Stop` at `0x01ED07A0`. It
returns false without acting only from idle state 0 or failed states 2 and 5.
For the other states it sets state 7 and calls wrapper method `0x00C8ED70`.
That method locks the wrapper at `+0x08`, loads its controller pointer from
`+0x00`, and enqueues stop reason `0xFF000023` through `0x00C98070`. A successful
submission creates the completion task at `CSMultiPlayMan+0x280`; a failed
submission immediately invokes reset `0x01ED0910`. Reset clears members and
requests, releases the matching wrapper, and returns the object to state 0.

There are only six static callers of the controller stop-task constructor.
The wrapper path above is the sole caller that supplies `0xFF000023`. Matching
callbacks forward a reason from their event payload or use `0xFF000022`, and a
separate controller method at `0x00CC6390` supplies `0xFF000021`. Recording the
reason therefore distinguishes the stage-policy request from room destruction,
kick, and lower-level controller failures without inferring ownership from the
final `sceNpMatching2LeaveRoom` call.

One unconfirmed stop path remains at `0x0193C657` inside stage update
`0x0193AC10`. The updater resolves the active index from
`stage_manager+0x118`, indexes a `0xA0`-byte stage-record array at
`stage_manager+0x28`, and reads the record UID at `+0x08`. When that UID differs
from the cached UID at the current local-state object `+0x3F8`, it updates the
cache and transition flags, then calls `CSMultiPlayMan::Stop` only if
multiplayer state is 1 or 3. The transported guest was in state 6, and the live
cross-map capture did not hit this checkpoint. It is therefore not the source
of the observed guest leave and remains unpatched.

The actual caller is owned by the stage-transition task hierarchy. Function
`0x01939970` allocates a `0x58`-byte scheduler wrapper at the owning stage
object's `+0x140`, allocates its `0x50`-byte payload, and constructs that payload
at `0x01946CE0` with vtable `0x05330540`. Stage update `0x01941C20` waits for the
wrapper state at `+0x0C` to become 3, releases the wrapper, and thereby invokes
payload destructor `0x019470F0`. The destructor releases its subordinate
objects at `+0x38/+0x40`, checks summon-reload phase
`[0x0553D6D0]+0x84`, and calls `CSMultiPlayMan::Stop` at `0x019471B1` whenever
that phase is not 3.

The clean capture proved the order. The destructor entered the stop method
with reload phase 2, current map `0x15000000`, multiplayer state 6, controller
state 2, and room ID 1. The wrapper supplied reason `0xFF000023`. Only afterward
did `SummonedMapReload.Native` and `Warp.StageDescriptor.Finalize` advance the
reload phase to 3 and load `0x18010000`; native leave then ran with controller
state 4 and the same room ID. No stage-UID policy event occurred.

Seamless mode retargets only the byte-verified call at `0x019471B1`. Instead of
calling `CSMultiPlayMan::Stop`, it calls the class's native matching-existence
predicate at `0x01ED00A0`. That method adjusts `this` to the embedded wrapper
at `+0x18` and tail-calls `0x00C8F570`, which locks wrapper `+0x08` and tests the
controller pointer at `+0x00`. An active room therefore follows the
destructor's original success branch and preserves its `reload_state+0xF0`
write without changing `CSMultiPlayMan` state or enqueueing a stop task. An
idle object still returns false. This keeps the decision at the recovered game
class boundary and leaves all other `CSMultiPlayMan::Stop` callers, NP room
callbacks, and HLE leave handling intact.

The next paired run proved that this retarget did exactly that, but also found
a later owner of the same policy. B completed the forced transition to
`0x18010000`; no stop request came from `0x019471B1`. Roughly nine seconds after
the target descriptor advanced the reload object to phase 4,
`SprjSessionManager::OnMatchingCheck` at `0x013808B0` entered with
`CSMultiPlayMan` in established join state 6. Its temporary WorldChrMan lookup
failed to find the multiplayer character while the destination world was being
rebuilt, so the callback called `CSMultiPlayMan::Stop` at call return
`0x013809BC`. That submitted reason `0xFF000023`, changed the matching controller
from established state 2 to stopping state 4, and voluntarily left room 1.
This is the direct cause of the most recent `Session Lost` result.

Static recovery shows that `OnMatchingCheck` and its sibling
`OnMatchingError` at `0x013809F0` belong to `SprjSessionManager`. The check uses
WorldChrMan's character table at singleton `0x0553E878`; the helper at
`0x01338CE0` scans character entries and tests the event's multiplayer IDs.
When neither ID resolves, the callback stops matching and calls the WorldChrMan
cleanup routine at `0x015BED10`. That behavior is correct for a missing remote
entity in a stable world, but a cross-map reload creates the same observation
while the entity table is necessarily empty. Retargeting this second stop call
would keep the current post-join warp alive, but it would also weaken a real
disconnect path. It is therefore retained unchanged.

The implementation now avoids that ambiguous class state rather than adding a
second teardown bypass. Cross-map setup is a two-phase search followed by the
ordinary claim:

1. The responder advertises normally while only searching; no NP room exists.
2. A host search sends its current game-owned placement descriptor in the
   `X-ShadPS4-Bloodborne-Host-Placement` request header.
3. If a matching responder advertises a different `AreaId`, shadNet marks that
   advertisement `Preparing`, stores the host descriptor, hides the responder
   from the current host search, and returns zero candidates. It has not created
   or claimed a room.
4. The responder's next normal `/summon_messenger/create` receives only the
   stored placement header. A delete caused by the map transition is retained
   while the advertisement is `Preparing`.
5. On `Game:Main`, `SosStatus.Update` consumes the descriptor only while
   `CSMultiPlayMan` is idle and has no matching controller. It calls the native
   forced-map, position, orientation, forced-warp, placement-selection, and
   `StageTransition` functions. It deliberately does not call
   `SummonedMapReload`; doing so before a room exists loaded default world
   progression instead of the character's real save state.
6. Once the destination map, exact SOS area, stage flags, matching state, player,
   and transform have remained valid for 20 consecutive updates, the client calls
   the recovered native item function at `0x018F9720` with Small Resonant Bell
   goods ID `205` and use argument `17`. It waits for the real effect `9005` and
   retries only once after 15 seconds if the effect never appears.
7. The resumed native bell task advertises the destination `AreaId`. shadNet moves
   the record from `Preparing` back to `Advertised`; the host's next ordinary
   search returns it, after which Bloodborne performs its normal claim, room,
   signaling, insertion, and summon flow with both clients already in one world.

Same-map searches skip the preparation phase and return the candidate
immediately. shadNet still performs no warp and owns no game state. It transports
the descriptor and gates visibility of the advertisement; all map loading and
item execution remain game-owned operations.

The received descriptor is a single-use pre-match warp token. The successful
transition marks it consumed so a later player-requested warp cannot be mistaken
for another pending handoff. The descriptor itself remains available for the
eventual multiplayer insertion, where the normal guest-handoff code can still
verify or apply the host transform. A response without the placement header
clears both the descriptor and its consumed state.

The earlier `0x00C8F4A0`/`0x00C982D0` hypothesis was incorrect. That pair
constructs unrelated async task type `0x1D` with selector `0x91`; neither
observer fired during the actual leave. The trace retains the stage-UID policy
checkpoint at `0x0193C5DE` to distinguish that independent path, then records
every `CSMultiPlayMan::Stop` caller, controller stop-task construction, reason,
and native leave entry. The checkpoint hook is placed at the non-relative state
load rather than the relative `call` at `0x0193C657`, which the generic
guest-code hook correctly refuses to relocate.

The trace no longer records calls from background floor-maintenance return
sites `0x018BEE67` and `0x018BEF34`. Those two sites accounted for millions of
irrelevant `CharacterWarp.SetOrCopyFloor` hits in idle captures. Calls from the
event-instruction handlers and all other callers remain visible, preserving
the evidence needed for lantern-follow analysis while avoiding continuous
JSONL writes and flushes.

## Validated paired cross-map run

The successful run used responder A (`wozzardman`, process 9510) in Hunter's
Dream and host B (`Andrews`, process 9698) at Great Bridge. A first advertised
map `0x15000000`, SOS area `210000`, under session
`e7be9072-9e8a-4f84-b423-84cb85f17f0f`. B searched from map `0x18010000`, SOS
area `241100`, at `[-125, -26, 64]`. shadNet returned zero candidates, marked A
`Preparing`, supplied the 54-byte placement header on A's next create, and
retained A's transition-driven delete.

A's trace then recorded the native `StageTransition` from Hunter's Dream to
Great Bridge. After arrival, `pre_match_guest_warp` reported
`placement_consumed`, preventing the cached header from initiating another
warp. The destination SOS area settled at `-241109`. The resume state reached
20 stable observations with `CSMultiPlayMan` state 0 and no controller, called
`ChrAction.UseItem.NativeApply` once with goods `205` and argument `17`, observed
effect `9005`, and reported `responder_resume.result=completed` after one
attempt. The bell remained visibly active. The same session then advertised
map `0x18010000` and shadNet returned it on B's next search.

B created room 1 and claimed A through the unmodified Bloodborne request. The
two clients exchanged signaling information and A joined the room. At final
insertion, A's `CSMultiPlayerIns.CrossMapGuestHandoff` repeatedly reported
`same_map` with the transported host descriptor still present. The only
`SummonedMapReload.Native` call occurred afterward in the ordinary same-map
summon path; it was not used for the pre-match transition. No
`CSMultiPlayMan.StopRequest`, `Matching2.StopTask.Enqueue`, or
`Matching2.LeaveRoom.Begin` was recorded during preparation.

Both clients displayed both characters, movement remained synchronized, and A
killed enemies while acting as the summoned player. A's existing Great Bridge
lantern was present after the pre-match load, confirming that the transition
preserved the real save's world progression. Its presence does not mean its
interaction was available: neither client received a lantern prompt while the
session remained active. This validates cross-map
discovery, automatic responder relocation, automatic native bell resumption,
ordinary room establishment, bilateral entity insertion, and combat sync for
Hunter's Dream to Great Bridge.

Remaining coverage should repeat this sequence across additional map pairs,
resolve the downstream host lantern prompt gate, then exercise lantern travel
while the session is established and verify disconnect, death, and
manual-silence recovery. Those are lifecycle and breadth tests; the core
cross-map summon path is now live validated.
