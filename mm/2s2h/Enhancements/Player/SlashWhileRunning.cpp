#include <libultraship/bridge/consolevariablebridge.h>
#include "2s2h/GameInteractor/GameInteractor.h"
#include "2s2h/ShipInit.hpp"

extern "C" {
#include "variables.h"

// Mirrors AttackAnimInfo in z_player.c
struct MeleeAttackAnimInfo {
    void* anim;
    void* animEnd;
    void* animEndTargeted;
    u8 hitStartFrame;
    u8 hitEndFrame;
};

extern MeleeAttackAnimInfo sMeleeAttackAnimInfo[];
extern PlayerUpperActionFunc sItemActionUpdateFuncs[];

void Player_SetUpperAction(PlayState* play, Player* player, PlayerUpperActionFunc upperActionFunc);
void func_8082DC38(Player* player);
void func_8082FA5C(PlayState* play, Player* player, PlayerMeleeWeaponState meleeWeaponState);
void func_8083375C(Player* player, PlayerMeleeWeaponAnimation meleeWeaponAnim);
s32 func_808401F4(PlayState* play, Player* player);
}

#define CVAR_NAME "gEnhancements.Player.SlashWhileRunning"
#define CVAR CVarGetInteger(CVAR_NAME, 0)

// Below this Link is walking/idling rather than running, so the normal slash is used.
#define MIN_SLASH_SPEED 3.6f

// The finisher of each slash is always two entries after it in sMeleeAttackAnimInfo (e.g. RIGHT_SLASH_1H ->
// RIGHT_COMBO_1H, FORWARD_SLASH_2H -> FORWARD_COMBO_2H).
#define COMBO_ANIM_OFFSET 2

// Frames used to blend the upper body back into the run cycle after the recovery animation.
#define FADE_OUT_FRAMES 5
static s32 sFadeOutFrame = 0;

static void RunningSlash_Finish(Player* player, PlayState* play) {
    func_8082DC38(player);
    Player_SetUpperAction(play, player, sItemActionUpdateFuncs[player->heldItemAction]);
    player->unk_ACC = 0;
}

// Fades the upper body from the last recovery pose back into the run cycle, so the torso doesn't snap forward.
static s32 RunningSlash_FadeOutUpperAction(Player* player, PlayState* play) {
    // Hold the final recovery pose.
    PlayerAnimation_Update(play, &player->skelAnimeUpper);
    sFadeOutFrame++;

    if (sFadeOutFrame >= FADE_OUT_FRAMES) {
        RunningSlash_Finish(player, play);
        return false; // nothing left to copy, the run cycle is fully in control
    }

    // Player_UpdateUpperBody steps this toward 0 by 0.25 and then blends in the upper body by (1 - weight), so the
    // 0.25 offset makes the upper body's share fall linearly from 1 to 0.
    player->skelAnimeUpperBlendWeight = static_cast<f32>(sFadeOutFrame) / FADE_OUT_FRAMES + 0.25f;
    return true;
}

static void RunningSlash_StartFadeOut(Player* player) {
    sFadeOutFrame = 0;
    player->upperActionFunc = RunningSlash_FadeOutUpperAction;
}

// Plays the animation that lowers the sword arm after a swing, like the idle recovery of a standing slash.
static s32 RunningSlash_RecoveryUpperAction(Player* player, PlayState* play) {
    if (PlayerAnimation_Update(play, &player->skelAnimeUpper)) {
        RunningSlash_StartFadeOut(player);
    }
    return true;
}

