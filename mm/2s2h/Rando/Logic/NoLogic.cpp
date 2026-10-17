#include "Logic.h"

extern "C" {
#include "variables.h"
#include "ShipUtils.h"
}

namespace Rando {

namespace Logic {

void ApplyNoLogicToSaveContext(std::vector<RandoCheckId>& checkPool, std::vector<RandoItemId>& itemPool) {
    PreplaceConfinedItems(checkPool, itemPool);
    FillChecksRandomly(checkPool, itemPool);
}

} // namespace Logic

} // namespace Rando
