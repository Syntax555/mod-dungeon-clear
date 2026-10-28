/* GNU Affero General Public License v3.0 or later; see LICENSE. */
#ifndef DC_EARNED_PROGRESSION_H
#define DC_EARNED_PROGRESSION_H
#include "Define.h"
class Player;
// IndividualProgression.h and PlayerbotAI.h both declare an unscoped GENERAL
// enumerator, so the coordinator reads earned progression through this unit.
namespace DcEarnedProgression
{
    bool StrictDefault();
    // The highest rewarded Individual Progression marker stage.
    uint8 Stage(Player* player);
    // Returns 0 (Vanilla), 1 (TBC), 2 (Wrath), or 255 for unsupported progression.
    uint8 Expansion(Player* player);
    // Individual Progression's map entry gates, checked without side effects.
    bool CanEnterMap(Player* player, uint32 mapId, float x, float y, float z);
}
#endif
