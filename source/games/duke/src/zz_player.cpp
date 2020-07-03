//-------------------------------------------------------------------------
/*
Copyright (C) 2010 EDuke32 developers and contributors

This file is part of EDuke32.

EDuke32 is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License version 2
as published by the Free Software Foundation.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
*/
//-------------------------------------------------------------------------
#include "ns.h"	// Must come before everything else!

#include "duke3d.h"
#include "demo.h"
#include "d_event.h"
#include "gamevar.h"

BEGIN_DUKE_NS


int32_t PHEIGHT = PHEIGHT_DUKE;

int32_t lastvisinc;
hudweapon_t hudweap;

extern int32_t g_levelTextTime, ticrandomseed;

int32_t g_numObituaries = 0;
int32_t g_numSelfObituaries = 0;


void P_AddKills(DukePlayer_t * const pPlayer, uint16_t kills)
{
    pPlayer->actors_killed += kills;
}

int P_GetOverheadPal(DukePlayer_t const * pPlayer)
{
    return sprite[pPlayer->i].pal;
}

#define DRAWEAP_CENTER 262144
#define weapsc(sc) scale(sc, hud_weaponscale, 100)

static int32_t g_dts_yadd;


// Set C-CON's WEAPON and WORKSLIKE gamevars.
void P_SetWeaponGamevars(int playerNum, const DukePlayer_t * const pPlayer)
{
    if (!WW2GI)
        return;
    SetGameVarID(g_iWeaponVarID, pPlayer->curr_weapon, pPlayer->i, playerNum);
    SetGameVarID(g_iWorksLikeVarID,
              ((unsigned)pPlayer->curr_weapon < MAX_WEAPONS) ? PWEAPON(playerNum, pPlayer->curr_weapon, WorksLike) : -1,
              pPlayer->i, playerNum);
}


#define TURBOTURNTIME (TICRATE/8) // 7
#define NORMALTURN    15
#define PREAMBLETURN  5
#define NORMALKEYMOVE 40
#define MAXVEL        ((NORMALKEYMOVE*2)+10)
#define MAXSVEL       ((NORMALKEYMOVE*2)+10)
#define MAXANGVEL     1024
#define MAXHORIZVEL   256

#define MOTOTURN      20
#define MAXVELMOTO    120

int32_t g_myAimStat = 0, g_oldAimStat = 0;
int32_t mouseyaxismode = -1;

enum inputlock_t
{
    IL_NOANGLE = 0x1,
    IL_NOHORIZ = 0x2,
    IL_NOMOVE  = 0x4,

    IL_NOTHING = IL_NOANGLE|IL_NOHORIZ|IL_NOMOVE,
};

static int P_CheckLockedMovement(int const playerNum)
{
    auto const pPlayer = g_player[playerNum].ps;

    if (pPlayer->on_crane >= 0)
        return IL_NOMOVE|IL_NOANGLE;

    if (pPlayer->newowner != -1)
        return IL_NOANGLE|IL_NOHORIZ;

    if (pPlayer->curr_weapon > 11) return 0;

    if (pPlayer->dead_flag || pPlayer->fist_incs || pPlayer->transporter_hold > 2 || pPlayer->hard_landing || pPlayer->access_incs > 0
        || pPlayer->knee_incs > 0
        || (PWEAPON(playerNum, pPlayer->curr_weapon, WorksLike) == TRIPBOMB_WEAPON && pPlayer->kickback_pic > 1
            && pPlayer->kickback_pic < PWEAPON(playerNum, pPlayer->curr_weapon, FireDelay)))
        return IL_NOTHING;

    return 0;
}

