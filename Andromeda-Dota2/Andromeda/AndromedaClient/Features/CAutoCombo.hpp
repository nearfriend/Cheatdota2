#pragma once

#include <Dota2/SDK/Types/CHandle.hpp>

#include <cstdint>
#include <string>
#include <vector>

// Generic, hero-agnostic burst combo: on a hotkey, fires every off-cooldown,
// affordable damage ability/item (plus optionally an auto-attack) at a manually
// selected target. Unlike CInvokerController this has no per-hero scripting, so it
// only knows about abilities present in the AbilityDamageData catalog (damage
// spells/items) - pure utility/disables without a damage value are not included.
class CAutoCombo final
{
public:
	auto OnRender() -> void;

	std::string GetStatus() const { return m_Status; }

	struct ComboPlanAction
	{
		enum class Kind : uint8_t
		{
			Ability,
			Item,
			Attack
		};

		Kind kind = Kind::Ability;
		std::string name;
		uint16_t key = 0;
		uint32_t delayMs = 0;
		bool noTarget = false;
		bool unitTarget = false;
		bool pointTarget = false;
	};

	struct ComboPlanState
	{
		bool active = false;
		uint32_t nextActionTick = 0;
		uint32_t expiresAt = 0;
		int targetEntIndex = -1;
		size_t actionIndex = 0;
		std::vector<ComboPlanAction> actions;
	};

private:
	auto CancelPlan( const char* reason = nullptr ) -> void;

	ComboPlanState m_Plan{};
	uint32_t m_NextThinkTick = 0;
	bool m_ComboKeyWasDown = false;
	std::string m_Status = "Idle";
};
