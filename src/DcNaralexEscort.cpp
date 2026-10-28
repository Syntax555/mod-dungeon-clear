/* GNU Affero General Public License v3.0 or later; see LICENSE. */
#include "DcNaralexEscort.h"
#include "DcEarnedPlay.h"
#include "DcWailingCaverns.h"
#include "Ai/Dungeon/DungeonClear/DcValueKeys.h"
#include "Ai/Dungeon/DungeonClear/Util/DcMovement.h"
#include "Ai/Dungeon/DungeonClear/Util/DcRun.h"
#include "Ai/Dungeon/DungeonClear/Util/DcTargeting.h"
#include "Ai/Dungeon/DungeonClear/Util/DungeonEventExecutor.h"
#include "AiObjectContext.h"
#include "AttackAction.h"
#include "Creature.h"
#include "Event.h"
#include "Group.h"
#include "InstanceScript.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "Playerbots.h"
#include "SmartAI.h"

namespace
{
    constexpr uint32 DiscipleEntry = 3678;
    constexpr uint32 MutanusBit = 1u << 7;

    bool Active(AiObjectContext* context)
    {
        return context && DcRun::Of(context).enabled && !DcRun::Of(context).paused &&
               DungeonEventExecutor::IsPersistentAnchoredEventActive(context) &&
               context->GetValue<DungeonEventProgress&>(DcKey::EventProgress)->Get().eventId == 2;
    }

    Creature* Disciple(Player* bot)
    {
        Creature* disciple = bot->FindNearestCreature(DiscipleEntry, 250.0f, true);
        return disciple && disciple->IsInWorld() && disciple->IsAlive() &&
               disciple->IsInMap(bot) && disciple->InSamePhase(bot) ? disciple : nullptr;
    }

    bool Interrupted(Player* bot)
    {
        return !bot->IsAlive() || bot->IsInCombat() || bot->IsInFlight() || bot->IsBeingTeleported() ||
               bot->m_mover != bot || bot->GetVehicle() || bot->GetTransport() ||
               bot->IsNonMeleeSpellCast(false) || bot->GetLootGUID();
    }

    StepResult Approach(Player* bot, AiObjectContext* context, Creature* disciple, float radius)
    {
        return DcWailingCaverns::Walk(bot, context, disciple->GetPositionX(),
                                      disciple->GetPositionY(), disciple->GetPositionZ(), radius);
    }
}

