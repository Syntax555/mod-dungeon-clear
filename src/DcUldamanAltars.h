/* GNU Affero General Public License v3.0 or later; see LICENSE. */
#ifndef DC_ULDAMAN_ALTARS_H
#define DC_ULDAMAN_ALTARS_H
#include "Define.h"
class Player;
enum class StepResult : uint8;

namespace DcUldamanAltars
{
    StepResult Run(Player* bot, uint32 altarEntry);
}
#endif
