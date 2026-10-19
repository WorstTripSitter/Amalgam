#include "AutoHeal.h"

#include "../../Players/PlayerUtils.h"
#include "../../Backtrack/Backtrack.h"
#include "../../CritHack/CritHack.h"
#include "../../Simulation/ProjectileSimulation/ProjectileSimulation.h"
#include "../AimbotProjectile/AimbotProjectile.h"

static bool ShouldIgnore(CBaseEntity* pEntity, CTFPlayer* pLocal, CTFWeaponBase* pWeapon)
{
	int iIgnoreFlags = Vars::Aimbot::Healing::DangerIgnore.Value & Vars::Aimbot::Healing::DangerIgnoreEnum::Shared;
	int iFunctionFlags = ShouldIgnoreEnum::Dormant;
	if (Vars::Aimbot::Healing::DangerIgnore.Value & Vars::Aimbot::Healing::DangerIgnoreEnum::Ignored)
		iFunctionFlags |= ShouldIgnoreEnum::Ignored;

	return F::AimbotGlobal.ShouldIgnore(pEntity, pLocal, pWeapon, iFunctionFlags, Vars::Aimbot::General::TargetEnum::All, iIgnoreFlags);
}

void CAutoHeal::AutoHeal(CUserCmd* pCmd)
{	// manage lagcomp
	if (!Vars::Aimbot::Healing::AutoHeal.Value)
		return;

	auto pTargetEntity = m_pWeapon->m_hHealingTarget().Get();
	auto pTarget = pTargetEntity ? pTargetEntity->As<CTFPlayer>() : nullptr;
	if (!pTarget || pCmd->buttons & IN_ATTACK && !(G::LastUserCmd->buttons & IN_ATTACK))
		return;

	m_iTargetIdx = pTarget->entindex();

	const bool AutoSwitchEnabled = Vars::Aimbot::Healing::AutoArrow.Value && Vars::Aimbot::Healing::AutoCrusaderCrossbowSwitch.Value;
	if (m_pLocal->GetWeaponFromSlot(SLOT_PRIMARY)->GetWeaponID() == TF_WEAPON_CROSSBOW && AutoSwitchEnabled && !(pCmd->buttons & IN_ATTACK2) && !pTarget->IsUbered() && pTarget->GetClassID() != ETFClassID::CTFReviveMarker && !pTarget->InCond(TF_COND_MEGAHEAL) && !m_pLocal->IsUbered())
	{
		if (const auto CrusaderCrossbow = m_pLocal->GetWeaponFromSlot(SLOT_PRIMARY))
		{
			float DeployTimeMultiplier = 1.0f;
			DeployTimeMultiplier = SDK::AttribHookValue(DeployTimeMultiplier, "mult_deploy_time", m_pLocal);
			DeployTimeMultiplier = SDK::AttribHookValue(DeployTimeMultiplier, "mult_single_wep_deploy_time", CrusaderCrossbow);
			DeployTimeMultiplier = std::max(DeployTimeMultiplier, 0.00001f);

			const float DeployTime = 0.5f * DeployTimeMultiplier;
			const float NextPrimaryAttack = CrusaderCrossbow->m_flNextPrimaryAttack();
			const float HealthPercent = pTarget->GetMaxHealth() ? static_cast<float>(pTarget->m_iHealth()) / pTarget->GetMaxHealth() * 100.0f : 100.0f;
			if (NextPrimaryAttack - DeployTime <= I::GlobalVars->curtime && HealthPercent < Vars::Aimbot::Healing::AutoCrusaderCrossbowSwitch_HealthThreshold.Value)
			{
				const float Charge = m_pWeapon->GetMedigunType() == MEDIGUN_RESIST ? 0.25f : 96.0f;
				if (m_pWeapon->m_flChargeLevel() < Charge
					|| pTarget->InCond(TF_COND_BULLET_IMMUNE) || pTarget->InCond(TF_COND_MEDIGUN_UBER_BULLET_RESIST)
					|| pTarget->InCond(TF_COND_BLAST_IMMUNE) || pTarget->InCond(TF_COND_MEDIGUN_UBER_BLAST_RESIST)
					|| pTarget->InCond(TF_COND_FIRE_IMMUNE) || pTarget->InCond(TF_COND_MEDIGUN_UBER_FIRE_RESIST))
				{
					I::EngineClient->ClientCmd_Unrestricted("slot1");

					m_flAutoSwitchExpireTime = NextPrimaryAttack > I::GlobalVars->curtime ? NextPrimaryAttack + 0.1f : I::GlobalVars->curtime + 1.1f;
					m_iAutoSwitch = 1;
					return;
				}
			}
		}
	}

	std::vector<TickRecord*> vRecords = {};
	if (!F::Backtrack.GetRecords(pTarget, vRecords))
		return;
	vRecords = F::Backtrack.GetValidRecords(vRecords, m_pLocal, true);
	if (!vRecords.size())
		return;

	Vec3 vEyePos = m_pLocal->GetShootPos();
	for (auto pRecord : vRecords)
	{
		Vec3 vCenter = pRecord->m_vOrigin + (pRecord->m_vMins + pRecord->m_vMaxs) / 2;
		if (SDK::VisPosWorld(m_pLocal, pTarget, vEyePos, vCenter))
		{
			pCmd->tick_count = TIME_TO_TICKS(pRecord->m_flSimTime) + TIME_TO_TICKS(F::Backtrack.GetFakeInterp());
			break;
		}
	}
}

void CAutoHeal::ActivateOnVoice(CUserCmd* pCmd)
{
	if (!Vars::Aimbot::Healing::ActivateOnVoice.Value)
		return;

	auto pTarget = m_pWeapon->m_hHealingTarget().Get();
	if (!pTarget
		|| Vars::Aimbot::Healing::HealPriority.Value == Vars::Aimbot::Healing::HealPriorityEnum::FriendsOnly
		&& !H::Entities.IsFriend(pTarget->entindex()) && !H::Entities.InParty(pTarget->entindex()))
		return;

	if (m_mMedicCallers.contains(pTarget->entindex()))
		pCmd->buttons |= IN_ATTACK2;
}

static inline Vec3 PredictOrigin(Vec3& vOrigin, Vec3 vVelocity, float flLatency, bool bTrace = true, Vec3 vMins = {}, Vec3 vMaxs = {}, unsigned int nMask = MASK_SOLID, float flNormal = 0.f)
{
	Vec3 vEnd = SDK::PredictOrigin(vOrigin, vVelocity, flLatency, bTrace, vMins, vMaxs, nMask, flNormal);
	return vEnd;
}

static inline bool TraceToEntity(CTFPlayer* pPlayer, CBaseEntity* pEntity, Vec3& vPlayerOrigin, Vec3& vEntityOrigin, unsigned int nMask = MASK_SHOT | CONTENTS_GRATE)
{
	CGameTrace trace = {};
	CTraceFilterWorldAndPropsOnly filter = {};

	if (pEntity->IsPlayer())
	{
		Vec3 vMins = pPlayer->m_vecMins() + 1;
		Vec3 vMaxs = pPlayer->m_vecMaxs() - 1;
		Vec3 vMinsS = (vMins - vMaxs) / 2;
		Vec3 vMaxsS = (vMaxs - vMins) / 2;

		std::vector<Vec3> vPoints = {
			Vec3(),
			Vec3(vMinsS.x, vMinsS.y, vMaxsS.z),
			Vec3(vMaxsS.x, vMinsS.y, vMaxsS.z),
			Vec3(vMinsS.x, vMaxsS.y, vMaxsS.z),
			Vec3(vMaxsS.x, vMaxsS.y, vMaxsS.z),
			Vec3(vMinsS.x, vMinsS.y, vMinsS.z),
			Vec3(vMaxsS.x, vMinsS.y, vMinsS.z),
			Vec3(vMinsS.x, vMaxsS.y, vMinsS.z),
			Vec3(vMaxsS.x, vMaxsS.y, vMinsS.z)
		};
		for (auto& vPoint : vPoints)
		{
			SDK::Trace(vPlayerOrigin + vPoint, vEntityOrigin, nMask, &filter, &trace);
			if (trace.fraction == 1.f)
				return true;
		}
	}
	else
	{
		SDK::Trace(vPlayerOrigin, vEntityOrigin, nMask, &filter, &trace);
		return trace.fraction == 1.f;
	}

	return false;
}

