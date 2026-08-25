#pragma once

#include <Dota2/SDK/Types/CHandle.hpp>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

// Generic, hero-agnostic burst combo: on a hotkey, fires every off-cooldown,
// affordable damage ability/item (plus optionally an auto-attack) at the nearest
// enemy hero (or a manually pinned entindex, if TargetEntIndex >= 0). Unlike
// CInvokerController this has no per-hero scripting, so it only knows about
// abilities present in the AbilityDamageData catalog (damage spells/items) -
// pure utility/disables without a damage value are not included.
class CAutoCombo final
{
public:
	auto OnRender() -> void;

	std::string GetStatus() const { return m_Status; }

	// Live ability name per slot (0..5 = Q W E D F R) of the local hero, for
	// the menu's spell-order editor. Empty string = slot unknown/unresolved.
	// Refreshed by OnRender (same render thread as the menu, no locking).
	const std::array<std::string , 6>& GetSlotNames() const { return m_SlotNames; }

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

	// Targeted casts span three think ticks (aim -> cast -> restore) instead of
	// one call: Dota consumes injected input asynchronously, so the cursor must
	// still be sitting on the target when the game actually processes the
	// key/click - moving and instantly restoring it made every unit/point-target
	// spell resolve at the old cursor position.
	enum class CastPhase : uint8_t
	{
		Aim,
		Cast,
		Restore
	};

	struct ComboPlanState
	{
		bool active = false;
		uint32_t nextActionTick = 0;
		uint32_t expiresAt = 0;
		int targetEntIndex = -1;
		size_t actionIndex = 0;
		CastPhase castPhase = CastPhase::Aim;
		int32_t prevCursorX = 0;
		int32_t prevCursorY = 0;
		bool hasPrevCursor = false;
		std::vector<ComboPlanAction> actions;
	};

private:
	auto CancelPlan( const char* reason = nullptr ) -> void;

	ComboPlanState m_Plan{};
	uint32_t m_NextThinkTick = 0;
	uint32_t m_NextSlotNameRefresh = 0;
	bool m_ComboKeyWasDown = false;
	std::string m_Status = "Idle";
	std::array<std::string , 6> m_SlotNames{};
};
