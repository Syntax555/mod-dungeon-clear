/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license,
 * version 3 or (at your option) any later version.
 */
#include "DcEarnedEvents.h"

#include "Ai/Dungeon/DungeonClear/Data/BossSpawnIndex.h"
#include "Ai/Dungeon/DungeonClear/Data/DungeonEventRegistry.h"
#include "Ai/Dungeon/DungeonClear/Overrides/BossRosterRegistry.h"
#include "Ai/Dungeon/DungeonClear/Util/DcDifficulty.h"
#include "DBCStores.h"
#include "GameObject.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "SharedDefines.h"

namespace
{
    enum class Opening { Closed, Click, Key };

    // The native lock check matches the opening spell's lock type before
    // checking professions. Special opening/closing and blasting spells are
    // not a plain click, even when SkillByLockType returns SKILL_NONE.
    Opening LockOpening(uint32 lockId, Player* bot)
    {
        if (!lockId)
            return Opening::Click;
        LockEntry const* lock = sLockStore.LookupEntry(lockId);
        if (!lock)
            return Opening::Closed;
        bool click = false;
        for (uint8 i = 0; i < MAX_LOCK_CASE; ++i)
        {
            if (lock->Type[i] == LOCK_KEY_ITEM && lock->Index[i] && bot && bot->HasItemCount(lock->Index[i], 1))
                return Opening::Key;
            if (lock->Type[i] == LOCK_KEY_SKILL && lock->Index[i] == LOCKTYPE_OPEN)
                click = true;
        }
        return click ? Opening::Click : Opening::Closed;
    }

    bool StepsAudited(DungeonEvent const& ev, std::string& reason)
    {
        for (EventStep const& step : ev.steps)
        {
            bool const ring = ev.mapId == 230 && ev.id == 1 && step.hookId == 1;
            bool const zumrah = ev.mapId == 209 && ev.id == 3 && step.hookId == 5;
            if (step.kind == EventStepKind::Custom && !ring && !zumrah)
            {
                reason = "event '" + ev.name + "' has no audited controller";
                return false;
            }
            if (step.kind == EventStepKind::EscortCreature && (ev.mapId != 43 || ev.id != 2))
            {
                reason = "event '" + ev.name + "' has no audited escort";
                return false;
            }
            bool const cannon = DcEarnedEvents::IsDefiasCannonStep(step);
            if (!DcEarnedEvents::StepAllowed(step) || (!cannon && (step.kind == EventStepKind::UseItem || step.itemId)))
            {
                reason = "event '" + ev.name + "' requires a scripted shortcut or unaudited item acquisition";
                return false;
            }
            if (cannon)
            {
                GameObjectTemplate const* chest = sObjectMgr->GetGameObjectTemplate(17155);
                GameObjectTemplate const* target = sObjectMgr->GetGameObjectTemplate(16398);
                if (!chest || !target || LockOpening(chest->GetLockId(), nullptr) != Opening::Click ||
                    target->GetLockId() != 83)
                {
                    reason = "Defias Cannon requires its ordinary gunpowder chest and item lock";
                    return false;
                }
            }
            if (step.kind != EventStepKind::UseGameObject)
                continue;
            GameObjectTemplate const* go = sObjectMgr->GetGameObjectTemplate(step.goEntry);
            if (!go || LockOpening(go->GetLockId(), nullptr) != Opening::Click)
            {
                reason = "event '" + ev.name + "' requires a key or locked object";
                return false;
            }
        }
        return true;
    }
}

namespace DcEarnedEvents
{
    bool IsDefiasCannonStep(EventStep const& step)
    {
        return step.kind == EventStepKind::UseItemOnGO && step.goEntry == 16398 &&
               step.itemId == 5397 && step.spellId == 6250 && step.hookId == 0;
    }

    bool IsNaralexStep(EventStep const& step)
    {
        return step.kind == EventStepKind::EscortCreature && step.creatureEntry == 3678 &&
               step.gossipOption == 0 && step.escortDoneEntry == 3654 && step.escortDoneBit == 7 &&
               step.hookId == 0 && step.instanceDataId < 0;
    }

    bool StepAllowed(EventStep const& step)
    {
        switch (step.kind)
        {
            case EventStepKind::Custom:
                return step.hookId == 1 || step.hookId == 5; // Audited native spatial triggers.
            case EventStepKind::MoveTo:
                return step.hookId == 0;
            case EventStepKind::UseItemOnGO:
                return IsDefiasCannonStep(step);
            case EventStepKind::EscortCreature:
                return IsNaralexStep(step);
            case EventStepKind::UseGameObject:
            case EventStepKind::Gossip:
            case EventStepKind::WaitForSpawn:
            case EventStepKind::WaitForGameObjectState:
            case EventStepKind::KillCreature:
            case EventStepKind::ClearRadius:
            case EventStepKind::Wait:
            case EventStepKind::UseItem:
                return true;
            default:
                return false;
        }
    }

    bool CanOpen(Player* bot, GameObject* go)
    {
        if (!bot || !go || go->HasGameObjectFlag(GO_FLAG_NOT_SELECTABLE) ||
            (go->HasGameObjectFlag(GO_FLAG_INTERACT_COND) && !go->ActivateToQuest(bot)))
            return false;
        Opening const opening = LockOpening(go->GetGOInfo()->GetLockId(), bot);
        // A locked object opens only through its key, never by a click.
        return go->HasGameObjectFlag(GO_FLAG_LOCKED) ? opening == Opening::Key : opening != Opening::Closed;
    }

    bool MapAudited(uint32 mapId, std::string& reason)
    {
        // BFD's native party chains cross unlinked mesh with authored jumps and swims.
        if (mapId == 48)
        {
            reason = "Blackfathom Deeps requires unaudited movement across disconnected navigation mesh";
            return false;
        }
        DcDiffKey const normal{false, DUNGEON_DIFFICULTY_NORMAL};
        // Optional encounter controllers can still own the only movement,
        // gossip or pull that advances required progress gates. Refusing
        // that controller must refuse autonomous recruitment as well.
        auto const essential = [](DungeonEvent const& ev)
        { return ev.required || ev.ownsThePull || ev.stepsOwnMovement; };
        // Event objectives are added by the per-dungeon roster corrections.
        // Audit the roster a running clear uses, not the bare encounter index.
        for (DungeonBossInfo const& info : BossRosterRegistry::Apply(
                 mapId, normal, BossSpawnIndex::Get(mapId, DUNGEON_DIFFICULTY_NORMAL)))
        {
            if (info.eventId)
            {
                DungeonEvent const* ev = DungeonEventRegistry::Find(mapId, info.eventId);
                if (!ev)
                {
                    reason = "objective '" + info.name + "' has no event data";
                    return false;
                }
                if (essential(*ev) && DcGateMatches(ev->gate, normal) && !StepsAudited(*ev, reason))
                    return false;
            }
            else if (info.onArriveHook)
            {
                reason = "objective '" + info.name + "' requires a scripted hook";
                return false;
            }
        }
        for (DungeonEvent const* ev : DungeonEventRegistry::Conditional(mapId, normal))
            if (essential(*ev) && !StepsAudited(*ev, reason))
                return false;
        return true;
    }
}
