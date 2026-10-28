/* GNU Affero General Public License v3.0 or later; see LICENSE. */
#include "DcDefiasCannon.h"
#include "DcEarnedEvents.h"
#include "DcEarnedPlay.h"
#include "Ai/Dungeon/DungeonClear/Util/DcFormGate.h"
#include "Ai/Dungeon/DungeonClear/Util/DungeonEventExecutor.h"
#include "GameObject.h"
#include "MotionMaster.h"
#include "Player.h"
#include "Playerbots.h"
#include "Spell.h"
#include "UseItemAction.h"
#include "WorldPacket.h"
#include "WorldSession.h"

namespace DcDefiasCannon
{
    StepResult Run(Player* bot)
    {
        if (!bot || bot->GetMapId() != 36 || !GET_PLAYERBOT_AI(bot))
            return StepResult::Failed;

        // Do not fire again on re-entry, after combat or after consumption.
        GameObject* door = bot->FindNearestGameObject(16397, 100.0f);
        if (door && door->isSpawned() && door->GetGoState() == GO_STATE_ACTIVE_ALTERNATIVE)
            return StepResult::Done;

        if (!bot->IsAlive() || bot->IsInCombat() || bot->IsInFlight() || bot->m_mover != bot ||
            bot->IsNonMeleeSpellCast(false) || bot->GetLootGUID())
            return StepResult::Running;

        Item* powder = bot->GetItemByEntry(5397);
        GameObject* target = bot->FindNearestGameObject(powder ? 16398 : 17155, 100.0f);
        if (!target || !target->isSpawned())
            return StepResult::Running;
        if (sPlayerbotAIConfig.DisallowedGameObjects.contains(target->GetEntry()) ||
            !DcEarnedEvents::CanOpen(bot, target))
            return StepResult::Blocked;

        if (bot->GetExactDist(target) > 3.0f || !target->IsWithinDistInMap(bot) ||
            !DungeonEventExecutor::HasGameObjectLos(bot, target))
        {
            if (!bot->isMoving() && GET_PLAYERBOT_AI(bot)->CanMove() &&
                DcEarnedPlay::ReachableLeg(bot, target->GetPositionX(), target->GetPositionY(), target->GetPositionZ()))
                bot->GetMotionMaster()->MovePoint(0, target->GetPositionX(), target->GetPositionY(),
                                                 target->GetPositionZ(), FORCED_MOVEMENT_NONE, 0.0f, 0.0f,
                                                 /*generatePath*/ true, false);
            return StepResult::Running;
        }

        if (bot->isMoving())
        {
            bot->StopMoving();
            return StepResult::Running;
        }

        if (powder)
        {
            DcFormGate::DropBlockingForm(bot, powder);
            UseItemAction(GET_PLAYERBOT_AI(bot)).UseItemOnGameObject(powder, target->GetGUID());
        }
        else
        {
            // A chest needs its opening spell, not GameObject::Use. Ask the
            // core for the player's own opening action and send the normal
            // opcode: lock, spell, loot eligibility and bag checks stay native.
            // StoreLootAction takes the powder as ITEM_CLASS_KEY, then releases
            // the chest, including its native looted event and ambush.
            SpellInfo const* opening = target->GetSpellForLock(bot);
            if (!opening)
                return StepResult::Blocked;
            if (opening->CheckShapeshift(uint32(bot->GetShapeshiftForm())) != SPELL_CAST_OK)
                bot->RemoveAurasByType(SPELL_AURA_MOD_SHAPESHIFT);
            SpellCastTargets targets;
            targets.SetGOTarget(target);
            WorldPacket packet(CMSG_CAST_SPELL);
            packet << uint8(1) << opening->Id << uint8(0);
            targets.Write(packet);
            bot->GetSession()->HandleCastSpellOpcode(packet);
        }
        return StepResult::Running;
    }
}
