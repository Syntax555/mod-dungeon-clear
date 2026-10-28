/* GNU Affero General Public License v3.0 or later; see LICENSE. */
#ifndef EARNED_DUNGEON_PARTIES_H
#define EARNED_DUNGEON_PARTIES_H
#include "Define.h"
class Player;
class Event;
namespace EarnedDungeonParties
{
    void LoadConfig();
    void Update(uint32 diff);
    bool AuthorizeInternalStart(Player* bot, Event const& event);
    bool RouteEnabled(uint32 mapId);
}
#endif