namespace DcNaralexEscort
{
    StepResult Run(Player* bot, AiObjectContext* context)
    {
        if (!bot || bot->GetMapId() != 43 || !bot->IsInWorld() || !GET_PLAYERBOT_AI(bot))
            return StepResult::Failed;
        // Queued actions must not start gossip or movement after pause/off/skip.
        if (!Active(context))
            return StepResult::Running;

        InstanceScript* instance = bot->GetInstanceScript();
        if (!instance)
            return StepResult::Blocked;
        for (uint32 type = 0; type < 4; ++type)
            if (instance->GetData(type) != DONE)
                return StepResult::Blocked;

        Creature* disciple = Disciple(bot);
        SmartAI* escortAI = disciple ? dynamic_cast<SmartAI*>(disciple->AI()) : nullptr;
        if (!escortAI)
            return StepResult::Blocked;
        // Mutanus spawning or dying does not finish Naralex's awakening. His
        // native action list sends this marker after its final dialogue and
        // starts the departure path. Never infer success from a missing NPC.
        if ((instance->GetCompletedEncounterMask() & MutanusBit) &&
            escortAI->GetData(2) == 2 && escortAI->GetScript()->GetPathId() == 3679)
            return StepResult::Done;

        if (Interrupted(bot))
            return StepResult::Running;

        if (!escortAI->IsEscorted())
        {
            if (disciple->GetFaction() != 35 || disciple->isMoving() || !bot->GetSession())
                return StepResult::Blocked;
            StepResult const approach = Approach(bot, context, disciple, 4.0f);
            if (approach != StepResult::Done)
                return approach;
            if (!bot->IsWithinLOSInMap(disciple))
                return StepResult::Blocked;

            // Starting with an absent or trailing member risks the native
            // escort's range reset. Followers gather through their own action.
            if (Group* group = bot->GetGroup())
                for (auto const& slot : group->GetMemberSlots())
                {
                    Player* member = ObjectAccessor::FindConnectedPlayer(slot.guid);
                    if (!member || !member->IsInWorld() || !member->IsInMap(bot) ||
                        !member->InSamePhase(bot) || Interrupted(member) ||
                        member->GetExactDist(disciple) > 15.0f || !member->IsWithinLOSInMap(disciple))
                        return StepResult::Running;
                }

            DungeonEventExecutor::SelectGossip(bot, disciple, 0);
            // A sent packet is no receipt: the core may refuse the menu option.
            return escortAI->IsEscorted() && disciple->GetFaction() == 250 &&
                   escortAI->GetScript()->GetPathId() == DiscipleEntry
                       ? StepResult::Running : StepResult::Blocked;
        }

        if (escortAI->GetScript()->GetPathId() != DiscipleEntry || disciple->GetFaction() != 250)
            return StepResult::Blocked;

        Unit* threat = nullptr;
        for (Unit* attacker : disciple->getAttackers())
            if (attacker && attacker->IsInWorld() && attacker->IsAlive() &&
                attacker->IsInMap(bot) && attacker->InSamePhase(bot) && bot->IsValidAttackTarget(attacker) &&
                (!attacker->ToCreature() || !attacker->ToCreature()->IsInEvadeMode()))
            {
                threat = attacker;
                break;
            }
        if (!threat)
            threat = DcTargeting::NearestHostileNearPoint(bot, context,
                disciple->GetPositionX(), disciple->GetPositionY(), disciple->GetPositionZ(), 18.0f, 10.0f);
        if (threat)
        {
            if (!threat->IsInWorld() || !threat->IsAlive() || !threat->IsInMap(bot) ||
                !threat->InSamePhase(bot) || !bot->IsValidAttackTarget(threat) ||
                (threat->ToCreature() && threat->ToCreature()->IsInEvadeMode()))
                return StepResult::Blocked;
            StepResult const approach = DcWailingCaverns::Walk(bot, context, threat->GetPositionX(),
                threat->GetPositionY(), threat->GetPositionZ(), 3.0f);
            if (approach == StepResult::Done)
            {
                if (!bot->IsWithinLOSInMap(threat))
                    return StepResult::Blocked;
                context->GetValue<Unit*>(DcKey::Stock::CurrentTarget)->Set(threat);
                MeleeAction(GET_PLAYERBOT_AI(bot)).Execute(Event());
                return StepResult::Running;
            }
            return approach;
        }

        StepResult const follow = Approach(bot, context, disciple, 6.0f);
        return follow == StepResult::Done ? StepResult::Running : follow;
    }

    bool Follow(Player* member, Player* leader)
    {
        if (!member || !leader || leader->GetMapId() != 43 ||
            !member->IsInWorld() || !member->IsInMap(leader) || !member->InSamePhase(leader))
            return false;
        PlayerbotAI* ai = GET_PLAYERBOT_AI(leader);
        AiObjectContext* context = ai ? ai->GetAiObjectContext() : nullptr;
        if (!Active(context))
            return false;

        Creature* disciple = Disciple(member);
        SmartAI* escortAI = disciple ? dynamic_cast<SmartAI*>(disciple->AI()) : nullptr;
        if (!escortAI || escortAI->GetData(2) == 2)
            return true;
        StepResult const result = Approach(member, context, disciple, 8.0f);
        if (result == StepResult::Failed || result == StepResult::Blocked)
        {
            DcRun::Of(context).paused = true;
            DcRun::Of(context).pauseReason = "No real navigation-mesh route for a Naralex escort party member.";
            context->GetValue<std::string&>(DcKey::StallReason)->Get() = DcRun::Of(context).pauseReason;
            DcMovement::StopBot(member, DcMovement::Stop::Hold);
            DcMovement::StopBot(leader, DcMovement::Stop::Hold);
        }
        return true;
    }
}
