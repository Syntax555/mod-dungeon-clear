/* GNU Affero General Public License v3.0 or later; see LICENSE. */
#ifndef DC_WAILING_CAVERNS_H
#define DC_WAILING_CAVERNS_H
#include "Define.h"

class Player;
class AiObjectContext;
struct EventStep;
enum class StepResult : uint8;

namespace DcWailingCaverns
{
    StepResult RunLeg(Player* bot, AiObjectContext* context, EventStep const& step);
    bool FollowRoute(Player* member, Player* leader);
    StepResult Walk(Player* bot, AiObjectContext* context, float x, float y, float z, float radius);
}
#endif
