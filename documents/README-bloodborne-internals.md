<!--
SPDX-FileCopyrightText: Copyright 2026 Wozzardman
SPDX-License-Identifier: GPL-2.0-or-later
-->

# bb seamless internal notes

mostly putting this here so I dont have to keep finding the same addresses again. This is for CUSA03173 01.09 only. Other versions probably moved all of this around.

Some names below are actually in the game, some are names I put on functions because the executable is stripped (anything using my internal naming convention is marked with ~~)

the basic path I have so far is something like

bell item -> sos state -> HTTP summon listing -> candidate -> request -> NP room -> signaling -> multiplayer insert -> guest reload

shadNet is doing the HTTP listing and matchmaking response side. shadPS4 still does the NP and P2P pieces. Bloodborne is still doing the bell, player creation, placement, map load and all the game state after that.

## bell check stuff

0x0157F200

~~Goods/Bell Availability. I dont have a real original C++ name for it yet. It is a big general item check and not just a Bell function.

goods I care about in it

200 = Beckoning 205 = Small Resonant 225 = Sinister

Beckoning ends up around 0x0157F855 in this function.

0x0157F8C8 was an unsigned area check. Negative SOS areas looked like a giant positive number because of that. Changed the branch to signed for seamless.

Then 0x0157F960 was still giving the Bell an area/event blocked result. That is the second Beckoning availability change.

Small and Sinister split off earlier at 0x0157F686, ~~ResponderBell.Availability.Entry in my trace.

I first only changed the area query result at 0x0157F6D1. That got farther but didnt make them usable in Hunters Dream. The function returned false later.

The current patch does this instead:

0x0157F6D1 sets the return register true

0x0157F6D3 goes to the normal stack checked return at the bottom of the function

This is after the function already decided the item is 205 or 225. It shouldnt make random items usable. Item execution itself is not replaced either.

0x015800D1 ~~ResponderBell.Availability.FinalResult is where I trace the final item availability answer. This is useful because I can see 0 or 1 without needing somebody to mash use on a grey item.

Hunters Dream test before that last change:

the Small Bell briefly returned 1 while loading, effect mask was 0x202205

then the mask went to 0x202207 and the answer stayed 0. Area was still 210000.

After the full responder return patch it stayed 1 and the Bell highlighted.

## another Bell path I kept mixing up with the first one

There is an active Bell update around 0x01506820. This runs after a Bell is already active and decides if it gets the normal searching state or the X.

0x015068BB is its area comparison

0x015068FC is the later result I changed to the normal active state

effects seen so far

9003 normal active/searching

9004 Bell X / cant work here

9005 Small Resonant active

I should probably keep those separate in my head. Inventory grey and active Bell X are not the same decision.

0x0191A750 is another shared area parser. It gets called from responder item availability, active responder status, advertisement building and candidate handling.

0x0191A8C3 was the final range reduction in it. Same unsigned negative area problem. The seamless patch keeps its normal table answer for valid signed negative areas.

## 0x018700D3 note because the name caused confusion

0x0186FE40 is what I have been calling the ~~SOS status producer.

That is my label, not a recovered function name.

0x018700D3 is inside it. It takes the area/event table result plus the sign of the SOS area and turns that into a restriction value. Seamless makes that one area derived restriction zero.

Later calls I named:

0x01872360 ~~SosStatus.Update

0x0187239E ~~SosStatus.NativeResult

This could belong to the same original class as SosSignMan. I havent recovered enough RTTI or vtables to say that for sure. The important part for me is it is an eligibility/status calculation. It isnt the function that copies the guest position or performs the guest map reload.

There may be a multiplayer zone event flag that is a cleaner way to enable the Hunters Dream. I still need the flag number and the script/read path. If it is only a multiplayer enable and has no progression use I would rather use that for the Dream-specific part.

dont forget the native area/event lookup itself is 0x0131D7B0. It takes the absolute area code and looks through the games area/event info. For the old -280050 test it resolved event flag 2800.

