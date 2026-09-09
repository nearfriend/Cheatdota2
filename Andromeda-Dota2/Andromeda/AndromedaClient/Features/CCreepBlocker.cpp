#include "CCreepBlocker.hpp"

#include <AndromedaClient/Features/FeatureSupport.hpp>
#include <AndromedaClient/Settings/Settings.hpp>
#include <Common/DevLog.hpp>
#include <Dota2/SDK/Interface/CGameEntitySystem.hpp>
#include <Dota2/SDK/Interface/CLocalHeroResolver.hpp>
#include <Dota2/SDK/Math/Math.hpp>
#include <Dota2/SDK/SDK.hpp>

#include <ImGui/imgui.h>

#include <algorithm>
#include <cmath>
#include <vector>

namespace
{
	// Team constants: Radiant=2, Dire=3 in Dota 2
	constexpr uint8_t kRadiantTeam = 2;
	constexpr uint8_t kDireTeam = 3;

	auto GetTeamName( uint8_t team ) -> const char*
	{
		if ( team == kRadiantTeam )
			return "Radiant";
		if ( team == kDireTeam )
			return "Dire";
		return "Unknown";
	}

	// Order timing: minimize delay after crash to move immediately.
	// Very tight timing for smooth, responsive blocking.
	constexpr uint32_t kOrderIntervalMs = 15;
	constexpr uint32_t kClickDelayMs = 2;
	constexpr uint32_t kRestoreDelayMs = 2;

	// Spatial parameters
	constexpr float kWaveSearchRadius = 900.f;
	constexpr float kMinWaveCoherence = 0.55f;

	// Units are discs, not points, and blocking is entirely a fact about where
	// those discs overlap. Standard Dota 2 collision radii - adjust if a patch
	// moves them, everything below is derived from these two numbers rather
	// than hardcoded separately.
	constexpr float kHeroCollisionRadius = 24.f;
	constexpr float kCreepCollisionRadius = 8.f;

	// Centre-to-centre distance at which hero and creep hulls touch. This is the
	// real blocking range: inside it the creep is physically stopped, outside it
	// the creep is walking freely no matter how good the aim looks on paper.
	constexpr float kContactRange = kHeroCollisionRadius + kCreepCollisionRadius;

	// Lateral offset past which a creep is entirely uncovered. Credit tapers
	// from full at kContactRange to nothing here, over roughly a creep's width,
	// so the covered line shifts smoothly instead of snapping when a creep
	// drifts a unit out of reach.
	constexpr float kCoverageFalloff = kContactRange + kCreepCollisionRadius * 3.f;

	// Keep the line already held unless another beats it by this margin. Pure
	// hysteresis: two lines covering the same creeps are equally good, and
	// swapping between them every order is the wobble all over again. Contact,
	// not a better score, is what normally ends a line - see kBumpCooldownMs.
	constexpr float kHeldLineBonus = 1.12f;

	// Centre-to-centre distance at which the hero counts as crashed into a
	// creep. Contact range plus a creep's own radius of slack, because origins
	// are sampled a frame apart and a hull graze should still register.
	constexpr float kContactDetectRange = kContactRange + kCreepCollisionRadius;

	// How long a creep stays "already dealt with" after the hero crashes into
	// it. The collision stalls that creep on its own; standing on it afterwards
	// blocks nothing new while the rest of the wave walks by. Long enough for
	// the hero to cross to another creep and back, short enough that a creep
	// which has recovered and retaken the lead gets blocked again.
	constexpr uint32_t kBumpCooldownMs = 1000;

	// Click targeting radius, which is NOT the collision radius - a right click
	// this close to a unit's centre selects the unit and becomes a follow order
	// instead of a move to that ground. It is much larger than the hulls, which
	// is why the block point cannot simply be placed at contact range on the
	// leading creep's own line and has to be slid clear of the models.
	constexpr float kCreepClearance = 70.f;

	// Direction tracking: how far wave's center must move to confirm heading direction
	constexpr float kMinTravelDistance = 25.f;
	constexpr uint32_t kTravelSampleMaxAgeMs = 1000;

	// Depth of the "front rank" - the band behind the leading creep whose creeps
	// steer the plug. The whole wave is never blocked directly: creeps collide
	// with each other, so stalling the front rank stalls the column behind it.
	// Roughly the depth of the melee clump; too shallow and the rank collapses
	// back to a single creep (the leader), too deep and the trailing ranged
	// creep drags the plug back off the leaders' line.
	constexpr float kFrontRankDepth = 200.f;

	// Two different questions, two different thresholds. Answering both with one
	// number is what this feature kept getting wrong: tighten it and the whole
	// wave drops out of the set on estimator noise, the feature stops ordering
	// and the creeps walk free; loosen it and an escaped creep defines the front
	// edge, which sends the hero down the lane after it. They are not the same
	// question and they do not want the same answer.

	// "Can the hero still get in front of this creep?" - decides where he
	// stands. It is exactly contact range, and for a physical reason rather than
	// a tuned one: while the creep's hull can still reach the hero's hull it is
	// still being stopped by him, and once it cannot, it is walking free and
	// will stay free - lane creeps move at 325 and most heroes move less. Only
	// creeps inside this band define the front edge and the rank, so nothing
	// further ahead can ever pull the block point down-lane.
	constexpr float kBlockableTolerance = kContactRange;

	// "Is there anything left here at all?" - decides whether to keep working.
	// Deliberately generous: measured lead over the front edge sits around
	// 40-100 units during normal blocking, so a tight gate plus a few tens of
	// units of noise reads as "wave escaped" and shuts the feature off at the
	// exact moment it is doing its job. Between the two thresholds the hero
	// holds his ground rather than either chasing or giving up.
	constexpr float kEscapeTolerance = 150.f;

	// Hard ceiling on how far down-lane a single order may send the hero past
	// his own position - roughly contact range, enough to re-seat the plug
	// against a creep drawing level with him and no more. Walking further than
	// this in the wave's own direction is following, not blocking: the creeps
	// are faster, so ground given up that way is never recovered.
	constexpr float kMaxForwardCommit = 100.f;

	// The order is always a DIAGONAL across the wave's front, never a straight
	// walk down the lane and never a pure sidestep.
	//
	// Straight ahead does not block anything for long: the hero and the creep
	// are then travelling the same line, he is the slower of the two, and the
	// creep steps around a body that is only ever directly in front of it. A
	// pure sidestep is worse - it leaves the creep's path entirely. Cutting
	// across its front keeps his hull between the creep and the way it is trying
	// to go, which is what blocking looks like by hand.
	//
	// The angle is held at 45 degrees (gaining ground while crossing) or 135
	// (giving ground while crossing) by giving the forward and sideways legs the
	// SAME LENGTH - a right triangle with equal sides has no other option. There
	// is no angle constant to tune here on purpose: the two legs being equal IS
	// the 45, and anything that changes one without the other bends it.
	//
	// The floor on how far in front of the leading creep the block point must
	// sit, along the wave's heading. Contact range, because that is where the
	// hulls touch - nearer and the creep is past him, further and it is walking
	// free. Enforced at the very end of the order, after every clamp and slide,
	// so no later stage can quietly drop the point behind the creep.
	constexpr float kMinAheadOfLeader = kContactRange;

	// How long one leg of the zigzag runs when nothing interrupts it.
	//
	// Contact is what normally ends a leg - he touches a creep, that creep is
	// stalled, and he sets off the other way. This timer only ends a leg that
	// never meets anything. Roughly the time to walk one leg at hero speed, so
	// a sweep across open ground does not run on past the width of the wave.
	constexpr uint32_t kZigMaxDwellMs = 250;

	// How far to one side the next leader has to be before it is taken as
	// telling the hero which way to cut. Roughly a hero hull: nearer than that
	// it is effectively straight behind the current leader, where cutting either
	// way is as good, and reading a side out of the noise would just make him
	// flick between them.
	constexpr float kNextLeaderSideThreshold = 25.f;

	// How close to the edge of the wave counts as having reached it. Measured on
	// the far extent as seen from the hero, so it is "there is barely any wave
	// left on this side". A hero hull, because once the last creep on that side
	// is within his own radius he is already covering it and the ground beyond
	// is empty lane.
	constexpr float kZigEdgeMargin = kHeroCollisionRadius;

	// Fraction of the freshly measured heading folded into the working lane
	// direction each order. The raw estimate swings 20 degrees and more between
	// orders - creeps turn to step around each other and the facing average
	// follows them - but the lane itself does not move. Everything downstream
	// is measured in this frame, so its noise turns directly into hero jitter.
	// At the 15ms order cadence this settles over roughly 100ms, far quicker
	// than a lane actually bends.
	constexpr float kDirectionSmoothing = 0.15f;

	// A single order may only move the block point this far from the last one.
	// The hero has a top speed; a block point that jumps further than he can
	// walk in one interval is a point he never reaches, and re-aiming at it
	// every order just makes him pivot in place. Clamping the point instead
	// keeps him walking a line he can actually hold.
	constexpr float kMaxBlockPointStepPerOrder = 90.f;

	// Safety: prevent infinite blocking (5 minute max to handle edge cases)
	constexpr uint32_t kMaxBlockDurationMs = 300000;

	struct WaveCreep
	{
		Vector3 origin{};
		float yaw = 0.f;
		bool hasYaw = false;
		C_BaseEntity* entity = nullptr;
		// Already crashed into recently, so it is stalled and the hero's body is
		// better spent on a creep that is still walking.
		bool bumped = false;
		// Got away for good. Written off by every part of the aim - it is not
		// the front-most creep, it does not widen the wave, it never joins the
		// rank. Still avoided when the click point is placed, because the model
		// is in the cursor's way regardless.
		bool escaped = false;
	};

	auto ForwardFromYaw( float yawDegrees ) -> Vector3
	{
		const float radians = yawDegrees * 0.01745329252f;
		return Vector3( std::cos( radians ) , std::sin( radians ) , 0.f );
	}

	auto Length2D( const Vector3& value ) -> float
	{
		return std::sqrt( value.m_x * value.m_x + value.m_y * value.m_y );
	}

