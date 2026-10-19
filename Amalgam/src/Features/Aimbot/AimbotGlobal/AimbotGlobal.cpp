#include "AimbotGlobal.h"

#include "../Aimbot.h"
#include "../../Players/PlayerUtils.h"
#include "../../Ticks/Ticks.h"
#include "../../EnginePrediction/EnginePrediction.h"
#include "../AutoHeal/AutoHeal.h"

static int GetPriorityIndex(CTFWeaponBase* LocalWeapon)
{
	if (LocalWeapon->GetSlot() != SLOT_MELEE)
	{
		if (F::AutoHeal.m_iAutoSwitch)
			return F::AutoHeal.m_iTargetIdx;
	}

	return -1;
}

std::vector<Target_t> CAimbotGlobal::ManageTargets(std::vector<Target_t>(*GetTargets)(CTFPlayer* pLocal, CTFWeaponBase* pWeapon), CTFPlayer* LocalPlayer, CTFWeaponBase* LocalWeapon, const int SortMethod, const int MaxTargets)
{
	auto ValidTargets = GetTargets(LocalPlayer, LocalWeapon);
	SortTargetsPre(ValidTargets, SortMethod);

	const auto PriorityIndex = GetPriorityIndex(LocalWeapon);
	if (PriorityIndex > 0) // not a strict weak ordering for sort, partition instead
		std::ranges::stable_partition(ValidTargets, [&](const Target_t& a) -> bool { return a.m_pEntity->entindex() == PriorityIndex; });

	ValidTargets.resize(std::min(static_cast<size_t>(MaxTargets), ValidTargets.size()));
	if (PriorityIndex <= 0)
		SortTargetsPost(ValidTargets, SortMethod);

	return ValidTargets;
}

namespace HealTune
{
	constexpr float HURT_BASE = 1000.0f;
	constexpr float HURT_RANGE = 800.0f;
	constexpr float HURT_CLASS_TIEBREAK = 150.0f;
	constexpr float HURT_DEADBAND_FRAC = 0.05f;
	constexpr float LOW_HP_BONUS = 400.0f;
	constexpr float LOW_HP_FRAC = 0.40f;
	constexpr float OVERHEAL_BASE = 600.0f;
	constexpr float OVERHEAL_ROOM_RANGE = 300.0f;
	constexpr float FINISH_BONUS = 250.0f;
	constexpr float FINISH_WINDOW_SECS = 1.5f;
	constexpr float CURRENT_PATIENT_BONUS = 60.0f;
	constexpr float DANGER_RANGE = 800.0f; // per 1.0 of expected damage / health, capped at 2.0
	constexpr float DANGER_CAP = 2.0f;
}

static float GetClassHealWeight(int iClass)
{
	switch (iClass)
	{
	case TF_CLASS_HEAVY: return 1.00f;
	case TF_CLASS_DEMOMAN:      return 0.90f;
	case TF_CLASS_SOLDIER:      return 0.85f;
	case TF_CLASS_PYRO:         return 0.60f;
	case TF_CLASS_SCOUT:        return 0.50f;
	case TF_CLASS_ENGINEER:     return 0.40f;
	case TF_CLASS_SNIPER:       return 0.35f;
	case TF_CLASS_SPY:          return 0.30f;
	case TF_CLASS_MEDIC:        return 0.30f;
	default:                    return 0.25f;
	}
}

