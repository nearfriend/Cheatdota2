#include "CLocalHeroResolver.hpp"

#include <Dota2/SDK/CSchemaOffset.hpp>
#include <Dota2/SDK/Interface/CGameEntitySystem.hpp>
#include <Dota2/SDK/Interface/IVEngineClient2.hpp>
#include <Dota2/SDK/SDK.hpp>
#include <Dota2/SDK/Types/CEntityData.hpp>

#include <Windows.h>

#include <cstring>
#include <string>

namespace
{
	struct LocalHeroOffsets
	{
		uint32_t health = 0;
		uint32_t team = 0;
		uint32_t heroPlayerId = 0;
		uint32_t playerOwnerId = 0;
		bool hasHeroPlayerId = false;
		bool hasPlayerOwnerId = false;
		bool resolved = false;
	};

	// Plain dereference, no VirtualQuery - `base` is always the game's own live
	// entity pointer, never attacker-controlled or independently computed
	// (matches CKillStealer's ReadField). This runs inside a full 64x512
	// identity-chunk scan on the render thread; a VirtualQuery per field read
	// here multiplies into tens of thousands of kernel calls per frame and
	// visibly freezes the game.
	template <typename T>
	auto ReadField( const void* base , uint32_t offset , T fallback = T{} ) -> T
	{
		if ( !base || !offset )
			return fallback;
		T value{};
		std::memcpy( &value , reinterpret_cast<const uint8_t*>( base ) + offset , sizeof( T ) );
		return value;
	}

	auto GetOffsets() -> LocalHeroOffsets&
	{
		static LocalHeroOffsets offsets{};
		if ( offsets.resolved )
			return offsets;

		static ULONGLONG lastAttemptTick = 0;
		const ULONGLONG now = GetTickCount64();
		if ( lastAttemptTick && now - lastAttemptTick < 500 )
			return offsets;
		lastAttemptTick = now;

		auto* schema = GetSchemaOffset();
		if ( !schema )
			return offsets;

		const bool hasHealth = schema->TryGetOffset( "C_BaseEntity" , "m_iHealth" , offsets.health );
		const bool hasTeam = schema->TryGetOffset( "C_BaseEntity" , "m_iTeamNum" , offsets.team );
		offsets.hasHeroPlayerId = schema->TryGetOffset( "C_DOTA_BaseNPC_Hero" , "m_iPlayerID" , offsets.heroPlayerId ) ||
			schema->TryGetOffset( "C_DOTA_BaseNPC" , "m_iPlayerID" , offsets.heroPlayerId );
		offsets.hasPlayerOwnerId = schema->TryGetOffset( "C_DOTA_BaseNPC" , "m_nPlayerOwnerID" , offsets.playerOwnerId );

		offsets.resolved = hasHealth && hasTeam && ( offsets.hasHeroPlayerId || offsets.hasPlayerOwnerId );
		return offsets;
	}

	auto IsLikelyPlayerId( int value ) -> bool
	{
		return value >= 0 && value < 24;
	}

	auto IsPlayableTeam( uint8_t team ) -> bool
	{
		return team == 2 || team == 3;
	}

	auto UnitPlayerId( const C_BaseEntity* entity , const LocalHeroOffsets& offsets ) -> int
	{
		if ( offsets.hasHeroPlayerId )
		{
			const int heroPlayerId = ReadField<int>( entity , offsets.heroPlayerId , -1 );
			if ( IsLikelyPlayerId( heroPlayerId ) )
				return heroPlayerId;
		}
		if ( offsets.hasPlayerOwnerId )
		{
			const int ownerPlayerId = ReadField<int>( entity , offsets.playerOwnerId , -1 );
			if ( IsLikelyPlayerId( ownerPlayerId ) )
				return ownerPlayerId;
		}
		return -1;
	}

	auto TryLocalPlayerId( int& outPlayerId ) -> bool
	{
		outPlayerId = -1;
		auto* engine = SDK::Interfaces::EngineToClient();
		if ( !engine )
			return false;
		engine->GetLocalPlayer( outPlayerId , 0 );
		return IsLikelyPlayerId( outPlayerId );
	}

	// Identity must come from entity->pEntityIdentity() (the entity's own
	// m_pEntity schema field), not a re-lookup through GetBaseEntity(index).
	// GetSchemaClassName() also returns null for entities reached via an
	// identity-chunk walk on this build, so it's kept only as a harmless
	// second check, never the sole path - see CAutoCombo.cpp's debug history
	// for how a null identity there silently broke name-based matching.
	auto LooksLikeHeroEntity( C_BaseEntity* entity ) -> bool
	{
		if ( !entity )
			return false;

		if ( auto* identity = entity->pEntityIdentity() )
		{
			auto Contains = [] ( const char* text , const char* needle )
			{
				return text && needle && std::strstr( text , needle ) != nullptr;
			};
			if ( const char* name = identity->Name().String(); Contains( name , "npc_dota_hero_" ) )
				return true;
			if ( const char* name = identity->DesingerName().String(); Contains( name , "npc_dota_hero_" ) )
				return true;
		}

		const char* className = entity->GetSchemaClassName();
		return className && ( std::strstr( className , "DOTA_BaseNPC_Hero" ) || std::strstr( className , "DOTA_Unit_Hero" ) );
	}

