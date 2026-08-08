#pragma once

#include <Common/Common.hpp>
#include <Dota2/SDK/Update/AbilityOffsets.hpp>
#include <vector>
#include <string>

class CDOTAInput;
class CUserCmd;
class C_DOTA_BaseNPC_Hero;
class C_DOTABaseAbility;
class CEntityInstance;

struct MeepoAbilityInfo
{
	std::string name;
	C_DOTABaseAbility* pAbility = nullptr;
	int level = 0;
	float cooldown = 0.0f;
	float cooldownLength = 0.0f;
	int manaCost = 0;
	float castRange = 0.0f;
	bool isActivated = false;
};

class CMeepoController
{
public:
	void OnCreateMove( CDOTAInput* pCDOTAInput , CUserCmd* pCUserCmd );
	
	// Get ability information for menu display
	const std::vector<MeepoAbilityInfo>& GetAbilities() const { return m_Abilities; }
	bool IsHeroResolved() const { return m_pHero != nullptr; }
	
	// Called from Hook_OnAddEntity when a hero entity is added
	void OnEntityAdded( CEntityInstance* pInst );
	// Called from Hook_OnAddEntity when an ability entity is added
	void OnAbilityAdded( CEntityInstance* pInst );

private:
	bool ResolveLocalHero();
	bool ResolveHeroByEntitySearch();
	bool RefreshAbilityList();

private:
	C_DOTA_BaseNPC_Hero* m_pHero = nullptr;
	uint32_t m_LastHeroHandle = 0xFFFFFFFFu;
	bool m_HeroClassificationComplete = false;
	std::vector<MeepoAbilityInfo> m_Abilities;

	uint32_t m_vecAbilitiesOffset = BaseNPCOffsets::m_vecAbilities;
	uint32_t m_iHeroIDOffset = 0x2A8; // Common offset for hero ID

	static constexpr int kMaxAbilitySlots = 24;
};