static inline int GetShotsWithinTime(int iWeaponID, float flFireRate, float flTime)
{
	int iTicks = TIME_TO_TICKS(flTime);

	int iDelay = 1;
	switch (iWeaponID)
	{
	case TF_WEAPON_MINIGUN:
	case TF_WEAPON_PIPEBOMBLAUNCHER:
	case TF_WEAPON_CANNON:
		iDelay = 2;
	}

	return 1 + (iTicks - iDelay) / std::ceilf(flFireRate / TICK_INTERVAL);
}

static inline float GetDamage(CBaseEntity* pProjectile, CTFPlayer* pOwner, CTFWeaponBase* pWeapon, CTFPlayer* pTarget, float flMult, Vec3 vProjectileOrigin, int* pType = nullptr)
{
	float flReturn = 100.f;
	switch (pProjectile->GetClassID())
	{
	case ETFClassID::CTFGrenadePipebombProjectile:
		flReturn = 100.f;
		break;
	case ETFClassID::CTFWeaponBaseMerasmusGrenade:
		flReturn = 50.f;
		break;
	case ETFClassID::CTFProjectile_SpellMeteorShower:
		flReturn = 100.f;
		if (pType)
			*pType = MEDIGUN_FIRE_RESIST;
		break;
	case ETFClassID::CTFProjectile_Arrow:
		flReturn = pProjectile->As<CTFProjectile_Arrow>()->CanHeadshot()
			? Math::RemapVal(pProjectile->As<CTFProjectile_Arrow>()->m_vInitialVelocity().Length(), 1800.f, 2600.f, 50.f, 120.f)
			: 40.f;
		if (pType)
			*pType = MEDIGUN_BULLET_RESIST;
		break;
	case ETFClassID::CTFProjectile_HealingBolt:
		flReturn = Math::RemapVal(pOwner->m_vecOrigin().DistTo(vProjectileOrigin), 200.f, 1600.f, 38.f, 75.f);
		if (pType)
			*pType = MEDIGUN_BULLET_RESIST;
		break;
	case ETFClassID::CTFProjectile_Rocket:
	case ETFClassID::CTFProjectile_EnergyBall:
		flReturn = flMult ? 90.f : Math::RemapVal(pOwner->m_vecOrigin().DistTo(vProjectileOrigin), 500.f, 920.f, 90.f, 48.f);
		break;
	case ETFClassID::CTFProjectile_SentryRocket:
		flReturn = flMult ? 100.f : Math::RemapVal(pOwner->m_vecOrigin().DistTo(vProjectileOrigin), 100.f, 920.f, 100.f, 50.f);
		break;
	case ETFClassID::CTFProjectile_BallOfFire:
		flReturn = pTarget->InCond(TF_COND_BURNING) || pTarget->InCond(TF_COND_BURNING_PYRO) ? 90.f : 30.f;
		if (pType)
			*pType = MEDIGUN_FIRE_RESIST;
		break;
	case ETFClassID::CTFProjectile_SpellFireball:
		flReturn = 100.f;
		if (pType)
			*pType = MEDIGUN_FIRE_RESIST;
		break;
	case ETFClassID::CTFProjectile_SpellLightningOrb:
		flReturn = 100.f;
		if (pType)
			*pType = MEDIGUN_FIRE_RESIST;
		break;
	case ETFClassID::CTFProjectile_Flare:
		flReturn = 30.f;
		if (pType)
			*pType = MEDIGUN_FIRE_RESIST;
		break;
	case ETFClassID::CTFProjectile_EnergyRing:
		flReturn = 60.f;
		if (pType)
			*pType = MEDIGUN_BULLET_RESIST;
	}
	if (pWeapon)
		flReturn *= SDK::AttribHookValue(1, "mult_dmg", pWeapon);
	return flReturn;
}

static inline float GetMult(CBaseEntity* pProjectile, CTFWeaponBase* pWeapon, CTFPlayer* pTarget)
{
	float flReturn = pTarget->IsMarked() ? 1.36f : 1.f;
	switch (pProjectile->GetClassID())
	{
	case ETFClassID::CTFGrenadePipebombProjectile:
	case ETFClassID::CTFWeaponBaseMerasmusGrenade:
	case ETFClassID::CTFProjectile_SpellMeteorShower:
		if (pProjectile->As<CTFWeaponBaseGrenadeProj>()->m_bCritical())
			flReturn = 3.f;
		else if (pProjectile->As<CTFWeaponBaseGrenadeProj>()->m_iDeflected() && (pProjectile->GetClassID() != ETFClassID::CTFGrenadePipebombProjectile || !pProjectile->GetAbsVelocity().IsZero()))
			flReturn = 1.36f;
		break;
	case ETFClassID::CTFProjectile_Arrow:
		if (pProjectile->As<CTFProjectile_Arrow>()->CanHeadshot())
		{
			flReturn = 3.f;
			break;
		}
		[[fallthrough]];
	case ETFClassID::CTFProjectile_HealingBolt:
		if (pProjectile->As<CTFProjectile_Arrow>()->m_bCritical())
			flReturn = 3.f;
		else if (pProjectile->As<CTFBaseRocket>()->m_iDeflected())
			flReturn = 1.36f;
		break;
	case ETFClassID::CTFProjectile_Rocket:
	case ETFClassID::CTFProjectile_BallOfFire:
	case ETFClassID::CTFProjectile_SentryRocket:
	case ETFClassID::CTFProjectile_SpellFireball:
	case ETFClassID::CTFProjectile_SpellLightningOrb:
		if (pProjectile->As<CTFProjectile_Rocket>()->m_bCritical())
			flReturn = 3.f;
		else if (pProjectile->As<CTFBaseRocket>()->m_iDeflected()
			|| pTarget->InCond(TF_COND_BLASTJUMPING) && pWeapon && SDK::AttribHookValue(0, "mini_crit_airborne", pWeapon) == 1)
			flReturn = 1.36f;
		break;
	case ETFClassID::CTFProjectile_EnergyBall:
		if (pProjectile->As<CTFProjectile_EnergyBall>()->m_bChargedShot())
			flReturn = 2.f; // whatever
		else if (pProjectile->As<CTFBaseRocket>()->m_iDeflected())
			flReturn = 1.36f;
		break;
	case ETFClassID::CTFProjectile_Flare:
		if (pProjectile->As<CTFProjectile_Flare>()->m_bCritical() || pTarget->InCond(TF_COND_BURNING) || pTarget->InCond(TF_COND_BURNING_PYRO))
			flReturn = 3.f;
		else if (pProjectile->As<CTFBaseRocket>()->m_iDeflected())
			flReturn = 1.36f;
	}
	return flReturn;
}

