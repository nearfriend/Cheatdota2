#pragma once

#include <Dota2/SDK/Types/CHandle.hpp>

#include <cstdint>
#include <string>
#include <vector>

class CKillStealer final
{
public:
	auto OnRender() -> void;

	struct KillPlanAction
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

	struct KillPlanState
	{
		bool active = false;
		uint32_t nextActionTick = 0;
		uint32_t expiresAt = 0;
		uint32_t targetHandle = INVALID_EHANDLE_INDEX;
		size_t actionIndex = 0;
		std::vector<KillPlanAction> actions;
	};

private:
	auto CancelPlan() -> void;

	KillPlanState m_Plan{};
	uint32_t m_LastStatusLogTick = 0;
	uint32_t m_LastDebugLogTick = 0;
	uint32_t m_NextThinkTick = 0;
	uint32_t m_NextHeroScanTick = 0;
	int m_NextHeroScanIndex = 0;
	std::vector<CHandle> m_HeroHandles;
	int m_LastScanEntities = 0;
	int m_LastScanChunks = 0;
	int m_LastScanControllers = 0;
	int m_LastScanDirectHeroes = 0;
};