## host looking for a Small Bell

CSRequestGetSos calls. The dotted names are my trace labels:

0x01E5FAF0 ~~CSRequestGetSos.Init

0x01E5FBF0 ~~CSRequestGetSos.UpdateSeamless

0x01E5F970 ~~CSRequestGetSos.InsertType. role 7 in the Small Bell tests

0x01E5FCE0 ~~CSRequestGetSos.BuildRequest

0x01E60270 ~~CSRequestGetSos.SelectedArea

The game polls this while Beckoning is active.

Responder creates its listing through the normal game HTTP request. shadNet stores it and returns it to the host search.

One odd bit that mattered: SummonData raw offset 0x79 is an available result count. A zero never got through pending candidate insertion. shadNet changes a zero to one in the copy it returns to search. It doesnt modify the stored advertisement or rewrite all the opaque data.

For the anywhere search, shadNet makes the returned top level area, region, channel, matching level and position look like the requesters search. The original opaque SummonData stays game owned besides that known result count normalization.

This got the response through Bloodbornes normal candidate filtering.

candidate stuff, roughly in order but I might add more in the middle later:

0x014BA980 ~~SummonCandidate.Entry

0x014BAA5C ~~SummonCandidate.Accepted

0x014BABCB ~~SummonCandidate.Submit

0x014B6F09 ~~SummonCandidate.FilterCycle

0x014B6F5D ~~SummonCandidate.ManagerScan

0x014B6F9D ~~SummonCandidate.LocalIdentityPassed

0x014B6FE1 ~~SummonCandidate.ManagerIdentityStatePassed

0x014B7070 ~~SummonCandidate.ManagerIdentityPassed

0x014B70FF ~~SummonCandidate.SessionGatePassed

0x014B714A ~~SummonCandidate.AreaGroupMismatch. This is a jne after the game divides the hosts manager area and candidate +0x98 area by 10 and compares them. My first real cross map test had A at -241109 and B at 210000, so it kept stopping here. Seamless nops only this mismatch branch and leaves the real SummonData alone.

0x014B7150 ~~SummonCandidate.AreaGatePassed

0x014B71B2 ~~SummonCandidate.NoPendingDuplicate

0x014B71F4 ~~SummonCandidate.FreshnessPassed

0x014B750B ~~SummonCandidate.PendingInserted

0x014B8E8D ~~SummonCandidate.Inserted

Copied saves using the same CharaId were not the self-match problem I expected. The game uses the online name copied into its candidate identity for that check. Separate shadNet accounts still matter.

## request selection mess

0x01873269 ~~SummonSelection.Scan

0x018732DD ~~SummonSelection.Matched

0x01874710 ~~SummonBuild.Entry

0x01875320 ~~SummonSelection.QueueInsert

0x01873605 ~~SummonSelection.RequestQueued

0x014BAEC0 ~~SummonSelection.RequestStart

0x014BAFE3 ~~SummonSelection.HandleAllocated

I added builder checkpoints because it kept returning null with no obvious reason.

0x0187477A ~~SummonBuild.RoleDelayPassed

0x0187481D ~~SummonBuild.SessionStatePassed

0x01874B79 ~~SummonBuild.AreaGroupMismatch. This is the builders second host/candidate area division and compare. The retry got through the candidate gate with host -241109 and candidate 210000, made the pending record and matched it, then returned null here. Seamless changes its equal-only jump to always continue on the normal builder path.

0x01874B8F ~~SummonBuild.ControlAndWorldStatePassed

0x01874BC7 ~~SummonBuild.GlobalCapacityPassed

0x01874C94 ~~SummonBuild.RoleCapacityPassed

0x01874E5A ~~SummonBuild.RoleRules

0x018750F7 ~~SummonBuild.Return

The defeated Great Bridge boss area failed before ~~SummonBuild.ControlAndWorldStatePassed. The call at 0x018749E1 asks 0x0131D7B0 about the area event state, then the branch at 0x018749E8 rejects it. I nop that rejection in seamless mode.

