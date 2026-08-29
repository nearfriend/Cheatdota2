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
	// A move order every ~180 ms. Faster than this and each order cancels the
	// previous one before the hero has taken a step - it stutters in place
	// instead of walking; much slower and the creeps walk around during the
	// gap.
	constexpr uint32_t kOrderIntervalMs = 180;
	constexpr uint32_t kClickDelayMs = 40;
	constexpr uint32_t kRestoreDelayMs = 30;

	// Only creeps this close to the hero count as "our wave".
	constexpr float kWaveSearchRadius = 900.f;
	// A block point closer than this to a creep risks the right click landing
	// on the creep itself, which is a follow order rather than a move.
	constexpr float kCreepClearance = 70.f;
	// The wave has to agree on a direction. Creeps that have stopped to fight
	// face every which way, and their average points nowhere - blocking is
	// over at that point anyway.
	constexpr float kMinWaveCoherence = 0.55f;
	// Travel-based direction: how far the wave's centre must move between
	// samples to count as marching, and how stale a sample may be before it
	// says nothing about the current heading.
	constexpr float kMinTravelDistance = 25.f;
	constexpr uint32_t kTravelSampleMaxAgeMs = 1000;

	struct WaveCreep
	{
		Vector3 origin{};
		float yaw = 0.f;
		bool hasYaw = false;
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

	auto FrontCreep( const std::vector<WaveCreep>& wave , const Vector3& direction ) -> const WaveCreep*
	{
		const WaveCreep* front = nullptr;
		float bestProjection = 0.f;
		for ( const auto& creep : wave )
		{
			const float projection = Dot2D( creep.origin , direction );
			if ( !front || projection > bestProjection )
			{
				front = &creep;
				bestProjection = projection;
			}
		}
		return front;
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

	// An order already in flight always finishes, even if the key was released
	// mid-sequence: abandoning it between the click and the restore would
	// leave the player's cursor parked out in the lane.
	AdvanceOrder( now );

	if ( !Settings::CreepBlocker::Enable )
	{
		m_Status = "Disabled";
		return;
	}

	if ( Settings::CreepBlocker::Key <= 0 )
	{
		m_Status = "No key bound";
		return;
	}

	const bool keyDown = ( GetAsyncKeyState( Settings::CreepBlocker::Key ) & 0x8000 ) != 0;
	if ( !keyDown )
	{
		m_Status = "Idle - hold the key while walking to lane";
		return;
	}

	if ( m_Phase != OrderPhase::Idle || now < m_NextOrderTick )
		return;

	// Same cadence whether or not an order went out: a failed attempt means
	// there was nothing to block, and re-running the entity scan every frame
	// to find that out again would cost far more than waiting a tick.
	TryIssueBlockOrder( now );
	m_NextOrderTick = now + kOrderIntervalMs;
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
	const auto& offsets = FeatureSupport::ResolveOffsets();
	if ( !offsets.resolved )
	{
		m_Status = "Waiting for schema";
		return false;
	}

	auto* entitySystem = SDK::Interfaces::GameEntitySystem();
	C_BaseEntity* hero = nullptr;
	int heroIndex = -1;
	if ( !CLocalHeroResolver::Resolve( entitySystem , hero , heroIndex ) )
	{
		m_Status = "Local hero not resolved";
		return false;
	}

	// A dead hero still resolves and still has a (stale) position, so without
	// this the feature would keep firing move orders at the wave through the
	// respawn timer.
	if ( FeatureSupport::ReadField<int>( hero , offsets.health , 0 ) <= 0 )
	{
		m_Status = "Hero is dead";
		return false;
	}

	Vector3 heroOrigin{};
	if ( !FeatureSupport::TryReadOrigin( hero , offsets , heroOrigin ) )
	{
		m_Status = "Hero position unavailable";
		return false;
	}

	const uint8_t heroTeam = FeatureSupport::ReadField<uint8_t>( hero , offsets.team , 0 );
	if ( !FeatureSupport::IsPlayableTeam( heroTeam ) )
	{
		m_Status = "Hero team unavailable";
		return false;
	}

	std::vector<WaveCreep> wave;
	CollectWave( entitySystem , offsets , heroOrigin , heroTeam , wave );
	if ( wave.empty() )
	{
		m_Status = "No allied wave nearby";
		return false;
	}

	Vector3 direction{};
	if ( !TryWaveDirection( wave , now , direction ) )
	{
		m_Status = "Wave is not marching - nothing to block";
		return false;
	}

	const WaveCreep* front = FrontCreep( wave , direction );
	if ( !front )
	{
		m_Status = "No leading creep";
		return false;
	}

	// Ahead of the leading creep, and on ITS line rather than a side picked
	// blindly. Alternating sides every order (which this did first) makes the
	// hero spend the whole walk crossing back and forth: at ~300 move speed it
	// covers barely 54 units in one 180 ms order, so it never arrives anywhere
	// and never actually stands in the way.
	//
	// Stepping toward wherever the creep currently is instead means the hero
	// converges on its path and then tracks it - when the creep tries to walk
	// around, its offset changes and the next order follows it across. SideStep
	// caps how far sideways one order may pull, so a creep that is wide of the
	// hero does not send it sprinting across the lane.
	const Vector3 lateral( -direction.m_y , direction.m_x , 0.f );
	const Vector3 heroToCreep( front->origin.m_x - heroOrigin.m_x , front->origin.m_y - heroOrigin.m_y , 0.f );
	const float creepOffset = Dot2D( heroToCreep , lateral );
	const float side = std::clamp( creepOffset , -Settings::CreepBlocker::SideStep , Settings::CreepBlocker::SideStep );

	Vector3 blockPoint(
		front->origin.m_x + direction.m_x * Settings::CreepBlocker::BlockAhead + lateral.m_x * ( side - creepOffset ) ,
		front->origin.m_y + direction.m_y * Settings::CreepBlocker::BlockAhead + lateral.m_y * ( side - creepOffset ) ,
		front->origin.m_z );
	blockPoint = BlockPointClearOfCreeps( wave , direction , blockPoint );

	const HWND window = FeatureSupport::WindowReadyForInput();
	if ( !window )
	{
		m_Status = "Game window not focused";
		return false;
	}

	// Ground-targeted: a move order has to aim at the point itself. Raising
	// the aim point the way a unit-target click does would project to ground
	// further from the camera, and the hero would walk past the wave.
	ImVec2 screen{};
	if ( !FeatureSupport::ProjectWorldToClient( window , blockPoint , true , screen ) )
	{
		m_Status = "Block point is off screen";
		return false;
	}

	if ( !FeatureSupport::MoveCursorToClientPoint( window , screen , m_PreviousCursor ) )
	{
		m_Status = "Could not aim at the block point";
		return false;
	}

	m_Phase = OrderPhase::Click;
	m_NextPhaseTick = now + kClickDelayMs;
	m_Status = "Blocking";

	static uint32_t lastLogTick = 0;
	if ( !lastLogTick || now - lastLogTick >= 1000 )
	{
		DEV_LOG( "[creep-block] wave=%zu dir=(%.2f,%.2f) front=(%.0f,%.0f) hero=(%.0f,%.0f) point=(%.0f,%.0f) side=%.0f\n" ,
			wave.size() , direction.m_x , direction.m_y , front->origin.m_x , front->origin.m_y ,
			heroOrigin.m_x , heroOrigin.m_y , blockPoint.m_x , blockPoint.m_y , side );
		lastLogTick = now;
	}

	return true;
}