	auto Normalized2D( const Vector3& value , Vector3& out ) -> bool
	{
		const float length = Length2D( value );
		if ( length < 0.0001f )
			return false;
		out = Vector3( value.m_x / length , value.m_y / length , 0.f );
		return true;
	}

	auto Dot2D( const Vector3& left , const Vector3& right ) -> float
	{
		return left.m_x * right.m_x + left.m_y * right.m_y;
	}

	// Every allied lane creep near the hero. Walks the identity chunks rather
	// than indexing GetHighestEntityIndex(): that index is build-dependent and
	// under-reports here, which silently truncates entity scans (the same trap
	// CKillStealer.cpp and CLastHitAssistant.cpp both had to back out of).
	auto CollectWave( CGameEntitySystem* entitySystem , const FeatureSupport::UnitOffsets& offsets ,
		const Vector3& heroOrigin , uint8_t heroTeam , std::vector<WaveCreep>& out ) -> void
	{
		out.clear();
		if ( !entitySystem )
			return;

		for ( int chunkIndex = 0; chunkIndex < MAX_ENTITY_LISTS; ++chunkIndex )
		{
			auto* chunk = entitySystem->m_pIdentityChunks[chunkIndex];
			if ( !chunk )
				continue;

			for ( int entryIndex = 0; entryIndex < MAX_ENTITIES_IN_LIST; ++entryIndex )
			{
				auto* identity = &chunk->m_pIdentities[entryIndex];
				auto* entity = identity->pBaseEntity();
				if ( !entity )
					continue;

				const int health = FeatureSupport::ReadField<int>( entity , offsets.health , 0 );
				if ( health <= 0 )
					continue;

				const uint8_t team = FeatureSupport::ReadField<uint8_t>( entity , offsets.team , 0 );
				if ( team != heroTeam )
					continue;

				const std::string name = FeatureSupport::EntityName( entity , identity );
				if ( !FeatureSupport::LooksLikeLaneCreep( entity , name , team ) )
					continue;

				WaveCreep creep{};
				if ( !FeatureSupport::TryReadOrigin( entity , offsets , creep.origin ) )
					continue;
				if ( FeatureSupport::Distance2D( creep.origin , heroOrigin ) > kWaveSearchRadius )
					continue;

				creep.hasYaw = FeatureSupport::TryReadYaw( entity , offsets , creep.yaw );
				creep.entity = entity;
				out.push_back( creep );
			}
		}
	}

	// Where the wave is heading, from the creeps' own facing. A walking creep
	// faces the way it walks, so this needs no position history and is right
	// on the very first frame the key goes down - which matters, because the
	// block has to start before the wave is past you.
	auto TryDirectionFromFacing( const std::vector<WaveCreep>& wave , Vector3& out ) -> bool
	{
		Vector3 sum( 0.f , 0.f , 0.f );
		int counted = 0;
		for ( const auto& creep : wave )
		{
			if ( !creep.hasYaw )
				continue;
			const Vector3 forward = ForwardFromYaw( creep.yaw );
			sum = Vector3( sum.m_x + forward.m_x , sum.m_y + forward.m_y , 0.f );
			++counted;
		}

		if ( counted <= 0 )
			return false;
		// Averaging unit vectors: the result's length IS how much the wave
		// agrees with itself, 1 for creeps marching in lockstep and ~0 for a
		// scattered fight.
		if ( Length2D( sum ) / static_cast<float>( counted ) < kMinWaveCoherence )
			return false;

		return Normalized2D( sum , out );
	}

	auto WaveCentroid( const std::vector<WaveCreep>& wave ) -> Vector3
	{
		Vector3 sum( 0.f , 0.f , 0.f );
		for ( const auto& creep : wave )
			sum = Vector3( sum.m_x + creep.origin.m_x , sum.m_y + creep.origin.m_y , 0.f );

		const float count = static_cast<float>( (std::max)( size_t( 1 ) , wave.size() ) );
		return Vector3( sum.m_x / count , sum.m_y / count , 0.f );
	}

	// Fallback for a build that does not replicate m_angRotation: watch where
	// the wave's centre of mass moves between orders. A creep covers ~58 units
	// in one 180 ms order interval, so a marching wave clears the threshold
	// easily while a wave stopped to fight never does - which is the same
	// "nothing to block" answer the facing check gives.
	//
	// Facing is still tried first: this one needs a previous sample, so it
	// cannot answer on the first frame the key goes down.
	auto TryDirectionFromTravel( const std::vector<WaveCreep>& wave , uint32_t now , Vector3& out ) -> bool
	{
		static Vector3 lastCentroid{};
		static uint32_t lastTick = 0;
		static bool hasSample = false;

		const Vector3 centroid = WaveCentroid( wave );
		bool resolved = false;

		if ( hasSample && now > lastTick && now - lastTick <= kTravelSampleMaxAgeMs )
		{
			const Vector3 delta( centroid.m_x - lastCentroid.m_x , centroid.m_y - lastCentroid.m_y , 0.f );
			if ( Length2D( delta ) >= kMinTravelDistance )
				resolved = Normalized2D( delta , out );
		}

		lastCentroid = centroid;
		lastTick = now;
		hasSample = true;
		return resolved;
	}

	auto TryWaveDirection( const std::vector<WaveCreep>& wave , uint32_t now , Vector3& out ) -> bool
	{
		// Both are called every time, never short-circuited: the travel
		// tracker has to keep sampling even while facing is answering, or it
		// would have no previous position on the tick facing first fails.
		const bool fromTravel = TryDirectionFromTravel( wave , now , out );
		Vector3 facing{};
		if ( TryDirectionFromFacing( wave , facing ) )
		{
			out = facing;
			return true;
		}
		return fromTravel;
	}

	// How far past the hero this creep has got, along the wave's heading.
	// Positive means it is ahead of him and pulling away.
	auto LeadOverHero( const WaveCreep& creep , const Vector3& direction , float heroProjection ) -> float
	{
		return Dot2D( creep.origin , direction ) - heroProjection;
	}

	// One creep's line across the lane, and how much it matters. `lateral` is
	// measured FROM THE HERO, not from the world origin: the lane frame rotates
	// a little every order, and a projection of a coordinate ~3700 units from
	// origin swings by hundreds of units when it does. Relative to the hero the
	// same rotation moves it by a few units, because the creep is close by.
	struct RankCreep
	{
		float lateral = 0.f;
		float weight = 0.f;
		bool bumped = false;
		C_BaseEntity* entity = nullptr;
	};

	// How much of the front rank a hero standing on this line actually covers.
	//
	// The hero is a disc of radius kHeroCollisionRadius, so he stops a creep
	// only while their hulls overlap. A lane is several hundred units across
	// and he covers about 64 of it, which is the fact the old lateral average
	// could not represent: averaging two creeps walking either side of him
	// returns the empty ground between them, a line that covers neither.
	// countBumped false scores only the creeps still worth blocking, which is
	// what the line choice runs on: a creep the hero has already crashed into is
	// stalled whether he stands on it or not, so covering it again earns nothing.
	// True scores every hull he physically reaches, which is what the log wants.
	auto CoverageScore( const std::vector<RankCreep>& rank , float line , bool countBumped ) -> float
	{
		float score = 0.f;
		for ( const auto& creep : rank )
		{
			if ( creep.bumped && !countBumped )
				continue;

			const float offset = std::fabs( creep.lateral - line );
			if ( offset >= kCoverageFalloff )
				continue;

			const float credit = offset <= kContactRange
				? 1.f
				: 1.f - ( offset - kContactRange ) / ( kCoverageFalloff - kContactRange );
			score += creep.weight * credit;
		}
		return score;
	}

	// The line worth standing on. Candidates are real creep lines, never an
	// average of them, so the hero always ends up body to body with something.
	//
	// The line already held gets a small bonus: when two lines cover the same
	// creeps they are equally good, and swapping between them every order is
	// the wobble this whole design exists to avoid.
	//
	// Creeps already crashed into are skipped as candidates and score nothing,
	// so the moment the hero connects with one, the line he is standing on goes
	// worthless and the next creep along wins on its own merits. That is the
	// hand-off: hold a line until contact, then take the next one. No special
	// case is needed to break the hysteresis - the held line simply stops
	// scoring, and the bonus has nothing left to protect.
	//
	// The line being held is identified by WHICH CREEP it belongs to, not by a
	// coordinate. Coordinates in this frame are not comparable across orders:
	// the lane heading moves a little each time, and the same line reads
	// hundreds of units different afterwards. A creep is the same creep whatever
	// the frame does, so anchoring to it is the only stable way to say "keep
	// covering what I was covering" - stored as a coordinate the bonus applied
	// to garbage and the choice collapsed to argmax on every order.
	//
	// Returns the index into rank, or -1 when there is nothing to stand on.
	auto ChooseCoverageLine( const std::vector<RankCreep>& rank , const C_BaseEntity* heldEntity ) -> int
	{
		int best = -1;
		float bestScore = 0.f;

		for ( size_t index = 0; index < rank.size(); ++index )
		{
			const auto& creep = rank[index];
			if ( creep.bumped )
				continue;

			float score = CoverageScore( rank , creep.lateral , false );
			if ( heldEntity && creep.entity == heldEntity )
				score *= kHeldLineBonus;

			if ( best < 0 || score > bestScore )
			{
				best = static_cast<int>( index );
				bestScore = score;
			}
		}

		if ( best >= 0 )
			return best;

		// Every creep in reach has been crashed into already. Rather than stand
		// on a line he is finished with, cover whatever is physically there -
		// the cooldowns expire in under a second and the cycle starts again.
		bestScore = 0.f;
		for ( size_t index = 0; index < rank.size(); ++index )
		{
			const float score = CoverageScore( rank , rank[index].lateral , true );
			if ( best < 0 || score > bestScore )
			{
				best = static_cast<int>( index );
				bestScore = score;
			}
		}

		return best;
	}