// TODO: account for all types of healing, including rescue ranger for buildings
float CAimbotGlobal::GetHealPriority(const CTFPlayer* LocalPlayer, CTFWeaponBase* LocalWeapon, CBaseEntity* Target)
{
	if (!LocalPlayer || !LocalWeapon || !Target)
		return -1.0f;

	const auto LocalMedigun = LocalWeapon->As<CWeaponMedigun>();
	const auto IsVaccinator = LocalMedigun && LocalMedigun->GetMedigunType() == MEDIGUN_RESIST;

	if (Target->IsPlayer())
	{
		const auto Player = Target->As<CTFPlayer>();
		if (Player->deadflag() || Player->IsAGhost() || Player->InCond(TF_COND_PHASE))
			return -1.0f;

		const auto CurrentHealth = Player->m_iHealth();
		const auto MaxHealth = Player->GetMaxHealth();
		const auto PlayerResource = H::Entities.GetResource();
		const auto MaxBuffedHealth = LocalMedigun ? LocalMedigun->GetMaxBuffedHealthForTarget(Player) : PlayerResource->m_iMaxBuffedHealth(Player->entindex());
		const auto MissingHealthFraction = static_cast<float>(MaxHealth - CurrentHealth) / static_cast<float>(MaxHealth);

		auto Score = 0.0f;

		if (MissingHealthFraction > HealTune::HURT_DEADBAND_FRAC)
		{
			Score = HealTune::HURT_BASE + HealTune::HURT_RANGE * MissingHealthFraction + HealTune::HURT_CLASS_TIEBREAK * GetClassHealWeight(Player->m_iClass());

			if (static_cast<float>(CurrentHealth) / static_cast<float>(MaxHealth) < HealTune::LOW_HP_FRAC)
				Score += HealTune::LOW_HP_BONUS;
		}
		else if (CurrentHealth < MaxBuffedHealth)
		{
			const auto OverhealRoom = static_cast<float>(MaxBuffedHealth - CurrentHealth) / static_cast<float>(MaxBuffedHealth - MaxHealth);
			Score = HealTune::OVERHEAL_BASE * GetClassHealWeight(Player->m_iClass()) + HealTune::OVERHEAL_ROOM_RANGE * OverhealRoom;
		}
		else
			return 0.0f;

		if (Vars::Aimbot::Healing::DangerPriority.Value && LocalMedigun)
			Score += HealTune::DANGER_RANGE * std::min(F::AutoHeal.GetDanger(const_cast<CTFPlayer*>(LocalPlayer), LocalMedigun, Player), HealTune::DANGER_CAP);

		if (LocalMedigun && Target == LocalMedigun->m_hHealingTarget().Get())
		{
			const auto TargetHealth = MissingHealthFraction > HealTune::HURT_DEADBAND_FRAC ? MaxHealth : MaxBuffedHealth;
			const auto SecondsLeft = static_cast<float>(TargetHealth - CurrentHealth) / LocalMedigun->GetHealRateForTarget(Player);
			const auto Seconds = std::clamp(1.0f - SecondsLeft / HealTune::FINISH_WINDOW_SECS, 0.0f, 1.0f);

			Score += HealTune::CURRENT_PATIENT_BONUS + HealTune::FINISH_BONUS * Seconds;
			//Score += HealTune::FINISH_BONUS * Seconds;
		}

		return Score;
	}

	if (Target->GetClassID() == ETFClassID::CTFReviveMarker)
		return 10000000.0f;

	return 0.0f;
}

void CAimbotGlobal::SortTargetsPre(std::vector<Target_t>& vTargets, int iMethod)
{
	switch (iMethod)
	{
	case Vars::Aimbot::General::TargetSelectionEnum::FOV:
		std::sort(vTargets.begin(), vTargets.end(), [&](const Target_t& a, const Target_t& b) -> bool
		{
			return a.m_flFOVTo < b.m_flFOVTo;
		});
		break;
	case Vars::Aimbot::General::TargetSelectionEnum::Distance:
	case Vars::Aimbot::General::TargetSelectionEnum::Hybrid:
		std::sort(vTargets.begin(), vTargets.end(), [&](const Target_t& a, const Target_t& b) -> bool
		{
			return a.m_flDistTo < b.m_flDistTo;
		});
		break;
	case Vars::Aimbot::General::TargetSelectionEnum::HealPriority:
		std::sort(vTargets.begin(), vTargets.end(), [&](const Target_t& a, const Target_t& b) -> bool
		{
			return GetHealPriority(H::Entities.GetLocal(), H::Entities.GetWeapon(), a.m_pEntity) > GetHealPriority(H::Entities.GetLocal(), H::Entities.GetWeapon(), b.m_pEntity);
		});
		break;
	}
}

