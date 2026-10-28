/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license,
 * version 3 or (at your option) any later version.
 */
#ifndef DC_EARNED_EVENTS_H
#define DC_EARNED_EVENTS_H
#include "Define.h"
#include <string>

class GameObject;
class Player;
struct EventStep;

// Scripted dungeon events under natural progression: only the actions an
// ordinary player could perform at that spot. Shortcuts the upstream event
// vocabulary uses (jumps across missing mesh, party teleports, granted quest
// items, triggered spells, escort speed overrides, custom hooks) are refused.
namespace DcEarnedEvents
{
    // Step kinds an earned party may run. Runtime checks still apply: real
    // paths, owned items and the object's own lock.
    bool StepAllowed(EventStep const& step);
    // Only this item-target pair has an audited ordinary acquisition path.
    bool IsDefiasCannonStep(EventStep const& step);
    // Only the native Wailing Caverns escort has an audited ordinary driver.
    bool IsNaralexStep(EventStep const& step);
    // The object can be opened by a click now: no lock, an open lock without a
    // profession, or a key item the bot carries. Lockpicking, spell locks and
    // conditional interaction stay with players.
    bool CanOpen(Player* bot, GameObject* go);
    // Every required objective of the normal five-player map consists of
    // ordinary steps. Autonomous parties start only on audited maps.
    bool MapAudited(uint32 mapId, std::string& reason);
}
#endif