void CAutoHeal::ComputeDangers(CTFPlayer* pTarget, float& flBulletDanger, float& flBlastDanger, float& flFireDanger, float& flMeleeDanger)
{
	float flBulletResist = pTarget->InCond(TF_COND_BULLET_IMMUNE) ? 1.f : pTarget->InCond(TF_COND_MEDIGUN_UBER_BULLET_RESIST) ? 0.75f : pTarget->InCond(TF_COND_MEDIGUN_SMALL_BULLET_RESIST) ? -0.1f : 0.f;
	float flBlastResist = pTarget->InCond(TF_COND_BLAST_IMMUNE) ? 1.f : pTarget->InCond(TF_COND_MEDIGUN_UBER_BLAST_RESIST) ? 0.75f : pTarget->InCond(TF_COND_MEDIGUN_SMALL_BLAST_RESIST) ? -0.1f : 0.f;
	float flFireResist = pTarget->InCond(TF_COND_FIRE_IMMUNE) ? 1.f : pTarget->InCond(TF_COND_MEDIGUN_UBER_FIRE_RESIST) ? 0.75f : pTarget->InCond(TF_COND_MEDIGUN_SMALL_FIRE_RESIST) ? -0.1f : 0.f;

	float flLatency = F::Backtrack.GetReal();
	int iPlayerHealth = std::max(pTarget->m_iHealth(), 1);
	auto pGameRules = I::TFGameRules();
	bool bMvM = pGameRules && pGameRules->m_bPlayingMannVsMachine();

	// victim-side damage multipliers (MvM resistance upgrades, Fists of Steel, Darwin's, etc), crit resist only scales the crit bonus
	float flAllTaken = SDK::AttribHookValue(1.f, "mult_dmgtaken", pTarget);
	if (pTarget->m_iClass() == TF_CLASS_HEAVY && pTarget->InCond(TF_COND_AIMING) && iPlayerHealth <= pTarget->GetMaxHealth() / 2)
		flAllTaken *= SDK::AttribHookValue(1.f, "spunup_damage_resistance", pTarget);
	float flRangedTaken = flAllTaken; // dmg_from_ranged covers bullet/blast/ignite but not afterburn (DMG_BURN)
	float flMeleeTaken = SDK::AttribHookValue(1.f, "mult_dmgtaken_from_melee", pTarget) * flAllTaken;
	if (auto pTargetWeapon = pTarget->m_hActiveWeapon()->As<CTFWeaponBase>())
	{
		flRangedTaken *= SDK::AttribHookValue(1.f, "dmg_from_ranged", pTargetWeapon);
		flMeleeTaken *= SDK::AttribHookValue(1.f, "dmg_from_melee", pTargetWeapon);
	}
	float flBulletTaken = SDK::AttribHookValue(1.f, "mult_dmgtaken_from_bullets", pTarget) * flRangedTaken;
	float flBlastTaken = SDK::AttribHookValue(1.f, "mult_dmgtaken_from_explosions", pTarget) * flRangedTaken;
	float flFireTaken = SDK::AttribHookValue(1.f, "mult_dmgtaken_from_fire", pTarget) * flRangedTaken;
	float flBurnTaken = SDK::AttribHookValue(1.f, "mult_dmgtaken_from_fire", pTarget) * flAllTaken;
	float flCritMult = 1.f + 2.f * SDK::AttribHookValue(1.f, "mult_dmgtaken_from_crit", pTarget);
	auto fGetTaken = [&](int iType)
	{
		switch (iType)
		{
		case MEDIGUN_BULLET_RESIST: return flBulletTaken;
		case MEDIGUN_BLAST_RESIST: return flBlastTaken;
		case MEDIGUN_FIRE_RESIST: return flFireTaken;
		}
		return 1.f;
	};
	Vec3 vTargetOrigin = PredictOrigin(pTarget->m_vecOrigin(), pTarget->m_vecVelocity(), pTarget->entindex() != I::EngineClient->GetLocalPlayer() ? flLatency : 0.f, true, pTarget->m_vecMins(), pTarget->m_vecMaxs(), pTarget->SolidMask());
	Vec3 vTargetCenter = vTargetOrigin + pTarget->GetOffset() / 2;
	Vec3 vTargetEye = vTargetOrigin + pTarget->GetViewOffset();

	for (auto pEntity : H::Entities.GetGroup(EntityEnum::PlayerEnemy))
	{
		auto pPlayer = pEntity->As<CTFPlayer>();
		int iIndex = pPlayer->entindex();
		if (bMvM && H::Entities.GetModel(iIndex) == FNV1A::Hash32Const("models/bots/demo/bot_sentry_buster.mdl"))
		{
			// taunts for 2s then deals 4x max(maxhealth, health) blast damage to anything within 300u with line of fire
			bool bDetonating = pPlayer->InCond(TF_COND_TAUNTING);
			Vec3 vBusterOrigin = bDetonating ? pPlayer->m_vecOrigin() : PredictOrigin(pPlayer->m_vecOrigin(), pPlayer->m_vecVelocity(), flLatency + 0.5f, true, pPlayer->m_vecMins(), pPlayer->m_vecMaxs(), pPlayer->SolidMask());
			Vec3 vBusterCenter = vBusterOrigin + pPlayer->GetOffset() / 2;
			if (vBusterCenter.DistTo(vTargetCenter) < 300.f && TraceToEntity(pTarget, pPlayer, vTargetCenter, vBusterCenter))
			{
				float flMult = flBlastResist > 0.f ? 1.f - flBlastResist : 1.f + flBlastResist;
				float flDamage = 4.f * std::max(pTarget->GetMaxHealth(), pTarget->m_iHealth()) * flMult * flBlastTaken;
				flBlastDanger += flDamage / iPlayerHealth * (bDetonating ? 1.f : 0.5f);
			}
			continue;
		}
		if (ShouldIgnore(pPlayer, m_pLocal, m_pWeapon) || !pPlayer->CanAttack(true, false))
			continue;

		auto pWeapon = pPlayer->m_hActiveWeapon()->As<CTFWeaponBase>();
		int nWeaponID = pWeapon ? pWeapon->GetWeaponID() : 0;
		auto eWeaponType = SDK::GetWeaponType(pWeapon);
		if (eWeaponType == EWeaponType::UNKNOWN
			|| nWeaponID == TF_WEAPON_DRG_POMSON || nWeaponID == TF_WEAPON_PIPEBOMBLAUNCHER
			|| nWeaponID == TF_WEAPON_MINIGUN && !pPlayer->InCond(TF_COND_AIMING))
			continue;

		Vec3 vPlayerOrigin = PredictOrigin(pPlayer->m_vecOrigin(), pPlayer->m_vecVelocity(), flLatency, true, pPlayer->m_vecMins(), pPlayer->m_vecMaxs(), pPlayer->SolidMask());
		Vec3 vPlayerEye = vPlayerOrigin + pPlayer->GetViewOffset();

		bool bCheater = F::PlayerUtils.HasTag(iIndex, F::PlayerUtils.TagToIndex(CHEATER_TAG));
		bool bZoom = pPlayer->InCond(TF_COND_ZOOMED);
		float flFOV = Math::CalcFov(H::Entities.GetEyeAngles(iIndex) + H::Entities.GetDeltaAngles(iIndex), Math::CalcAngle(vPlayerEye, bZoom ? vTargetEye : vTargetCenter));
		bool bFOV = bCheater || flFOV < (bZoom ? 10 : eWeaponType == EWeaponType::HITSCAN ? 30 : 90);
		if (!bFOV)
			continue;

		// skip the traces for anyone who can't reach the target within the window anyway
		float flDistance = vPlayerEye.DistTo(vTargetCenter);
		float flMaxDistance = std::numeric_limits<float>::max();
		ProjectileInfo tProjInfo = {};
		switch (eWeaponType)
		{
		case EWeaponType::HITSCAN:
			if (!bCheater)
				flMaxDistance = 100.f / std::clamp(pWeapon->GetWeaponSpread(), 0.001f, 1.f);
			break;
		case EWeaponType::PROJECTILE:
			if (!F::ProjSim.GetInfo(pPlayer, pWeapon, {}, tProjInfo, ProjSimEnum::NoRandomAngles | ProjSimEnum::MaxSpeed))
				continue;
			flMaxDistance = tProjInfo.m_flVelocity * flLatency + F::AimbotProjectile.GetSplashRadius(pWeapon, pPlayer) + pTarget->GetSize().Length() / 2;
			break;
		case EWeaponType::MELEE: // CTFWeaponBaseMelee::GetSwingRange, plus what they can close in a quarter second
		{
			float flReach = pPlayer->InCond(TF_COND_SHIELD_CHARGE) ? 128.f : SDK::AttribHookValue(0, "is_a_sword", pWeapon) ? 72.f : 48.f;
			flReach *= pPlayer->m_flModelScale();
			flMaxDistance = flReach + pTarget->GetSize().Length() / 2 + pPlayer->m_vecVelocity().Length() * 0.25f;
		}
		}
		if (flDistance > flMaxDistance || !TraceToEntity(pTarget, pPlayer, vTargetCenter, vPlayerEye))
			continue;

		bool bCrits = pPlayer->IsCritBoosted() || F::CritHack.WeaponCanCrit(pWeapon);
		bool bMinicrits = pPlayer->IsMiniCritBoosted() || pTarget->IsMarked();
		float flDamage, flMult = bCrits ? flCritMult : bMinicrits ? 1.36f : 1.f;
		if (eWeaponType == EWeaponType::MELEE)
		{
			// not resistable, only feeds heal priority. knife only counts as a backstab from behind (CTFKnife::IsBehindAndFacingTarget)
			bool bBackstab = false;
			if (nWeaponID == TF_WEAPON_KNIFE)
			{
				Vec3 vTargetForward, vSpyForward;
				Math::AngleVectors(H::Entities.GetEyeAngles(pTarget->entindex()), &vTargetForward);
				Math::AngleVectors(H::Entities.GetEyeAngles(iIndex), &vSpyForward);
				vTargetForward.z = vSpyForward.z = 0.f;
				Vec3 vToTarget = vTargetOrigin - vPlayerOrigin; vToTarget.z = 0.f;
				vTargetForward.Normalize(), vSpyForward.Normalize(), vToTarget.Normalize();
				bBackstab = vToTarget.Dot(vTargetForward) > 0.f && vToTarget.Dot(vSpyForward) > 0.5f && vTargetForward.Dot(vSpyForward) > -0.3f;
			}
			flDamage = bBackstab ? 2.f * iPlayerHealth : pWeapon->GetDamage() * flMult * flMeleeTaken;
			flMeleeDanger += flDamage / iPlayerHealth;
			continue;
		}
		float flFireRate = std::min(pWeapon->GetFireRate(), 1.f);
		int iBulletCount = pWeapon->GetBulletsPerShot();
		int iShotsWithinTime = GetShotsWithinTime(nWeaponID, flFireRate, flLatency + TICKS_TO_TIME(bCheater ? 22 : 0));
		switch (nWeaponID)
		{
		case TF_WEAPON_SNIPERRIFLE:
		case TF_WEAPON_SNIPERRIFLE_DECAP:
		case TF_WEAPON_SNIPERRIFLE_CLASSIC:
		{
			bool bClassic = nWeaponID == TF_WEAPON_SNIPERRIFLE_CLASSIC;
			bool bHeadshot = pWeapon->As<CTFSniperRifle>()->GetRifleType() != RIFLE_JARATE;
			bool bPiss = SDK::AttribHookValue(0, "jarate_duration", pWeapon) > 0;
			auto fGetSniperDot = [](CBaseEntity* pEntity) -> CSniperDot*
			{
				for (auto pDot : H::Entities.GetGroup(EntityEnum::SniperDots))
				{
					if (pDot->m_hOwnerEntity().Get() == pEntity)
						return pDot->As<CSniperDot>();
				}
				return nullptr;
			};
			if (CSniperDot* pPlayerDot = fGetSniperDot(pEntity))
			{
				float flChargeTime = std::max(SDK::AttribHookValue(3.f, "mult_sniper_charge_per_sec", pWeapon), 1.5f);
				flDamage = Math::RemapVal(TICKS_TO_TIME(I::ClientState->m_ClockDriftMgr.m_nServerTick) - pPlayerDot->m_flChargeStartTime() - 0.3f, 0.f, flChargeTime, 50.f, 150.f);
				if (bClassic && flDamage != 150.f)
					flDamage = SDK::AttribHookValue(flDamage, "bodyshot_damage_modify", pWeapon);
				else
					flMult = std::max(bHeadshot || bClassic ? flCritMult : bPiss ? 1.36f : 1.f, flMult);
				break;
			}
			if (SDK::AttribHookValue(0, "sniper_only_fire_zoomed", pWeapon) && !bZoom)
				flDamage = 0.f;
			else if (bClassic || !bZoom)
				flDamage = SDK::AttribHookValue(50.f, "bodyshot_damage_modify", pWeapon);
			else
				flDamage = 150.f, flMult = flMult = std::max(bHeadshot || bClassic ? flCritMult : bPiss ? 1.36f : 1.f, flMult);
			break;
		}
		case TF_WEAPON_COMPOUND_BOW:
			flDamage = 120.f, flMult = flCritMult;
			break;
		case TF_WEAPON_FLAMETHROWER:
			flDamage = 80.f * flFireRate;
			break;
		default:
			flDamage = pWeapon->GetDamage(false);
		}

		switch (eWeaponType)
		{
		case EWeaponType::HITSCAN:
		{
			flMult = flBulletResist > 0.f ? 1.f - flBulletResist : flMult + flBulletResist;
			flDamage *= flMult * flBulletTaken * (pWeapon ? SDK::AttribHookValue(1, "mult_dmg", pWeapon) : 1);

			float flSpread = std::clamp(pWeapon->GetWeaponSpread(), 0.001f, 1.f);
			float flMappedCount = Math::RemapVal(flDistance, 20 / flSpread, 100 / flSpread, iBulletCount, 1);
			float flDamageInLatency = flDamage * flMappedCount * iShotsWithinTime;
			float flDamageDanger = flDamageInLatency / iPlayerHealth;
			float flDistanceDanger = Math::RemapVal(flDistance, 100, 100 / flSpread, 1.f, 0.001f);
			if (bCheater) // may use seed pred +/ crits
				flDistanceDanger = std::max(flDistanceDanger, bCrits ? 1.f : 0.34f);

			flBulletDanger += flDamageDanger * flDistanceDanger;
			break;
		}
		case EWeaponType::PROJECTILE:
		{
			int iType = MEDIGUN_BLAST_RESIST;
			switch (nWeaponID)
			{
			case TF_WEAPON_COMPOUND_BOW:
			case TF_WEAPON_CROSSBOW:
			case TF_WEAPON_SHOTGUN_BUILDING_RESCUE:
			case TF_WEAPON_SYRINGEGUN_MEDIC:
			case TF_WEAPON_RAYGUN: iType = MEDIGUN_BULLET_RESIST; break;
			case TF_WEAPON_FLAMETHROWER: // maybe just test full range?
			case TF_WEAPON_FLAME_BALL:
			case TF_WEAPON_FLAREGUN:
			case TF_WEAPON_FLAREGUN_REVENGE: iType = MEDIGUN_FIRE_RESIST; break;
			}

			switch (iType)
			{
			case MEDIGUN_BULLET_RESIST: flMult = flBulletResist > 0.f ? 1.f - flBulletResist : flMult + flBulletResist; break;
			case MEDIGUN_BLAST_RESIST: flMult = flBlastResist > 0.f ? 1.f - flBlastResist : flMult + flBlastResist; break;
			case MEDIGUN_FIRE_RESIST: flMult = flFireResist > 0.f ? 1.f - flFireResist : flMult + flFireResist; break;
			}
			flDamage *= flMult * fGetTaken(iType) * (pWeapon ? SDK::AttribHookValue(1, "mult_dmg", pWeapon) : 1);

			float flRadius = flMaxDistance;
			float flDamageInLatency = flDamage * iBulletCount * iShotsWithinTime;
			float flDamageDanger = flDamageInLatency / iPlayerHealth;
			float flDistanceDanger = Math::RemapVal(flDistance, flRadius, flRadius, 1.f, 0.001f);

			switch (iType)
			{
			case MEDIGUN_BULLET_RESIST: flBulletDanger += flDamageDanger * flDistanceDanger; break;
			case MEDIGUN_BLAST_RESIST: flBlastDanger += flDamageDanger * flDistanceDanger; break;
			case MEDIGUN_FIRE_RESIST: flFireDanger += flDamageDanger * flDistanceDanger; break;
			}
		}
		}
	}

	for (auto pEntity : H::Entities.GetGroup(EntityEnum::BuildingEnemy))
	{
		auto pSentry = pEntity->As<CObjectSentrygun>();
		if (!pSentry->IsSentrygun())
			continue;

		if (pSentry->m_hEnemy().Get() != pTarget && pSentry->m_hAutoAimTarget().Get() != pTarget || !pSentry->m_iAmmoShells())
			continue;

		// tf_sentrygun_damage 16 / tf_sentrygun_mini_damage 8, 0.2s at level 1 else 0.1s, x0.5 wrangled, x0.75 mini, x0.4 crit canteen builder
		auto pBuilder = pSentry->m_hBuilder()->As<CTFPlayer>();
		float flDistance = pSentry->GetCenter().DistTo(vTargetCenter);
		float flDamage = pSentry->m_bMiniBuilding() ? 8.f : 16.f, flMult = pTarget->IsMarked() ? 1.36f : 1.f;
		float flFireRate = pSentry->m_iUpgradeLevel() == 1 ? 0.2f : 0.1f;
		if (pBuilder)
		{
			flDamage = SDK::AttribHookValue(flDamage, "mult_engy_sentry_damage", pBuilder);
			flFireRate = SDK::AttribHookValue(flFireRate, "mult_sentry_firerate", pBuilder);
			if (pBuilder->InCond(TF_COND_CRITBOOSTED_USER_BUFF))
				flFireRate *= 0.4f;
		}
		if (pSentry->m_bPlayerControlled())
			flFireRate *= 0.5f;
		if (pSentry->m_bMiniBuilding() && !pSentry->m_bDisposableBuilding())
			flFireRate *= 0.75f;
		int iShotsWithinTime = GetShotsWithinTime(0, flFireRate, 1.f /*flLatency*/);
		flMult = flBulletResist > 0.f ? 1.f - flBulletResist : flMult + flBulletResist;
		flDamage *= flMult * flBulletTaken;

		float flDamageInLatency = flDamage * iShotsWithinTime;
		float flDamageDanger = flDamageInLatency / iPlayerHealth;
		float flDistanceDanger = Math::RemapVal(flDistance, 1100.f, 3200.f, 1.f, 0.001f);
		flBulletDanger += flDamageDanger * flDistanceDanger;
	}

	for (auto pEntity : H::Entities.GetGroup(EntityEnum::WorldProjectile))
	{
		CTFPlayer* pOwner = nullptr;
		CTFWeaponBase* pWeapon = nullptr;

		switch (pEntity->GetClassID())
		{
		case ETFClassID::CTFGrenadePipebombProjectile:
		case ETFClassID::CTFWeaponBaseMerasmusGrenade:
		case ETFClassID::CTFProjectile_SpellMeteorShower:
			pWeapon = pEntity->As<CTFGrenadePipebombProjectile>()->m_hOriginalLauncher()->As<CTFWeaponBase>();
			pOwner = pEntity->As<CTFWeaponBaseGrenadeProj>()->m_hThrower()->As<CTFPlayer>();
			break;
		case ETFClassID::CTFProjectile_Arrow:
		case ETFClassID::CTFProjectile_HealingBolt:
		case ETFClassID::CTFProjectile_Rocket:
		case ETFClassID::CTFProjectile_SentryRocket:
		case ETFClassID::CTFProjectile_BallOfFire:
		case ETFClassID::CTFProjectile_SpellFireball:
		case ETFClassID::CTFProjectile_SpellLightningOrb:
		case ETFClassID::CTFProjectile_EnergyBall:
		case ETFClassID::CTFProjectile_Flare:
			pWeapon = pEntity->As<CTFBaseRocket>()->m_hLauncher()->As<CTFWeaponBase>();
			pOwner = pWeapon ? pWeapon->m_hOwner()->As<CTFPlayer>() : nullptr;
			break;
		case ETFClassID::CTFProjectile_EnergyRing:
			pWeapon = pEntity->As<CTFBaseProjectile>()->m_hLauncher()->As<CTFWeaponBase>();
			pOwner = pWeapon ? pWeapon->m_hOwner()->As<CTFPlayer>() : nullptr;
		}
		if (!pOwner || ShouldIgnore(pOwner, m_pLocal, m_pWeapon)
			|| (!SDK::FriendlyFire() || pEntity->GetClassID() == ETFClassID::CTFProjectile_HealingBolt) && pOwner->m_iTeamNum() == pTarget->m_iTeamNum()
			|| pWeapon && !pWeapon->GetDamage())
			continue;

		Vec3 vVelocity = F::ProjSim.GetVelocity(pEntity);
		float flRadius = F::AimbotProjectile.GetSplashRadius(pEntity, pWeapon, pOwner);

		Vec3 vProjectileOrigin = PredictOrigin(pEntity->m_vecOrigin(), vVelocity, flLatency, true, pEntity->m_vecMins(), pEntity->m_vecMaxs());
		if (!TraceToEntity(pTarget, pEntity, vTargetEye, vProjectileOrigin, MASK_SHOT))
			continue;

		int iType = MEDIGUN_BLAST_RESIST;
		float flMult = GetMult(pEntity, pWeapon, pTarget);
		if (flMult == 3.f)
			flMult = flCritMult;
		float flDamage = GetDamage(pEntity, pOwner, pWeapon, pTarget, flMult, vProjectileOrigin, &iType);
		switch (iType)
		{
		case MEDIGUN_BULLET_RESIST: flMult = flBulletResist > 0.f ? 1.f - flBulletResist : flMult + flBulletResist; break;
		case MEDIGUN_BLAST_RESIST: flMult = flBlastResist > 0.f ? 1.f - flBlastResist : flMult + flBlastResist; break;
		case MEDIGUN_FIRE_RESIST: flMult = flFireResist > 0.f ? 1.f - flFireResist : flMult + flFireResist; break;
		}
		flDamage *= flMult * fGetTaken(iType);

		flRadius += vVelocity.Length() * flLatency + pTarget->GetSize().Length() / 2;
		float flDamageDanger = flDamage / iPlayerHealth;
		float flDistanceDanger = Math::RemapVal(pTarget->m_vecOrigin().DistTo(vProjectileOrigin), flRadius, flRadius, 1.f, 0.001f);

		switch (iType)
		{
		case MEDIGUN_BULLET_RESIST:
			flBulletDanger += flDamageDanger * flDistanceDanger;
			break;
		case MEDIGUN_BLAST_RESIST:
			flBlastDanger += flDamageDanger * flDistanceDanger;
			break;
		case MEDIGUN_FIRE_RESIST:
			flFireDanger += flDamageDanger * flDistanceDanger;
		}
	}

	// player_hurt only records damage to us or our heal target
	bool bRecorded = m_flDamagedTime > 0.f && (pTarget->entindex() == I::EngineClient->GetLocalPlayer() || pTarget == m_pWeapon->m_hHealingTarget().Get());
	if (bRecorded)
	{
		switch (m_iDamagedType)
		{
		case MEDIGUN_BULLET_RESIST:
			flBulletDanger += m_flDamagedDPS / iPlayerHealth;
			break;
		case MEDIGUN_BLAST_RESIST:
			flBlastDanger += m_flDamagedDPS / iPlayerHealth;
			break;
		case MEDIGUN_FIRE_RESIST:
			flFireDanger += m_flDamagedDPS / iPlayerHealth;
		}
	}
	if (!(bRecorded && m_iDamagedType == MEDIGUN_FIRE_RESIST) && (pTarget->InCond(TF_COND_BURNING) || pTarget->InCond(TF_COND_BURNING_PYRO)))
		flFireDanger += 8.f * flBurnTaken / iPlayerHealth;
}