Right after it 0x018749F0 had another sign-only rejection. The negative area table had returned valid sentinel -1 and this branch rejected it anyway. That one is also nopped in seamless.

All the later role/capacity/session rules still run.

## actual position and reload calls

These names came out of game registration/string work, not just my labels:

SetSosSignWarp wrapper 0x0132E260

SetSummonedPos wrapper 0x01332BB0

SetSosSignPos wrapper 0x01333BA0

SummonedMapReload wrapper 0x01336B90

native reload it calls 0x0131E5C0

SetSosSignPos copies position/orientation/map into global placement storage at +0x14A0 +0x14B0 +0x14C0.

SetSosSignWarp sets the selector byte at +0x1520.

Received host placement seems to come in here:

0x0156E7A0 ~~ReceivedSummonPlacement.Set, writes the record around global +0x1620

0x0156E6E0 ~~ReceivedSummonPlacement.ConditionalSet

0x01E4FF1E ~~CSMultiPlayerIns.PlacementCopy

0x01332BC0 ~~SummonedPlacement.SelectMap

0x0131E5C0 ~~SummonedMapReload.Native

CSMultiPlayerIns runs from around 0x01E4FCF0 through 0x01E50D70. My trace names are ~~CSMultiPlayerIns.Init, ~~CSMultiPlayerIns.StartNotifyWait, ~~CSMultiPlayerIns.StartWait, ~~CSMultiPlayerIns.FirstSyncWait, ~~CSMultiPlayerIns.Create, ~~CSMultiPlayerIns.CreateWait, ~~CSMultiPlayerIns.Update, ~~CSMultiPlayerIns.WaitExit, ~~CSMultiPlayerIns.Delete and ~~CSMultiPlayerIns.DeleteWait. This was a good sanity check that Bloodborne created the joined player and not some custom emulator actor.

I did not patch SetSosSignPos, SetSosSignWarp or SummonedMapReload. They are traced so I can see when the normal game path uses them.

Things live tested successfully so far:

normal area in front of a living boss

same boss fog transition after joining

defeated boss area with SOS area -241109

Hunters Dream 210000

The Dream run did the native request builder, NP room, signaling, SummonedMapReload, character creation and movement sync.

I finally did the actual different map test, A was at Great Bridge with packed map 0x18010000 and SOS area -241109 while B was in Hunters Dream at 0x15000000 and area 210000. Matchmaking worked all the way through, room created, signaling was mutual, B ran the normal multiplayer insert and reload, the session was active but B stayed in the Dream.

The reason was in the placement data. B received 0x15000000 and its own Dream coordinates again. The separate summoned map slot was still ffffffff and the warp byte was 0. The normal claim request does not send a HostData field either, only session/user/character IDs, so shadNet has no host transform in that request to fix this with. I checked the actual serializer at 0x01E98650 after capturing the raw request and it literally only adds CharaId, TargetUserId, TargetCharaId, SessionId and UserId, also HostData did not resolve as a game string at all so the fake HostData in my old broker test was just a bad assumption.

0x01874710 ~~SummonBuild.HostPlacementRewrite is the hook I added at the native builder entry. If the prepared player map is different from the hosts current map I replace only map, xyz, heading and SOS area in that prepared descriptor before Bloodborne copies the whole thing into its own queue at 0x01875320. I get position at transform +1E0 and heading at +1D4 from the same chain used by SetSosSignPos, the map is resolved from its active map list at global 0x0553B148 and area is manager root +A80. That active map thing is nested, +20 on the first object is the index, +10 points at another object where +18 is count and +20 is the entries pointer, entries are A0 each and map is +8. I originally read count and entries inline on the first object and caught that in static comparison before testing it. Same map summons dont get rewritten. The next live run proved this hook did write and queue 0x18010000 but that object never goes in the native claim, B still got its own Dream sign placement, so now I cache this same exact rewritten host transform and shadPS4 puts it in a versioned X-ShadPS4-Bloodborne-Host-Placement header on the already happening host search. If shadNet sees a responder on another map it marks that listing Preparing, hides it from that search and gives the header back on the responders next create response, it does not invent coords or put extra fields in the game JSON. I tested the header route separately and got the exact bytes back.

