/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "Ai/Dungeon/DungeonClear/Data/Events/DungeonEventTables.h"
#include "Ai/Dungeon/DungeonClear/Data/Events/DungeonRosterBuilders.h"

void RegisterWailingCavernsEvents(std::vector<DungeonEvent>& out)
{
    // Native patrol 381170 joins Anacondra's platform to the upper caverns.
    // Backtrack over that ramp instead of crossing either disconnected drop.
    // Every leg must have a complete mesh path, including its original floor;
    // the patrol data alone cannot prove connectivity in the installed mmaps.
    out.push_back(
        EventBuilder(43, 1, "Walk to Lord Serpentis")
            .Anchored(5)
            .Persistent()
            .StepsOwnMovement()
            .MoveTo(-206.648f, 58.6654f, -49.4123f, 2.0f).Timeout(120000)
            .MoveTo(-187.331f, 56.9268f, -49.9537f, 2.0f).Timeout(120000)
            .MoveTo(-169.269f, 54.0309f, -40.636f, 2.0f).Timeout(120000)
            .MoveTo(-147.96f, 31.9855f, -29.7619f, 2.0f).Timeout(120000)
            .MoveTo(-120.163f, -24.624f, -28.5832f, 2.0f).Timeout(120000)
            .Build());

    out.push_back(
        EventBuilder(43, 3, "Walk back to the lower caverns")
            .Anchored(7)
            .Persistent()
            .StepsOwnMovement()
            .MoveTo(-147.96f, 31.9855f, -29.7619f, 2.0f).Timeout(120000)
            .MoveTo(-169.269f, 54.0309f, -40.636f, 2.0f).Timeout(120000)
            .MoveTo(-187.331f, 56.9268f, -49.9537f, 2.0f).Timeout(120000)
            .MoveTo(-206.648f, 58.6654f, -49.4123f, 2.0f).Timeout(120000)
            .MoveTo(-134.97f, 125.40f, -78.09f, 2.0f).Timeout(120000)
            .Build());

    // The native SmartAI owns the walk, ambushes, banish channel, ritual,
    // Mutanus and Naralex's awakening. A finite budget also covers a dead,
    // missing or reset escort; no phase is advanced by the clear driver.
    out.push_back(
        EventBuilder(43, 2, "Escort the Disciple of Naralex")
            .Anchored(7)
            .Persistent()
            .StepsOwnMovement()
            .MoveTo(-134.97f, 125.40f, -78.09f, 2.0f).Timeout(120000)
            .EscortCreature(3678, 0, 3654, 7).Timeout(900000)
            .Build());
}

void RegisterWailingCavernsRoster(std::vector<BossRosterPatch>& t)
{
    using namespace DcRoster;

    BossRosterPatch p;
    p.mapId = 43;
    DungeonBossInfo mutanus = MakeBoss(3654, 43, "Mutanus the Devourer",
                                       151.27f, 252.26f, -102.82f, 0);
    mutanus.encounterIndex = 7;
    // Stable objective ordering keeps the return walk before the escort, and
    // the escort before Mutanus's boss anchor. Only native kill credit sets bit 7.
    p.add = {
        MakeObjective(OBJ(1), 5, 43, "Walk to Lord Serpentis",
                      -206.648f, 58.6654f, -49.4123f, 6.0f, 0, 0, 1),
        MakeObjective(OBJ(3), 7, 43, "Walk back to the lower caverns",
                      -81.8554f, 32.2565f, -30.9939f, 6.0f, 0, 0, 3),
        MakeObjective(OBJ(2), 7, 43, "Escort the Disciple of Naralex",
                      -134.97f, 125.40f, -78.09f, 18.0f, 0, 0, 2),
        mutanus,
    };
    t.push_back(std::move(p));
}