void CAutoHeal::GetDangers(CTFPlayer* pTarget, bool bVaccinator, float& flBulletOut, float& flBlastOut, float& flFireOut, float* pMeleeOut)
{
	// our own uber patient keeps their danger, otherwise the beam would hop off them the moment the uber lands
	if (pTarget->IsInvulnerable() && !(m_pWeapon->m_bChargeRelease() && pTarget == m_pWeapon->m_hHealingTarget().Get()))
		return;

	// the expensive part is the same for the vaccinator and heal priority, only compute it once per target per tick
	if (m_iDangerCacheTick != I::GlobalVars->tickcount)
	{
		m_mDangerCache.clear();
		m_iDangerCacheTick = I::GlobalVars->tickcount;
	}
	auto [it, bInserted] = m_mDangerCache.try_emplace(pTarget->entindex());
	if (bInserted)
		ComputeDangers(pTarget, it->second[0], it->second[1], it->second[2], it->second[3]);
	auto [flBulletDanger, flBlastDanger, flFireDanger, flMeleeDanger] = it->second; // copy, the scaling below must not touch the cache
	if (pMeleeOut)
		*pMeleeOut = std::max(*pMeleeOut, flMeleeDanger);

	float flBulletResist = pTarget->InCond(TF_COND_BULLET_IMMUNE) ? 1.f : pTarget->InCond(TF_COND_MEDIGUN_UBER_BULLET_RESIST) ? 0.75f : 0.f;
	float flBlastResist = pTarget->InCond(TF_COND_BLAST_IMMUNE) ? 1.f : pTarget->InCond(TF_COND_MEDIGUN_UBER_BLAST_RESIST) ? 0.75f : 0.f;
	float flFireResist = pTarget->InCond(TF_COND_FIRE_IMMUNE) ? 1.f : pTarget->InCond(TF_COND_MEDIGUN_UBER_FIRE_RESIST) ? 0.75f : 0.f;

	// ignore resistances we are already charged with, else scale
	if (flBulletResist >= (bVaccinator ? 0.75f : 1.f))
		flBulletDanger = 0.f;
	else
		flBulletDanger *= Vars::Aimbot::Healing::AutoVaccinatorBulletScale.Value / 100.f;
	if (flBlastResist >= (bVaccinator ? 0.75f : 1.f))
		flBlastDanger = 0.f;
	else
		flBlastDanger *= Vars::Aimbot::Healing::AutoVaccinatorBlastScale.Value / 100.f;
	if (flFireResist >= (bVaccinator ? 0.75f : 1.f))
		flFireDanger = 0.f;
	else
		flFireDanger *= Vars::Aimbot::Healing::AutoVaccinatorFireScale.Value / 100.f;

	flBulletOut = std::max(flBulletOut, flBulletDanger);
	flBlastOut = std::max(flBlastOut, flBlastDanger);
	flFireOut = std::max(flFireOut, flFireDanger);
}