	// Where the plug has to sit, in the lane's frame and RELATIVE TO THE HERO.
	// Both figures are offsets from where he stands, never absolute projections:
	// the frame rotates slightly every order, and an absolute projection of a
	// world coordinate thousands of units from origin is a different number
	// afterwards even though nothing moved.
	struct WaveFront
	{
		float leadOffset = 0.f; // leading blockable creep, along the lane, from the hero
		float lateral = 0.f;    // the line to stand on, across the lane, from the hero
		float coverage = 0.f;   // weighted rank actually covered from that line
		float groundZ = 0.f;
		// The leading blockable creep's own world position. Kept because the
		// block point has to be checked against it AFTER every clamp and slide
		// below has had its say - leadOffset is measured from the hero, so it
		// stops being comparable the moment anything moves the point for a
		// reason of its own.
		Vector3 leaderOrigin{};
		bool hasLeader = false;
		// RULE 1's creep: the front-most one still in play, blockable or not.
		//
		// Distinct from leaderOrigin above, and the distinction is the whole of
		// rule 1. leaderOrigin comes from the BLOCKABLE band only, so a creep
		// that slipped more than kBlockableTolerance past the hero drops out of
		// it and stops being aimed at - which is exactly how creeps got in front
		// of him and stayed there. This one never drops a creep until it has
		// escaped outright, so the aim keeps trying to get ahead of everything
		// that can still be caught.
		Vector3 frontMostOrigin{};
		float frontMostLead = 0.f;   // along the lane, from the hero
		bool hasFrontMost = false;
		// Diagnostics for the RECOVER-entry question: is a hero_ahead collapse a
		// single creep genuinely lagging, or the same order re-projected onto a
		// jerked lane heading? Identity that holds while the number jumps points
		// at reprojection; identity that hops points at membership/estimator
		// noise; a steady identity tracking downward is the real thing. Lateral
		// offset scales how much a heading swing can move frontMostLead, so a
		// large one beside a drift spike is the tell for a phantom.
		C_BaseEntity* frontMostEntity = nullptr;
		float frontMostLateral = 0.f; // the front-most creep, across the lane, from the hero
		float secondLead = 0.f;       // next-highest lead among creeps still in play
		bool hasSecond = false;
		// How wide the wave is across the lane, as offsets from the hero.
		//
		// This is what makes "in front of ALL creeps" mean anything. Being ahead
		// along the lane is only half of it - the hero's hull is 64 units wide
		// and a lane wave spans several hundred, so creeps to either side of him
		// walk past however far ahead he is. The sweep has to run between these
		// two edges to put his body in every creep's path in turn; swept around
		// his own position instead, it covers whatever happens to be near him
		// and lets the rest through.
		float lateralMin = 0.f;
		float lateralMax = 0.f;
		bool hasExtent = false;
		// RULE 2. The creep that becomes the leader once the current one is
		// stalled - the second most advanced still worth blocking.
		//
		// Blocking the leader is what promotes this one, so by the time the
		// hand-off happens it is already too late to start moving: the hero has
		// to be crossing toward it while he is still on the first. That is what
		// the diagonal is for, and this is the creep it aims at.
		Vector3 nextLeaderOrigin{};
		float nextLateral = 0.f;  // across the lane, from the hero
		bool hasNextLeader = false;
		int rankSize = 0;
		int bumpedCount = 0;    // of those, ones already crashed into
		C_BaseEntity* lineEntity = nullptr; // creep whose line was chosen
		bool valid = false;
		// Nothing is blockable any more, but the wave has not cleared out
		// either. The hero stands his ground on the line he already holds -
		// creeps still coming up behind can bunch against him there - instead of
		// setting off after the ones that got by.
		bool holding = false;
	};

	// Deliberately not "pick the leading creep and chase it". Which creep leads
	// changes every time the hero body-blocks one - blocking it slows it, the
	// creep behind takes the lead, and an order aimed at whoever leads right now
	// swings the hero sideways to the new leader. That swing is what opens the
	// gap the rest of the wave walks through, and it repeats, so the hero
	// wobbles across the lane instead of plugging it.
	//
	// The fix is to stop aiming at a creep at all. Longitudinally the aim point
	// tracks the leading edge; laterally it picks the line across the lane that
	// physically covers the most of the front rank, holding the previous line
	// when the choice is close. Two creeps swapping the lead changes neither, so
	// there is nothing to wobble about.
	auto ComputeWaveFront( const std::vector<WaveCreep>& wave , const Vector3& direction ,
		const Vector3& lateralAxis , const Vector3& heroOrigin , float heroProjection ,
		const C_BaseEntity* heldEntity ) -> WaveFront
	{
		WaveFront front{};

		// The front edge is the leading creep the hero is STILL IN FRONT OF.
		// Creeps past him are excluded here and nowhere else matters as much:
		// this single max is what the block point is built from, so one escaped
		// creep left in it puts the aim point beyond the hero and walks him down
		// the lane behind the wave.
		float leadOffset = 0.f;
		bool hasLead = false;
		int presentCount = 0;
		// Second place, tracked alongside first so rule 2 has a creep to aim at.
		float nextLead = 0.f;
		const WaveCreep* leaderCreep = nullptr;
		const WaveCreep* nextCreep = nullptr;
		for ( const auto& creep : wave )
		{
			const float lead = LeadOverHero( creep , direction , heroProjection );
			// Written off already - see the escape table. Not the front-most
			// creep, not part of the wave's width, never in the rank.
			if ( creep.escaped )
				continue;

			++presentCount;

			// Front-most first, and deliberately BEFORE the blockable filter
			// below. This is the creep rule 1 measures against, and it has to
			// include the ones that have got past the hero - those are precisely
			// the creeps he needs to be told to get back in front of.
			if ( !front.hasFrontMost || lead > front.frontMostLead )
			{
				// Old front-most drops to second place, so the log can show
				// whether the lead is held by one creep or being traded around.
				if ( front.hasFrontMost )
				{
					front.secondLead = front.frontMostLead;
					front.hasSecond = true;
				}
				front.frontMostLead = lead;
				front.frontMostOrigin = creep.origin;
				front.frontMostEntity = creep.entity;
				const Vector3 heroToFront( creep.origin.m_x - heroOrigin.m_x ,
					creep.origin.m_y - heroOrigin.m_y , 0.f );
				front.frontMostLateral = Dot2D( heroToFront , lateralAxis );
				front.hasFrontMost = true;
			}
			else if ( !front.hasSecond || lead > front.secondLead )
			{
				front.secondLead = lead;
				front.hasSecond = true;
			}

			// Width of the wave, over the same set - every creep still in play,
			// not just the blockable ones. A creep that has edged past the hero
			// is still one he has to sweep across to get back in front of.
			{
				const Vector3 heroToCreep( creep.origin.m_x - heroOrigin.m_x ,
					creep.origin.m_y - heroOrigin.m_y , 0.f );
				const float side = Dot2D( heroToCreep , lateralAxis );
				if ( !front.hasExtent )
				{
					front.lateralMin = side;
					front.lateralMax = side;
					front.hasExtent = true;
				}
				else
				{
					front.lateralMin = ( std::min )( front.lateralMin , side );
					front.lateralMax = ( std::max )( front.lateralMax , side );
				}
			}

			if ( lead > kBlockableTolerance )
				continue;

			if ( !hasLead || lead > leadOffset )
			{
				// New leader; the old one drops into second place, which is
				// exactly what rule 2 wants to aim at.
				if ( hasLead )
				{
					nextLead = leadOffset;
					nextCreep = leaderCreep;
				}
				leadOffset = lead;
				leaderCreep = &creep;
				front.groundZ = creep.origin.m_z;
				front.leaderOrigin = creep.origin;
				front.hasLeader = true;
				hasLead = true;
			}
			else if ( !nextCreep || lead > nextLead )
			{
				nextLead = lead;
				nextCreep = &creep;
			}
		}

		if ( nextCreep )
		{
			const Vector3 heroToNext( nextCreep->origin.m_x - heroOrigin.m_x ,
				nextCreep->origin.m_y - heroOrigin.m_y , 0.f );
			front.nextLeaderOrigin = nextCreep->origin;
			front.nextLateral = Dot2D( heroToNext , lateralAxis );
			front.hasNextLeader = true;
		}

		// Nothing left to get in front of, but creeps are still about: hold the
		// line. The caller pins the block point to the hero's own position, so
		// he shuffles across the lane without giving up ground down it.
		if ( !hasLead )
		{
			front.valid = presentCount > 0;
			front.holding = front.valid;
		}

		std::vector<RankCreep> rank;
		rank.reserve( wave.size() );
		for ( const auto& creep : wave )
		{
			const float lead = LeadOverHero( creep , direction , heroProjection );
			if ( creep.escaped )
				continue;

			// While holding there is no front edge to measure depth from, so
			// every creep still present contributes to the line the hero covers.
			if ( hasLead && lead > kBlockableTolerance )
				continue;

			const float depth = hasLead ? leadOffset - lead : 0.f;
			if ( depth > kFrontRankDepth )
				continue;

			// Linear falloff: the leading creep matters most to cover, the ones
			// at the back of the rank least. The leader always weighs 1.
			// Measured from the hero, so the number survives the frame turning.
			const Vector3 heroToCreep( creep.origin.m_x - heroOrigin.m_x ,
				creep.origin.m_y - heroOrigin.m_y , 0.f );

			RankCreep entry{};
			entry.lateral = Dot2D( heroToCreep , lateralAxis );
			entry.weight = 1.f - depth / kFrontRankDepth;
			entry.bumped = creep.bumped;
			entry.entity = creep.entity;
			rank.push_back( entry );

			if ( creep.bumped )
				++front.bumpedCount;
		}

		front.rankSize = static_cast<int>( rank.size() );

		const int chosen = rank.empty() ? -1 : ChooseCoverageLine( rank , heldEntity );
		if ( chosen < 0 )
		{
			front.valid = false;
			front.holding = false;
			return front;
		}

		front.leadOffset = leadOffset;
		front.lateral = rank[chosen].lateral;
		front.lineEntity = rank[chosen].entity;
		front.coverage = CoverageScore( rank , front.lateral , true );
		front.valid = true;
		return front;
	}

