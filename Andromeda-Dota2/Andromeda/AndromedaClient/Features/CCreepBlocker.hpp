#pragma once

#include <Dota2/SDK/Math/Vector3.hpp>

#include <Windows.h>

#include <cstdint>
#include <string>

// Body-blocks your own creep wave on the way to lane, for as long as a key is held.
//
// Maintains continuous forward momentum with the creep wave:
// - Detects creep wave direction and position
// - Continuously repositions hero in front of creeps
// - Issues frequent movement commands to prevent hero from stopping after collision
// - Dynamically adjusts to track creep movement
class CCreepBlocker final
{
public:
	auto OnRender() -> void;

	auto GetStatus() const -> const std::string& { return m_Status; }

private:
	enum class OrderPhase : uint8_t
	{
		Idle,
		Click,
		Restore
	};

	auto AdvanceOrder( uint32_t now ) -> void;
	auto TryIssueBlockOrder( uint32_t now ) -> bool;
	auto ValidateBlockingConditions( uint32_t now ) -> bool;

	OrderPhase m_Phase = OrderPhase::Idle;
	uint32_t m_NextPhaseTick = 0;
	uint32_t m_NextOrderTick = 0;
	uint32_t m_BlockingStartTick = 0;
	POINT m_PreviousCursor{};
	std::string m_Status = "Idle";

	bool m_isCreepBlocking = false;
};
