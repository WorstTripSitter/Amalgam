#pragma once

#include "../../../SDK/SDK.h"

class CAutoHeal
{
private:
	void AutoHeal(CUserCmd* pCmd);
	void ActivateOnVoice(CUserCmd* pCmd);
	void AutoVaccinator(CUserCmd* pCmd);
	void GetDangers(CTFPlayer* pTarget, bool bVaccinator, float& flBulletDanger, float& flBlastDanger, float& flFireDanger);
	void SwapResistType(CUserCmd* pCmd, int iType);
	void ActivateResistType(CUserCmd* pCmd, int iType);

	CTFPlayer* m_pLocal = nullptr;
	CWeaponMedigun* m_pWeapon = nullptr;

	int m_iResistType = -1;
	float m_flChargeLevel = 0.f;
	float m_flSwapTime = 0.f;
	bool m_bPreventResistSwap = false;
	bool m_bPreventResistCharge = false;

	int m_iDamagedType = -1;
	float m_flDamagedDPS = -1;
	float m_flDamagedTime = 0.f;

public:
	void Run(CTFPlayer* pLocal, CTFWeaponBase* pWeapon, CUserCmd* pCmd);
	void Event(IGameEvent* pEvent, uint32_t uHash);

	float m_flAutoSwitchExpireTime = 0.f;
	int m_iAutoSwitch = 0;
	int m_iTargetIdx = -1;
	std::unordered_mapset<int> m_mMedicCallers = {};
};

ADD_FEATURE(CAutoHeal, AutoHeal);
