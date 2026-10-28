/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "Ai/Dungeon/DungeonClear/Data/Events/DungeonEventTables.h"
#include "Ai/Dungeon/DungeonClear/Data/Events/DungeonRosterBuilders.h"

// The cannon objective gates the ship after Gilnid. Its ordinary item-use
// driver loots the gunpowder chest if needed, walks back and uses the carried
// powder. Only the native door state completes the persistent event.

void RegisterDeadminesEvents(std::vector<DungeonEvent>& out)
{
    constexpr uint32 DM_GO_IRON_CLAD_DOOR = 16397;
    constexpr uint32 DM_DOOR_OPEN_STATE   = 2;  // GO_STATE_ACTIVE_ALTERNATIVE
    constexpr uint32 DM_GO_CANNON = 16398;
    constexpr uint32 DM_ITEM_GUNPOWDER = 5397;
    constexpr uint32 DM_SPELL_FIRE_CANNON = 6250;

    out.push_back(
        EventBuilder(36, 1, "Iron Clad Door (Defias Cannon)")
            .Anchored(/*encounterIndex*/ 3)
            .Persistent()
            .StepsOwnMovement()
            .CompleteWhenGOState(DM_GO_IRON_CLAD_DOOR, DM_DOOR_OPEN_STATE, 100.0f)
            .MoveTo(-107.56f, -659.67f, 7.21f, /*radius*/ 5.0f)
            .UseItemOnGO(DM_ITEM_GUNPOWDER, DM_SPELL_FIRE_CANNON, DM_GO_CANNON,
                         -107.56f, -659.67f, 7.21f)
            .Timeout(120000)
            .WaitForGOState(DM_GO_IRON_CLAD_DOOR, DM_DOOR_OPEN_STATE)
            .Timeout(120000)
            .Build());
}

// --- roster patch (relocated from BossRosterRegistry) --------------------
void RegisterDeadminesRoster(std::vector<BossRosterPatch>& t)
{
    using namespace DcRoster;

    // --- The Deadmines (map 36) — Defias Cannon door ------------------
    // The Iron Clad Door (GO 16397, lock 202 — rogue-pick only) seals the
    // foundry from the pirate ship; a bot party can never click it open.
    // Add the Defias Cannon (GO 16398) as an OBJECTIVE anchor so boss-nav
    // drives the tank to it between Gilnid (bit 2, last foundry boss) and
    // Mr. Smite (bit 3, first ship boss); the event (eventId 1) fires the
    // cannon, opening the door to the whole ship (Smite 3, Cookie 4,
    // Greenskin 5, VanCleef 6). encounterIndex 3 is shared with Mr. Smite:
    // the objective-before-boss tie-break + the picker's strictly-greater
    // advance hand the objective over first, then the boss. No gateEntry
    // (the event owns completion via the door-open gate; see
    // DeadminesEvents).
    //
    // --- Sneed (bit 1) — anchored on Sneed's Shredder ---------------------
    // instance_encounters 162 credits KILL_CREATURE 643 (Sneed), but 643 has
    // no `creature` row: he only appears when Sneed's Shredder (642, guid
    // 79223) dies and its SmartAI casts Eject Sneed (5141). BossSpawnIndex
    // walks spawns, so the derived roster is Rhahk'zor, Gilnid, Smite,
    // Cookie, Greenskin, VanCleef — Sneed is silently missing, and nothing
    // routes the party into the Mast Room (Sneed's death opens Mast Room
    // Door 16400). Same shape as Gundrak's Drakkari Colossus: anchor on the
    // entry that HAS a spawn (the Shredder) and give it the missing
    // encounter's real bit with MakeBossWithBit. Completion rides the mask
    // as usual once Sneed himself dies. The remove keeps a roster that
    // already derives 642 (a fork crediting the Shredder for this bit) from
    // listing it twice.
    {
        BossRosterPatch p;
        p.mapId = 36;
        p.remove = { 642 };
        p.add = {
            MakeBossWithBit(642, 36, "Sneed's Shredder", -289.453f, -513.009f, 49.6785f,
                            /*encounterIndex*/ 1),
            MakeObjective(OBJ(1), /*encounterIndex*/ 3, 36, "Iron Clad Door",
                          -107.56f, -659.67f, 7.21f, /*arriveRadius*/ 10.0f,
                          /*gateEntry*/ 0, /*hook*/ 0, /*eventId*/ 1),
        };
        t.push_back(std::move(p));
    }
}