The guest insert at 0x01E4FD50 copies received +1620 position, +1630 orientation and +1644 map into both the normal SOS placement and summoned placement banks. It does not set +14F0 or the +1520 selector byte though. I checked StartWait, FirstSyncWait, Create and CreateWait too and none of them calls the map selector or stage transition, retail never needed this because it did not let the two maps mismatch in the first place. ~~SummonedPlacement.SelectedMap at 0x01332CD0 logs the final map the selector actually picked.

0x01E4FF3A ~~CSMultiPlayerIns.CrossMapGuestHandoff is the post-copy hook I added for that missing case. I only let it do anything when the multiplayer insert role at +E8 is 0 which was B in the captures, received placement is marked valid, the transported header has a real map if one arrived, it is different from the map B has loaded, the vectors are finite and there isnt already a stage load or forced warp going. Same map and host objects just leave. The header transform replaces the responder sign copy in the summoned placement bank right before the native calls and a later create response without it clears the old cached value so it cannot leak into another summon. When all of that passes I call the games setters at 0x0156CF20 for forced position, 0x0156CF40 orientation, 0x0156CF10 map and 0x0156CF60 for the warp byte, then 0x01332BC0 selects it with -1. The first live run did actually say applied with transported_host true and transition result 1, B loaded 0x18010000 at the exact Great Bridge host position -132.559601 -27.0188828 55.6097145, so the header and native warp parts are not guesses now. B left the room during that reload though and the clients could not see each other.

I tried retaining that room three ways while narrowing this down, a fake successful leave, no leave callback, then an aborted leave callback. One got stuck black and the others loaded without finishing the summon or showing the other player. I removed all of that Matching2 interception because it was hiding the real ordering problem and there is no leave special case in the current code.

The normal join callbacks queue SummonReloadStart before the reload serializer. I first called ForceSummonReloadStart 0x01389620 after the forced map selector because that one sets an extra manager byte before it can tail call 0x0131E5C0, but the next clean trace showed the wrapper returned without ever hitting ~~SummonedMapReload.Native. Its disassembly has another state gate and this hook runs too early for that gate, so summon_reload_started true only meant I entered the wrapper, it did not mean it reloaded anything. B still warped and then left room 1 about 25 seconds after joining and neither client could see the other.

The build after that test used SummonedMapReload 0x01336B90 instead. That is a real named game wrapper, it checks the multiplayer global then unconditionally tail calls 0x0131E5C0. Calling it after 0x01332BC0 finally made ~~SummonedMapReload.Native show up and B stayed in room 1 for over 40 seconds, so that fixed the leave, but it also overwrote the staged map and ~~Warp.RespawnPlacement.Apply went back to 0x15000000 so B landed in the Dream again. I tried it before all the setters, between the forced selector and stage transition, then after StageTransition too. All three orders still made the later placement task 0x15000000. The last one proves the selector was not failing because ~~Warp.StageTransition entered with 0x18010000 and only after that ~~SummonedMapReload.Native ran, then the task changed back to the Dream.

