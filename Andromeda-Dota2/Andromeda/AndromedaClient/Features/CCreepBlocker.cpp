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
	// Order timing: empirically tuned balance between responsiveness and command queueing.
	// 60ms enables hero to catch up and position ahead of incoming creep wave.
	constexpr uint32_t kOrderIntervalMs = 60;
	constexpr uint32_t kClickDelayMs = 40;
	constexpr uint32_t kRestoreDelayMs = 30;

	// Spatial parameters
	constexpr float kWaveSearchRadius = 900.f;
	constexpr float kCreepClearance = 70.f;
	constexpr float kMinWaveCoherence = 0.55f;

	// Direction tracking: how far wave's center must move to confirm heading direction
	constexpr float kMinTravelDistance = 25.f;
	constexpr uint32_t kTravelSampleMaxAgeMs = 1000;

	// Continuous blocking configuration
	// Hero chases and collides with front creep continuously
	// As creep tries to escape, hero follows and blocks the path
	constexpr float kBlockingFollowDistance = 30.f;

	// A creep this far ahead of the hero along the wave's heading counts as
	// already past and unblockable - chasing it from behind can't close the
	// gap, so it's dropped in favor of a creep still in reach. Kept tight,
	// just over the ~30 unit design offset that kBlockingFollowDistance
	// already puts between hero and creep during healthy blocking plus one
	// order's worth of creep travel (a creep covers roughly 20-45 units per
	// ~70-130ms order cycle), so a creep that's genuinely gotten away is
	// dropped almost as soon as it happens rather than after a long chase.
	constexpr float kMaxCatchUpDistance = 90.f;

	// Once an order has already been issued, a single order may only move
	// the block point this far from the last one. The raw target can jump a
	// long way between orders - the front creep can swap, and the wave
	// direction estimate is itself noisy - and clicking straight to a
	// distant new point can overshoot past the creep and drop contact.
	// Stepping toward it in small increments instead keeps the hero pressed
	// against the creep on every order.
	constexpr float kMaxBlockPointStepPerOrder = 50.f;

	// Safety: prevent infinite blocking (5 minute max to handle edge cases)
	constexpr uint32_t kMaxBlockDurationMs = 300000;

	struct WaveCreep
	{
		Vector3 origin{};
		float yaw = 0.f;
		bool hasYaw = false;
		// Identity of the underlying entity, so the caller can tell whether
		// the front creep picked this order is the same one as last order or
		// a genuine retarget - a position-distance heuristic can't tell that
		// apart reliably, an entity pointer can.
		C_BaseEntity* entity = nullptr;
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

	// Prefers the most-advanced creep the hero can still get ahead of - one
	// that hasn't outrun the hero's own position along the wave's heading by
	// more than kMaxCatchUpDistance. That way, once the creep being blocked
	// slips past, the next order retargets whichever creep hasn't escaped.
	//
	// Falls back to the single most-advanced creep in the wave when none are
	// in reach - covers both "the whole wave got past" and the hero simply
	// not having caught up to any of it yet (e.g. the initial approach) -
	// so there is always a direction to chase instead of giving up.
	auto FrontCreep( const std::vector<WaveCreep>& wave , const Vector3& direction , const Vector3& heroOrigin ) -> const WaveCreep*
	{
		const float heroProjection = Dot2D( heroOrigin , direction );

		const WaveCreep* reachableFront = nullptr;
		float bestReachableProjection = 0.f;
		const WaveCreep* overallFront = nullptr;
		float bestProjection = 0.f;

		for ( const auto& creep : wave )
		{
			const float projection = Dot2D( creep.origin , direction );

			if ( !overallFront || projection > bestProjection )
			{
				overallFront = &creep;
				bestProjection = projection;
			}

			if ( projection - heroProjection > kMaxCatchUpDistance )
				continue;

			if ( !reachableFront || projection > bestReachableProjection )
			{
				reachableFront = &creep;
				bestReachableProjection = projection;
			}
		}

		return reachableFront ? reachableFront : overallFront;
	}

	// Pushed further ahead until it is clear of every creep model, so the
	// right click lands on ground. A right click that lands on an allied creep
	// is a follow order: the hero would trail the wave instead of blocking it.
	auto BlockPointClearOfCreeps( const std::vector<WaveCreep>& wave , const Vector3& direction , Vector3 point ) -> Vector3
	{
		for ( int attempt = 0; attempt < 3; ++attempt )
		{
			bool clear = true;
			for ( const auto& creep : wave )
			{
				if ( FeatureSupport::Distance2D( creep.origin , point ) < kCreepClearance )
				{
					clear = false;
					break;
				}
			}

			if ( clear )
				break;

			point = Vector3( point.m_x + direction.m_x * kCreepClearance ,
				point.m_y + direction.m_y * kCreepClearance , point.m_z );
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
		return;
	}

	if ( Settings::CreepBlocker::Key <= 0 )
	{
		m_Status = "No key bound";
		m_isCreepBlocking = false;
		m_Marker.valid = false;
		return;
	}

	const bool keyDown = ( GetAsyncKeyState( Settings::CreepBlocker::Key ) & 0x8000 ) != 0;
	if ( !keyDown )
	{
		m_Status = "Idle - hold the key while walking to lane";
		m_isCreepBlocking = false;
		m_Marker.valid = false;
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
		// If order fails, clear blocking state to avoid stale state
		m_isCreepBlocking = false;
	}

	// Use shorter interval (30ms) when in contact, normal interval (60ms) otherwise
	uint32_t nextInterval = kOrderIntervalMs;
	if ( m_Marker.valid && orderIssued )
	{
		const Vector3 heroToBlock( m_Marker.blockPoint.m_x - m_Marker.heroOrigin.m_x ,
			m_Marker.blockPoint.m_y - m_Marker.heroOrigin.m_y , 0.f );
		const float distToBlock = Length2D( heroToBlock );
		if ( distToBlock < 100.f )
		{
			nextInterval = 30;
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
			DEV_LOG( "[creep-block] FAIL: hero team unavailable (team=%d)\n" , (int)heroTeam );
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

	Vector3 direction{};
	if ( !TryWaveDirection( wave , now , direction ) )
	{
		m_Status = "Wave is not marching - nothing to block";
		if ( shouldLog )
			DEV_LOG( "[creep-block] FAIL: wave not marching (wave_size=%zu)\n" , wave.size() );
		return false;
	}

	// Creep stickiness: if already blocking a creep and haven't reached the
	// minimum order count, stay with the current one even if another creep
	// is slightly more advanced. This prevents flipping between creeps.
	const WaveCreep* front = nullptr;
	if ( m_isCreepBlocking && m_BlockingOrderCount < kMinOrdersPerTarget && m_Marker.valid && m_Marker.entity )
	{
		// Try to find the current target in the wave
		for ( const auto& creep : wave )
		{
			if ( creep.entity == m_Marker.entity )
			{
				front = &creep;
				break;
			}
		}
	}

	// If no sticky target or not found, select the normal front creep
	if ( !front )
	{
		front = FrontCreep( wave , direction , heroOrigin );
		m_BlockingOrderCount = 0;
	}
	else
	{
		m_BlockingOrderCount++;
	}

	if ( !front || !front->entity )
	{
		m_Status = "No leading creep";
		if ( shouldLog )
			DEV_LOG( "[creep-block] FAIL: no leading creep found (wave_size=%zu)\n" , wave.size() );
		return false;
	}

	// Target the front creep's CURRENT position to chase and collide continuously
	// As creep tries to walk around, hero follows and blocks escape paths
	const Vector3 lateral( -direction.m_y , direction.m_x , 0.f );
	const Vector3 heroToCreep( front->origin.m_x - heroOrigin.m_x , front->origin.m_y - heroOrigin.m_y , 0.f );
	const float heroToCreepDist = Length2D( heroToCreep );
	const float creepOffset = Dot2D( heroToCreep , lateral );
	const float creepAlongDir = Dot2D( heroToCreep , direction );

	// Cut off escape paths: if creep is to the side, push block point to that side
	// This prevents creeps from walking around the hero
	const float side = std::clamp( creepOffset , -Settings::CreepBlocker::SideStep , Settings::CreepBlocker::SideStep );

	// Block point: position hero to intercept the creep.
	// Strategy depends on creep position relative to hero:
	// - If creep is ahead: position hero behind/under it (normal blocking)
	// - If creep is behind: move hero directly to the creep (catch-up mode)
	// The wave will push the creep forward; we position to meet it.
	Vector3 blockPoint;
	if ( creepAlongDir >= -20.f )
	{
		// Creep is ahead or roughly at same position: use normal blocking formula
		blockPoint = Vector3(
			front->origin.m_x + lateral.m_x * ( side - creepOffset ) - direction.m_x * kBlockingFollowDistance ,
			front->origin.m_y + lateral.m_y * ( side - creepOffset ) - direction.m_y * kBlockingFollowDistance ,
			front->origin.m_z );
	}
	else
	{
		// Creep is behind: move hero directly to the creep so we're in position when it comes forward
		// Add a small forward bias along the wave direction to be ready
		blockPoint = Vector3(
			front->origin.m_x + direction.m_x * 50.f ,
			front->origin.m_y + direction.m_y * 50.f ,
			front->origin.m_z );
	}
	blockPoint = BlockPointClearOfCreeps( wave , direction , blockPoint );

	// Step toward the raw target in small increments rather than snapping to
	// it, but ONLY if we're still targeting the same creep from the last order.
	// If we've retargeted to a new creep (because the old one got away), snap
	// to the new target instead - otherwise we'd creep toward it for many orders
	// before getting close enough to maintain blocking contact.
	const bool sameCreep = m_Marker.valid && m_Marker.entity == front->entity;
	if ( m_Marker.valid && sameCreep )
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
		DEV_LOG( "  VALIDATION: heroToBlock_dist=%.0f, projection_along_dir=%.0f (should be positive)\n" ,
			heroToBlockDist , blockPointAlongDir );
		if ( blockPointAlongDir < 0 )
		{
			DEV_LOG( "  WARNING: block point is BEHIND hero (not ahead along wave direction)!\n" );
		}
		if ( heroToBlockDist < 30.f )
		{
			DEV_LOG( "  WARNING: hero already very close to block point (%.0f units), might not move!\n" , heroToBlockDist );
		}
	}

	// Track if we're switching to a new creep
	if ( !m_Marker.valid || m_Marker.entity != front->entity )
	{
		m_BlockingOrderCount = 0;
	}

	m_Marker.heroOrigin = heroOrigin;
	m_Marker.blockPoint = blockPoint;
	m_Marker.entity = front->entity;
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
		m_Status = "Blocking";
	}
	else
	{
		m_Status = "Blocking (continuous)";
	}

	if ( shouldLog )
	{
		const bool targetChanged = !m_Marker.valid || m_Marker.entity != front->entity;
		const Vector3 heroToBlock( blockPoint.m_x - heroOrigin.m_x , blockPoint.m_y - heroOrigin.m_y , 0.f );
		const float heroToBlockDist = Length2D( heroToBlock );
		DEV_LOG( "[creep-block] SUCCESS wave=%zu dir=(%.2f,%.2f) target=%p\n" , wave.size() , direction.m_x , direction.m_y , front->entity );
		DEV_LOG( "  creep@(%.0f,%.0f) hero@(%.0f,%.0f) heroToCreep_dist=%.0f along_dir=%.0f offset_side=%.0f\n" ,
			front->origin.m_x , front->origin.m_y , heroOrigin.m_x , heroOrigin.m_y , heroToCreepDist , creepAlongDir , creepOffset );
		DEV_LOG( "  side_clamped=%.0f block_point@(%.0f,%.0f) heroToBlock_dist=%.0f step=%s clamped=%s\n" ,
			side , blockPoint.m_x , blockPoint.m_y , heroToBlockDist , targetChanged ? "RETARGET" : "same", sameCreep && m_Marker.valid ? "YES" : "no" );
	}

	return true;
}

auto CCreepBlocker::DrawBlockMarker() const -> void
{
	if ( !Settings::CreepBlocker::DrawBlockMarker || !m_Marker.valid )
		return;

	ImVec2 blockScreen{};
	if ( !Math::WorldToScreen( m_Marker.blockPoint , blockScreen ) )
		return;

	auto* drawList = ImGui::GetForegroundDrawList();
	const ImU32 color = m_isCreepBlocking ? IM_COL32( 90 , 220 , 130 , 235 ) : IM_COL32( 235 , 190 , 60 , 235 );
	drawList->AddCircle( blockScreen , 16.f , color , 24 , 2.f );

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
