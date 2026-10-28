/* GNU Affero General Public License v3.0 or later; see LICENSE. */
#include "DcRingOfLaw.h"
#include "DcEarnedPlay.h"
#include "Ai/Dungeon/DungeonClear/DcValueKeys.h"
#include "Ai/Dungeon/DungeonClear/Overrides/ObjectiveHookRegistry.h"
#include "Ai/Dungeon/DungeonClear/Util/DcMovement.h"
#include "Ai/Dungeon/DungeonClear/Util/DcRun.h"
#include "Ai/Dungeon/DungeonClear/Util/DungeonEventExecutor.h"
#include "AiObjectContext.h"
#include "Group.h"
#include "InstanceScript.h"
#include "MotionMaster.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Opcodes.h"
#include "Player.h"
#include "Playerbots.h"
#include "WorldPacket.h"
#include "WorldSession.h"

namespace
{
    constexpr uint32 RingState = 1; // TYPE_RING_OF_LAW
    constexpr uint32 RingTrigger = 1526;
    constexpr float FloorZ = -53.9f;

    bool Gather(Player* bot, AreaTrigger const* trigger)
    {
        PlayerbotAI* ai = GET_PLAYERBOT_AI(bot);
        if (!ai || !bot->IsAlive() || bot->IsInCombat() || bot->IsInFlight() ||
            bot->IsBeingTeleported() || bot->m_mover != bot ||
            bot->IsNonMeleeSpellCast(false) || bot->GetLootGUID() ||
            !DcMovement::DcMovementAllowed(ai))
            return false;

        if (bot->IsInAreaTriggerRadius(trigger))
        {
            DcMovement::StopBot(bot, DcMovement::Stop::Hold);
            return true;
        }
        // The trigger centre is above the arena floor. Walk on the mesh;
        // only the native volume test, never a movement order, proves arrival.
        if (!bot->isMoving() && ai->CanMove() &&
            DcEarnedPlay::ReachableLeg(bot, trigger->x, trigger->y, FloorZ))
            bot->GetMotionMaster()->MovePoint(0, trigger->x, trigger->y, FloorZ,
                                             FORCED_MOVEMENT_NONE, 0.0f, 0.0f,
                                             /*generatePath*/ true, false);
        return false;
    }
}

namespace DcRingOfLaw
{
    bool IsActive(Player* leader)
    {
        if (!leader || leader->GetMapId() != 230 || !leader->IsInWorld())
            return false;
        PlayerbotAI* ai = GET_PLAYERBOT_AI(leader);
        AiObjectContext* context = ai ? ai->GetAiObjectContext() : nullptr;
        return context && DcRun::Of(context).enabled && !DcRun::Of(context).paused &&
               DungeonEventExecutor::IsPersistentAnchoredEventActive(context) &&
               context->GetValue<DungeonEventProgress&>(DcKey::EventProgress)->Get().eventId == 1;
    }

    ObjectiveArriveResult Run(Player* bot)
    {
        if (!bot || bot->GetMapId() != 230 || !bot->IsInWorld() ||
            !GET_PLAYERBOT_AI(bot) || !bot->GetSession())
            return ObjectiveArriveResult::Blocked;
        // A queued objective can outlive dc off, which clears the pause flag.
        AiObjectContext* context = GET_PLAYERBOT_AI(bot)->GetAiObjectContext();
        if (!context || !DcRun::Of(context).enabled || DcRun::Of(context).paused)
            return ObjectiveArriveResult::Running;
        InstanceScript* instance = bot->GetInstanceScript();
        AreaTrigger const* trigger = sObjectMgr->GetAreaTrigger(RingTrigger);
        if (!instance || !trigger || trigger->map != 230)
            return ObjectiveArriveResult::Blocked;

        uint32 const state = instance->GetData(RingState);
        // This is an EncounterState, not a monotonic phase counter. Grimstone
        // alone finishes it, after the boss dies and his final waypoint runs.
        if (state == DONE)
            return ObjectiveArriveResult::Done;
        if (state != NOT_STARTED && state != IN_PROGRESS)
            return ObjectiveArriveResult::Running;
        if (!Gather(bot, trigger) || state == IN_PROGRESS)
            return ObjectiveArriveResult::Running;

        // Closing the entrance on a trailing member would strand it. Followers
        // gather through their own action; humans must walk in themselves.
        if (Group* group = bot->GetGroup())
            for (auto const& slot : group->GetMemberSlots())
            {
                Player* member = ObjectAccessor::FindConnectedPlayer(slot.guid);
                if (!member || !member->IsInWorld() || !member->IsAlive() ||
                    !member->IsInMap(bot) || !member->InSamePhase(bot) ||
                    member->IsInFlight() || member->IsBeingTeleported() ||
                    !member->IsInAreaTriggerRadius(trigger))
                    return ObjectiveArriveResult::Running;
            }

        // Reattempt after an interrupted run or native reset. The core owns
        // its post-wipe cooldown, summons, gates, combat and encounter credit.
        WorldPacket packet(CMSG_AREATRIGGER);
        packet << uint32(RingTrigger);
        bot->GetSession()->HandleAreaTriggerOpcode(packet);
        return ObjectiveArriveResult::Running;
    }

    bool Follow(Player* member, Player* leader)
    {
        if (!member || !IsActive(leader) ||
            !member->IsInWorld() || !member->IsInMap(leader) || !member->InSamePhase(leader))
            return false;
        InstanceScript* instance = leader->GetInstanceScript();
        AreaTrigger const* trigger = sObjectMgr->GetAreaTrigger(RingTrigger);
        if (!instance || instance->GetData(RingState) != NOT_STARTED || !trigger || trigger->map != 230)
            return false;

        Gather(member, trigger);
        return true; // The ordinary formation must not pull us out of the volume.
    }
}