void P_GetInput(int const playerNum)
{
    auto      &thisPlayer = g_player[playerNum];
    auto const pPlayer    = thisPlayer.ps;
    ControlInfo info;

    if ((pPlayer->gm & (MODE_MENU|MODE_TYPE)) || (ud.pause_on && !inputState.GetKeyStatus(sc_Pause)))
    {
        if (!(pPlayer->gm&MODE_MENU))
            CONTROL_GetInput(&info);

        localInput = {};
        localInput.bits    = (((int32_t)g_gameQuit) << SK_GAMEQUIT);
        localInput.extbits |= (1 << 7);

        return;
    }

	D_ProcessEvents();

	bool mouseaim = in_mousemode || buttonMap.ButtonDown(gamefunc_Mouse_Aiming);

    if (numplayers == 1)
    {
        pPlayer->aim_mode = in_mousemode;
        pPlayer->auto_aim = cl_autoaim;
        pPlayer->weaponswitch = cl_weaponswitch;
    }


    CONTROL_GetInput(&info);


    // JBF: Run key behaviour is selectable
	
	int const     playerRunning    = G_CheckAutorun(buttonMap.ButtonDown(gamefunc_Run));
    int const     turnAmount       = playerRunning ? (NORMALTURN << 1) : NORMALTURN;
    constexpr int analogTurnAmount = (NORMALTURN << 1);
    int const     keyMove          = playerRunning ? (NORMALKEYMOVE << 1) : NORMALKEYMOVE;
    constexpr int analogExtent     = 32767; // KEEPINSYNC sdlayer.cpp

    input_t input {};

    if (buttonMap.ButtonDown(gamefunc_Strafe))
    {
        static int strafeyaw;

        input.svel = -(info.mousex + strafeyaw) >> 3;
        strafeyaw  = (info.mousex + strafeyaw) % 8;

        input.svel -= info.dyaw * keyMove / analogExtent;
    }
    else
    {
        input.q16avel = fix16_sadd(input.q16avel, fix16_sdiv(fix16_from_int(info.mousex), F16(32)));
        input.q16avel = fix16_sadd(input.q16avel, fix16_from_int(info.dyaw / analogExtent * (analogTurnAmount << 1)));
    }

    if (mouseaim)
        input.q16horz = fix16_sadd(input.q16horz, fix16_sdiv(fix16_from_int(info.mousey), F16(64)));
    else
        input.fvel = -(info.mousey >> 3);

    if (!in_mouseflip) input.q16horz = -input.q16horz;

    input.q16horz = fix16_ssub(input.q16horz, fix16_from_int(info.dpitch * analogTurnAmount / analogExtent));
    input.svel -= info.dx * keyMove / analogExtent;
    input.fvel -= info.dz * keyMove / analogExtent;

    static double lastInputTicks;
    auto const    currentHiTicks    = timerGetHiTicks();
    double const  elapsedInputTicks = currentHiTicks - lastInputTicks;

    lastInputTicks = currentHiTicks;

    auto scaleAdjustmentToInterval = [=](double x) { return x * REALGAMETICSPERSEC / (1000.0 / elapsedInputTicks); };

    if (buttonMap.ButtonDown(gamefunc_Strafe))
    {
        if (!localInput.svel)
        {
            if (buttonMap.ButtonDown(gamefunc_Turn_Left) && !(pPlayer->movement_lock & 4) && !localInput.svel)
                input.svel = keyMove;

            if (buttonMap.ButtonDown(gamefunc_Turn_Right) && !(pPlayer->movement_lock & 8) && !localInput.svel)
                input.svel = -keyMove;
        }
    }
    else
    {
        static int32_t turnHeldTime;
        static int32_t lastInputClock;  // MED
        int32_t const  elapsedTics = (int32_t)totalclock - lastInputClock;

        lastInputClock = (int32_t) totalclock;

        if (buttonMap.ButtonDown(gamefunc_Turn_Left))
        {
            turnHeldTime += elapsedTics;
            input.q16avel = fix16_ssub(input.q16avel, fix16_from_dbl(scaleAdjustmentToInterval((turnHeldTime >= TURBOTURNTIME) ? (turnAmount << 1) : (PREAMBLETURN << 1))));
        }
        else if (buttonMap.ButtonDown(gamefunc_Turn_Right))
        {
            turnHeldTime += elapsedTics;
            input.q16avel = fix16_sadd(input.q16avel, fix16_from_dbl(scaleAdjustmentToInterval((turnHeldTime >= TURBOTURNTIME) ? (turnAmount << 1) : (PREAMBLETURN << 1))));
        }
        else
            turnHeldTime = 0;
    }

    if (localInput.svel < keyMove && localInput.svel > -keyMove)
    {
        if (buttonMap.ButtonDown(gamefunc_Strafe_Left) && !(pPlayer->movement_lock & 4))
            input.svel += keyMove;

        if (buttonMap.ButtonDown(gamefunc_Strafe_Right) && !(pPlayer->movement_lock & 8))
            input.svel += -keyMove;
    }

    if (localInput.fvel < keyMove && localInput.fvel > -keyMove)
    {
        if (RR)
        {
            /*if (buttonMap.ButtonDown(gamefunc_Quick_Kick))
            {
                localInput.bits |= buttonMap.ButtonDown(gamefunc_Move_Forward)<<SK_AIM_UP;
                localInput.bits |= buttonMap.ButtonDown(gamefunc_Move_Backward)<<SK_AIM_DOWN;
            }
            else*/
            {
                if (pPlayer->drink_amt >= 66 && pPlayer->drink_amt <= 87)
                {
                    if (buttonMap.ButtonDown(gamefunc_Move_Forward))
                    {
                        input.fvel += keyMove;
                        if (pPlayer->drink_amt & 1)
                            input.svel += keyMove;
                        else
                            input.svel -= keyMove;
                    }

                    if (buttonMap.ButtonDown(gamefunc_Move_Backward))
                    {
                        input.fvel += -keyMove;
                        if (pPlayer->drink_amt & 1)
                            input.svel -= keyMove;
                        else
                            input.svel += keyMove;
                    }
                }
                else
                {
                    if (buttonMap.ButtonDown(gamefunc_Move_Forward))
                        input.fvel += keyMove;

                    if (buttonMap.ButtonDown(gamefunc_Move_Backward))
                        input.fvel += -keyMove;
                }
            }
        }
        else
        {
            if (buttonMap.ButtonDown(gamefunc_Move_Forward) && !(pPlayer->movement_lock & 1))
                input.fvel += keyMove;

            if (buttonMap.ButtonDown(gamefunc_Move_Backward) && !(pPlayer->movement_lock & 2))
                input.fvel += -keyMove;
        }
    }

    int weaponSelection;

    for (weaponSelection = gamefunc_Weapon_10; weaponSelection >= gamefunc_Weapon_1; --weaponSelection)
    {
        if (buttonMap.ButtonDown(weaponSelection))
        {
            weaponSelection -= (gamefunc_Weapon_1 - 1);
            break;
        }
    }

    if (buttonMap.ButtonDown(gamefunc_Last_Weapon))
        weaponSelection = 14;
    else if (buttonMap.ButtonDown(gamefunc_Alt_Weapon))
        weaponSelection = 13;
    else if (buttonMap.ButtonPressed(gamefunc_Next_Weapon) || (buttonMap.ButtonDown(gamefunc_Dpad_Select) && input.fvel > 0))
    {
        weaponSelection = 12;
        buttonMap.ClearButton(gamefunc_Next_Weapon);
    }
    else if (buttonMap.ButtonPressed(gamefunc_Previous_Weapon) || (buttonMap.ButtonDown(gamefunc_Dpad_Select) && input.fvel < 0))
    {
        weaponSelection = 11;
        buttonMap.ClearButton(gamefunc_Previous_Weapon);
    }
    else if (weaponSelection == gamefunc_Weapon_1-1)
        weaponSelection = 0;

    if ((localInput.bits & 0xf00) == 0)
        localInput.bits |= (weaponSelection << SK_WEAPON_BITS);

    localInput.bits |= (buttonMap.ButtonDown(gamefunc_Fire) << SK_FIRE);
    localInput.bits |= (buttonMap.ButtonDown(gamefunc_Open) << SK_OPEN);

    int const sectorLotag = pPlayer->cursectnum != -1 ? sector[pPlayer->cursectnum].lotag : 0;
    int const crouchable = sectorLotag != 2 && (sectorLotag != 1 || pPlayer->spritebridge);

    if (buttonMap.ButtonDown(gamefunc_Toggle_Crouch))
    {
        pPlayer->crouch_toggle = !pPlayer->crouch_toggle && crouchable;

        if (crouchable)
            buttonMap.ClearButton(gamefunc_Toggle_Crouch);
    }

    if (buttonMap.ButtonDown(gamefunc_Crouch) || buttonMap.ButtonDown(gamefunc_Jump) || pPlayer->jetpack_on || (!crouchable && pPlayer->on_ground))
        pPlayer->crouch_toggle = 0;

    int const crouching = buttonMap.ButtonDown(gamefunc_Crouch) || buttonMap.ButtonDown(gamefunc_Toggle_Crouch) || pPlayer->crouch_toggle;

    localInput.bits |= (buttonMap.ButtonDown(gamefunc_Jump) << SK_JUMP) | (crouching << SK_CROUCH);

    localInput.bits |= (buttonMap.ButtonDown(gamefunc_Aim_Up) || (buttonMap.ButtonDown(gamefunc_Dpad_Aiming) && input.fvel > 0)) << SK_AIM_UP;
    localInput.bits |= (buttonMap.ButtonDown(gamefunc_Aim_Down) || (buttonMap.ButtonDown(gamefunc_Dpad_Aiming) && input.fvel < 0)) << SK_AIM_DOWN;
    localInput.bits |= (buttonMap.ButtonDown(gamefunc_Center_View) << SK_CENTER_VIEW);

    localInput.bits |= (buttonMap.ButtonDown(gamefunc_Look_Left) << SK_LOOK_LEFT) | (buttonMap.ButtonDown(gamefunc_Look_Right) << SK_LOOK_RIGHT);
    localInput.bits |= (buttonMap.ButtonDown(gamefunc_Look_Up) << SK_LOOK_UP) | (buttonMap.ButtonDown(gamefunc_Look_Down) << SK_LOOK_DOWN);

    localInput.bits |= (playerRunning << SK_RUN);

    localInput.bits |= (buttonMap.ButtonDown(gamefunc_Inventory_Left) || (buttonMap.ButtonDown(gamefunc_Dpad_Select) && (input.svel > 0 || input.q16avel < 0))) << SK_INV_LEFT;
    localInput.bits |= (buttonMap.ButtonDown(gamefunc_Inventory_Right) || (buttonMap.ButtonDown(gamefunc_Dpad_Select) && (input.svel < 0 || input.q16avel > 0))) << SK_INV_RIGHT;
    localInput.bits |= (buttonMap.ButtonDown(gamefunc_Inventory) << SK_INVENTORY);

    localInput.bits |= (buttonMap.ButtonDown(gamefunc_Steroids) << SK_STEROIDS) | (buttonMap.ButtonDown(gamefunc_NightVision) << SK_NIGHTVISION);
    localInput.bits |= (buttonMap.ButtonDown(gamefunc_MedKit) << SK_MEDKIT) | (buttonMap.ButtonDown(gamefunc_Holo_Duke) << SK_HOLODUKE);
    localInput.bits |= (buttonMap.ButtonDown(gamefunc_Jetpack) << SK_JETPACK);

    localInput.bits |= buttonMap.ButtonDown(gamefunc_Holster_Weapon) << SK_HOLSTER;
    localInput.bits |= buttonMap.ButtonDown(gamefunc_Quick_Kick) << SK_QUICK_KICK;
    localInput.bits |= buttonMap.ButtonDown(gamefunc_TurnAround) << SK_TURNAROUND;

    localInput.bits |= (mouseaim << SK_AIMMODE);
    localInput.bits |= (g_gameQuit << SK_GAMEQUIT);
    localInput.bits |= inputState.GetKeyStatus(sc_Pause) << SK_PAUSE;
    //localInput.bits |= ((uint32_t)inputState.GetKeyStatus(sc_Escape)) << SK_ESCAPE; fixme.This needs to be done differently

    if (RR)
    {
        if (TEST_SYNC_KEY(localInput.bits, SK_CROUCH))
            localInput.bits &= ~(1 << SK_JUMP);
        if (pPlayer->drink_amt > 88)
            localInput.bits |= 1 << SK_LOOK_LEFT;
        if (pPlayer->drink_amt > 99)
            localInput.bits |= 1 << SK_LOOK_DOWN;
    }

    if (buttonMap.ButtonDown(gamefunc_Dpad_Select))
    {
        input.fvel = 0;
        input.svel = 0;
        input.q16avel = 0;
    }
    else if (buttonMap.ButtonDown(gamefunc_Dpad_Aiming))
        input.fvel = 0;

    localInput.extbits |= (buttonMap.ButtonDown(gamefunc_Move_Forward) || (input.fvel > 0));
    localInput.extbits |= (buttonMap.ButtonDown(gamefunc_Move_Backward) || (input.fvel < 0)) << 1;
    localInput.extbits |= (buttonMap.ButtonDown(gamefunc_Strafe_Left) || (input.svel > 0)) << 2;
    localInput.extbits |= (buttonMap.ButtonDown(gamefunc_Strafe_Right) || (input.svel < 0)) << 3;
    localInput.extbits |= buttonMap.ButtonDown(gamefunc_Turn_Left)<<4;
    localInput.extbits |= buttonMap.ButtonDown(gamefunc_Turn_Right)<<5;

    int const movementLocked = P_CheckLockedMovement(playerNum);

    if ((ud.scrollmode && ud.overhead_on) || (movementLocked & IL_NOTHING) == IL_NOTHING)
    {
        if (ud.scrollmode && ud.overhead_on)
        {
            ud.folfvel = input.fvel;
            ud.folavel = fix16_to_int(input.q16avel);
        }

        localInput.fvel = localInput.svel = 0;
        localInput.q16avel = localInput.q16horz = 0;
    }
    else
    {
        if (!(movementLocked & IL_NOMOVE))
        {
            localInput.fvel = clamp(localInput.fvel + input.fvel, -MAXVEL, MAXVEL);
            localInput.svel = clamp(localInput.svel + input.svel, -MAXSVEL, MAXSVEL);
        }

        if (!(movementLocked & IL_NOANGLE))
        {
            localInput.q16avel = fix16_sadd(localInput.q16avel, input.q16avel);
            pPlayer->q16ang    = fix16_sadd(pPlayer->q16ang, input.q16avel) & 0x7FFFFFF;
        }

        if (!(movementLocked & IL_NOHORIZ))
        {
            localInput.q16horz = fix16_clamp(fix16_sadd(localInput.q16horz, input.q16horz), F16(-MAXHORIZVEL), F16(MAXHORIZVEL));
            pPlayer->q16horiz  = fix16_clamp(fix16_sadd(pPlayer->q16horiz, input.q16horz), F16(HORIZ_MIN), F16(HORIZ_MAX));
        }
    }

    // A horiz diff of 128 equal 45 degrees, so we convert horiz to 1024 angle units

    if (thisPlayer.horizAngleAdjust)
    {
        float const horizAngle
        = atan2f(pPlayer->q16horiz - F16(100), F16(128)) * (512.f / fPI) + scaleAdjustmentToInterval(thisPlayer.horizAngleAdjust);
        pPlayer->q16horiz = F16(100) + Blrintf(F16(128) * tanf(horizAngle * (fPI / 512.f)));
    }
    else if (pPlayer->return_to_center > 0 || thisPlayer.horizRecenter)
    {
        pPlayer->q16horiz = fix16_sadd(pPlayer->q16horiz, fix16_from_dbl(scaleAdjustmentToInterval(fix16_to_dbl(fix16_from_dbl(200 / 3) - fix16_sdiv(pPlayer->q16horiz, F16(1.5))))));

        if ((!pPlayer->return_to_center && thisPlayer.horizRecenter) || (pPlayer->q16horiz >= F16(99.9) && pPlayer->q16horiz <= F16(100.1)))
        {
            pPlayer->q16horiz = F16(100);
            thisPlayer.horizRecenter = false;
        }

        if (pPlayer->q16horizoff >= F16(-0.1) && pPlayer->q16horizoff <= F16(0.1))
            pPlayer->q16horizoff = 0;
    }
 
    // calculates automatic view angle for playing without a mouse
    if (!pPlayer->aim_mode && pPlayer->on_ground && sectorLotag != ST_2_UNDERWATER && (sector[pPlayer->cursectnum].floorstat & 2))
    {
        // this is some kind of horse shit approximation of where the player is looking, I guess?
        vec2_t const adjustedPosition = { pPlayer->pos.x + (sintable[(fix16_to_int(pPlayer->q16ang) + 512) & 2047] >> 5),
                                          pPlayer->pos.y + (sintable[fix16_to_int(pPlayer->q16ang) & 2047] >> 5) };
        int16_t currentSector = pPlayer->cursectnum;
 
        updatesector(adjustedPosition.x, adjustedPosition.y, &currentSector);
 
        if (currentSector >= 0)
        {
            int const slopeZ = getflorzofslope(pPlayer->cursectnum, adjustedPosition.x, adjustedPosition.y);
            if ((pPlayer->cursectnum == currentSector) || (klabs(getflorzofslope(currentSector, adjustedPosition.x, adjustedPosition.y) - slopeZ) <= ZOFFSET6))
                pPlayer->q16horizoff = fix16_sadd(pPlayer->q16horizoff, fix16_from_dbl(scaleAdjustmentToInterval(mulscale16(pPlayer->truefz - slopeZ, 160))));
        }
    }
 
    if (pPlayer->q16horizoff > 0)
    {
        pPlayer->q16horizoff = fix16_ssub(pPlayer->q16horizoff, fix16_from_dbl(scaleAdjustmentToInterval(fix16_to_dbl((pPlayer->q16horizoff >> 3) + fix16_one))));
        pPlayer->q16horizoff = fix16_max(pPlayer->q16horizoff, 0);
    }
    else if (pPlayer->q16horizoff < 0)
    {
        pPlayer->q16horizoff = fix16_sadd(pPlayer->q16horizoff, fix16_from_dbl(scaleAdjustmentToInterval(fix16_to_dbl((-pPlayer->q16horizoff >> 3) + fix16_one))));
        pPlayer->q16horizoff = fix16_min(pPlayer->q16horizoff, 0);
    }
 
    if (thisPlayer.horizSkew)
        pPlayer->q16horiz = fix16_sadd(pPlayer->q16horiz, fix16_from_dbl(scaleAdjustmentToInterval(fix16_to_dbl(thisPlayer.horizSkew))));
 
    pPlayer->q16horiz = fix16_clamp(pPlayer->q16horiz, F16(HORIZ_MIN), F16(HORIZ_MAX));
}

