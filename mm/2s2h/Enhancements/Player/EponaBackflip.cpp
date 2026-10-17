#include <libultraship/bridge/consolevariablebridge.h>
#include "2s2h/GameInteractor/GameInteractor.h"
#include "2s2h/ShipInit.hpp"

extern "C" {
#include "variables.h"
#include "overlays/actors/ovl_En_Horse/z_en_horse.h"

extern Input* sPlayerControlInput;
extern s32 gHorseIsMounted;

void func_80839860(Player* player, PlayState* play, s32 controlStickDirection); // Link's hop, or backflip
void EnHorse_StartBraking(EnHorse* horse, PlayState* play);
bool func_8082DA90(PlayState* play);
}

#define CVAR_NAME "gEnhancements.Player.EponaBackflip"
#define CVAR CVarGetInteger(CVAR_NAME, 0)

// At this speed or more the horse counts as moving, and slows to a stop when Link backflips off of it.
#define MIN_HORSE_SPEED 1.0f

// The horse Link just backflipped off of while it was moving. While it is set, that horse slows to a stop instead of
// stopping at once.
static EnHorse* sBackflipHorse = nullptr;

static bool EponaBackflip_IsHorseMoving(EnHorse* horse) {
    return horse->actor.speed >= MIN_HORSE_SPEED &&
           (horse->action == ENHORSE_ACTION_MOUNTED_WALK || horse->action == ENHORSE_ACTION_MOUNTED_TROT ||
            horse->action == ENHORSE_ACTION_MOUNTED_GALLOP);
}

static bool EponaBackflip_CanStart(Player* player, PlayState* play) {
    EnHorse* horse = reinterpret_cast<EnHorse*>(player->rideActor);

    if (horse == nullptr || !(player->stateFlags1 & PLAYER_STATE1_800000)) {
        return false;
    }
    // R held, A pressed
    if (!CHECK_BTN_ALL(sPlayerControlInput->cur.button, BTN_R) ||
        !CHECK_BTN_ALL(sPlayerControlInput->press.button, BTN_A)) {
        return false;
    }
    // Only an ordinary horse that is either moving, or standing still and ready to be dismounted (the same check
    // vanilla uses for getting off)
    if (horse->actor.params != ENHORSE_0 || (!EponaBackflip_IsHorseMoving(horse) && !EN_HORSE_CHECK_4(horse))) {
        return false;
    }
    // Not while still climbing on, aiming, in a cutscene, or otherwise busy
    if (player->av2.actionVar2 == 0 || (player->stateFlags1 & PLAYER_STATE1_100000) ||
        play->csCtx.state != CS_STATE_IDLE || player->csAction != PLAYER_CSACTION_NONE || func_8082DA90(play)) {
        return false;
    }
    return true;
}

// Gets Link off the horse without the slow, stationary dismount and makes him backflip away with the same animation
// and jump that vanilla uses for a backflip on foot.
static void EponaBackflip_Start(Player* player, PlayState* play) {
    EnHorse* horse = reinterpret_cast<EnHorse*>(player->rideActor);

    // Same as the end of the vanilla dismount (Player_Action_53)
    horse->actor.child = nullptr; // the horse sees that it has no rider
    player->stateFlags1 &= ~PLAYER_STATE1_800000;
    player->actor.parent = nullptr;
    gHorseIsMounted = false;
    Camera_ChangeSetting(Play_GetCamera(play, CAM_ID_MAIN), CAM_SET_NORMAL0);

    if (CHECK_QUEST_ITEM(QUEST_SONG_EPONA) || (DREG(1) != 0)) {
        gSaveContext.save.saveInfo.horseData.sceneId = play->sceneId;
        gSaveContext.save.saveInfo.horseData.pos.x = horse->actor.world.pos.x;
        gSaveContext.save.saveInfo.horseData.pos.y = horse->actor.world.pos.y;
        gSaveContext.save.saveInfo.horseData.pos.z = horse->actor.world.pos.z;
        gSaveContext.save.saveInfo.horseData.yaw = horse->actor.shape.rot.y;
    }

    // A horse that is standing still just freezes like after any dismount, the braking would look wrong.
    sBackflipHorse = EponaBackflip_IsHorseMoving(horse) ? horse : nullptr;
    func_80839860(player, play, PLAYER_STICK_DIR_BACKWARD);
}

void RegisterEponaBackflip() {
    // While riding, R + A makes Link backflip off the horse.
    COND_VB_SHOULD(VB_START_HORSE_BACKFLIP, CVAR, {
        Player* player = va_arg(args, Player*);
        PlayState* play = va_arg(args, PlayState*);

        if (EponaBackflip_CanStart(player, play)) {
            EponaBackflip_Start(player, play);
            *should = true;
        }
    });

    // A on its own makes the horse dash, don't do that when R is held for the backflip.
    COND_VB_SHOULD(VB_START_HORSE_BOOST, CVAR, {
        if (CHECK_BTN_ALL(sPlayerControlInput->cur.button, BTN_R)) {
            *should = false;
        }
    });

    // Vanilla stops the horse dead as soon as its rider gets off. After a backflip it brakes with its normal
    // skidding stop instead.
    COND_VB_SHOULD(VB_FREEZE_HORSE_WHEN_RIDER_LEAVES, CVAR, {
        EnHorse* horse = va_arg(args, EnHorse*);
        PlayState* play = va_arg(args, PlayState*);

        if (horse == sBackflipHorse) {
            *should = false;
            EnHorse_StartBraking(horse, play);
        }
    });

    // Once it has stopped, the horse without a rider stands still like it does after any dismount, instead of being
    // ready to react to the controller.
    COND_VB_SHOULD(VB_FREEZE_UNRIDDEN_HORSE_AFTER_BRAKING, CVAR, {
        EnHorse* horse = va_arg(args, EnHorse*);

        if (horse == sBackflipHorse) {
            *should = true;
            sBackflipHorse = nullptr;
        }
    });
}

static RegisterShipInitFunc initFunc(RegisterEponaBackflip, { CVAR_NAME });
