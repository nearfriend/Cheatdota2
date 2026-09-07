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
	constexpr float kMaxForwardCommit = 40.f;

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
		for ( const auto& creep : wave )
		{
			const float lead = LeadOverHero( creep , direction , heroProjection );
			if ( lead > kEscapeTolerance )
				continue;

			++presentCount;
			if ( lead > kBlockableTolerance )
				continue;

			if ( !hasLead || lead > leadOffset )
			{
				leadOffset = lead;
				front.groundZ = creep.origin.m_z;
				hasLead = true;
			}
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
			if ( lead > kEscapeTolerance )
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

	// The lane's own frame: along it, and across it.
	const Vector3 lateralAxis( -direction.m_y , direction.m_x , 0.f );
	const float heroProjection = Dot2D( heroOrigin , direction );

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

	// Across the lane, walk onto the covering line rather than jumping to it.
	// SideStep caps how far one order may pull the hero sideways, so a change of
	// line is crossed over several orders with his body sweeping the ground in
	// between - which blocks creeps on the way - instead of teleporting the aim
	// point across and leaving that ground open.
	// front.lateral is already measured from the hero, so it IS the shift.
	const float lineShift = std::clamp( front.lateral ,
		-Settings::CreepBlocker::SideStep , Settings::CreepBlocker::SideStep );

	m_HeldLineEntity = front.lineEntity;

	// Along the lane, the plug stands a fixed standoff in front of the leading
	// edge. Standoff is the whole game: too short and the click resolves behind
	// the leader so it walks past, too long and the hero is out in front of the
	// wave rather than in it, giving the creeps free ground every order.
	//
	// While holding, there is no leading edge left to stand in front of, so the
	// hero keeps the line he is on and only works sideways.
	float forwardStep = front.holding
		? 0.f
		: front.leadOffset + Settings::CreepBlocker::BlockAhead;

	// The invariant that makes following impossible, whatever the estimate says:
	// an order may never send the hero more than contact range down-lane of
	// where he already stands. Everything above is meant to respect this
	// already - the front edge only ever comes from creeps he is in front of -
	// but this is the property that actually matters, so it is enforced rather
	// than assumed. Walking down-lane is how the hero ends up trailing the wave,
	// and he is slower than the creeps, so that ground never comes back.
	forwardStep = ( std::min )( forwardStep , kMaxForwardCommit );

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

	// Validation: check if block point is actually forward of hero along the expected direction
	const Vector3 heroToBlock( blockPoint.m_x - heroOrigin.m_x , blockPoint.m_y - heroOrigin.m_y , 0.f );
	const float heroToBlockDist = Length2D( heroToBlock );
	const float blockPointAlongDir = Dot2D( heroToBlock , direction );

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
		// down_lane is the number that matters for the following bug: it is how
		// far the order sends the hero in the wave's own direction. It must
		// never exceed kMaxForwardCommit.
		// coverage is the weighted count of front-rank creeps whose hulls the
		// hero actually reaches from the chosen line. Near zero while creeps
		// are present means he is standing on empty ground blocking nobody.
		// coverage is the weighted count of front-rank creeps whose hulls the
		// hero actually reaches from the chosen line. Near zero while creeps
		// are present means he is standing on empty ground blocking nobody.
		// bumped is how many of the rank he has already crashed into and handed
		// off; a fresh contact is what moves him onto the next creep's line.
		DEV_LOG( "  line_shift=%.0f coverage=%.2f/%d bumped=%d(+%d) block_point@(%.0f,%.0f) dist=%.0f down_lane=%.0f (limit %.0f)\n" ,
			lineShift , front.coverage , front.rankSize , front.bumpedCount , freshContacts ,
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

auto CCreepBlocker::DrawBlockMarker() const -> void
{
	if ( !Settings::CreepBlocker::DrawBlockMarker || !m_Marker.valid )
		return;

	ImVec2 blockScreen{};
	if ( !Math::WorldToScreen( m_Marker.blockPoint , blockScreen ) )
		return;

	auto* drawList = ImGui::GetForegroundDrawList();

	// Distance from hero to block point
	const Vector3 heroToBlock( m_Marker.blockPoint.m_x - m_Marker.heroOrigin.m_x ,
		m_Marker.blockPoint.m_y - m_Marker.heroOrigin.m_y , 0.f );
	const float distToBlock = Length2D( heroToBlock );

	// RED circle when hero is at block point (ready for next creep)
	// GREEN circle when blocking is active
	// YELLOW circle when idle
	ImU32 color;
	float circleSize = 16.f;
	if ( distToBlock < 50.f && m_isCreepBlocking )
	{
		// Hero at block point: RED - ready to receive next creep
		color = IM_COL32( 255 , 50 , 50 , 255 );
		circleSize = 20.f;  // Larger to indicate active blocking position
	}
	else if ( m_isCreepBlocking )
	{
		// Actively blocking: GREEN
		color = IM_COL32( 90 , 220 , 130 , 235 );
	}
	else
	{
		// Idle: YELLOW
		color = IM_COL32( 235 , 190 , 60 , 235 );
	}

	drawList->AddCircle( blockScreen , circleSize , color , 24 , 2.f );

	ImVec2 heroScreen{};
	if ( Math::WorldToScreen( m_Marker.heroOrigin , heroScreen ) )
		drawList->AddLine( heroScreen , blockScreen , color , 1.5f );
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
