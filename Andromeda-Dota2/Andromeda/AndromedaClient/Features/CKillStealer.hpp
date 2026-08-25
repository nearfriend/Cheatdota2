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
	auto DrawDebugOverlay() -> void;

	PlanState m_Plan{};
	uint32_t m_NextThinkTick = 0;

	// Cached snapshot of the last think-tick's state, drawn every frame as an
	// on-screen readout when Debug Logs is on - so detection can be verified
	// live in-game instead of only via debug.log after the fact.
	struct DebugReadout
	{
		bool valid = false;
		bool localResolved = false;
		std::string localName;
		// Which resolver picked localName, so a positional guess is visibly
		// distinguishable from an authoritative match.
		std::string localSource;
		std::string nearestEnemyName;
		int nearestEnemyHp = 0;
		int nearestEnemyMaxHp = 0;
		float nearestEnemyDist = -1.f;
		int opposingCount = 0;
		int inRangeCount = 0;
		// Opposing heroes excluded for having no replicated position at all.
		int noPositionCount = 0;
		bool planActive = false;
	};
	DebugReadout m_Debug{};
};
