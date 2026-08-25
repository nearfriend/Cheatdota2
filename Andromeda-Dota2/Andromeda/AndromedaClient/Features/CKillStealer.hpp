#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Automated kill-secure: on a lethal target, casts the fewest off-cooldown,
// affordable abilities/items needed to finish it (see CKillStealer.cpp).
class CKillStealer final
{
public:
	auto OnRender() -> void;

	struct PlanAction
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
		bool isDamageAmplifier = false;
	};

	struct PlanState
	{
		bool active = false;
		uint32_t nextActionTick = 0;
		uint32_t expiresAt = 0;
		int targetEntIndex = -1;
		size_t actionIndex = 0;
		std::vector<PlanAction> actions;
	};

private:
	auto OnRenderInner() -> void;
	auto CancelPlan() -> void;

	PlanState m_Plan{};
	uint32_t m_NextThinkTick = 0;
};