I split that call apart after this. The bottom of 0x0131E5C0 sets +84 on the object from 0x0553D6D0 to 3 after it builds the buffers, and there is a tiny native state call at 0x0178D9A0 that takes the object and does that same +84 = 3 then returns. I first called that before the forced stage transition and it kept B in room 1, transition took 0x18010000 too, but no Great Bridge placement task ever got made and B stayed in the Dream. So state 3 there was too early and was blocking the constructor I needed. I moved it to ~~Warp.StageDescriptor.Finalize 0x01944F23 which is the common join after the game has filled the descriptor in r9 and the next run finally made the Great Bridge task, state was 2 then 3 and map was 0x18010000. It still looked like a flat grey screen because the repeated insert copies replaced my host xyz with the Dream xyz -10.813 -6.980 -21.013 before placement, basically Great Bridge loaded with B underneath or outside the map, and B left room 1 just before the later serializer ran. Refreshing the transported placement every time ~~CSMultiPlayerIns.CrossMapGuestHandoff repeats fixed that part, the next task really resolved -132.559601 -27.0188828 55.6097145 on Great Bridge. I also called the full SummonedMapReload wrapper at ~~Warp.StageDescriptor.Finalize and it built the buffers before placement but the state was still 2 when it returned, it called twice because I rearmed it then B left right as placement happened. That whole descriptor time reload order is not current anymore. It was useful for proving the map and placement calls but it was the wrong time to have a room alive and one early version even loaded default world progression instead of the real save.

I finally traced the actual leave all the way back instead of guessing from the final Matching2 call. The stage UID check at 0x0193C657 never even ran on B because B was CSMultiPlayMan state 6 and that check only handles 1 or 3. The stop came from 0x019471B1 inside the destructor at 0x019470F0, a stage task payload built at 0x01946CE0 with vtable 0x05330540 and owned through the scheduler object at the stage flow +140. It got destroyed while the reload phase at global 0x0553D6D0 +84 was still 2 so it called CSMultiPlayMan Stop 0x01ED07A0, that went through wrapper 0x00C8ED70 with reason FF000023, controller state went 2 to 4 and room 1 left after B had loaded 18010000. The next patch changes only that calls target to 0x01ED00A0 which is already a CSMultiPlayMan function, it adjusts to the wrapper at +18, locks it and uses 0x00C8F570 to return if the matching controller exists. So the destructor still takes its own success branch and writes +F0 when a room exists but it doesnt ask the room to stop, and when there isnt a controller it still gets false. Other Stop callers and sceNpMatching2LeaveRoom arent touched.

I checked 0x01332BC0 more carefully and it doesnt use the first argument at all, it keeps esi as the warp info id and if +1520 is set it takes the map straight from +14F0 then writes the selected map at +0C. So passing 0 and -1 isnt missing some mystery context pointer. I also mapped the other functions that use the same selector plus stage transition and the extra flags in them are not summon setup, the actual names in the game are HostDead_1 at 0x01381A60, SoloPlayDeath_2 at 0x01381DE0, PartyGhostDeath_2 at 0x013824C0, PlayerKill_4030_1 at 0x01383A00, BlockClear2_1 at 0x01385470, BlockClear2_3 at 0x01385930, OnReviveMagic_1 at 0x01389E80, OnLeave_Limit at 0x0138A4A0 and Failed_BossAreaMission_LeaveMap at 0x0138B4E0. SetSelfBloodMapUid at 0x0132E170 calls the selector too but does not start the load. This is why I kept the handoff down to the forced placement setters, selector and normal transition instead of copying death or leave state into it.

The failed different-map captures are useful controls now. First A built and queued 0x15000000 from B and B copied and selected the same map. Then ~~SummonBuild.HostPlacementRewrite correctly changed A's queued object to 0x18010000 but B still got 0x15000000 because that object is not serialized by 0x01E98650. That is the one hole the versioned header crosses, all of the matchmaking and actual warp still use the normal game paths.

The working order is before the room now. Host search sees the other map and shadNet puts the responder in Preparing instead of returning a candidate. ~~SosStatus.Update on the responder only accepts the placement while CSMultiPlayMan is idle with no controller, calls the four forced placement setters, 0x01332BC0 and ~~Warp.StageTransition, and does not call SummonedMapReload. After the new map and the exact host SOS area are stable for 20 updates I call 0x018F9720 with goods 205 and argument 17 which is the same native Small Resonant item function the manual ring hit. I wait for real effect 9005, there is one retry after 15 seconds and no loop spamming it. When the normal create advertises the host map shadNet makes it visible again and only then does the host discover it and start the ordinary claim and room flow.