	// Slid sideways until it is clear of every creep model, so the right click
	// lands on ground. A right click that lands on an allied creep is a follow
	// order: the hero would trail the wave instead of blocking it.
	//
	// Sideways, never forwards. Pushing the point down-lane to clear a creep is
	// how the hero ends up walking away from the wave he is supposed to be
	// standing in front of - and if the creep in the way is one that already
	// escaped, pushing forward is chasing it. Sliding across the lane keeps the
	// standoff the caller picked and only changes which part of the lane is
	// plugged, which the next order corrects anyway.
	auto BlockPointClearOfCreeps( const std::vector<WaveCreep>& wave , const Vector3& lateralAxis , Vector3 point ) -> Vector3
	{
		for ( int attempt = 0; attempt < 3; ++attempt )
		{
			const WaveCreep* blocker = nullptr;
			for ( const auto& creep : wave )
			{
				if ( FeatureSupport::Distance2D( creep.origin , point ) < kCreepClearance )
				{
					blocker = &creep;
					break;
				}
			}

			if ( !blocker )
				break;

			// Away from the creep in the way, along the lane's cross axis.
			const Vector3 offset( point.m_x - blocker->origin.m_x , point.m_y - blocker->origin.m_y , 0.f );
			const float side = Dot2D( offset , lateralAxis ) >= 0.f ? 1.f : -1.f;
			point = Vector3( point.m_x + lateralAxis.m_x * kCreepClearance * side ,
				point.m_y + lateralAxis.m_y * kCreepClearance * side , point.m_z );
		}

		return point;
	}
}

auto CCreepBlocker::OnRender() -> void
{
	const uint32_t now = static_cast<uint32_t>( GetTickCount64() );

	AdvanceOrder( now );

	if ( !Settings::CreepBlocker::Enable )
	{
		m_Status = "Disabled";
		m_isCreepBlocking = false;
		m_Marker.valid = false;
		m_HasLaneDirection = false;
		m_HeldLineEntity = nullptr;
		m_CreepDebugCount = 0;
		m_HasDebugFrame = false;
		m_CutSide = 0;
		m_ZigSide = 1;
		m_ZigFlipTick = 0;
		m_WasCrashed = false;
		m_EscapedCount = 0;
		m_FreshCrash = false;
		for ( auto& bump : m_Bumped )
			bump = {};
		return;
	}

	if ( Settings::CreepBlocker::Key <= 0 )
	{
		m_Status = "No key bound";
		m_isCreepBlocking = false;
		m_Marker.valid = false;
		m_HasLaneDirection = false;
		m_HeldLineEntity = nullptr;
		m_CreepDebugCount = 0;
		m_HasDebugFrame = false;
		m_CutSide = 0;
		m_ZigSide = 1;
		m_ZigFlipTick = 0;
		m_WasCrashed = false;
		m_EscapedCount = 0;
		m_FreshCrash = false;
		for ( auto& bump : m_Bumped )
			bump = {};
		return;
	}

	const bool keyDown = ( GetAsyncKeyState( Settings::CreepBlocker::Key ) & 0x8000 ) != 0;
	if ( !keyDown )
	{
		m_Status = "Idle - hold the key while walking to lane";
		m_isCreepBlocking = false;
		m_Marker.valid = false;
		m_HasLaneDirection = false;
		m_HeldLineEntity = nullptr;
		m_CreepDebugCount = 0;
		m_HasDebugFrame = false;
		m_CutSide = 0;
		m_ZigSide = 1;
		m_ZigFlipTick = 0;
		m_WasCrashed = false;
		m_EscapedCount = 0;
		m_FreshCrash = false;
		for ( auto& bump : m_Bumped )
			bump = {};
		return;
	}

	// Validate blocking conditions remain satisfied if already blocking
	if ( m_isCreepBlocking && !ValidateBlockingConditions( now ) )
	{
		m_isCreepBlocking = false;
	}

	// Drawn every frame, not just on the order cadence, so the marker
	// doesn't visibly stutter at 60ms steps.
	DrawBlockMarker();

	if ( m_Phase != OrderPhase::Idle || now < m_NextOrderTick )
		return;

	// Issue movement order on regular cadence
	const bool orderIssued = TryIssueBlockOrder( now );
	if ( !orderIssued )
	{
		// If order fails, clear blocking state to avoid stale state.
		// Retry on the next frame rather than sitting out the cadence - a
		// failure is usually transient (window focus, a frame with the block
		// point off screen) and the wave does not wait.
		m_isCreepBlocking = false;
		m_NextOrderTick = now;
		return;
	}

	// Adaptive intervals: very tight when close for smooth collision, looser when far
	uint32_t nextInterval = kOrderIntervalMs;

	// A fresh crash outranks the ladder below. The instant he touches a creep
	// the sweep has already reversed, and the order carrying him the other way
	// should go out on the very next frame - any wait here is time spent leaning
	// on a creep that is already stopped while the rest of the wave walks past.
	if ( m_FreshCrash )
	{
		m_NextOrderTick = now;
		return;
	}

	if ( m_Marker.valid && orderIssued )
	{
		const Vector3 heroToBlock( m_Marker.blockPoint.m_x - m_Marker.heroOrigin.m_x ,
			m_Marker.blockPoint.m_y - m_Marker.heroOrigin.m_y , 0.f );
		const float distToBlock = Length2D( heroToBlock );
		if ( distToBlock < 20.f )
		{
			// Hero very close: immediate reposition for next creep
			nextInterval = 0;
		}
		else if ( distToBlock < 40.f )
		{
			nextInterval = 5;  // Ultra-tight: maximum smoothness
		}
		else if ( distToBlock < 100.f )
		{
			nextInterval = 8;  // Very close: smooth aggressive pursuit
		}
		else if ( distToBlock < 200.f )
		{
			nextInterval = 12;  // Medium: smooth approach
		}
		else if ( distToBlock < 350.f )
		{
			nextInterval = 15;  // Far: normal pursuit
		}
	}
	m_NextOrderTick = now + nextInterval;
}

auto CCreepBlocker::AdvanceOrder( uint32_t now ) -> void
{
	if ( m_Phase == OrderPhase::Idle || now < m_NextPhaseTick )
		return;

	if ( m_Phase == OrderPhase::Click )
	{
		FeatureSupport::SendRightClick();
		m_Phase = OrderPhase::Restore;
		m_NextPhaseTick = now + kRestoreDelayMs;
		return;
	}

	FeatureSupport::MoveCursorToScreen( m_PreviousCursor.x , m_PreviousCursor.y );
	m_Phase = OrderPhase::Idle;
}

