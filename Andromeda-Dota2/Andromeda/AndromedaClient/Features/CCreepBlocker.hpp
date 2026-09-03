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
	auto DrawBlockMarker() const -> void;

	OrderPhase m_Phase = OrderPhase::Idle;
	uint32_t m_NextPhaseTick = 0;
	uint32_t m_NextOrderTick = 0;
	uint32_t m_BlockingStartTick = 0;
	POINT m_PreviousCursor{};
	std::string m_Status = "Idle";

	bool m_isCreepBlocking = false;

	// Last block point issued, cached for DrawBlockMarker so the circle and
	// line track it every frame instead of only on the ~60ms order cadence.
	// Also tracks the targeted creep entity so the step clamp can tell a
	// retarget (new entity) from a normal position update (same entity).
	struct
	{
		Vector3 heroOrigin{};
		Vector3 blockPoint{};
		class C_BaseEntity* entity = nullptr;
		bool valid = false;
	} m_Marker;

	// Creep stickiness: don't switch targets too frequently. When actively
	// blocking a creep (hero close to block point), stay with that creep for
	// at least this many orders before reconsidering targets. Prevents the
	// hero from bouncing between multiple creeps in a wave.
	uint32_t m_BlockingOrderCount = 0;
	static constexpr uint32_t kMinOrdersPerTarget = 3;
};
