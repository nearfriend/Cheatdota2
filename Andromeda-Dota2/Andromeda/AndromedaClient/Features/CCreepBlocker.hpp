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

	auto RegisterEscaped( class C_BaseEntity* entity ) -> void;
	auto IsEscaped( const class C_BaseEntity* entity ) const -> bool;

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

	// Which side of the covering line the hero is currently sweeping toward:
	// +1 left of the wave's heading, -1 right. Flips every time he reaches the
	// current side, which is what makes the path a zigzag instead of a straight
	// walk down the lane in front of the wave.
	//
	// A straight line only ever blocks the creeps directly behind him; the ones
	// either side walk past untouched. Sweeping across their front drags his
	// hull through the whole width of the wave, so every creep meets it in turn
	// - which is how the block is done by hand.
	int m_ZigSide = 1;
	// When the current side was taken. Only a backstop: normally the side flips
	// on arrival, but if the hero cannot reach the target - blocked, slowed,
	// stuck on terrain - this stops him leaning against one side forever.
	uint32_t m_ZigFlipTick = 0;

	// Contact state last order, so a fresh crash can be told from still being
	// in contact. The sweep flips on the rising edge only - flipping while he
	// stays in contact would reverse him every order and he would vibrate in
	// place instead of crossing the wave.
	bool m_WasCrashed = false;
	// Set on that rising edge, read by OnRender to send the next order with no
	// delay at all: the moment he touches a creep, the order carrying him the
	// other way should already be going out.
	bool m_FreshCrash = false;

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

	// Creeps that have got away for good. Once one is past the hero by more than
	// kEscapeTolerance it is written here and ignored from then on - it does not
	// count as the front-most creep, it does not set the wave's width, and it
	// never earns a place in the rank.
	//
	// Sticky on purpose, unlike the distance test it replaces. A plain
	// "is it more than N units ahead right now" check lets a creep drift back
	// into the set whenever the heading estimate wobbles a few degrees, and the
	// hero then turns to chase something he had already written off. It stays
	// escaped until the key is released.
	//
	// Escaped creeps are still avoided when the click point is placed - the
	// model is in the way of the cursor whether the blocker cares about the
	// creep or not.
	//
	// Pointers are only ever compared, never dereferenced, so a creep dying with
	// an entry still in the table is harmless.
	static constexpr int kEscapeMemory = 24;
	class C_BaseEntity* m_Escaped[kEscapeMemory]{};
	int m_EscapedCount = 0;

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

	// Debug view of the two rules, so they can be watched on screen instead of
	// reconstructed from the log afterwards. Written on the order cadence,
	// read every frame; drawing must never be able to change what the blocker
	// decides, so none of this is read back by the logic.
	struct
	{
		// RULE 1: the creep the block point has to stay in front of, and
		// whether it actually is. Non-zero pastCount is the rule broken.
		Vector3 frontMostOrigin{};
		bool hasFrontMost = false;
		int pastCount = 0;
		// RULE 2: the creep about to inherit the lead, and whether the diagonal
		// is currently aimed at it.
		Vector3 nextLeaderOrigin{};
		bool hasNextLeader = false;
		bool aimedAtNext = false;
		// The diagonal actually ordered, for drawing the angle.
		float legForward = 0.f;
		float legSide = 0.f;
		float angleDegrees = 0.f;
		bool crashed = false;
	} m_RuleDebug;
};