void P_GetInputMotorcycle(int playerNum)
{
    auto      &thisPlayer = g_player[playerNum];
    auto const pPlayer    = thisPlayer.ps;
    ControlInfo info;

    if ((pPlayer->gm & (MODE_MENU|MODE_TYPE)) || (ud.pause_on && !inputState.GetKeyStatus(sc_Pause)))
    {
        if (!(pPlayer->gm&MODE_MENU))
            CONTROL_GetInput(&info);

        localInput = {};
        localInput.bits    = (((int32_t)g_gameQuit) << SK_GAMEQUIT);
        localInput.extbits |= (1 << 7);

        return;
    }

    D_ProcessEvents();

	bool mouseaim = in_mousemode || buttonMap.ButtonDown(gamefunc_Mouse_Aiming);

    if (numplayers == 1)
    {
        pPlayer->aim_mode = in_mousemode;
        pPlayer->auto_aim = cl_autoaim;
        pPlayer->weaponswitch = cl_weaponswitch;
    }

    CONTROL_GetInput(&info);

    // JBF: Run key behaviour is selectable
    int const     playerRunning    = G_CheckAutorun(buttonMap.ButtonDown(gamefunc_Run));
    constexpr int analogTurnAmount = (NORMALTURN << 1);
    int const     keyMove          = playerRunning ? (NORMALKEYMOVE << 1) : NORMALKEYMOVE;
    constexpr int analogExtent     = 32767; // KEEPINSYNC sdlayer.cpp

    input_t input {};

    input.q16avel = fix16_sadd(input.q16avel, fix16_sdiv(fix16_from_int(info.mousex), F16(32)));
    input.q16avel = fix16_sadd(input.q16avel, fix16_from_int(info.dyaw / analogExtent * (analogTurnAmount << 1)));

    input.svel -= info.dx * keyMove / analogExtent;
    input.fvel -= info.dz * keyMove / analogExtent;

    static double lastInputTicks;
    auto const    currentHiTicks    = timerGetHiTicks();
    double const  elapsedInputTicks = currentHiTicks - lastInputTicks;

    lastInputTicks = currentHiTicks;

    auto scaleAdjustmentToInterval = [=](double x) { return x * REALGAMETICSPERSEC / (1000.0 / elapsedInputTicks); };

    pPlayer->crouch_toggle = 0;

    localInput.bits |= buttonMap.ButtonDown(gamefunc_Fire) << SK_FIRE;
    localInput.bits |= buttonMap.ButtonDown(gamefunc_Steroids) << SK_STEROIDS;
    localInput.bits |= buttonMap.ButtonDown(gamefunc_NightVision) << SK_NIGHTVISION;
    localInput.bits |= buttonMap.ButtonDown(gamefunc_MedKit) << SK_MEDKIT;
    localInput.bits |= (buttonMap.ButtonDown(gamefunc_Inventory_Left) ||
                 (buttonMap.ButtonDown(gamefunc_Dpad_Select) && (input.svel > 0 || input.q16avel < 0))) << SK_INV_LEFT;
    localInput.bits |= inputState.GetKeyStatus(sc_Pause) << SK_PAUSE;
    localInput.bits |= buttonMap.ButtonDown(gamefunc_Holo_Duke) << SK_HOLODUKE;
    localInput.bits |= buttonMap.ButtonDown(gamefunc_Jetpack) << SK_JETPACK;
    localInput.bits |= (g_gameQuit << SK_GAMEQUIT);
    localInput.bits |= (buttonMap.ButtonDown(gamefunc_Inventory_Right) ||
                 (buttonMap.ButtonDown(gamefunc_Dpad_Select) && (input.svel < 0 || input.q16avel > 0))) << SK_INV_RIGHT;
    localInput.bits |= buttonMap.ButtonDown(gamefunc_Open) << SK_OPEN;
    localInput.bits |= buttonMap.ButtonDown(gamefunc_Inventory) << SK_INVENTORY;
    //localInput.bits |= ((uint32_t)inputState.GetKeyStatus(sc_Escape)) << SK_ESCAPE; fixme.This needs to be done differently

    if (buttonMap.ButtonDown(gamefunc_Dpad_Select))
    {
        input.fvel = 0;
        input.svel = 0;
        input.q16avel = 0;
    }

    if (buttonMap.ButtonDown(gamefunc_Dpad_Aiming))
        input.fvel = 0;
    
    localInput.extbits |= (buttonMap.ButtonDown(gamefunc_Move_Forward) || (input.fvel > 0));
    localInput.extbits |= (buttonMap.ButtonDown(gamefunc_Move_Backward) || (input.fvel < 0)) << 1;
    localInput.extbits |= (buttonMap.ButtonDown(gamefunc_Strafe_Left) || (input.svel > 0)) << 2;
    localInput.extbits |= (buttonMap.ButtonDown(gamefunc_Strafe_Right) || (input.svel < 0)) << 3;

    int turnAmount;
    int const turn = input.q16avel / 32;
    int turnLeft = buttonMap.ButtonDown(gamefunc_Turn_Left) || buttonMap.ButtonDown(gamefunc_Strafe_Left);
    int turnRight = buttonMap.ButtonDown(gamefunc_Turn_Right) || buttonMap.ButtonDown(gamefunc_Strafe_Right);
    int avelScale = F16((turnLeft || turnRight) ? 1 : 0);
    if (turn)
    {
        turnAmount = (MOTOTURN << 1);
        avelScale = fix16_max(avelScale, fix16_clamp(fix16_mul(turn, turn),0,F16(1)));
        if (turn < 0)
            turnLeft = 1;
        else if (turn > 0)
            turnRight = 1;
    }
    else
        turnAmount = MOTOTURN;

    input.svel = input.fvel = input.q16avel = 0;

    localInput.bits |= turnLeft << SK_AIM_DOWN;
    localInput.bits |= turnRight << SK_LOOK_LEFT;

    static int32_t turnHeldTime;
    static int32_t lastInputClock;  // MED
    int32_t const  elapsedTics = (int32_t)totalclock - lastInputClock;

    int const moveBack = buttonMap.ButtonDown(gamefunc_Move_Backward) && pPlayer->MotoSpeed <= 0;

    if (pPlayer->MotoSpeed == 0 || !pPlayer->on_ground)
    {
        if (turnLeft)
        {
            pPlayer->TiltStatus--;
            if (pPlayer->TiltStatus < -10)
                pPlayer->TiltStatus = -10;
        }
        else if (turnRight)
        {
            pPlayer->TiltStatus++;
            if (pPlayer->TiltStatus > 10)
                pPlayer->TiltStatus = 10;
        }
    }
    else
    {
        if (turnLeft || pPlayer->moto_drink < 0)
        {
            turnHeldTime += elapsedTics;
            pPlayer->TiltStatus--;
            if (pPlayer->TiltStatus < -10)
                pPlayer->TiltStatus = -10;
            if (turnHeldTime >= TURBOTURNTIME && pPlayer->MotoSpeed > 0)
            {
                if (moveBack)
                    input.q16avel = fix16_sadd(input.q16avel, fix16_from_dbl(scaleAdjustmentToInterval(turnAmount)));
                else
                    input.q16avel = fix16_ssub(input.q16avel, fix16_from_dbl(scaleAdjustmentToInterval(turnAmount)));
            }
            else
            {
                if (moveBack)
                    input.q16avel = fix16_sadd(input.q16avel, fix16_from_dbl(scaleAdjustmentToInterval(turnAmount / (8 / 3))));
                else
                    input.q16avel = fix16_ssub(input.q16avel, fix16_from_dbl(scaleAdjustmentToInterval(turnAmount / (8 / 3))));
            }
        }
        else if (turnRight || pPlayer->moto_drink > 0)
        {
            turnHeldTime += elapsedTics;
            pPlayer->TiltStatus++;
            if (pPlayer->TiltStatus > 10)
                pPlayer->TiltStatus = 10;
            if (turnHeldTime >= TURBOTURNTIME && pPlayer->MotoSpeed > 0)
            {
                if (moveBack)
                    input.q16avel = fix16_ssub(input.q16avel, fix16_from_dbl(scaleAdjustmentToInterval(turnAmount)));
                else
                    input.q16avel = fix16_sadd(input.q16avel, fix16_from_dbl(scaleAdjustmentToInterval(turnAmount)));
            }
            else
            {
                if (moveBack)
                    input.q16avel = fix16_ssub(input.q16avel, fix16_from_dbl(scaleAdjustmentToInterval(turnAmount / (8 / 3))));
                else
                    input.q16avel = fix16_sadd(input.q16avel, fix16_from_dbl(scaleAdjustmentToInterval(turnAmount / (8 / 3))));
            }
        }
        else
        {
            turnHeldTime = 0;

            if (pPlayer->TiltStatus > 0)
                pPlayer->TiltStatus--;
            else if (pPlayer->TiltStatus < 0)
                pPlayer->TiltStatus++;
        }
    }

    if (pPlayer->moto_underwater)
    {
        pPlayer->MotoSpeed = 0;
    }
    else
    {
        localInput.bits |= (buttonMap.ButtonDown(gamefunc_Move_Forward) || buttonMap.ButtonDown(gamefunc_Strafe)) << SK_JUMP;
        localInput.bits |= buttonMap.ButtonDown(gamefunc_Move_Backward) << SK_AIM_UP;
        localInput.bits |= buttonMap.ButtonDown(gamefunc_Run) << SK_CROUCH;
    }

    input.q16avel      = fix16_mul(input.q16avel, avelScale);
    localInput.q16avel = fix16_sadd(localInput.q16avel, input.q16avel);
    pPlayer->q16ang    = fix16_sadd(pPlayer->q16ang, input.q16avel) & 0x7FFFFFF;
    localInput.fvel    = clamp((input.fvel += pPlayer->MotoSpeed), -(MAXVELMOTO / 8), MAXVELMOTO);

    if (TEST_SYNC_KEY(localInput.bits, SK_JUMP))
    {
        localInput.bits |= 1;
    }
}