static s32 RunningSlash_UpperAction(Player* player, PlayState* play) {
    MeleeAttackAnimInfo* info = &sMeleeAttackAnimInfo[player->meleeWeaponAnimation];
    f32 curFrame = player->skelAnimeUpper.curFrame;

    // Vanilla's sword hit handler: clanging off walls, stick and Razor Sword wear, and hit reactions.
    bool handled = func_808401F4(play, player);

    // A weapon breaking (Deku Stick, Razor Sword) swaps the held item. That is only queued here and carried out at
    // the start of the next frame, when this action is replaced without ever running again. Turn the hitbox off
    // before bailing out, or it would stay on ("infinite sword glitch").
    if ((player->stateFlags3 & PLAYER_STATE3_START_CHANGING_HELD_ITEM) ||
        player->upperActionFunc != RunningSlash_UpperAction) {
        func_8082DC38(player);
        return false;
    }

    if (handled) {
        RunningSlash_Finish(player, play);
        return false;
    }

    if (PlayerAnimation_Update(play, &player->skelAnimeUpper)) {
        void* recoveryAnim = Player_CheckHostileLockOn(player) ? info->animEndTargeted : info->animEnd;

        func_8082DC38(player);
        if (recoveryAnim == nullptr) {
            RunningSlash_StartFadeOut(player);
        } else {
            // Set directly instead of Player_SetUpperAction, which would cut off the swing's voice.
            player->upperActionFunc = RunningSlash_RecoveryUpperAction;
            PlayerAnimation_PlayOnce(play, &player->skelAnimeUpper, static_cast<PlayerAnimationHeader*>(recoveryAnim));
        }
        return true;
    }

    // Same hitbox timing as func_8083FCF0, but driven by the upper body animation.
    if (curFrame > info->hitEndFrame) {
        func_8082DC38(player);
    } else if (curFrame >= 0.0f) {
        player->stateFlags3 |= PLAYER_STATE3_2000000;
        func_8082FA5C(play, player,
                      (curFrame >= info->hitStartFrame) ? PLAYER_MELEE_WEAPON_STATE_1
                                                        : PLAYER_MELEE_WEAPON_STATE_MINUS_1);
    }

    return true;
}

static bool RunningSlash_CanStart(Player* player, PlayerMeleeWeaponAnimation meleeWeaponAnim) {
    PlayerMeleeWeapon weapon = Player_GetMeleeWeaponHeld(player);

    if (player->transformation != PLAYER_FORM_HUMAN) {
        return false;
    }
    if (weapon != PLAYER_MELEEWEAPON_SWORD_KOKIRI && weapon != PLAYER_MELEEWEAPON_SWORD_RAZOR &&
        weapon != PLAYER_MELEEWEAPON_SWORD_GILDED && weapon != PLAYER_MELEEWEAPON_SWORD_TWO_HANDED &&
        weapon != PLAYER_MELEEWEAPON_DEKU_STICK) {
        return false;
    }
    // Only plain slashes (either the one-handed or two-handed version); spin attacks and stabs keep their vanilla
    // behavior.
    switch (meleeWeaponAnim) {
        case PLAYER_MWA_FORWARD_SLASH_1H:
        case PLAYER_MWA_FORWARD_SLASH_2H:
        case PLAYER_MWA_RIGHT_SLASH_1H:
        case PLAYER_MWA_RIGHT_SLASH_2H:
        case PLAYER_MWA_LEFT_SLASH_1H:
        case PLAYER_MWA_LEFT_SLASH_2H:
            break;
        default:
            return false;
    }
    if (!(player->actor.bgCheckFlags & BGCHECKFLAG_GROUND) || (player->stateFlags1 & PLAYER_STATE1_8000000)) {
        return false;
    }
    return player->speedXZ >= MIN_SLASH_SPEED;
}

// The slash to play. Swords swing horizontally; the Deku Stick keeps the vertical slash vanilla gives it. The
// two-handed variants are already accounted for in the animation vanilla chose, and by the item held.
static PlayerMeleeWeaponAnimation RunningSlash_GetSlashAnim(Player* player, PlayerMeleeWeaponAnimation vanillaAnim) {
    if (player->heldItemAction == PLAYER_IA_DEKU_STICK) {
        return vanillaAnim;
    }
    return Player_IsHoldingTwoHandedWeapon(player) ? PLAYER_MWA_RIGHT_SLASH_2H : PLAYER_MWA_RIGHT_SLASH_1H;
}