void CAutoHeal::SwapResistType(CUserCmd* pCmd, int iType)
{
	if (m_iResistType != iType && !(G::LastUserCmd->buttons & IN_RELOAD))
		pCmd->buttons |= IN_RELOAD;
}

void CAutoHeal::ActivateResistType(CUserCmd* pCmd, int iType)
{
	if (m_flChargeLevel >= 0.25f && m_iResistType == iType)
		pCmd->buttons |= IN_ATTACK2;
}

void CAutoHeal::AutoCharge(CUserCmd* pCmd)
{
	if (!Vars::Aimbot::Healing::AutoCharge.Value)
	{
		m_iUberDangerTicks = 0;
		return;
	}

	bool bResist = m_pWeapon->GetMedigunType() == MEDIGUN_RESIST;
	int iChargeType = m_pWeapon->GetChargeType(); // what a pop actually does, set_weapon_mode alone doesn't say
	if (bResist && m_iResistType == -1)
		m_iResistType = m_pWeapon->GetResistType();

	auto pGameRules = I::TFGameRules();
	bool bRoundRunning = !pGameRules || !pGameRules->m_bInSetup() && pGameRules->m_iRoundState() == GR_STATE_RND_RUNNING;
	bool bMvM = pGameRules && pGameRules->m_bPlayingMannVsMachine();

	auto pPatient = m_pWeapon->m_hHealingTarget()->As<CTFPlayer>();
	if (pPatient && Vars::Aimbot::Healing::HealPriority.Value == Vars::Aimbot::Healing::HealPriorityEnum::FriendsOnly
		&& !H::Entities.IsFriend(pPatient->entindex()) && !H::Entities.InParty(pPatient->entindex()))
		pPatient = nullptr;

	// expected damage / health per channel, channels we already resist with a deployed bubble don't count (bVaccinator zeroes them)
	auto fGetDangers = [&](CTFPlayer* pTarget, std::array<float, MEDIGUN_NUM_RESISTS>* pChannels = nullptr)
	{
		float flBullet = 0.f, flBlast = 0.f, flFire = 0.f, flMelee = 0.f;
		GetDangers(pTarget, true, flBullet, flBlast, flFire, &flMelee);
		if (pChannels)
		{
			(*pChannels)[MEDIGUN_BULLET_RESIST] = std::max((*pChannels)[MEDIGUN_BULLET_RESIST], flBullet);
			(*pChannels)[MEDIGUN_BLAST_RESIST] = std::max((*pChannels)[MEDIGUN_BLAST_RESIST], flBlast);
			(*pChannels)[MEDIGUN_FIRE_RESIST] = std::max((*pChannels)[MEDIGUN_FIRE_RESIST], flFire);
		}
		return flBullet + flBlast + flFire + flMelee;
	};
	bool bRecentlyHurt = m_flDamagedTime > 0.f; // player_hurt on us or our patient within the last second

	float flDangerThreshold = Vars::Aimbot::Healing::AutoChargeDanger.Value / 100.f;
	float flHealthThreshold = Vars::Aimbot::Healing::AutoChargeHealth.Value / 100.f;
	if (bMvM && !bResist)
	{
		// hold a full charge for the push unless a giant or tank is actually on the field, chunks regenerate fast enough not to care
		bool bPush = false;
		for (auto pEntity : H::Entities.GetGroup(EntityEnum::PlayerEnemy))
		{
			if (pEntity->As<CTFPlayer>()->m_bIsMiniBoss() && pEntity->As<CTFPlayer>()->IsAlive())
			{
				bPush = true;
				break;
			}
		}
		for (auto pEntity : H::Entities.GetGroup(EntityEnum::WorldNPC))
		{
			if (pEntity->GetClassID() == ETFClassID::CTFTankBoss)
			{
				bPush = true;
				break;
			}
		}
		if (!bPush)
			flDangerThreshold *= 1.5f;
	}

	auto fLowHealth = [&](CTFPlayer* pTarget)
	{
		return pTarget->m_iHealth() < 25 || bRecentlyHurt && pTarget->m_iHealth() <= pTarget->GetMaxHealth() * flHealthThreshold;
	};

	// vaccinator: keep the resist on whatever is hurting us most, pop a chunk when it gets serious
	if (bResist)
	{
		std::array<float, MEDIGUN_NUM_RESISTS> vResistDangers = { 0.f, 0.f, 0.f };
		bool bLowHealth = false;
		fGetDangers(m_pLocal, &vResistDangers), bLowHealth |= !m_pLocal->IsInvulnerable() && fLowHealth(m_pLocal);
		if (pPatient)
			fGetDangers(pPatient, &vResistDangers), bLowHealth |= !pPatient->IsInvulnerable() && fLowHealth(pPatient);

		int iTargetResist = MEDIGUN_BULLET_RESIST; float flTargetDanger = std::numeric_limits<float>::lowest();
		for (int iResist = MEDIGUN_BULLET_RESIST; iResist < MEDIGUN_NUM_RESISTS; iResist++)
		{
			if (float flDanger = vResistDangers[iResist]; flTargetDanger < flDanger)
				iTargetResist = iResist, flTargetDanger = flDanger;
		}
		if (flTargetDanger)
			SwapResistType(pCmd, iTargetResist);
		if (bRoundRunning && m_pLocal->CanAttack(true, true) && (flTargetDanger >= flDangerThreshold || bLowHealth && flTargetDanger > 0.f))
			ActivateResistType(pCmd, iTargetResist);

		if (m_bPreventResistSwap)
			pCmd->buttons &= ~IN_RELOAD;
		if (m_bPreventResistCharge)
			pCmd->buttons &= ~IN_ATTACK2;
		if (pCmd->buttons & IN_RELOAD && !(G::LastUserCmd->buttons & IN_RELOAD))
		{
			m_iResistType = (m_iResistType + 1) % 3;
			m_flSwapTime = I::GlobalVars->curtime + F::Backtrack.GetReal(MAX_FLOWS, false) * 1.5f + 0.1f;
		}
		if (m_flSwapTime < I::GlobalVars->curtime)
			m_iResistType = m_pWeapon->GetResistType();
		m_flChargeLevel = m_pWeapon->m_flChargeLevel();
		return;
	}

	// full charges: uber, kritz, megaheal
	if (iChargeType != MEDIGUN_CHARGE_INVULN && iChargeType != MEDIGUN_CHARGE_CRITICALBOOST && iChargeType != MEDIGUN_CHARGE_MEGAHEAL
		|| m_pWeapon->m_flChargeLevel() < 1.f || m_pWeapon->m_bChargeRelease() || !bRoundRunning || !m_pLocal->CanAttack(true, true))
	{
		m_iUberDangerTicks = 0;
		return;
	}

	// kritzkrieg: nobody is protected, so pop when our patient can hurt something and we aren't about to die
	if (iChargeType == MEDIGUN_CHARGE_CRITICALBOOST)
	{
		m_iUberDangerTicks = 0;
		if (!pPatient || pPatient->IsInvulnerable() || pPatient->IsCritBoosted() || fGetDangers(m_pLocal) >= flDangerThreshold)
			return;
		switch (pPatient->m_iClass())
		{
		case TF_CLASS_SOLDIER: case TF_CLASS_DEMOMAN: case TF_CLASS_HEAVY: case TF_CLASS_PYRO: case TF_CLASS_SCOUT: break;
		default: return;
		}
		if (!pPatient->CanAttack(true, false))
			return;

		Vec3 vPatientEye = pPatient->GetEyePosition();
		for (auto pEntity : H::Entities.GetGroup(EntityEnum::PlayerEnemy))
		{
			auto pEnemy = pEntity->As<CTFPlayer>();
			if (!pEnemy->IsAlive() || pEnemy->IsInvulnerable() || pEnemy->m_vecOrigin().DistTo(pPatient->m_vecOrigin()) > 1500.f)
				continue;
			if (Math::CalcFov(H::Entities.GetEyeAngles(pPatient->entindex()), Math::CalcAngle(vPatientEye, pEnemy->GetCenter())) > 60.f)
				continue;
			if (SDK::VisPosWorld(pPatient, pEnemy, vPatientEye, pEnemy->GetCenter()))
			{
				pCmd->buttons |= IN_ATTACK2;
				return;
			}
		}
		return;
	}

	// uber / quick-fix
	auto fShouldPop = [&](CTFPlayer* pTarget, bool& bPanic)
	{
		if (!pTarget || pTarget->IsInvulnerable() || iChargeType == MEDIGUN_CHARGE_MEGAHEAL && pTarget->InCond(TF_COND_MEGAHEAL))
			return false;
		if (fLowHealth(pTarget))
			return bPanic = true;
		float flDanger = fGetDangers(pTarget);
		if (flDanger >= 3.f) // lethal and then some (sentry buster, crit rocket on a hurt patient), don't wait
			return bPanic = true;
		return flDanger >= flDangerThreshold;
	};

	bool bPop = false, bPanic = false;
	bPop |= fShouldPop(m_pLocal, bPanic);
	if (pPatient)
		bPop |= fShouldPop(pPatient, bPanic);

	// a teammate the beam is about to swing to (danger priority switches at +0.5 over the current patient) also counts
	if (!bPop && Vars::Aimbot::Healing::DangerPriority.Value)
	{
		float flRange = (m_pWeapon->m_pWeaponInfo() ? m_pWeapon->m_pWeaponInfo()->GetWeaponData(TF_WEAPON_PRIMARY_MODE).m_flRange : 450.f) * 1.2f; // GetStickRange
		float flPatientDanger = pPatient ? fGetDangers(pPatient) : 0.f;
		Vec3 vEyePos = m_pLocal->GetShootPos();
		for (auto pEntity : H::Entities.GetGroup(EntityEnum::PlayerTeam))
		{
			auto pTeammate = pEntity->As<CTFPlayer>();
			if (pTeammate == m_pLocal || pTeammate == pPatient || !pTeammate->IsAlive() || pTeammate->IsInvulnerable()
				|| pTeammate->m_vecOrigin().DistTo(m_pLocal->m_vecOrigin()) > flRange
				|| Vars::Aimbot::Healing::HealPriority.Value == Vars::Aimbot::Healing::HealPriorityEnum::FriendsOnly
				&& !H::Entities.IsFriend(pTeammate->entindex()) && !H::Entities.InParty(pTeammate->entindex()))
				continue;
			if (!SDK::VisPosWorld(m_pLocal, pTeammate, vEyePos, pTeammate->GetCenter()))
				continue;

			float flDanger = fGetDangers(pTeammate);
			if (flDanger >= flDangerThreshold && flDanger > flPatientDanger + 0.5f)
			{
				bPop = true;
				break;
			}
		}
	}

	// a single tick of danger is usually a projectile passing by, ask for a few in a row unless it's a panic
	m_iUberDangerTicks = bPop ? m_iUberDangerTicks + 1 : 0;
	if (bPanic || m_iUberDangerTicks >= 3)
		pCmd->buttons |= IN_ATTACK2;
}