void CAimbotGlobal::SortTargetsPost(std::vector<Target_t>& vTargets, int iMethod)
{
	switch (iMethod)
	{
	case Vars::Aimbot::General::TargetSelectionEnum::Hybrid:
		std::sort(vTargets.begin(), vTargets.end(), [&](const Target_t& a, const Target_t& b) -> bool
		{
			return a.m_flFOVTo < b.m_flFOVTo;
		});
		break;
	}

	std::stable_sort(vTargets.begin(), vTargets.end(), [&](const Target_t& a, const Target_t& b) -> bool // keep the pre-sort order within equal priorities
	{
		return a.m_nPriority > b.m_nPriority;
	});
}

float CAimbotGlobal::GetAimFOV()
{	// restrict now vs later
	return Vars::Aimbot::General::LeadAndRestrict.Value ? 180.f : Vars::Aimbot::General::AimFOV.Value;
}

bool CAimbotGlobal::EntityCenterInFOV(CBaseEntity* pTarget, const Vec3& vLocalPos, const Vec3& vLocalAngles, float& flFOVTo, Vec3& vPos, Vec3& vAngleTo)
{
	vPos = pTarget->GetCenter();
	vAngleTo = Math::CalcAngle(vLocalPos, vPos);
	flFOVTo = Math::CalcFov(vLocalAngles, vAngleTo);

	return flFOVTo < GetAimFOV();
}

bool CAimbotGlobal::PlayerBoneInFOV(CTFPlayer* pTarget, const Vec3& vLocalPos, const Vec3& vLocalAngles, float& flFOVTo, Vec3& vPos, Vec3& vAngleTo, int iHitboxes)
{
	if (pTarget->IsDormant())
		return EntityCenterInFOV(pTarget, vLocalPos, vLocalAngles, flFOVTo, vPos, vAngleTo);

	matrix3x4* aBones = F::Backtrack.GetBones(pTarget);
	if (!Vars::Visuals::Removals::Interpolation.Value)
	{
		std::vector<TickRecord*> vRecords = {};
		if (F::Backtrack.GetRecords(pTarget, vRecords) && !vRecords.empty())
		{
			float flLerp = Vars::Visuals::Removals::Lerp.Value ? 0.f : G::Lerp;
			for (auto pRecord : vRecords)
			{
				if (F::EnginePrediction.m_flOldCurrentTime - pRecord->m_flSimTime > flLerp)
				{
					aBones = pRecord->m_aBones;
					break;
				}
			}
		}
	}
	if (!aBones)
		return false;

	float flMinFOV = 180.f;
	for (int nHitbox = 0; nHitbox < pTarget->GetNumOfHitboxes(); nHitbox++)
	{
		if (!IsHitboxValid(pTarget, nHitbox, iHitboxes))
			continue;

		Vec3 vCurPos = pTarget->GetHitboxCenter(aBones, nHitbox);
		Vec3 vCurAngleTo = Math::CalcAngle(vLocalPos, vCurPos);
		float flCurFOVTo = Math::CalcFov(vLocalAngles, vCurAngleTo);

		if (flCurFOVTo < flMinFOV)
		{
			vPos = vCurPos;
			vAngleTo = vCurAngleTo;
			flFOVTo = flMinFOV = flCurFOVTo;
		}
	}

	return flMinFOV < GetAimFOV();
}

bool CAimbotGlobal::IsHitboxValid(CBaseEntity* pEntity, int nHitbox, int iHitboxes)
{
	switch (pEntity->GetHitboxToBase(nHitbox))
	{
	case -1: return true;
	case HITBOX_HEAD: return iHitboxes & Vars::Aimbot::Hitscan::HitboxesEnum::Head;
	case HITBOX_SPINE0:
	case HITBOX_SPINE1:
	case HITBOX_SPINE2:
	case HITBOX_SPINE3: return iHitboxes & Vars::Aimbot::Hitscan::HitboxesEnum::Body;
	case HITBOX_PELVIS: return iHitboxes & Vars::Aimbot::Hitscan::HitboxesEnum::Pelvis;
	case HITBOX_LEFT_UPPERARM:
	case HITBOX_LEFT_FOREARM:
	case HITBOX_LEFT_HAND:
	case HITBOX_RIGHT_UPPERARM:
	case HITBOX_RIGHT_FOREARM:
	case HITBOX_RIGHT_HAND: return iHitboxes & Vars::Aimbot::Hitscan::HitboxesEnum::Arms;
	case HITBOX_LEFT_THIGH:
	case HITBOX_LEFT_CALF:
	case HITBOX_LEFT_FOOT:
	case HITBOX_RIGHT_THIGH:
	case HITBOX_RIGHT_CALF:
	case HITBOX_RIGHT_FOOT: return iHitboxes & Vars::Aimbot::Hitscan::HitboxesEnum::Legs;
	}

	return false;
}

