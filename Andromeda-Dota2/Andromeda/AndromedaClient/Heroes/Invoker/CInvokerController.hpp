#pragma once

#include <Common/Common.hpp>
#include <vector>
#include <string>

class CDOTAInput;
class CUserCmd;
class C_DOTA_BaseNPC_Hero;
class C_DOTABaseAbility;

class CInvokerController
{
public:
	void OnCreateMove( CDOTAInput* pCDOTAInput , CUserCmd* pCUserCmd );

private:
	struct AbilityEntry
	{
		int slot = -1;
		std::string name;

		bool operator==( const AbilityEntry& other ) const noexcept
		{
			return slot == other.slot && name == other.name;
		}
	};

private:
	bool ResolveLocalHero();
	bool EnsureAbilityOffsets();
	bool RefreshAbilityList();
	void TickLua();
	void LogAbilitiesOnce();

private:
	C_DOTA_BaseNPC_Hero* m_pHero = nullptr;
	uint32_t m_LastHeroHandle = 0xFFFFFFFFu;
	bool m_HeroClassificationComplete = false;
	uint32_t m_AbilityArrayOffset = 0;
	bool m_OffsetsReady = false;
	bool m_LoggedAbilities = false;
	std::vector<AbilityEntry> m_Abilities;
};
