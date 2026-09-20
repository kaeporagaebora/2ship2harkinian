#include <algorithm>
#include <libultraship/bridge/consolevariablebridge.h>
#include "2s2h/GameInteractor/GameInteractor.h"
#include "2s2h/ShipInit.hpp"

#define CVAR_NAME "gEnhancements.Player.UnsheatheWithoutSlashing"
#define CVAR CVarGetInteger(CVAR_NAME, 0)
#define CVAR_SPEED_NAME "gEnhancements.Player.UnsheatheSpeed"
// The slider's minimum, also enforced here in case a lower value was saved earlier.
#define MIN_DRAW_SPEED 0.5f
#define CVAR_SPEED std::max(CVarGetFloat(CVAR_SPEED_NAME, 1.0f), MIN_DRAW_SPEED)

void RegisterUnsheatheWithoutSlashing() {
    COND_VB_SHOULD(VB_USE_HELD_ITEM_AFTER_CHANGE, CVAR, {
        Player* player = va_arg(args, Player*);
        ItemId heldItemId = static_cast<ItemId>(player->heldItemId);
        if ((heldItemId == ITEM_SWORD_KOKIRI) || (heldItemId == ITEM_SWORD_RAZOR) ||
            (heldItemId == ITEM_SWORD_GILDED)) {
            *should = false;
        }
    });

    // Slows down drawing the sword, which is otherwise done very quickly.
    COND_VB_SHOULD(VB_SET_HELD_ITEM_CHANGE_SPEED, CVAR && CVAR_SPEED < 1.0f, {
        va_arg(args, Player*);
        s32 heldItemAction = va_arg(args, s32);
        f32* speed = va_arg(args, f32*);

        // Only when drawing a sword, not when putting an item away (then the new item action is PLAYER_IA_NONE). The
        // speed is negative when the animation plays backwards, which vanilla does for some draws, so scale it
        // as is to keep its direction.
        if (heldItemAction == PLAYER_IA_SWORD_KOKIRI || heldItemAction == PLAYER_IA_SWORD_RAZOR ||
            heldItemAction == PLAYER_IA_SWORD_GILDED) {
            *speed *= CVAR_SPEED;
        }
    });
}

static RegisterShipInitFunc initFunc(RegisterUnsheatheWithoutSlashing, { CVAR_NAME, CVAR_SPEED_NAME });