static bool RunningSlash_IsActive(Player* player) {
    return player->upperActionFunc == RunningSlash_UpperAction ||
           player->upperActionFunc == RunningSlash_RecoveryUpperAction ||
           player->upperActionFunc == RunningSlash_FadeOutUpperAction;
}

void RegisterSlashWhileRunning() {
    // If Link stops mid-swing, vanilla would start overriding the whole body, including the root position, with the
    // slash animation. That makes the model jump forward with the finisher. Keep it to the upper body instead.
    COND_VB_SHOULD(VB_COPY_UPPER_BODY_LIMBS_ONLY, CVAR, {
        Player* player = va_arg(args, Player*);
        if (RunningSlash_IsActive(player)) {
            *should = true;
        }
    });

    // Using or swapping to another item mid-swing would replace the swing's upper body action before it can end the
    // sword's hitbox, leaving the hitbox stuck on (the "infinite sword glitch"), so ignore items until it's over.
    COND_VB_SHOULD(VB_PROCESS_ITEM_BUTTONS, CVAR, {
        Player* player = va_arg(args, Player*);
        if (player->upperActionFunc == RunningSlash_UpperAction) {
            *should = false;
        }
    });

    COND_VB_SHOULD(VB_START_RUNNING_SLASH, CVAR, {
        Player* player = va_arg(args, Player*);
        PlayState* play = va_arg(args, PlayState*);
        PlayerMeleeWeaponAnimation meleeWeaponAnim = static_cast<PlayerMeleeWeaponAnimation>(va_arg(args, int));

        // func_808335F4 arms a stored forward lunge (PLAYER_STATE2_40000000) when running forward. Vanilla consumes it
        // in Player_Action_84, which never runs for a running slash, so clear it or the next standing slash lunges.
        player->stateFlags2 &= ~PLAYER_STATE2_40000000;

        // Ignore extra presses while a running slash is already in progress.
        if (player->upperActionFunc == RunningSlash_UpperAction) {
            *should = true;
            return;
        }

        if (!RunningSlash_CanStart(player, meleeWeaponAnim)) {
            return;
        }

        PlayerMeleeWeaponAnimation baseAnim = RunningSlash_GetSlashAnim(player, meleeWeaponAnim);
        func_8083375C(player, baseAnim); // sets the weapon's damage

        // Same combo counting as func_80833864: the third consecutive slash is the finisher.
        PlayerMeleeWeaponAnimation slashAnim = baseAnim;
        if (player->meleeWeaponAnimation != slashAnim || player->unk_ADD >= 3) {
            player->unk_ADD = 0;
        }
        player->unk_ADD++;
        if (player->unk_ADD >= 3) {
            slashAnim = static_cast<PlayerMeleeWeaponAnimation>(baseAnim + COMBO_ANIM_OFFSET);
        }
        player->meleeWeaponAnimation = slashAnim;

        Player_SetUpperAction(play, player, RunningSlash_UpperAction);
        // Same speed vanilla plays its slashes at, so the frames the hitbox is keyed to last as long.
        PlayerAnimation_PlayOnceSetSpeed(play, &player->skelAnimeUpper,
                                         static_cast<PlayerAnimationHeader*>(sMeleeAttackAnimInfo[slashAnim].anim),
                                         PLAYER_ANIM_ADJUSTED_SPEED);

        // Time window for chaining the next slash: until the swing ends, plus a few frames. Negative like vanilla's
        // "B released" state, so holding B doesn't turn into a full-body spin attack charge.
        player->unk_ADC = -static_cast<s32>(player->skelAnimeUpper.animLength / PLAYER_ANIM_ADJUSTED_SPEED + 4.0f);

        *should = true;
    });
}

static RegisterShipInitFunc initFunc(RegisterSlashWhileRunning, { CVAR_NAME });