auto CCreepBlocker::TryIssueBlockOrder( uint32_t now ) -> bool
{
	static uint32_t lastLogTick = 0;
	const bool shouldLog = !lastLogTick || now - lastLogTick >= 500;
	if ( shouldLog )
		lastLogTick = now;

	const auto& offsets = FeatureSupport::ResolveOffsets();
	if ( !offsets.resolved )
	{
		m_Status = "Waiting for schema";
		if ( shouldLog )
			DEV_LOG( "[creep-block] FAIL: schema not resolved\n" );
		return false;
	}

	auto* entitySystem = SDK::Interfaces::GameEntitySystem();
	C_BaseEntity* hero = nullptr;
	int heroIndex = -1;
	if ( !CLocalHeroResolver::Resolve( entitySystem , hero , heroIndex ) )
	{
		m_Status = "Local hero not resolved";
		if ( shouldLog )
			DEV_LOG( "[creep-block] FAIL: local hero not resolved\n" );
		return false;
	}

	if ( FeatureSupport::ReadField<int>( hero , offsets.health , 0 ) <= 0 )
	{
		m_Status = "Hero is dead";
		if ( shouldLog )
			DEV_LOG( "[creep-block] FAIL: hero is dead\n" );
		return false;
	}

	Vector3 heroOrigin{};
	if ( !FeatureSupport::TryReadOrigin( hero , offsets , heroOrigin ) )
	{
		m_Status = "Hero position unavailable";
		if ( shouldLog )
			DEV_LOG( "[creep-block] FAIL: hero position unavailable\n" );
		return false;
	}

	const uint8_t heroTeam = FeatureSupport::ReadField<uint8_t>( hero , offsets.team , 0 );
	if ( !FeatureSupport::IsPlayableTeam( heroTeam ) )
	{
		m_Status = "Hero team unavailable";
		if ( shouldLog )
			DEV_LOG( "[creep-block] FAIL: hero team unavailable (team=%d, %s)\n" , (int)heroTeam , GetTeamName( heroTeam ) );
		return false;
	}

	std::vector<WaveCreep> wave;
	CollectWave( entitySystem , offsets , heroOrigin , heroTeam , wave );
	if ( wave.empty() )
	{
		m_Status = "No allied wave nearby";
		if ( shouldLog )
			DEV_LOG( "[creep-block] FAIL: no allied wave nearby (search_radius=%.0f)\n" , kWaveSearchRadius );
		return false;
	}

	// Anything the hero is physically crashed into right now is a creep he has
	// already done his job on - the collision stalls it whether he keeps leaning
	// on it or not. Record the contact, then mark everything still inside its
	// cooldown, so the line choice below hands him on to a creep that is still
	// walking rather than parking him against one that has already stopped.
	int freshContacts = 0;
	float nearestCreep = kWaveSearchRadius;
	for ( auto& creep : wave )
	{
		const float gap = FeatureSupport::Distance2D( creep.origin , heroOrigin );
		nearestCreep = ( std::min )( nearestCreep , gap );
		if ( gap > kContactDetectRange )
			continue;

		if ( !IsBumped( creep.entity , now ) )
			++freshContacts;
		RegisterBump( creep.entity , now );
	}

	for ( auto& creep : wave )
		creep.bumped = IsBumped( creep.entity , now );

	Vector3 rawDirection{};
	if ( !TryWaveDirection( wave , now , rawDirection ) )
	{
		m_Status = "Wave is not marching - nothing to block";
		if ( shouldLog )
			DEV_LOG( "[creep-block] FAIL: wave not marching (wave_size=%zu)\n" , wave.size() );
		return false;
	}

	// Smooth the heading before anything is measured in it. A frame that swings
	// between orders moves the front edge, the lateral centre and the escape
	// test all at once, and the hero is left chasing an estimate rather than a
	// wave. Held across orders and reset when the key is released.
	Vector3 direction = rawDirection;
	if ( m_HasLaneDirection )
	{
		const Vector3 blended(
			m_LaneDirection.m_x + ( rawDirection.m_x - m_LaneDirection.m_x ) * kDirectionSmoothing ,
			m_LaneDirection.m_y + ( rawDirection.m_y - m_LaneDirection.m_y ) * kDirectionSmoothing , 0.f );
		Vector3 normalized{};
		if ( Normalized2D( blended , normalized ) )
			direction = normalized;
	}
	m_LaneDirection = direction;
	m_HasLaneDirection = true;

	// Snapshot for the overlay, taken once the heading is settled and the bump
	// flags are marked, so what gets drawn is what this order actually worked
	// from rather than a re-derivation that could quietly disagree with it.
	m_CreepDebugCount = 0;
	m_DebugHeroOrigin = heroOrigin;
	m_HasDebugFrame = true;
	for ( const auto& creep : wave )
	{
		if ( m_CreepDebugCount >= kDebugCreeps )
			break;

		auto& entry = m_CreepDebug[m_CreepDebugCount++];
		entry.origin = creep.origin;
		entry.hasFacing = creep.hasYaw;
		entry.facing = creep.hasYaw ? ForwardFromYaw( creep.yaw ) : direction;
		entry.contact = FeatureSupport::Distance2D( creep.origin , heroOrigin ) <= kContactDetectRange;
		entry.bumped = creep.bumped;
	}

	// The lane's own frame: along it, and across it.
	const Vector3 lateralAxis( -direction.m_y , direction.m_x , 0.f );
	const float heroProjection = Dot2D( heroOrigin , direction );

	// Write off whatever has got away, and keep it written off.
	//
	// A creep this far past the hero cannot be caught - lane creeps walk at 325
	// and most heroes move less - so every order still counting it drags the aim
	// down-lane after a creep that is only going to get further away, and the
	// creeps still in front of him go unblocked while that happens.
	//
	// Recorded rather than re-tested each order. The old form asked "is it more
	// than kEscapeTolerance ahead right now", which let a creep sitting near the
	// boundary drop back into the set every time the heading estimate moved a
	// few degrees; the hero would then turn back toward something he had already
	// given up on, and turn away again a moment later.
	int escapedThisOrder = 0;
	for ( auto& creep : wave )
	{
		if ( !IsEscaped( creep.entity ) &&
			LeadOverHero( creep , direction , heroProjection ) > kEscapeTolerance )
		{
			RegisterEscaped( creep.entity );
			++escapedThisOrder;
		}

		creep.escaped = IsEscaped( creep.entity );
	}

	const WaveFront front = ComputeWaveFront( wave , direction , lateralAxis , heroOrigin ,
		heroProjection , m_HeldLineEntity );
	if ( !front.valid )
	{
		// Every creep in range is already past the hero: there is nothing left
		// to body-block, so stop ordering and drop the marker rather than
		// walking the hero after a wave he can't get in front of again.
		m_Status = "Wave already escaped - nothing to block";
		m_Marker.valid = false;
		if ( shouldLog )
			DEV_LOG( "[creep-block] FAIL: whole wave escaped (wave_size=%zu, hero_along=%.0f, tolerance=%.0f)\n" ,
				wave.size() , heroProjection , kEscapeTolerance );
		return false;
	}

	// Across the lane, the hero SWEEPS BACK AND FORTH across the covering line
	// rather than settling on it.
	//
	// Sitting on the line only blocks whatever is directly behind him - a lane
	// is several hundred units wide and his hull covers about 64 of it, so
	// creeps either side simply walk past. Sweeping drags that hull through the
	// full width of the wave, so every creep meets it in turn, and a creep that
	// steps around him walks into where he is going next. It is also just what
	// blocking looks like by hand: nobody blocks a wave by walking a straight
	// line in front of it.
	//
	m_HeldLineEntity = front.lineEntity;

	// The creep he is working, so contact with THAT one can be told apart from
	// contact with whatever else happens to be nearby.
	const WaveCreep* lineCreep = nullptr;
	for ( const auto& creep : wave )
	{
		if ( creep.entity == front.lineEntity )
		{
			lineCreep = &creep;
			break;
		}
	}

	const bool crashed = !front.holding && lineCreep
		&& FeatureSupport::Distance2D( lineCreep->origin , heroOrigin ) <= kContactDetectRange;

	// Contact is the trigger for the next leg of the zigzag. The moment he
	// touches a creep, that creep is stalled and the useful thing to do is set
	// off across the wave the other way - the creeps he has NOT touched are the
	// ones still walking. Leaning on the one he has just stopped blocks nothing
	// new while the rest stream past either side.
	//
	// Rising edge only. While he stays in contact the side must hold, or it
	// would flip every order at the order cadence and he would vibrate in place
	// instead of crossing.
	m_FreshCrash = crashed && !m_WasCrashed;
	m_WasCrashed = crashed;

	if ( m_FreshCrash )
	{
		m_ZigSide = -m_ZigSide;
		m_ZigFlipTick = now;
	}

	// Flip on the dwell timer too, for the case where he crosses without
	// touching anything. With the fixed-length diagonal below the aim point
	// keeps its distance from him as he walks, so there is no "arrived" moment
	// to detect - the timer is what ends a leg that never meets a creep.
	if ( !m_FreshCrash && m_ZigFlipTick && now - m_ZigFlipTick > kZigMaxDwellMs )
	{
		m_ZigSide = -m_ZigSide;
		m_ZigFlipTick = now;
	}
	else if ( !m_ZigFlipTick )
	{
		m_ZigFlipTick = now;
	}

	// RULE 2, and it overrides both flips above. The diagonal is not a blind
	// alternation - it is aimed at the creep that is about to become the leader,
	// so the hero is already crossing toward it while he is still on the current
	// one. By the time blocking the first creep promotes the second, he is
	// there.
	//
	// Only when that creep is clearly to one side. Directly behind the leader it
	// says nothing about which way to cut, so the alternation above stands and
	// the hero keeps sweeping rather than freezing on an ambiguous answer.
	bool aimedAtNext = false;
	if ( front.hasNextLeader && std::fabs( front.nextLateral ) > kNextLeaderSideThreshold )
	{
		const int nextSide = front.nextLateral >= 0.f ? 1 : -1;
		if ( nextSide != m_ZigSide )
			m_ZigFlipTick = now;
		m_ZigSide = nextSide;
		aimedAtNext = true;
	}

	// Reached the edge of the wave: turn round. This is what keeps the sweep
	// covering ALL of it rather than oscillating over one part.
	//
	// The extents are measured from the hero, so lateralMax dropping to about
	// zero means there is no creep left on his right - he is at or past that
	// edge and the useful direction is back across. Same on the other side.
	//
	// Checked after rule 2 on purpose: aiming at the next leader is worth more
	// than finishing a leg, but not worth walking off the end of the wave, and
	// this is the correction that stops that.
	if ( front.hasExtent )
	{
		if ( m_ZigSide > 0 && front.lateralMax <= kZigEdgeMargin )
		{
			m_ZigSide = -1;
			m_ZigFlipTick = now;
		}
		else if ( m_ZigSide < 0 && front.lateralMin >= -kZigEdgeMargin )
		{
			m_ZigSide = 1;
			m_ZigFlipTick = now;
		}
	}

	// How far forward the block point has to be for RULE 1 to hold, as an offset
	// from the hero. BlockAhead is the standoff the player asked for;
	// kMinAheadOfLeader is the floor under it, since a point nearer than contact
	// range is not in front of the creep in any useful sense.
	//
	// Measured from the FRONT-MOST creep, not the leading blockable one. That is
	// the fix for the hero not being in front: the blockable band cuts off at
	// kBlockableTolerance, so a creep further past him than that used to vanish
	// from this sum entirely and he stopped trying to get ahead of it. The
	// end-of-order rule then had to drag the point forward by ninety-odd units
	// to compensate, which both arrived too late to steer him and flattened the
	// diagonal to twenty degrees. Aiming at the front-most creep here means the
	// correction has nothing left to do.
	const float standoff = ( std::max )( Settings::CreepBlocker::BlockAhead , kMinAheadOfLeader );
	const float minForward = front.hasFrontMost ? front.frontMostLead + standoff : 0.f;

	// THE DIAGONAL. Both legs are the same length, which is the entire reason
	// the angle comes out at 45 or 135 degrees: a right triangle with equal
	// sides has no other option.
	//
	// This replaces a forward term and a sideways term that were computed
	// independently - the standoff decided one, the sweep decided the other -
	// so the angle between them was whatever those two happened to work out to,
	// anything from a straight walk down the lane to a pure sidestep. Nothing
	// held it at a diagonal at all.
	// Has a creep got past him? This decides everything below, because rule 1
	// outranks the diagonal and the two want different things here.
	const bool behind = front.hasFrontMost && front.frontMostLead > 0.f;

	// Never sweep away from a wave he has not reached yet. When the covering
	// line is further off than a sidestep, the side is forced toward it; only
	// once he is within reach does the sweep alternate freely. Without this the
	// zigzag is happy to oscillate in open ground beside the creeps.
	if ( std::fabs( front.lateral ) > Settings::CreepBlocker::SideStep )
		m_ZigSide = front.lateral >= 0.f ? 1 : -1;

	float forwardStep = 0.f;
	float lineShift = 0.f;

	if ( behind )
	{
		// RECOVERY. A creep is past him and rule 1 is broken; getting back in
		// front is the only thing that matters until it is not.
		//
		// The diagonal is dropped here on purpose, and the reason is arithmetic
		// rather than preference: at 45 degrees he closes down-lane at his own
		// speed times 0.707 - about 212 for a 300-speed hero - while lane creeps
		// walk at 325. Crossing while behind therefore LOSES ground every order,
		// and a capture showed exactly that, hero_ahead running -43, -84, -148
		// with no way back. Straight down-lane spends every unit of his speed on
		// the gap, which is the only setting under which it closes at all.
		//
		// kMaxForwardCommit is deliberately not applied. It exists to stop him
		// chasing a wave he is already in front of; while he is behind, refusing
		// to aim far enough forward is refusing to obey rule 1, and that cap
		// does not get to outrank it. A capture showed the cap holding the aim
		// at 100 while the rule needed 180, with the end-of-order correction
		// then shoving the difference in and flattening the angle to 29 degrees.
		forwardStep = minForward;

		// COVERAGE HOLDS DURING RECOVERY when more than the escaping creep is
		// still in the rank. A capture (line ~722) showed the failure the old
		// near-straight chase caused: the hero drops the sweep to run down-lane
		// after one creep he cannot catch (300 vs 325, lead held ~120 and never
		// closed), and while the sweep is gone the OTHER creeps walk the flanks
		// he stopped covering, reach the dead band, and escape alongside the one
		// he was chasing - gone jumped +2 in a single order. Chasing straight
		// spent his whole speed on a catch that never happens and lost the rest
		// of the wave doing it. So keep his body across the width instead: sweep
		// toward the covering edge exactly as the in-front branch does, but never
		// let the side leg exceed the forward push, so down-lane stays dominant
		// and rule 1 is still being pursued for the front-most creep - no creep
		// is written off, he simply stops abandoning the many to chase the one.
		//
		// With nothing else in the rank there is no width to hold, so fall back
		// to the straight chase: it is the only setting that can still close on a
		// lone straggler that is barely ahead.
		if ( front.rankSize > 1 )
		{
			const float edgeTarget = m_ZigSide > 0 ? front.lateralMax : front.lateralMin;
			const float sweepReach = front.hasExtent
				? std::fabs( edgeTarget ) + kHeroCollisionRadius
				: Settings::CreepBlocker::SideStep;
			lineShift = static_cast<float>( m_ZigSide ) * ( std::min )( sweepReach , forwardStep );
		}
		else
		{
			lineShift = static_cast<float>( m_ZigSide ) * ( Settings::CreepBlocker::SideStep * 0.25f );
		}
	}
	else
	{
		// IN FRONT. Rule 1 already holds, so the diagonal is free to do its job.
		//
		// The leg is long enough to carry him to the far EDGE of the wave, not a
		// fixed sidestep from wherever he stands. That is the difference between
		// sweeping the whole wave and sweeping a patch of it: a capture showed
		// coverage stuck at 1.6 of 3-4 creeps with nearest_creep never under 75,
		// which is a hero crossing back and forth beside the wave rather than
		// through it, while the creeps he never reached walked past.
		//
		// One hull of margin past the edge creep, so he actually clears it
		// instead of stopping on its centre line.
		const float edgeTarget = m_ZigSide > 0 ? front.lateralMax : front.lateralMin;
		const float sweepReach = front.hasExtent
			? std::fabs( edgeTarget ) + kHeroCollisionRadius
			: Settings::CreepBlocker::SideStep;

		// SideStep is the floor, not the value: it keeps a sweep alive when the
		// wave is narrow or its extent is unavailable.
		float leg = ( std::max )( Settings::CreepBlocker::SideStep , sweepReach );

		// 135 degrees - cutting BACK across the wave's front - only when the
		// hero is far enough ahead that giving up that ground still leaves the
		// point in front of the front-most creep. Otherwise 45 degrees, gaining
		// ground while crossing. The rule decides which of the two angles is
		// available; it is never a free choice.
		const bool forwardDiagonal = -leg < minForward;
		if ( forwardDiagonal && minForward > leg )
		{
			// A short leg would land behind the creep. Grow the diagonal
			// instead of bending it - the angle is the thing being preserved.
			leg = minForward;
		}

		// The cap may shorten the diagonal, but never below what rule 1 needs.
		// Capping under minForward is what forced the end-of-order correction to
		// fire, and that correction is forward-only, so it bends the very angle
		// this branch exists to hold.
		leg = ( std::min )( leg , ( std::max )( kMaxForwardCommit , minForward ) );

		forwardStep = forwardDiagonal ? leg : -leg;
		lineShift = static_cast<float>( m_ZigSide ) * leg;
	}

	// m_CutSide is kept only so the log can still show which way he is cutting;
	// the sweep above is what actually steers him now.
	m_CutSide = m_ZigSide;

	// Built as an offset from the hero rather than from the world origin. The
	// old form reconstructed an absolute point from two absolute projections,
	// which is exact only while the frame holds still - and it does not, so a
	// few degrees of heading drift moved the point hundreds of units sideways
	// with nothing in the world having changed. While holding there is no front
	// creep to take ground height from, so the hero's own height stands in.
	Vector3 blockPoint(
		heroOrigin.m_x + direction.m_x * forwardStep + lateralAxis.m_x * lineShift ,
		heroOrigin.m_y + direction.m_y * forwardStep + lateralAxis.m_y * lineShift ,
		front.holding ? heroOrigin.m_z : front.groundZ );

	blockPoint = BlockPointClearOfCreeps( wave , lateralAxis , blockPoint );

	// Step toward the raw target in small increments rather than snapping to it.
	// Applied on every order now, with no retarget exemption: there is no target
	// identity left to change, so a large jump is noise in the wave estimate
	// rather than a real move, and snapping to noise is what pivots the hero.
	if ( m_Marker.valid )
	{
		const Vector3 step( blockPoint.m_x - m_Marker.blockPoint.m_x , blockPoint.m_y - m_Marker.blockPoint.m_y , 0.f );
		const float stepLength = Length2D( step );
		if ( stepLength > kMaxBlockPointStepPerOrder )
		{
			const float scale = kMaxBlockPointStepPerOrder / stepLength;
			blockPoint = Vector3( m_Marker.blockPoint.m_x + step.m_x * scale ,
				m_Marker.blockPoint.m_y + step.m_y * scale , blockPoint.m_z );
		}
	}

	// FIRST RULE, enforced last: the block point is IN FRONT OF EVERY CREEP,
	// along the direction the wave is walking. Always.
	//
	// EVERY creep, not just the one picked as leader. The leader is chosen from
	// the blockable band only, so a creep that slipped past that band but has
	// not escaped outright would sit ahead of the block point while the rule
	// still read as satisfied. Measuring the true front-most creep closes that
	// gap; when the leader IS front-most, which is the normal case, this comes
	// out identical.
	//
	// It is checked here, at the very end, against real world positions - not
	// asserted earlier and hoped for. Everything upstream works in offsets from
	// the HERO, and three separate stages move the point for reasons of their
	// own after those offsets are chosen: the standoff clamp, the sideways slide
	// that keeps the click off creep models, and the per-order step clamp. That
	// last one is the worst offender, because it drags the new point back toward
	// the PREVIOUS order's point, and the wave has walked forward since - so a
	// point that was correct when built lands behind the creep by the time it is
	// used.
	//
	// Any shortfall is pushed straight back out along the wave's heading. That
	// only ever moves the point down-lane, so it cannot bend the diagonal
	// sideways, and the floor is contact range: close enough that the hulls
	// touch, which is the only distance at which a block physically happens.
	float aheadOfLeader = 0.f;
	float aheadCorrection = 0.f;
	int creepsPastBlock = 0;

	// The same front-most creep the aim was built from, so the check and the aim
	// cannot disagree. Creeps beyond kEscapeTolerance are excluded from it on
	// purpose - they cannot be caught (lane creeps move 325, most heroes less),
	// so demanding the point stay in front of one would walk the hero down the
	// lane after a wave he has already lost.
	if ( front.hasFrontMost )
	{
		const Vector3 creepToBlock( blockPoint.m_x - front.frontMostOrigin.m_x ,
			blockPoint.m_y - front.frontMostOrigin.m_y , 0.f );
		aheadOfLeader = Dot2D( creepToBlock , direction );

		if ( aheadOfLeader < kMinAheadOfLeader )
		{
			aheadCorrection = kMinAheadOfLeader - aheadOfLeader;

			// Pushed along the DIAGONAL, not straight down-lane. A forward-only
			// shove satisfies the rule while flattening the very angle the order
			// was built around - a capture caught it taking 45 degrees down to
			// 29. Adding an equal sideways component on the side already being
			// cut to keeps the correction parallel to the order it is
			// correcting, so the angle survives.
			//
			// The lateral term is skipped while recovering, where the order is
			// deliberately not a 45 and adding one would slow the catch-up.
			const float lateralPart = behind ? 0.f
				: aheadCorrection * ( lineShift >= 0.f ? 1.f : -1.f );

			blockPoint = Vector3(
				blockPoint.m_x + direction.m_x * aheadCorrection + lateralAxis.m_x * lateralPart ,
				blockPoint.m_y + direction.m_y * aheadCorrection + lateralAxis.m_y * lateralPart ,
				blockPoint.m_z );
			aheadOfLeader = kMinAheadOfLeader;
		}
	}

	// Rule 1, verified rather than assumed: how many creeps still in play are
	// past the final block point. Should be zero on every order. Anything else
	// is the rule failing, and the count says how badly.
	const float blockAlong = Dot2D( blockPoint , direction );
	for ( const auto& creep : wave )
	{
		if ( creep.escaped )
			continue;
		if ( Dot2D( creep.origin , direction ) > blockAlong )
			++creepsPastBlock;
	}

	// Validation: check if block point is actually forward of hero along the expected direction
	const Vector3 heroToBlock( blockPoint.m_x - heroOrigin.m_x , blockPoint.m_y - heroOrigin.m_y , 0.f );
	const float heroToBlockDist = Length2D( heroToBlock );
	const float blockPointAlongDir = Dot2D( heroToBlock , direction );

	// Snapshot for the on-screen debug, taken from the values this order
	// actually used rather than re-derived while drawing - a re-derivation can
	// quietly disagree with the decision it is supposed to be showing.
	m_RuleDebug.hasFrontMost = front.hasFrontMost;
	m_RuleDebug.frontMostOrigin = front.frontMostOrigin;
	m_RuleDebug.pastCount = creepsPastBlock;
	m_RuleDebug.hasNextLeader = front.hasNextLeader;
	m_RuleDebug.nextLeaderOrigin = front.nextLeaderOrigin;
	m_RuleDebug.aimedAtNext = aimedAtNext;
	m_RuleDebug.legForward = blockPointAlongDir;
	m_RuleDebug.legSide = lineShift;
	m_RuleDebug.angleDegrees = ( std::fabs( blockPointAlongDir ) > 0.001f || std::fabs( lineShift ) > 0.001f )
		? std::atan2( std::fabs( lineShift ) , blockPointAlongDir ) * 57.2957795f
		: 0.f;
	m_RuleDebug.crashed = crashed;

	if ( shouldLog )
	{
		// A block point behind the hero is normal and not worth warning about:
		// it means the hero is out in front of the wave and is being walked
		// back into it. What is worth watching is how much the smoothing had to
		// correct - a large, sustained gap between the raw and working heading
		// means the wave is not marching coherently and every measurement
		// downstream is suspect.
		const float headingDrift = Length2D( Vector3( rawDirection.m_x - direction.m_x ,
			rawDirection.m_y - direction.m_y , 0.f ) );
		DEV_LOG( "  frame: raw_dir=(%.2f,%.2f) working_dir=(%.2f,%.2f) drift=%.2f\n" ,
			rawDirection.m_x , rawDirection.m_y , direction.m_x , direction.m_y , headingDrift );
	}

	m_Marker.heroOrigin = heroOrigin;
	m_Marker.blockPoint = blockPoint;
	m_Marker.valid = true;

	const HWND window = FeatureSupport::WindowReadyForInput();
	if ( !window )
	{
		m_Status = "Game window not focused";
		if ( shouldLog )
			DEV_LOG( "[creep-block] FAIL: game window not focused\n" );
		return false;
	}

	ImVec2 screen{};
	if ( !FeatureSupport::ProjectWorldToClient( window , blockPoint , true , screen ) )
	{
		m_Status = "Block point is off screen";
		if ( shouldLog )
			DEV_LOG( "[creep-block] FAIL: block point off screen point(%.0f,%.0f,%.0f)\n" ,
				blockPoint.m_x , blockPoint.m_y , blockPoint.m_z );
		return false;
	}

	if ( !FeatureSupport::MoveCursorToClientPoint( window , screen , m_PreviousCursor ) )
	{
		m_Status = "Could not aim at the block point";
		if ( shouldLog )
			DEV_LOG( "[creep-block] FAIL: could not move cursor to screen(%.0f,%.0f)\n" , screen.x , screen.y );
		return false;
	}

	m_Phase = OrderPhase::Click;
	m_NextPhaseTick = now + kClickDelayMs;

	// Track blocking state with duration safety limit
	if ( !m_isCreepBlocking )
	{
		m_BlockingStartTick = now;
		m_isCreepBlocking = true;
	}

	m_Status = front.holding
		? std::string( "Holding the line [" ) + GetTeamName( heroTeam ) + "] - front rank got by"
		: std::string( "Blocking [" ) + GetTeamName( heroTeam ) + "]";


	if ( shouldLog )
	{
		DEV_LOG( "[creep-block] %s team=%s wave=%zu rank=%d dir=(%.2f,%.2f)\n" ,
			front.holding ? "HOLD" : "BLOCK" , GetTeamName( heroTeam ) , wave.size() , front.rankSize ,
			direction.m_x , direction.m_y );
		// lead is how far the hero is in front of the leading blockable creep;
		// nearest is the closest creep of any kind, which is what has to fall
		// inside contact range before a bump can register.
		DEV_LOG( "  hero@(%.0f,%.0f) leads_front_edge_by=%.0f nearest_creep=%.0f (contact at %.0f)\n" ,
			heroOrigin.m_x , heroOrigin.m_y , -front.leadOffset , nearestCreep , kContactDetectRange );
		// RULE 1. past=0 is the rule holding: no creep still in play is ahead of
		// the block point. Any other value is the rule broken, and the number
		// says how many got by. ahead is the margin over the front-most creep,
		// measured on the FINAL point after every clamp and slide; it must never
		// read below min. corr is how far the rule had to shove the point back
		// out to keep that true - zero means everything upstream already agreed,
		// while a large steady corr means some stage above fights it every order.
		// hero_ahead is the one that answers "is my hero in front of every
		// creep" - the block point being in front is necessary but not the same
		// thing, and only this number says whether HE got there. Positive means
		// he is ahead of the front-most creep still in play; negative means that
		// many units behind it, and creeps are past him no matter what past=
		// says about the aim point.
		// mode=RECOVER means a creep is past him and the diagonal has been
		// dropped so every unit of his speed goes into closing the gap; at 45
		// degrees only 0.707 of it would, which is less than a creep walks, so
		// crossing while behind loses ground instead of gaining it. mode=BLOCK
		// is the normal state, rule 1 already satisfied and the diagonal free
		// to work. Sustained RECOVER means he cannot get back in front at all.
		// gone is how many creeps have been written off for the rest of this
		// hold, and (+n) how many joined them on this order. Every number on
		// these lines - hero_ahead, past, coverage, wave_lat - counts only the
		// creeps still in play, so a rising gone with the rest looking healthy
		// means the wave is being lost rather than blocked.
		DEV_LOG( "  RULE1 hero_ahead=%.0f mode=%s past=%d gone=%d(+%d) ahead=%.0f (min %.0f) corr=%.0f\n" ,
			front.hasFrontMost ? -front.frontMostLead : 0.f , behind ? "RECOVER" : "BLOCK" ,
			creepsPastBlock , m_EscapedCount , escapedThisOrder ,
			aheadOfLeader , kMinAheadOfLeader , aheadCorrection );

		// FRONT. Diagnostics to settle whether a hero_ahead collapse is one creep
		// really lagging or the same order re-projected onto a jerked heading.
		// ent is the front-most creep's identity: it holding across a jump in lead
		// means reprojection or that one creep drifting; ent hopping means the
		// lead is being traded around (membership/estimator noise). lat is that
		// creep's offset across the lane - a large lat next to a drift spike on
		// the frame line is how a few degrees of heading swing move lead by a
		// hundred units. second is the next creep's lead; lead jumping while
		// second stays put is a single-creep effect, both moving together is the
		// whole set being re-projected.
		DEV_LOG( "  FRONT ent=%p lead=%.0f lat=%.0f second=%.0f gap=%.0f\n" ,
			static_cast<const void*>( front.hasFrontMost ? front.frontMostEntity : nullptr ) ,
			front.hasFrontMost ? front.frontMostLead : 0.f ,
			front.hasFrontMost ? front.frontMostLateral : 0.f ,
			front.hasSecond ? front.secondLead : 0.f ,
			( front.hasFrontMost && front.hasSecond ) ? front.frontMostLead - front.secondLead : 0.f );

		// RULE 2. next_lat is where the creep about to inherit the lead sits
		// across the lane, from the hero. aim=1 means the diagonal is pointed at
		// it, which is the rule working; aim=0 means it was too near straight
		// behind the leader to say, so the sweep is alternating instead. zig is
		// the side being cut to, and crash=1 marks the order where contact
		// reversed it.
		// wave_lat is the wave's left and right edges as seen from the hero, and
		// the sweep has to run between them. Read it against line_shift on the
		// DIAG line: a shift that never reaches either edge is a hero crossing
		// beside the wave, and the creeps outside his sweep are the ones that
		// escape while he blocks the first. coverage says how many of the rank
		// his hull actually reaches from where he is now; well under rank means
		// most of the wave is walking past untouched.
		DEV_LOG( "  RULE2 next_lat=%.0f aim=%d zig=%+d crash=%d wave_lat=[%.0f,%.0f] bumped=%d(+%d) coverage=%.2f/%d\n" ,
			front.hasNextLeader ? front.nextLateral : 0.f , aimedAtNext ? 1 : 0 ,
			m_ZigSide , m_FreshCrash ? 1 : 0 ,
			front.hasExtent ? front.lateralMin : 0.f , front.hasExtent ? front.lateralMax : 0.f ,
			front.bumpedCount , freshContacts , front.coverage , front.rankSize );

		// THE DIAGONAL. fwd and side are the two legs; they are equal by
		// construction, so angle should read 45 or 135 and nothing else. A
		// reading that is not one of those two means something downstream bent
		// the order after the legs were set.
		const float legAngle = ( std::fabs( blockPointAlongDir ) > 0.001f || std::fabs( lineShift ) > 0.001f )
			? std::atan2( std::fabs( lineShift ) , blockPointAlongDir ) * 57.2957795f
			: 0.f;
		DEV_LOG( "  DIAG fwd=%.0f side=%.0f angle=%.0f block@(%.0f,%.0f) dist=%.0f down_lane=%.0f (limit %.0f)\n" ,
			blockPointAlongDir , lineShift , legAngle ,
			blockPoint.m_x , blockPoint.m_y , heroToBlockDist , blockPointAlongDir , kMaxForwardCommit );
	}

	return true;
}

