/* GNU Affero General Public License v3.0 or later; see LICENSE. */
#ifndef DC_DEFIAS_CANNON_H
#define DC_DEFIAS_CANNON_H
#include "Define.h"
class Player;
enum class StepResult : uint8;

namespace DcDefiasCannon
{
    StepResult Run(Player* bot);
}
#endif
