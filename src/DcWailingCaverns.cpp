/* GNU Affero General Public License v3.0 or later; see LICENSE. */
#include "DcWailingCaverns.h"
#include "DcEarnedPlay.h"
#include "Ai/Dungeon/DungeonClear/DcValueKeys.h"
#include "Ai/Dungeon/DungeonClear/Data/DungeonEventRegistry.h"
#include "Ai/Dungeon/DungeonClear/Util/DcMovement.h"
#include "Ai/Dungeon/DungeonClear/Util/DcRun.h"
#include "Ai/Dungeon/DungeonClear/Util/DungeonEventExecutor.h"
#include "AiObjectContext.h"
#include "Group.h"
#include "ObjectAccessor.h"
#include "PathGenerator.h"
#include "Player.h"
#include "Playerbots.h"
#include <cmath>

namespace DcWailingCaverns
{
    StepResult Walk(Player* bot, AiObjectContext* context, float x, float y, float z, float radius)
    {
        if (!bot || !context || bot->GetMapId() != 43 || !GET_PLAYERBOT_AI(bot))
            return StepResult::Failed;
        if (!DcRun::Of(context).enabled || DcRun::Of(context).paused)
            return StepResult::Running;
        PlayerbotAI* ai = GET_PLAYERBOT_AI(bot);
        if (!bot->IsInWorld() || !bot->IsAlive() || bot->IsInCombat() || bot->IsInFlight() ||
            bot->IsBeingTeleported() || bot->m_mover != bot || bot->GetVehicle() || bot->GetTransport() ||
            bot->IsNonMeleeSpellCast(false) || bot->GetLootGUID() || !ai->CanMove() ||
            !DcMovement::DcMovementAllowed(ai))
            return StepResult::Running;

        auto const failed = [&]()
        {
            context->GetValue<std::string&>(DcKey::StallReason)->Get() =
                "Wailing Caverns route is unsafe or unreachable; no movement shortcut is permitted.";
            return StepResult::Failed;
        };
        if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z) ||
            !std::isfinite(radius) || radius <= 0.0f)
            return failed();
        if (bot->GetExactDist(x, y, z) <= radius)
        {
            DcMovement::StopBot(bot, DcMovement::Stop::Hold);
            return StepResult::Done;
        }
        if (bot->isMoving())
            return StepResult::Running;

        PathGenerator path(bot);
        if (!path.CalculatePath(x, y, z, false) || !DcEarnedPlay::UsesRealPath(path) ||
            !(path.GetPathType() & PATHFIND_NORMAL) ||
            (path.GetPathType() & (PATHFIND_INCOMPLETE | PATHFIND_SHORT)) ||
            (path.GetPath().back() - G3D::Vector3(x, y, z)).magnitude() > 1.0f)
            return failed();

        Movement::PointsArray points = path.GetPath();
        // Native normalization can shift the starting height. Validate the
        // first actual movement segment from the bot's current position too.
        points.front() = G3D::Vector3(bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ());
        if (!DcEarnedPlay::RealSpline(bot, points))
            return failed();
        DcMovement::SplinePath(ai, points);
        return StepResult::Running;
    }

    StepResult RunLeg(Player* bot, AiObjectContext* context, EventStep const& step)
    {
        StepResult const result = Walk(bot, context, step.x, step.y, step.z, step.radius);
        if (result != StepResult::Done)
            return result;
        // These steps own movement, so the ordinary advance action's party
        // gates are inactive. Every member must physically finish this leg.
        if (Group* group = bot->GetGroup())
            for (auto const& slot : group->GetMemberSlots())
            {
                Player* member = ObjectAccessor::FindConnectedPlayer(slot.guid);
                if (!member || !member->IsInWorld() || !member->IsAlive() ||
                    !member->IsInMap(bot) || !member->InSamePhase(bot) ||
                    member->IsInCombat() || member->IsInFlight() || member->IsBeingTeleported() ||
                    member->m_mover != member || member->GetVehicle() || member->GetTransport() ||
                    member->IsNonMeleeSpellCast(false) || member->GetLootGUID() ||
                    member->GetExactDist(step.x, step.y, step.z) > step.radius)
                    return StepResult::Running;
            }
        return StepResult::Done;
    }

    bool FollowRoute(Player* member, Player* leader)
    {
        if (!member || !leader || leader->GetMapId() != 43 || !member->IsInWorld() ||
            !member->IsInMap(leader) || !member->InSamePhase(leader))
            return false;
        PlayerbotAI* ai = GET_PLAYERBOT_AI(leader);
        AiObjectContext* context = ai ? ai->GetAiObjectContext() : nullptr;
        if (!context || !DcRun::Of(context).enabled || DcRun::Of(context).paused ||
            !DungeonEventExecutor::IsPersistentAnchoredEventActive(context))
            return false;
        auto const& progress = context->GetValue<DungeonEventProgress&>(DcKey::EventProgress)->Get();
        if (progress.eventId != 1 && progress.eventId != 2 && progress.eventId != 3)
            return false;
        DungeonEvent const* event = DungeonEventRegistry::Find(43, progress.eventId);
        if (!event || progress.stepIndex >= event->steps.size())
            return false;
        EventStep const& step = event->steps[progress.stepIndex];
        if (step.kind != EventStepKind::MoveTo)
            return false;

        StepResult const result = Walk(member, context, step.x, step.y, step.z, step.radius);
        if (result == StepResult::Failed || result == StepResult::Blocked)
        {
            DcRun::Of(context).paused = true;
            DcRun::Of(context).pauseReason = "No real navigation-mesh route for a Wailing Caverns party member.";
            context->GetValue<std::string&>(DcKey::StallReason)->Get() = DcRun::Of(context).pauseReason;
            DcMovement::StopBot(member, DcMovement::Stop::Hold);
            DcMovement::StopBot(leader, DcMovement::Stop::Hold);
        }
        return true;
    }

}
