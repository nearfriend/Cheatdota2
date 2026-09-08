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
// - Stands a settable standoff in front of that edge, on the line across the
//   lane that covers the most of the front rank
// - Moves to the next creep's line the moment it crashes into one, since a
//   stalled creep no longer needs him and the rest of the wave still does
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

	auto RegisterBump( class C_BaseEntity* entity , uint32_t now ) -> void;
	auto IsBumped( const class C_BaseEntity* entity , uint32_t now ) const -> bool;

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

	// The creep whose line the hero is currently covering, kept so the next order
	// can prefer to stay on it.
	//
	// Held as an identity, not a coordinate. Lateral coordinates live in the lane
	// frame, which turns a little every order, so the same line reads hundreds of
	// units different one order later and a stored number is not comparable to a
	// fresh one. A creep is the same creep whatever the frame does. Compared
	// only, never dereferenced.
	class C_BaseEntity* m_HeldLineEntity = nullptr;

	// Which side of the lane the hero is currently cutting across while crashed
	// into a creep: +1 left of the wave's heading, -1 right, 0 before the first
	// contact. Held across orders so he commits to a diagonal instead of
	// reversing it every time the creep's facing wobbles - and reversing mid
	// cut is worse than either direction, since he ends up back where the creep
	// wanted to go.
	int m_CutSide = 0;

	// Creeps the hero has already crashed into, and when. Leaning on one creep
	// only stalls that creep - the rest of the wave walks past while he does it.
	// So contact is the signal to move on: a creep in here is skipped when the
	// next covering line is picked, until its entry ages out and it is worth
	// blocking again. Pointers are only ever compared, never dereferenced, so a
	// creep dying with an entry still in the table is harmless.
	static constexpr int kBumpMemory = 8;
	struct BumpedCreep
	{
		class C_BaseEntity* entity = nullptr;
		uint32_t tick = 0;
	};
	BumpedCreep m_Bumped[kBumpMemory]{};

	// Snapshot of what the last order saw, purely so the overlay can draw it.
	// Kept apart from the working state on purpose: it is written on the order
	// cadence but read every frame, and drawing must never be able to change
	// what the blocker decides.
	static constexpr int kDebugCreeps = 16;
	struct CreepDebug
	{
		Vector3 origin{};
		Vector3 facing{};
		bool hasFacing = false;
		bool contact = false;
		bool bumped = false;
	};
	CreepDebug m_CreepDebug[kDebugCreeps]{};
	int m_CreepDebugCount = 0;
	// Hero position the lane heading was measured from, so the heading arrow can
	// be drawn from where it actually applies.
	Vector3 m_DebugHeroOrigin{};
	bool m_HasDebugFrame = false;
};