bool CAimbotGlobal::IsHitboxValid(int nHitbox, int iHitboxes)
{
	switch (nHitbox)
	{
	case BOUNDS_HEAD: return iHitboxes & Vars::Aimbot::Projectile::HitboxesEnum::Head;
	case BOUNDS_BODY: return iHitboxes & Vars::Aimbot::Projectile::HitboxesEnum::Body;
	case BOUNDS_FEET: return iHitboxes & Vars::Aimbot::Projectile::HitboxesEnum::Feet;
	}

	return false;
}

bool CAimbotGlobal::ShouldMultipoint(CBaseEntity* pEntity, int nHitbox, int iHitboxes)
{
	if (Vars::Aimbot::Hitscan::MultipointScale.Value <= 0.f)
		return false;

	if (!iHitboxes)
		return true;

	switch (pEntity->GetHitboxToBase(nHitbox))
	{
	case HITBOX_HEAD: return iHitboxes & Vars::Aimbot::Hitscan::HitboxesEnum::Head;
	case HITBOX_SPINE0:
	case HITBOX_SPINE1:
	case HITBOX_SPINE2:
	case HITBOX_SPINE3:
	case HITBOX_PELVIS: return iHitboxes & Vars::Aimbot::Hitscan::HitboxesEnum::Body;
	case HITBOX_LEFT_UPPERARM:
	case HITBOX_LEFT_FOREARM:
	case HITBOX_LEFT_HAND:
	case HITBOX_RIGHT_UPPERARM:
	case HITBOX_RIGHT_FOREARM:
	case HITBOX_RIGHT_HAND: return iHitboxes & Vars::Aimbot::Hitscan::HitboxesEnum::Arms;
	case HITBOX_LEFT_THIGH:
	case HITBOX_LEFT_CALF:
	case HITBOX_LEFT_FOOT:
	case HITBOX_RIGHT_THIGH:
	case HITBOX_RIGHT_CALF:
	case HITBOX_RIGHT_FOOT: return iHitboxes & Vars::Aimbot::Hitscan::HitboxesEnum::Legs;
	}

	return false;
}

bool CAimbotGlobal::ShouldAimAtAngle(Vec3 vAngles)
{
	if (!Vars::Aimbot::General::LeadAndRestrict.Value)
		return true;

	return Math::CalcFov(I::EngineClient->GetViewAngles(), vAngles) < Vars::Aimbot::General::AimFOV.Value;
}