	auto ResolveByPlayerId( CGameEntitySystem* entitySystem , C_BaseEntity*& outEntity , int& outEntIndex ) -> bool
	{
		const auto& offsets = GetOffsets();
		if ( !entitySystem || !offsets.resolved )
			return false;

		int localPlayerId = -1;
		if ( !TryLocalPlayerId( localPlayerId ) )
			return false;

		C_BaseEntity* match = nullptr;
		int matchEntIndex = -1;
		bool ambiguous = false;

		for ( int chunkIndex = 0; chunkIndex < MAX_ENTITY_LISTS; ++chunkIndex )
		{
			auto* chunk = entitySystem->m_pIdentityChunks[chunkIndex];
			if ( !chunk )
				continue;

			for ( int entryIndex = 0; entryIndex < MAX_ENTITIES_IN_LIST; ++entryIndex )
			{
				auto* entity = chunk->m_pIdentities[entryIndex].pBaseEntity();
				if ( !entity )
					continue;

				if ( ReadField<int>( entity , offsets.health , 0 ) <= 0 )
					continue;
				if ( !IsPlayableTeam( ReadField<uint8_t>( entity , offsets.team , 0 ) ) )
					continue;
				if ( UnitPlayerId( entity , offsets ) != localPlayerId )
					continue;
				if ( !LooksLikeHeroEntity( entity ) )
					continue;

				// Ambiguous - two units claiming our slot (illusions/clones that
				// kept the owner's id). Refuse rather than pick one at random.
				if ( match )
				{
					ambiguous = true;
					break;
				}
				match = entity;
				matchEntIndex = chunkIndex * MAX_ENTITIES_IN_LIST + entryIndex;
			}
			if ( ambiguous )
				break;
		}

		if ( !match || ambiguous )
			return false;

		outEntity = match;
		outEntIndex = matchEntIndex;
		return true;
	}

	// FALLBACK: the engine's local-player-controller, whose m_hAssignedHero is
	// by definition our hero. Correct in principle but has never actually
	// succeeded on this build; kept in case a future build fixes it.
	auto ResolveViaController( CGameEntitySystem* entitySystem , C_BaseEntity*& outEntity , int& outEntIndex ) -> bool
	{
		auto* controller = CGameEntitySystem::GetLocalPlayerController();
		if ( !controller )
			return false;

		const CHandle heroHandle = controller->m_hAssignedHero();
		if ( !heroHandle.IsValid() )
			return false;

		auto* entity = entitySystem->GetBaseEntityFromHandle( heroHandle );
		if ( !entity )
			return false;

		const auto& offsets = GetOffsets();
		if ( offsets.resolved && ReadField<int>( entity , offsets.health , 0 ) <= 0 )
			return false;

		outEntity = entity;
		outEntIndex = heroHandle.GetEntryIndex();
		return true;
	}
}

auto CLocalHeroResolver::ResolveByPlayerIdOnly( CGameEntitySystem* entitySystem , C_BaseEntity*& outEntity , int& outEntIndex ) -> bool
{
	return ResolveByPlayerId( entitySystem , outEntity , outEntIndex );
}

auto CLocalHeroResolver::Resolve( CGameEntitySystem* entitySystem , C_BaseEntity*& outEntity , int& outEntIndex ) -> bool
{
	// Reuse the last trusted hero while the primary path is briefly
	// unavailable (loading hitch, dead/respawning), so callers don't flicker
	// between a resolved and unresolved target every other frame - mirrors
	// CKillStealer's ResolveLocalHero cache.
	static int s_CachedEntIndex = -1;

	if ( ResolveByPlayerId( entitySystem , outEntity , outEntIndex ) )
	{
		s_CachedEntIndex = outEntIndex;
		return true;
	}

	if ( s_CachedEntIndex >= 0 && entitySystem )
	{
		if ( auto* cached = entitySystem->GetBaseEntity( s_CachedEntIndex ) )
		{
			const auto& offsets = GetOffsets();
			if ( !offsets.resolved || ReadField<int>( cached , offsets.health , 0 ) > 0 )
			{
				outEntity = cached;
				outEntIndex = s_CachedEntIndex;
				return true;
			}
		}
		s_CachedEntIndex = -1;
	}

	if ( ResolveViaController( entitySystem , outEntity , outEntIndex ) )
	{
		s_CachedEntIndex = outEntIndex;
		return true;
	}

	return false;
}
