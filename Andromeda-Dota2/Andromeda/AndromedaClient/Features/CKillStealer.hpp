#pragma once

#include <Dota2/SDK/Types/CHandle.hpp>

#include <cstdint>
#include <vector>

class CKillStealer final
{
public:
	auto OnRender() -> void;

private:
	uint32_t m_LastTargetHandle = INVALID_EHANDLE_INDEX;
	uint32_t m_LastCastTick = 0;
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