void CAutoHeal::Run(CTFPlayer* pLocal, CTFWeaponBase* pWeapon, CUserCmd* pCmd)
{
	if (pWeapon->GetWeaponID() != TF_WEAPON_MEDIGUN)
	{
		bool bAutoSwitch = m_iAutoSwitch != 0;
		if (m_iAutoSwitch == 2 || !(Vars::Aimbot::Healing::AutoArrow.Value && Vars::Aimbot::Healing::AutoCrusaderCrossbowSwitch.Value) && bAutoSwitch)
		{
			I::EngineClient->ClientCmd_Unrestricted("slot2");
			m_iAutoSwitch = 0;
			return;
		}

		m_mMedicCallers.clear();
		m_iResistType = -1;
		m_flDamagedTime = 0.f;
		if (!bAutoSwitch)
			m_iTargetIdx = -1;
		return;
	}

	if (m_iAutoSwitch == 1)
	{
		if (!(Vars::Aimbot::Healing::AutoArrow.Value && Vars::Aimbot::Healing::AutoCrusaderCrossbowSwitch.Value))
			m_iAutoSwitch = 0;
		else
		{
			I::EngineClient->ClientCmd_Unrestricted("slot1");
			return;
		}
	}

	m_pLocal = pLocal, m_pWeapon = pWeapon->As<CWeaponMedigun>();
	AutoHeal(pCmd);
	ActivateOnVoice(pCmd); m_mMedicCallers.clear();
	AutoCharge(pCmd);
	m_flDamagedTime = std::max(m_flDamagedTime - TICK_INTERVAL, 0.f);
}

