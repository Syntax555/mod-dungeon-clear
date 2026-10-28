/* GNU Affero General Public License v3.0 or later; see LICENSE. */
#include "DcUldamanAltars.h"
#include "DcEarnedEvents.h"
#include "DcEarnedPlay.h"
#include "Ai/Dungeon/DungeonClear/Util/DungeonEventExecutor.h"
#include "ChatHelper.h"
#include "Creature.h"
#include "Event.h"
#include "GameObject.h"
#include "InstanceScript.h"
#include "MotionMaster.h"
#include "Player.h"
#include "Playerbots.h"
#include "UseItemAction.h"

#include <list>

namespace DcUldamanAltars
{
    StepResult Run(Player* bot, uint32 altarEntry)
    {
        if (!bot || bot->GetMapId() != 70 || !GET_PLAYERBOT_AI(bot) ||
            (altarEntry != 130511 && altarEntry != 133234))
            return StepResult::Failed;

        InstanceScript* instance = bot->GetInstanceScript();
        if (!instance)
            return StepResult::Blocked;
        bool const keepers = altarEntry == 130511;
        uint32 const state = instance->GetData(keepers ? 1 : 2);
        if (state == DONE)
            return StepResult::Done;
        if (!keepers && state == IN_PROGRESS)
            return StepResult::Running;

        // The keepers do not set IN_PROGRESS. Any awake keeper means their
        // death chain already owns the event; another ritual could wake more.
        std::list<Creature*> creatures;
        bot->GetCreatureListWithEntryInGrid(creatures, keepers ? 4857 : 2748, 100.0f);
        bool present = false;
        for (Creature* creature : creatures)
        {
            if (!creature->IsAlive() || !creature->IsInMap(bot) || !creature->InSamePhase(bot))
                continue;
            present = true;
            if (!creature->HasAura(10255))
                return StepResult::Running;
        }
        if (!present)
            return StepResult::Running;

        if (!bot->IsAlive() || bot->IsInCombat() || bot->IsInFlight() || bot->m_mover != bot ||
            bot->IsNonMeleeSpellCast(false) || bot->GetLootGUID())
            return StepResult::Running;

        GameObject* altar = bot->FindNearestGameObject(altarEntry, 100.0f);
        if (!altar || !altar->isSpawned())
            return StepResult::Running;
        if (altar->GetGoType() != GAMEOBJECT_TYPE_SUMMONING_RITUAL ||
            sPlayerbotAIConfig.DisallowedGameObjects.contains(altarEntry) ||
            !DcEarnedEvents::CanOpen(bot, altar))
            return StepResult::Blocked;
        // The core waits five seconds and removes interrupted channels itself.
        // Neither the participant count nor a successful click proves completion.
        if (altar->getLootState() != GO_READY)
            return StepResult::Running;

        if (!altar->IsWithinDistInMap(bot) ||
            !DungeonEventExecutor::HasGameObjectLos(bot, altar))
        {
            if (!bot->isMoving() && GET_PLAYERBOT_AI(bot)->CanMove() &&
                DcEarnedPlay::ReachableLeg(bot, altar->GetPositionX(), altar->GetPositionY(), altar->GetPositionZ()))
                bot->GetMotionMaster()->MovePoint(0, altar->GetPositionX(), altar->GetPositionY(),
                                                 altar->GetPositionZ(), FORCED_MOVEMENT_NONE, 0.0f, 0.0f,
                                                 /*generatePath*/ true, false);
            return StepResult::Running;
        }

        if (bot->isMoving())
        {
            bot->StopMoving();
            return StepResult::Running;
        }

        UseItemAction(GET_PLAYERBOT_AI(bot)).Execute(Event("use", ChatHelper::FormatGameobject(altar)));
        // Hold this objective through the fight, so a native reset retries the
        // ritual instead of leaving a completed altar anchor before a stoned boss.
        return StepResult::Running;
    }
}
