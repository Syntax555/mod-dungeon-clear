/* GNU Affero General Public License v3.0 or later; see LICENSE. */
#include "DcEarnedProgression.h"
#include "IndividualProgression.h"

namespace DcEarnedProgression
{
    bool StrictDefault()
    {
        return sIndividualProgression->IsStrictDefaultProgression();
    }

    uint8 Stage(Player* player)
    {
        return sIndividualProgression->GetPlayerProgressionFromQuests(player);
    }

    // The character's own rewarded progression markers decide its earned era.
    uint8 Expansion(Player* player)
    {
        return IndividualProgression_GetEarnedEra(player);
    }

    bool CanEnterMap(Player* player, uint32 mapId, float x, float y, float z)
    {
        return sIndividualProgression->CanEnterMap(player, mapId, x, y, z);
    }
}
