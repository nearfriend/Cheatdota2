#pragma once

#include <Dota2/SDK/Math/Vector3.hpp>

#include <Windows.h>

#include <cstdint>
#include <string>

// Body-blocks your own creep wave on the way to lane, for as long as a key is held.
//
// Plugs the wave's leading edge rather than chasing a creep:
// - Estimates the lane heading from the wave itself
// - Measures the front rank in that frame - how far the leading edge has come,
//   and where across the lane its creeps are
// - Stands a settable standoff in front of that edge, leaning toward whichever
//   side of the rank is furthest along
// - Re-issues the order frequently so the hero keeps walking into the wave
//   instead of stopping on contact
//
// Aiming at the wave rather than at a creep is what keeps the hero steady: the
// leading creep changes every time one gets blocked, and an order that follows
// whoever leads right now swings the hero sideways and opens the gap the rest
// of the wave walks through.
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
	// It is also the anchor for the per-order step clamp.
	//
	// No creep entity is tracked here any more: the hero plugs the wave's
	// leading edge, not any one creep, so there is no target identity to hold
	// on to and no retarget for the clamp to make an exception for.
	struct
	{
		Vector3 heroOrigin{};
		Vector3 blockPoint{};
		bool valid = false;
	} m_Marker;

	// Working lane heading, smoothed across orders. The per-order estimate is
	// noisy enough to swing the whole coordinate frame, and every measurement
	// the feature makes is taken in that frame, so it is filtered once here
	// rather than having each consumer cope with the jitter separately.
	Vector3 m_LaneDirection{};
	bool m_HasLaneDirection = false;

	// The line across the lane the hero is currently covering, kept so the next
	// order can prefer to stay on it. Deliberately the line that was CHOSEN, not
	// the point that was finally clicked: the click gets slid sideways to clear
	// creep models, and feeding that back in would let one slide latch the hero
	// onto ground he never meant to cover.
	float m_HeldLine = 0.f;
	bool m_HasHeldLine = false;
};
