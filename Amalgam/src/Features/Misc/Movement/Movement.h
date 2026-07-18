#pragma once

#include "../../../SDK/SDK.h"

class C_Features_Misc_Movement
{
    static void AutoBunnyhop(CTFPlayer* LocalPlayer, CUserCmd* UserCommand);
public:
    static void EarlyCreateMove(CTFPlayer* LocalPlayer, CUserCmd* UserCommand);
    void LateCreateMove();
};

ADD_FEATURE(C_Features_Misc_Movement, Movement);