// Newest contact wins its slot; a creep already in the table just has its
// timestamp refreshed, so leaning on one creep for a while does not fill the
// table with copies of it. When the table is full the least recently touched
// entry is reused - empty slots have tick 0 and so are always chosen first.
auto CCreepBlocker::RegisterBump( C_BaseEntity* entity , uint32_t now ) -> void
{
	if ( !entity )
		return;

	int oldest = 0;
	for ( int index = 0; index < kBumpMemory; ++index )
	{
		if ( m_Bumped[index].entity == entity )
		{
			m_Bumped[index].tick = now;
			return;
		}

		if ( m_Bumped[index].tick < m_Bumped[oldest].tick )
			oldest = index;
	}

	m_Bumped[oldest].entity = entity;
	m_Bumped[oldest].tick = now;
}

auto CCreepBlocker::IsBumped( const C_BaseEntity* entity , uint32_t now ) const -> bool
{
	if ( !entity )
		return false;

	for ( const auto& bump : m_Bumped )
	{
		if ( bump.entity == entity )
			return now >= bump.tick && now - bump.tick < kBumpCooldownMs;
	}

	return false;
}

// No ageing and no eviction, unlike the bump table. A creep that has got away
// has got away for the rest of the hold, and the table is cleared when the key
// comes up. Once it is full the extra creeps simply keep being re-tested, which
// is the old behaviour and no worse than it was.
auto CCreepBlocker::RegisterEscaped( C_BaseEntity* entity ) -> void
{
	if ( !entity || m_EscapedCount >= kEscapeMemory || IsEscaped( entity ) )
		return;

	m_Escaped[m_EscapedCount++] = entity;
}

