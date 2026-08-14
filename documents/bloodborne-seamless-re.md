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
- shadPS4 owns HTTP routing plus PS4 NP matching, signaling, and P2P transport.
- Bloodborne owns bell eligibility, summon lifecycle transitions, character
  insertion, summon-point selection, and warp/map reload behavior.

shadNet no longer adds a private `SeamlessWarp` field, and shadPS4 no longer
parses one. That metadata was not consumed by Bloodborne and could not invoke a
native warp.

The captured `SummonData` blob is 224 bytes. In the current capture its four
little-endian floats at offsets `0x5C`, `0x60`, `0x64`, and `0x68` are
`143.97487`, `-116.94293`, `-87.73028`, and `1.8583716`, matching the exact
player position plus heading behind the rounded JSON `PosX/PosY/PosZ` values.
This is another reason shadNet preserves game-owned `SummonData` and `HostData`
bytes. The one confirmed exception is the response-only available-result count
at SummonData offset `0x79`, described below. A two-client claim capture is
needed to confirm the parallel `HostData` layout and its role in guest
placement.

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
and owns the reload. Live captures still need to establish host/guest ordering
and which callback sets `+0x14F0/+0x1520` in same-area and cross-area cases
before any function is called or hooked to change behavior.

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
prompt therefore occurs earlier in the engine-owned Healing Fountain interaction
availability path. `HealingFountain.Register.Match` (`0x017C67C0`) passively
captures the matching registration object and vtable so that this prompt gate can
be isolated without replacing the native warp.

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
the normally unsummonable defeated-boss arena. The next test should keep this
successful session connected while the host performs a stage transition so
guest reload ordering can be captured.
