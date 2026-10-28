/* GNU Affero General Public License v3.0 or later; see LICENSE. */
#ifndef DC_NARALEX_ESCORT_H
#define DC_NARALEX_ESCORT_H
#include "Define.h"
class Player;
class AiObjectContext;
enum class StepResult : uint8;

namespace DcNaralexEscort
{
    StepResult Run(Player* bot, AiObjectContext* context);
    bool Follow(Player* member, Player* leader);
}
#endif