That finally passed live. wozzardman started in the Dream and Andrews hosted at Great Bridge, responder map changed from 15000000 to 18010000, area settled at -241109, one native 205 call brought the bell back and shadNet saw the same session advertise at the bridge. The host found one result, room 1 formed, signaling went both directions and final ~~CSMultiPlayerIns.CrossMapGuestHandoff said same_map. Both clients showed both players and movement plus enemy kills synced. The bridge lantern was present too so it loaded the actual save progression, that only proves the save and map state were right though and not that the lantern could be used during the session. I mark the received header consumed after the first successful pre-warp because before that fix a later manual warp back to the Dream got mistaken for another pending handoff and sent the player back to the bridge. I keep the coordinates around for final insertion but the pre-warp itself only gets one use.

## warp stuff, this part is still kind of scattered

Normal lantern/headstone travel does not use exactly the same entry path as a summoned guest.

0x0132E010 WarpNextStage

0x0132E050 WarpNextStage_Bonfire

0x013CDF30 ~~Warp.RespawnPointParam / respawn point request

0x013CDE30 ~~Warp.StageTransition

0x01944D76 ~~Warp.StageDescriptor

0x0194100A ~~Warp.StageTransition.AcknowledgeCheck

0x01938F65 ~~Warp.RespawnPoint.Resolve

I had 0x0154AFC0 ~~Warp.RespawnTransform.Resolve and 0x01939C80 ~~Warp.RespawnPlacement.Apply marked as candidates. Ordinary travel did not hit either after resource lookup in the later captures, so I should not call those the final lantern placement path yet.

Some PlayerWarpTool calls that may be useful as fallback/debug paths:

0x0154D180 RemoteWarpPlayer

0x0154D5E0 RemoteResetPlayer

0x0154D7A0 GetPlayerInfo

0x0154DC50 RemoteWarpPlayerWorldPos

0x0154E0B0 RemoteWarpPlayerMapStudioPos

0x0154EA30 handles the parsed map and world position request

0x0154B110 is reached by its same-map placement path

Remote in those names means the debug command interface. It does not mean I found a network RPC that warps another client.

Also event instructions include WarpNextStage and warp to respawn point. Those may end up being the clean way to make an already connected summon follow host travel, but I want to observe the natural host/guest transition first. I cloned HotPocketRemix/BloodborneEventScripts separately and common event 7200 made this less vague, it does not disable network sync, line 0 ends if the process is already a Client, then it waits on the lamp flag and calls Warp Player to Respawn Point with that lamp instances respawn param. A same map guest may already have the task waiting from before it became a Client while a guest that cross-map loaded after joining may kill it at line 0, I need one live lamp use to tell instead of guessing. The stage warp trace now writes current map and whether local effect 9001 says host.

I checked the actual common.emevd.dcx too instead of only trusting the decoded repo. The zlib part starts at 4C and inflates to 11950 bytes, event 7200 is record 31 at 660, its instructions start at 7D70 and the first one really is bank 1003 command 6 with args 00 01 00 00 at F55C for end if Client. If the guest doesnt naturally follow I have one exact event instruction to deal with, I am not going to globally bypass end-if-client because lots of completely unrelated map events use it.

I stopped tracing the normal floor maintenance returns at 0x018BEE67 and 0x018BEF34 because they were hitting ~~CharacterWarp.SetOrCopyFloor millions of times and filling the json for no RE value. The EMEVD warp callers and the rest of the native callers still get recorded.

The join callbacks explain the reload names too. OnBeJoinStart_White and its black/force/etc copies queue event 4059 named SummonReloadStart, 0x01389440 handles that and may call 0x0131E5C0, ForceSummonReloadStart is 0x01389620 and sets one extra manager byte before the same call. So this still ends at ~~SummonedMapReload.Native on the joining process and it doesnt contain a host map or lamp respawn param.