float CAutoHeal::GetDanger(CTFPlayer* pLocal, CWeaponMedigun* pWeapon, CTFPlayer* pTarget)
{
	if (!pLocal || !pWeapon || !pTarget)
		return 0.f;

	m_pLocal = pLocal, m_pWeapon = pWeapon;
	float flBulletDanger = 0.f, flBlastDanger = 0.f, flFireDanger = 0.f, flMeleeDanger = 0.f;
	GetDangers(pTarget, false, flBulletDanger, flBlastDanger, flFireDanger, &flMeleeDanger);
	return flBulletDanger + flBlastDanger + flFireDanger + flMeleeDanger;
}

void CAutoHeal::Event(IGameEvent* pEvent, uint32_t uHash)
{
	switch (uHash)
	{
	case FNV1A::Hash32Const("player_hurt"):
	{
		//auto pLocal = H::Entities.GetLocal();
		auto pWeapon = H::Entities.GetWeapon()->As<CWeaponMedigun>();
		if (/*!pLocal ||*/ !pWeapon || pWeapon->GetWeaponID() != TF_WEAPON_MEDIGUN)
			return;
		if (!Vars::Aimbot::Healing::DangerPriority.Value && !Vars::Aimbot::Healing::AutoCharge.Value)
			return;

		int iVictim = I::EngineClient->GetPlayerForUserID(pEvent->GetInt("userid"));
		int iAttacker = I::EngineClient->GetPlayerForUserID(pEvent->GetInt("attacker"));
		int iDamage = pEvent->GetInt("damageamount");
		int iWeaponID = pEvent->GetInt("weaponid");
		bool bCrit = pEvent->GetBool("crit");
		bool bMinicrit = pEvent->GetBool("minicrit");

		int iTarget = pWeapon->m_hHealingTarget().GetEntryIndex();
		if (iVictim == iAttacker || iVictim != I::EngineClient->GetLocalPlayer()
			&& (iVictim != iTarget
				|| Vars::Aimbot::Healing::HealPriority.Value == Vars::Aimbot::Healing::HealPriorityEnum::FriendsOnly
				&& !H::Entities.IsFriend(iTarget) && !H::Entities.InParty(iTarget)))
			return;

		auto pEntity = I::ClientEntityList->GetClientEntity(iAttacker)->As<CTFPlayer>();
		if (!pEntity || !pEntity->IsPlayer() /*|| ShouldIgnore(pEntity, pLocal, pWeapon)*/)
			return;

		auto pWeapon2 = pEntity->m_hActiveWeapon()->As<CTFWeaponBase>();
		if (!pWeapon2 || pWeapon2->GetWeaponID() != iWeaponID || pWeapon2->GetFireRate() > 1.f)
			return;

		float flFireRate = pWeapon2->GetFireRate();
		float flMult = SDK::AttribHookValue(1, "mult_dmg", pWeapon2);
		switch (SDK::GetWeaponType(pWeapon2))
		{
		case EWeaponType::HITSCAN:
			m_iDamagedType = MEDIGUN_BULLET_RESIST;
			m_flDamagedDPS = iDamage / flFireRate;
			break;
		case EWeaponType::PROJECTILE:
			switch (iWeaponID)
			{
			case TF_WEAPON_FLAMETHROWER:
				if (!bCrit && !bMinicrit || iDamage / flMult < (bCrit ? 48 : 22))
				{
					m_iDamagedType = MEDIGUN_FIRE_RESIST;
					float flBurnMult = SDK::AttribHookValue(1, "mult_wpn_burndmg", pWeapon2);
					if (!bCrit && !bMinicrit && iDamage <= 4 * flBurnMult)
						m_flDamagedDPS = Vars::Aimbot::Healing::AutoVaccinatorFlamethrowerDamageOnly.Value ? 0.f : 8.f * flBurnMult;
					else
						m_flDamagedDPS = 80.f * flMult * (bCrit ? 3.f : bMinicrit ? 1.36f : 1.f);
				}
				else // better way to assume reflect?
				{
					m_iDamagedType = MEDIGUN_BLAST_RESIST;
					m_flDamagedDPS = iDamage;
				}
				break;
			case TF_WEAPON_COMPOUND_BOW:
			case TF_WEAPON_CROSSBOW:
			case TF_WEAPON_SHOTGUN_BUILDING_RESCUE:
			case TF_WEAPON_SYRINGEGUN_MEDIC:
			case TF_WEAPON_RAYGUN:
				m_iDamagedType = MEDIGUN_BULLET_RESIST;
				m_flDamagedDPS = iDamage / flFireRate;
				break;
			case TF_WEAPON_FLAME_BALL:
			case TF_WEAPON_FLAREGUN:
			case TF_WEAPON_FLAREGUN_REVENGE:
				m_iDamagedType = MEDIGUN_FIRE_RESIST;
				m_flDamagedDPS = iDamage / flFireRate;
				break;
			default:
				m_iDamagedType = MEDIGUN_BLAST_RESIST;
				m_flDamagedDPS = iDamage / flFireRate;
			}
			break;
		default:
			return;
		}
		if (Vars::Aimbot::Healing::AutoVaccinatorFlamethrowerDamageOnly.Value && (iWeaponID != TF_WEAPON_FLAMETHROWER || m_iDamagedType != MEDIGUN_FIRE_RESIST))
			m_flDamagedDPS = 0.f;
		if (!m_flDamagedDPS)
			return;

		m_flDamagedTime = 1.f;
		return;
	}
	case FNV1A::Hash32Const("player_spawn"):
	{
		if (I::EngineClient->GetPlayerForUserID(pEvent->GetInt("userid")) != I::EngineClient->GetLocalPlayer())
			return;

		m_flDamagedTime = 0.f;
	}
	}
}
