#include "Movement.h"

#include "../../AntiCheatCompatibility/AntiCheatCompatibility.h"

void C_Features_Misc_Movement::AutoBunnyhop(CTFPlayer* LocalPlayer, CUserCmd* UserCommand)
{
    const auto LocalWeapon = H::Entities.GetWeapon();
    if (LocalWeapon && LocalWeapon->GetWeaponID() == TF_WEAPON_GRAPPLINGHOOK && LocalWeapon->As<CTFGrapplingHook>()->m_hProjectile())
        return;

    static auto StaticAttempt = false;
    static auto StaticValid = false;
    const auto LastAttempt = StaticAttempt;
    const auto LastValid = StaticValid;
    const auto CurrentAttempt = StaticAttempt = G::OriginalCmd.buttons & IN_JUMP;
    const auto CurrentValid = StaticValid = LocalPlayer->m_hGroundEntity() && !LocalPlayer->IsDucking();
    if (!CurrentValid || CurrentValid && G::LastUserCmd->buttons & IN_JUMP)
        UserCommand->buttons &= ~IN_JUMP;

    static auto LastAttemptTime = 0.0f;
    const auto WasPressed = CurrentAttempt && !LastAttempt;
    const auto HasParachute = SDK::AttribHookValue(0, "parachute_attribute", LocalPlayer) && !LocalPlayer->InCond(TF_COND_PARACHUTE_ACTIVE);
    const auto AllowJump = !HasParachute || G::OriginalCmd.buttons & IN_DUCK;
    const auto ManualJump = AllowJump && (WasPressed || HasParachute && I::GlobalVars->curtime < LastAttemptTime + 0.1f);

    if (WasPressed && !CurrentValid)
        LastAttemptTime = I::GlobalVars->curtime;

    if (ManualJump)
        UserCommand->buttons |= IN_JUMP;

    F::AntiCheatCompatibility.BunnyHop(UserCommand, CurrentValid, LastValid);
}

void C_Features_Misc_Movement::EarlyCreateMove(CTFPlayer* LocalPlayer, CUserCmd* UserCommand)
{
    if (LocalPlayer == nullptr || LocalPlayer->deadflag() || LocalPlayer->IsAGhost() || LocalPlayer->IsTaunting() ||
        LocalPlayer->m_MoveType() != MOVETYPE_WALK || LocalPlayer->IsSwimming() || LocalPlayer->InCond(TF_COND_SHIELD_CHARGE))
        return;

    if (Vars::Misc::Movement::Bunnyhop.Value)
        AutoBunnyhop(LocalPlayer, UserCommand);

    if (LocalPlayer->InCond(TF_COND_HALLOWEEN_KART))
        return;
}

void C_Features_Misc_Movement::LateCreateMove()
{
}