## loose things

Messenger SosSign states, names are mine:

0x01CC9460 ~~Messenger.SosSign.Init

0x01CC94B0 ~~Messenger.SosSign.Wait

0x01CC94E0 ~~Messenger.SosSign.Finish

I only trace those.

I finally got the actual Register Healing Fountain route instead of guessing from a nearby record. Event group 2009 command 05 jumps to 0x017C60D6 and that handler reads the six script args then calls 0x0133B030, event flag, entity, distance, angle, initial sword number and sword level. I call the observer at that real native entry ~~HealingFountain.Register.Native. 0x017C67C0 was just a record lookup/removal loop so I removed ~~HealingFountain.Register.Match and the giant pointer dump, the vtable label I had on it wasnt justified.

I found the actual chair messenger task names after following the STEP strings, these are game names and not mine: CSChairMessengerRespawnPointNotifyStep::STEP_Init is 0x01E5C6C0, STEP_Update is 0x01E5C890 and STEP_Finish is 0x01E5CAA0. My observer names are ~~ChairMessenger.RespawnPointNotify.Init and ~~ChairMessenger.RespawnPointNotify.Update. This looked promising for the missing lantern prompt at first but I followed Update into 0x01EA06D0 and it builds the /api_ChairMessRespawnPointNotice WebAPI call with the respawn point ID, so it is server notification and not local prompt creation. The +9F6, +9F8 and +A50 checks are gates for that online request, I am not clearing them. +9F6 and +9F8 are latched from changes in an online state object at global 0x056C7048, the games own network debug text calls +9F5/+9F6 LAN connect/disconnect and +9F7/+9F8 sign in/sign out, +A50 comes from separate player state code around 0x0193A3E0. I left the observers because that mapping is still real but they are read only.

I followed the relocated vtable for the action object 0x0133B030 registers through 0x01327650 and its big update starts at 0x012F5C40. It calculates four blocked bytes at object +48 through +4B before calling 0x0146E250 with the final action enabled state. +49 is the multiplayer one, at 0x012F8315 it checks if the session has more than one player and also checks pending matchmaking then 0x012F8323 stores the answer. That is why the lamp prompt vanishes after the guest joins, finally an actual local prompt path and not the chair WebAPI thing.

~~HealingFountain.Availability.Host is my hook name for 0x012F836E, it is after all four bytes were calculated but before they get used. I clear only +49 when the local player has SpEffect 9001. Common event 9190 puts 9001 on a player only when they are multiplayer and host, so the guest keeps the normal restriction and the other +48/+4A/+4B reasons can still block the lamp. I originally wrote that this got the host prompt back but the live cross map session proved that was wrong. Andrews was the host, 9001 was true, my hook kept reporting applied and +48/+49/+4A/+4B were all zero afterward, but neither client had a lantern prompt. So +49 is a real multiplayer availability byte and clearing it works, it just is not the whole prompt restriction and there is another action selection or display gate later. Common event 7200 still shows the later travel event itself allows a multiplayer host once the action can actually be started. OnEvent_BonfireRespawn callback 0x0138CA20 only stores its event object in globals and 0x019C9B20 forReturnToSummonedCoordPosAng only registers the parameter name, neither one is the runtime warp.

The SosSignMan string does exist in the executable. I havent assigned every SOS function to that class. Need more vtable/xref proof.

All byte patches check the original bytes before writing. If the signature isnt right the patch stops instead of treating the address as universal.

The repeating sceNetEpollAbort stub still returns success, but its message is trace level now. sceNpNotifyPlusFeature is also trace-only because Bloodborne called its zero return stub constantly and filled the log.

More complete/clean notes are in bloodborne-seamless-re.md. The actual current trace names and patch bytes in src/core/bloodborne_re.cpp matter more than this file if I forget to update it.

This file was generated with ChatGPT from my RE notes, disassembly and runtime captures. I asked for it to stay like a scratchpad instead of reading like finished documentation.