bool CAimbotGlobal::ShouldIgnore(CBaseEntity* pEntity, CTFPlayer* pLocal, CTFWeaponBase* pWeapon, int iFunctionFlags, int iTargetFlags, int iIgnoreFlags)
{
	if (iFunctionFlags & ShouldIgnoreEnum::Dormant ? pEntity->IsDormant() : !H::Entities.GetDormancy(pEntity->entindex()))
		return true;

	if (auto pGameRules = I::TFGameRules())
	{
		if (pGameRules->m_bTruceActive() && (SDK::FriendlyFire() || pLocal->m_iTeamNum() != pEntity->m_iTeamNum()))
			return true;
	}

	switch (pEntity->GetClassID())
	{
	case ETFClassID::CTFPlayer:
	{
		auto pPlayer = pEntity->As<CTFPlayer>();
		if (pPlayer == pLocal || !pPlayer->IsAlive() || pPlayer->IsAGhost())
			return true;

		if (!SDK::FriendlyFire() && pLocal->m_iTeamNum() == pEntity->m_iTeamNum())
			return false;

		if (iFunctionFlags & ShouldIgnoreEnum::Ignored && F::PlayerUtils.IsIgnored(pPlayer->entindex())
			|| iIgnoreFlags & Vars::Aimbot::General::IgnoreEnum::Unprioritized && !F::PlayerUtils.IsPrioritized(pPlayer->entindex()))
			return true;

		if (iIgnoreFlags & Vars::Aimbot::General::IgnoreEnum::Friends && H::Entities.IsFriend(pPlayer->entindex())
			|| iIgnoreFlags & Vars::Aimbot::General::IgnoreEnum::Party && H::Entities.InParty(pPlayer->entindex())
			|| iIgnoreFlags & Vars::Aimbot::General::IgnoreEnum::Invulnerable && pPlayer->IsInvulnerable() && SDK::AttribHookValue(0, "crit_forces_victim_to_laugh", pWeapon) <= 0
			|| iIgnoreFlags & Vars::Aimbot::General::IgnoreEnum::Invisible && pPlayer->IsInvisible(Vars::Aimbot::General::IgnoreInvisible.Value / 100.f)
			|| iIgnoreFlags & Vars::Aimbot::General::IgnoreEnum::DeadRinger && pPlayer->m_bFeignDeathReady()
			|| iIgnoreFlags & Vars::Aimbot::General::IgnoreEnum::Taunting && pPlayer->IsTaunting()
			|| iIgnoreFlags & Vars::Aimbot::General::IgnoreEnum::Disguised && pPlayer->InCond(TF_COND_DISGUISED))
			return true;
		if (iIgnoreFlags & Vars::Aimbot::General::IgnoreEnum::Vaccinator)
		{
			switch (G::PrimaryWeaponType)
			{
			case EWeaponType::HITSCAN:
				if (pPlayer->InCond(TF_COND_MEDIGUN_UBER_BULLET_RESIST) && SDK::AttribHookValue(0, "mod_pierce_resists_absorbs", pWeapon) != 0)
					return true;
				break;
			case EWeaponType::PROJECTILE:
				switch (pWeapon->GetWeaponID())
				{
				case TF_WEAPON_FLAMETHROWER:
				case TF_WEAPON_FLAREGUN:
					if (pPlayer->InCond(TF_COND_MEDIGUN_UBER_FIRE_RESIST))
						return true;
					break;
				case TF_WEAPON_COMPOUND_BOW:
					if (pPlayer->InCond(TF_COND_MEDIGUN_UBER_BULLET_RESIST))
						return true;
					break;
				default:
					if (pPlayer->InCond(TF_COND_MEDIGUN_UBER_BLAST_RESIST))
						return true;
				}
			}
		}

		return false;
	}
	case ETFClassID::CObjectSentrygun:
	case ETFClassID::CObjectDispenser:
	case ETFClassID::CObjectTeleporter:
	{
		auto pBuilding = pEntity->As<CBaseObject>();
		if (pLocal->m_iTeamNum() == pBuilding->m_iTeamNum())
			return false;

		if (!(iTargetFlags & Vars::Aimbot::General::TargetEnum::Sentry) && pBuilding->IsSentrygun()
			|| !(iTargetFlags & Vars::Aimbot::General::TargetEnum::Dispenser) && pBuilding->IsDispenser()
			|| !(iTargetFlags & Vars::Aimbot::General::TargetEnum::Teleporter) && pBuilding->IsTeleporter())
			return true;

		if (auto pOwner = pBuilding->m_hBuilder().Get())
		{
			if (iFunctionFlags & ShouldIgnoreEnum::Ignored && F::PlayerUtils.IsIgnored(pOwner->entindex()))
				return true;

			if (iIgnoreFlags & Vars::Aimbot::General::IgnoreEnum::Friends && H::Entities.IsFriend(pOwner->entindex())
				|| iIgnoreFlags & Vars::Aimbot::General::IgnoreEnum::Party && H::Entities.InParty(pOwner->entindex()))
				return true;
		}

		return false;
	}
	case ETFClassID::CTFGrenadePipebombProjectile:
	{
		auto pProjectile = pEntity->As<CTFGrenadePipebombProjectile>();

		if (!(iTargetFlags & Vars::Aimbot::General::TargetEnum::Stickies))
			return true;

		if (pLocal->m_iTeamNum() == pEntity->m_iTeamNum())
			return true;

		if (auto pOwner = pProjectile->m_hThrower().Get();
			iFunctionFlags& ShouldIgnoreEnum::Ignored && pOwner && F::PlayerUtils.IsIgnored(pOwner->entindex()))
			return true;

		if (pProjectile->m_iType() != TF_GL_MODE_REMOTE_DETONATE || !pProjectile->m_bTouched())
			return true;

		return false;
	}
	case ETFClassID::CTFBaseBoss:
	case ETFClassID::CTFTankBoss:
	case ETFClassID::CMerasmus:
	case ETFClassID::CEyeballBoss:
	case ETFClassID::CHeadlessHatman:
	case ETFClassID::CZombie:
	{
		if (!(iTargetFlags & Vars::Aimbot::General::TargetEnum::NPCs))
			return true;

		if (pEntity->GetClassID() == ETFClassID::CEyeballBoss
			? pLocal->m_iTeamNum() != TF_TEAM_HALLOWEEN
			: pLocal->m_iTeamNum() == pEntity->m_iTeamNum())
			return true;

		return false;
	}
	case ETFClassID::CTFGenericBomb:
	case ETFClassID::CTFPumpkinBomb:
	{
		if (!(iTargetFlags & Vars::Aimbot::General::TargetEnum::Bombs))
			return true;

		if (!ValidBomb(pLocal, pWeapon, pEntity))
			return true;

		return false;
	}
	}

	return true;
}

