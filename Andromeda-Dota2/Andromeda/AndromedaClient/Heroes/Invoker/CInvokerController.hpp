#pragma once

#include <Common/Common.hpp>
#include <cstdint>
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

	// UI status helpers (Heroes tab).
	std::string GetComboStatus() const { return m_ComboStatus; }

	struct AbilityEntry
	{
		int slot = -1;
		std::string name;

		bool operator==( const AbilityEntry& other ) const noexcept
		{
			return slot == other.slot && name == other.name;
		}
	};

	const std::vector<AbilityEntry>& GetAbilities() const { return m_Abilities; }

private:
	enum class ComboPhase : uint8_t
	{
		Idle,
		PressingOrbs,
		Invoking,
		WaitingForSlot,
		CastingFinal
	};

	struct ComboState
	{
		bool active = false;
		ComboPhase phase = ComboPhase::Idle;
		int spellIndex = -1;
		std::vector<uint16_t> orbKeys;
		size_t orbIndex = 0;
		uint32_t nextActionTick = 0;
		uint32_t phaseDeadline = 0;
		uint16_t finalKey = 0;
	};

private:
	bool ResolveLocalHero();
	bool EnsureAbilityOffsets();
	bool EnsureOriginOffsets();
	bool RefreshAbilityList();
	void TickLua();
	void LogAbilitiesOnce();

	bool StartCombo( int spellIndex );
	void AdvanceCombo();
	void CancelCombo( const char* reason );
	bool ResolveFinalKey( const char* abilityName , uint16_t& outKey ) const;

private:
	C_DOTA_BaseNPC_Hero* m_pHero = nullptr;
	uint32_t m_LastHeroHandle = 0xFFFFFFFFu;
	bool m_HeroClassificationComplete = false;
	uint32_t m_AbilityArrayOffset = 0;
	bool m_OffsetsReady = false;
	bool m_LoggedAbilities = false;
	std::vector<AbilityEntry> m_Abilities;

	uint32_t m_SceneNodeOffset = 0;
	uint32_t m_AbsOriginOffset = 0;
	bool m_OriginOffsetsReady = false;

	ComboState m_Combo{};
	std::string m_ComboStatus = "Idle";
	bool m_ComboKeyWasDown = false;
};
