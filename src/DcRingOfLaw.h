/* GNU Affero General Public License v3.0 or later; see LICENSE. */
#ifndef DC_RING_OF_LAW_H
#define DC_RING_OF_LAW_H

class Player;
enum class ObjectiveArriveResult;

namespace DcRingOfLaw
{
    bool IsActive(Player* leader);
    ObjectiveArriveResult Run(Player* bot);
    bool Follow(Player* member, Player* leader);
}
#endif