int CAimbotGlobal::GetPriority(int iIndex)
{
	return F::PlayerUtils.GetPriority(iIndex);
}

bool CAimbotGlobal::ShouldAim()
{
	switch (Vars::Aimbot::General::AimType.Value)
	{
	case Vars::Aimbot::General::AimTypeEnum::Plain:
	case Vars::Aimbot::General::AimTypeEnum::Silent:
		if (!G::CanPrimaryAttack && !G::Reloading && !F::Ticks.IsTimingUnsure())
			return false;
	}

	return true;
}

bool CAimbotGlobal::ShouldHoldAttack(CTFWeaponBase* pWeapon)
{
	switch (Vars::Aimbot::General::AimHoldsFire.Value)
	{
	case Vars::Aimbot::General::AimHoldsFireEnum::MinigunOnly:
		if (pWeapon->GetWeaponID() != TF_WEAPON_MINIGUN)
			break;
		[[fallthrough]];
	case Vars::Aimbot::General::AimHoldsFireEnum::Always:
		if (!(G::LastUserCmd->buttons & IN_ATTACK) || !Vars::Aimbot::General::AimType.Value || F::Aimbot.m_bRunningSecondary)
			break;
		if (!G::CanPrimaryAttack && !pWeapon->IsInReload() || pWeapon->GetWeaponID() == TF_WEAPON_GRAPPLINGHOOK && pWeapon->As<CTFGrapplingHook>()->m_hProjectile())
			return true;
	}
	return false;
}

// will not predict for projectile weapons
bool CAimbotGlobal::ValidBomb(CTFPlayer* pLocal, CTFWeaponBase* pWeapon, CBaseEntity* pBomb)
{
	Vec3 vOrigin = pBomb->m_vecOrigin();
	float flRadiusSqr = powf(300.f, 2);

	CBaseEntity* pEntity;
	for (CEntitySphereQuery sphere(vOrigin, 300.f);
		pEntity = sphere.GetCurrentEntity();
		sphere.NextEntity())
	{
		if (pEntity == pLocal || pEntity->IsPlayer() && (!pEntity->As<CTFPlayer>()->IsAlive() || pEntity->As<CTFPlayer>()->IsAGhost())
			|| !SDK::FriendlyFire() && pEntity->m_iTeamNum() == pLocal->m_iTeamNum())
			continue;

		Vec3 vPos; pEntity->m_Collision()->CalcNearestPoint(vOrigin, &vPos);
		if (vOrigin.DistToSqr(vPos) > flRadiusSqr)
			continue;

		if (pEntity->IsPlayer() || pEntity->IsBuilding() || pEntity->IsNPC())
		{
			if (ShouldIgnore(pEntity, pLocal, pWeapon))
				continue;

			if (!SDK::VisPosCollideable(pBomb, pEntity, vOrigin, pEntity->IsPlayer() ? pEntity->m_vecOrigin() + pEntity->As<CTFPlayer>()->GetViewOffset() : pEntity->GetCenter(), MASK_SHOT))
				continue;

			return true;
		}
	}

	return false;
}