auto CCreepBlocker::IsEscaped( const C_BaseEntity* entity ) const -> bool
{
	if ( !entity )
		return false;

	for ( int index = 0; index < m_EscapedCount; ++index )
	{
		if ( m_Escaped[index] == entity )
			return true;
	}

	return false;
}

auto CCreepBlocker::DrawBlockMarker() const -> void
{
	if ( !Settings::CreepBlocker::DrawBlockMarker )
		return;

	auto* drawList = ImGui::GetForegroundDrawList();

	// Where each creep is pointing, drawn from the creep itself. This is the
	// facing the heading estimator reads, so it is the input to check first when
	// the hero walks somewhere strange: arrows that disagree with each other, or
	// with the lane arrow below, mean the frame everything is measured in is a
	// guess. A creep with no replicated yaw gets the lane heading instead and is
	// drawn faint, so a wave of faint arrows says the facing path is unavailable
	// and the estimate is coming from travel alone.
	for ( int index = 0; index < m_CreepDebugCount; ++index )
	{
		const auto& creep = m_CreepDebug[index];

		ImVec2 creepScreen{};
		if ( !Math::WorldToScreen( creep.origin , creepScreen ) )
			continue;

		// Contact state, so the hand-off can be watched happening: white while
		// the hero is against it, grey once it counts as dealt with.
		ImU32 ringColor = IM_COL32( 90 , 170 , 255 , 200 );
		if ( creep.contact )
			ringColor = IM_COL32( 255 , 255 , 255 , 255 );
		else if ( creep.bumped )
			ringColor = IM_COL32( 130 , 130 , 130 , 180 );

		drawList->AddCircle( creepScreen , 7.f , ringColor , 12 , 1.5f );

		constexpr float kArrowLength = 90.f;
		const Vector3 tip( creep.origin.m_x + creep.facing.m_x * kArrowLength ,
			creep.origin.m_y + creep.facing.m_y * kArrowLength , creep.origin.m_z );

		ImVec2 tipScreen{};
		if ( !Math::WorldToScreen( tip , tipScreen ) )
			continue;

		const ImU32 arrowColor = creep.hasFacing
			? IM_COL32( 90 , 170 , 255 , 230 )
			: IM_COL32( 90 , 170 , 255 , 80 );
		drawList->AddLine( creepScreen , tipScreen , arrowColor , creep.hasFacing ? 2.f : 1.f );
		drawList->AddCircleFilled( tipScreen , creep.hasFacing ? 3.f : 2.f , arrowColor , 8 );
	}

	// The working lane heading, from the hero. This is the frame the front edge,
	// the escape test and the block point are all measured in, so it should
	// point the same way the creep arrows do. When it does not, that difference
	// is the bug rather than anything downstream of it.
	if ( m_HasDebugFrame && m_HasLaneDirection )
	{
		constexpr float kLaneArrowLength = 200.f;
		const Vector3 tip( m_DebugHeroOrigin.m_x + m_LaneDirection.m_x * kLaneArrowLength ,
			m_DebugHeroOrigin.m_y + m_LaneDirection.m_y * kLaneArrowLength , m_DebugHeroOrigin.m_z );

		ImVec2 originScreen{};
		ImVec2 tipScreen{};
		if ( Math::WorldToScreen( m_DebugHeroOrigin , originScreen ) && Math::WorldToScreen( tip , tipScreen ) )
		{
			const ImU32 laneColor = IM_COL32( 255 , 200 , 60 , 220 );
			drawList->AddLine( originScreen , tipScreen , laneColor , 2.5f );
			drawList->AddCircleFilled( tipScreen , 4.f , laneColor , 10 );
		}
	}

	if ( !m_Marker.valid )
		return;

	ImVec2 blockScreen{};
	if ( !Math::WorldToScreen( m_Marker.blockPoint , blockScreen ) )
		return;

	// The block point is always red. It marks one thing - the spot the hero is
	// being sent to - and a marker that changes colour with state has to be
	// decoded before it can be read. On station is still distinguishable: the
	// ring grows and gains an inner one.
	const Vector3 heroToBlock( m_Marker.blockPoint.m_x - m_Marker.heroOrigin.m_x ,
		m_Marker.blockPoint.m_y - m_Marker.heroOrigin.m_y , 0.f );
	const bool onStation = Length2D( heroToBlock ) < 50.f && m_isCreepBlocking;

	const ImU32 blockColor = IM_COL32( 255 , 50 , 50 , 255 );
	drawList->AddCircle( blockScreen , onStation ? 20.f : 14.f , blockColor , 24 , 2.5f );
	if ( onStation )
		drawList->AddCircle( blockScreen , 8.f , blockColor , 16 , 1.5f );

	ImVec2 heroScreen{};
	const bool hasHero = Math::WorldToScreen( m_Marker.heroOrigin , heroScreen );
	if ( hasHero )
		drawList->AddLine( heroScreen , blockScreen , blockColor , 1.5f );

	// RULE 1, drawn. Yellow ring on the front-most creep still in play - the one
	// the block point must stay ahead of. A red bar through it means creeps have
	// got past the block point, which is the rule broken and the thing to look
	// at before anything else on screen.
	if ( m_RuleDebug.hasFrontMost )
	{
		ImVec2 frontScreen{};
		if ( Math::WorldToScreen( m_RuleDebug.frontMostOrigin , frontScreen ) )
		{
			const bool ruleBroken = m_RuleDebug.pastCount > 0;
			const ImU32 ruleColor = ruleBroken
				? IM_COL32( 255 , 60 , 60 , 255 )
				: IM_COL32( 255 , 210 , 60 , 230 );
			drawList->AddCircle( frontScreen , 12.f , ruleColor , 20 , 2.5f );
			if ( ruleBroken )
			{
				drawList->AddLine( ImVec2( frontScreen.x - 16.f , frontScreen.y ) ,
					ImVec2( frontScreen.x + 16.f , frontScreen.y ) , ruleColor , 3.f );
			}
		}
	}

	// RULE 2, drawn. Cyan ring on the creep about to inherit the lead, with a
	// line from the block point to it when the diagonal is aimed there. That
	// line is the anticipation working: the hero is already crossing toward the
	// creep he will have to block next. No line means the next creep was too
	// near straight behind the leader to give a side, so the sweep is
	// alternating instead.
	if ( m_RuleDebug.hasNextLeader )
	{
		ImVec2 nextScreen{};
		if ( Math::WorldToScreen( m_RuleDebug.nextLeaderOrigin , nextScreen ) )
		{
			const ImU32 nextColor = m_RuleDebug.aimedAtNext
				? IM_COL32( 80 , 220 , 255 , 240 )
				: IM_COL32( 80 , 220 , 255 , 110 );
			drawList->AddCircle( nextScreen , 9.f , nextColor , 16 , 2.f );
			if ( m_RuleDebug.aimedAtNext )
				drawList->AddLine( blockScreen , nextScreen , nextColor , 2.f );
		}
	}

	// The diagonal, as a number, next to the point it describes. It should read
	// 45 or 135 and nothing else - the two legs are built equal, so any other
	// value means something bent the order after they were set. Green while it
	// holds, red the moment it does not.
	char angleText[48];
	const int angle = static_cast<int>( m_RuleDebug.angleDegrees + 0.5f );
	const bool angleOk = std::abs( angle - 45 ) <= 3 || std::abs( angle - 135 ) <= 3;
	snprintf( angleText , sizeof( angleText ) , "%d deg%s past=%d" ,
		angle , m_RuleDebug.crashed ? " CRASH" : "" , m_RuleDebug.pastCount );
	drawList->AddText( ImVec2( blockScreen.x + 18.f , blockScreen.y - 8.f ) ,
		angleOk ? IM_COL32( 120 , 255 , 140 , 235 ) : IM_COL32( 255 , 90 , 90 , 235 ) ,
		angleText );
}