void P_GetInputBoat(int playerNum)
{
    auto      &thisPlayer = g_player[playerNum];
    auto const pPlayer    = thisPlayer.ps;
    ControlInfo info;

    if ((pPlayer->gm & (MODE_MENU|MODE_TYPE)) || (ud.pause_on && !inputState.GetKeyStatus(sc_Pause)))
    {
        if (!(pPlayer->gm&MODE_MENU))
            CONTROL_GetInput(&info);

        localInput = {};
        localInput.bits    = (((int32_t)g_gameQuit) << SK_GAMEQUIT);
        localInput.extbits |= (1 << 7);

        return;
    }

    D_ProcessEvents();

	bool mouseaim = in_mousemode || buttonMap.ButtonDown(gamefunc_Mouse_Aiming);

    if (numplayers == 1)
    {
        pPlayer->aim_mode = in_mousemode;
        pPlayer->auto_aim = cl_autoaim;
        pPlayer->weaponswitch = cl_weaponswitch;
    }

    CONTROL_GetInput(&info);

    // JBF: Run key behaviour is selectable
    int const     playerRunning    = G_CheckAutorun(buttonMap.ButtonDown(gamefunc_Run));
    constexpr int analogTurnAmount = (NORMALTURN << 1);
    int const     keyMove          = playerRunning ? (NORMALKEYMOVE << 1) : NORMALKEYMOVE;
    constexpr int analogExtent     = 32767; // KEEPINSYNC sdlayer.cpp

    input_t input {};

    input.q16avel = fix16_sadd(input.q16avel, fix16_sdiv(fix16_from_int(info.mousex), F16(32)));
    input.q16avel = fix16_sadd(input.q16avel, fix16_from_int(info.dyaw / analogExtent * (analogTurnAmount << 1)));

    input.svel -= info.dx * keyMove / analogExtent;
    input.fvel -= info.dz * keyMove / analogExtent;

    static double lastInputTicks;
    auto const    currentHiTicks    = timerGetHiTicks();
    double const  elapsedInputTicks = currentHiTicks - lastInputTicks;

    lastInputTicks = currentHiTicks;

    auto scaleAdjustmentToInterval = [=](double x) { return x * REALGAMETICSPERSEC / (1000.0 / elapsedInputTicks); };

    pPlayer->crouch_toggle = 0;

    localInput.bits |= buttonMap.ButtonDown(gamefunc_Fire) << SK_FIRE;
    localInput.bits |= buttonMap.ButtonDown(gamefunc_Steroids) << SK_STEROIDS;
    localInput.bits |= buttonMap.ButtonDown(gamefunc_NightVision) << SK_NIGHTVISION;
    localInput.bits |= buttonMap.ButtonDown(gamefunc_MedKit) << SK_MEDKIT;
    localInput.bits |= (buttonMap.ButtonDown(gamefunc_Inventory_Left) ||
                 (buttonMap.ButtonDown(gamefunc_Dpad_Select) && (input.svel > 0 || input.q16avel < 0))) << SK_INV_LEFT;
    localInput.bits |= inputState.GetKeyStatus(sc_Pause) << SK_PAUSE;
    localInput.bits |= buttonMap.ButtonDown(gamefunc_Holo_Duke) << SK_HOLODUKE;
    localInput.bits |= buttonMap.ButtonDown(gamefunc_Jetpack) << SK_JETPACK;
    localInput.bits |= (g_gameQuit << SK_GAMEQUIT);
    localInput.bits |= (buttonMap.ButtonDown(gamefunc_Inventory_Right) ||
                 (buttonMap.ButtonDown(gamefunc_Dpad_Select) && (input.svel < 0 || input.q16avel > 0))) << SK_INV_RIGHT;
    localInput.bits |= buttonMap.ButtonDown(gamefunc_Open) << SK_OPEN;
    localInput.bits |= buttonMap.ButtonDown(gamefunc_Inventory) << SK_INVENTORY;
    //localInput.bits |= ((uint32_t)inputState.GetKeyStatus(sc_Escape)) << SK_ESCAPE; fixme.This needs to be done differently

    if (buttonMap.ButtonDown(gamefunc_Dpad_Select))
    {
        input.fvel = 0;
        input.svel = 0;
        input.q16avel = 0;
    }

    if (buttonMap.ButtonDown(gamefunc_Dpad_Aiming))
        input.fvel = 0;
    
    localInput.extbits |= (buttonMap.ButtonDown(gamefunc_Move_Forward) || (input.fvel > 0));
    localInput.extbits |= (buttonMap.ButtonDown(gamefunc_Move_Backward) || (input.fvel < 0)) << 1;
    localInput.extbits |= (buttonMap.ButtonDown(gamefunc_Strafe_Left) || (input.svel > 0)) << 2;
    localInput.extbits |= (buttonMap.ButtonDown(gamefunc_Strafe_Right) || (input.svel < 0)) << 3;

    int turnAmount;
    int const turn = input.q16avel / 32;
    int turnLeft = buttonMap.ButtonDown(gamefunc_Turn_Left) || buttonMap.ButtonDown(gamefunc_Strafe_Left);
    int turnRight = buttonMap.ButtonDown(gamefunc_Turn_Right) || buttonMap.ButtonDown(gamefunc_Strafe_Right);
    int avelScale = F16((turnLeft || turnRight) ? 1 : 0);
    if (turn)
    {
        turnAmount = (MOTOTURN << 1);
        avelScale = fix16_max(avelScale, fix16_clamp(fix16_mul(turn, turn),0,F16(1)));
        if (turn < 0)
            turnLeft = 1;
        else if (turn > 0)
            turnRight = 1;
    }
    else
        turnAmount = MOTOTURN;

    input.svel = input.fvel = input.q16avel = 0;

    localInput.bits |= (buttonMap.ButtonDown(gamefunc_Move_Forward) || buttonMap.ButtonDown(gamefunc_Strafe)) << SK_JUMP;
    localInput.bits |= buttonMap.ButtonDown(gamefunc_Move_Backward) << SK_AIM_UP;
    localInput.bits |= buttonMap.ButtonDown(gamefunc_Run) << SK_CROUCH;

    localInput.bits |= turnLeft << SK_AIM_DOWN;
    localInput.bits |= turnRight << SK_LOOK_LEFT;

    static int32_t turnHeldTime;
    static int32_t lastInputClock;  // MED
    int32_t const  elapsedTics = (int32_t)totalclock - lastInputClock;

    if (pPlayer->MotoSpeed != 0)
    {
        if (turnLeft || pPlayer->moto_drink < 0)
        {
            turnHeldTime += elapsedTics;
            if (!pPlayer->NotOnWater)
            {
                pPlayer->TiltStatus--;
                if (pPlayer->TiltStatus < -10)
                    pPlayer->TiltStatus = -10;
                if (turnHeldTime >= TURBOTURNTIME)
                    input.q16avel = fix16_ssub(input.q16avel, fix16_from_dbl(scaleAdjustmentToInterval(turnAmount)));
                else
                    input.q16avel = fix16_ssub(input.q16avel, fix16_from_dbl(scaleAdjustmentToInterval(turnAmount / (10 / 3))));
            }
            else
                if (turnHeldTime >= TURBOTURNTIME)
                    input.q16avel = fix16_ssub(input.q16avel, fix16_from_dbl(scaleAdjustmentToInterval(turnAmount / 3)));
                else
                    input.q16avel = fix16_ssub(input.q16avel, fix16_from_dbl(scaleAdjustmentToInterval((turnAmount / (10 / 3)) / 3)));
        }
        else if (turnRight || pPlayer->moto_drink > 0)
        {
            turnHeldTime += elapsedTics;
            if (!pPlayer->NotOnWater)
            {
                pPlayer->TiltStatus++;
                if (pPlayer->TiltStatus > 10)
                    pPlayer->TiltStatus = 10;
                if (turnHeldTime >= TURBOTURNTIME)
                    input.q16avel = fix16_sadd(input.q16avel, fix16_from_dbl(scaleAdjustmentToInterval(turnAmount)));
                else
                    input.q16avel = fix16_sadd(input.q16avel, fix16_from_dbl(scaleAdjustmentToInterval(turnAmount / (10 / 3))));
            }
            else
                if (turnHeldTime >= TURBOTURNTIME)
                    input.q16avel = fix16_sadd(input.q16avel, fix16_from_dbl(scaleAdjustmentToInterval(turnAmount / 3)));
                else
                    input.q16avel = fix16_sadd(input.q16avel, fix16_from_dbl(scaleAdjustmentToInterval((turnAmount / (10 / 3)) / 3)));
        }
        else if (!pPlayer->NotOnWater)
        {
            turnHeldTime = 0;

            if (pPlayer->TiltStatus > 0)
                pPlayer->TiltStatus--;
            else if (pPlayer->TiltStatus < 0)
                pPlayer->TiltStatus++;
        }
    }
    else if (!pPlayer->NotOnWater)
    {
        turnHeldTime = 0;

        if (pPlayer->TiltStatus > 0)
            pPlayer->TiltStatus--;
        else if (pPlayer->TiltStatus < 0)
            pPlayer->TiltStatus++;
    }

    input.q16avel      = fix16_mul(input.q16avel, avelScale);
    localInput.q16avel = fix16_sadd(localInput.q16avel, input.q16avel);
    pPlayer->q16ang    = fix16_sadd(pPlayer->q16ang, input.q16avel) & 0x7FFFFFF;
    localInput.fvel    = clamp((input.fvel += pPlayer->MotoSpeed), -(MAXVELMOTO / 8), MAXVELMOTO);
}

void P_AddAmmo(DukePlayer_t * const pPlayer, int const weaponNum, int const addAmount)
{
    pPlayer->ammo_amount[weaponNum] += addAmount;

    if (pPlayer->ammo_amount[weaponNum] > max_ammo_amount[weaponNum])
        pPlayer->ammo_amount[weaponNum] = max_ammo_amount[weaponNum];
}

void P_CheckWeapon(DukePlayer_t *pPlayer)
{
    checkavailweapon(pPlayer);
}

void P_UpdatePosWhenViewingCam(DukePlayer_t *pPlayer)
{
    int const newOwner      = pPlayer->newowner;
    pPlayer->pos            = *(vec3_t *)&sprite[newOwner];
    pPlayer->q16ang           = fix16_from_int(SA(newOwner));
    pPlayer->vel.x          = 0;
    pPlayer->vel.y          = 0;
    sprite[pPlayer->i].xvel = 0;
    pPlayer->look_ang       = 0;
    pPlayer->rotscrnang     = 0;
}


END_DUKE_NS