auto CCreepBlocker::ValidateBlockingConditions( uint32_t now ) -> bool
{
	// Quick validation that blocking should remain active
	// Returns false if any condition fails

	// Safety: enforce maximum block duration
	if ( m_BlockingStartTick && now - m_BlockingStartTick > kMaxBlockDurationMs )
		return false;

	const auto& offsets = FeatureSupport::ResolveOffsets();
	if ( !offsets.resolved )
		return false;

	auto* entitySystem = SDK::Interfaces::GameEntitySystem();
	C_BaseEntity* hero = nullptr;
	int heroIndex = -1;
	if ( !CLocalHeroResolver::Resolve( entitySystem , hero , heroIndex ) )
		return false;

	if ( FeatureSupport::ReadField<int>( hero , offsets.health , 0 ) <= 0 )
		return false;

	Vector3 heroOrigin{};
	if ( !FeatureSupport::TryReadOrigin( hero , offsets , heroOrigin ) )
		return false;

	const uint8_t heroTeam = FeatureSupport::ReadField<uint8_t>( hero , offsets.team , 0 );
	if ( !FeatureSupport::IsPlayableTeam( heroTeam ) )
		return false;

	std::vector<WaveCreep> wave;
	CollectWave( entitySystem , offsets , heroOrigin , heroTeam , wave );
	if ( wave.empty() )
		return false;

	return true;
}
