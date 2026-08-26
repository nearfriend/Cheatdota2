#pragma once

#include <Common/Common.hpp>
#include <Common/MemoryEngine.hpp>
#include <cstring>
#include <Dota2/SDK/SDK.hpp>
#include <Dota2/SDK/Interface/IVEngineClient2.hpp>

#include <Dota2/SDK/Update/Offsets.hpp>
#include <Dota2/SDK/Types/CHandle.hpp>
#include <Dota2/SDK/Types/CEntityData.hpp>
#include <Dota2/SDK/Interface/CLocalHeroResolver.hpp>

#define MAX_ENTITIES_IN_LIST 512
#define MAX_ENTITY_LISTS 64
#define MAX_TOTAL_ENTITIES MAX_ENTITIES_IN_LIST * MAX_ENTITY_LISTS

class C_BaseEntity;

class CEntityIdentities
{
public:
	CEntityIdentity m_pIdentities[MAX_ENTITIES_IN_LIST];
};

class CGameEntitySystem
{
public:
	template<typename T = C_BaseEntity>
	auto GetBaseEntity( int nIndex ) -> T*
	{
		if ( nIndex <= -1 || nIndex >= ( MAX_TOTAL_ENTITIES - 1 ) )
			return nullptr;

		CEntityIdentities* pChunkToUse = m_pIdentityChunks[nIndex / MAX_ENTITIES_IN_LIST];
		
		if ( !pChunkToUse )
			return nullptr;

		CEntityIdentity* pIdentity = &pChunkToUse->m_pIdentities[nIndex % MAX_ENTITIES_IN_LIST];
		
		if ( !pIdentity )
			return nullptr;

		return static_cast<T*>( pIdentity->pBaseEntity() );
	}

	auto GetBaseEntityFromHandle( CHandle hEntity ) -> C_BaseEntity*
	{
		return GetBaseEntity( static_cast<int>( static_cast<uint16_t>( hEntity.GetEntryIndex() ) & ENT_ENTRY_MASK ) );
	}

	auto GetHighestEntityIndex() -> int
	{
		return CUSTOM_OFFSET( int , OFFSET_CGameEntitySystem_GetHighestEntityIndex );
	}

	static auto GetLocalPlayerController() -> C_DOTAPlayerController*;

private:
	static auto ResolveLocalPlayerControllerBySchema() -> C_DOTAPlayerController*;

private:
	PAD( 0x10 );
public:
	CEntityIdentities* m_pIdentityChunks[MAX_ENTITY_LISTS];
};

inline auto CGameEntitySystem::GetLocalPlayerController() -> C_DOTAPlayerController*
{
	// The pattern-scanned game function crashes on current builds (see debug.log crash at that address).
	return ResolveLocalPlayerControllerBySchema();
}

inline auto CGameEntitySystem::ResolveLocalPlayerControllerBySchema() -> C_DOTAPlayerController*
{
	auto* pGES = SDK::Interfaces::GameEntitySystem();
	if ( !pGES )
		return nullptr;

	auto* pSchema = GetSchemaOffset();
	if ( !pSchema )
		return nullptr;

	// IMPORTANT: GetSchemaClassName() returns null for entities reached via
	// the identity-chunk walk on this build (verified by CAutoCombo's raw
	// dump - every chunk-walked entity printed class='<null>'). It therefore
	// must never be a hard gate anywhere in this function: with the old
	// className filter every controller was skipped before any strategy ran,
	// which is why the Steam-ID and local-flag strategies "never worked" and
	// pCUserCmd stayed null in Hook_OnCreateMove.
	uint32_t assignedHeroOffset = 0;
	const bool hasAssignedHeroOffset =
		pSchema->TryGetOffset( "C_DOTAPlayerController" , "m_hAssignedHero" , assignedHeroOffset ) &&
		assignedHeroOffset != 0 && assignedHeroOffset < 0x4000;

	static CHandle s_CachedHandle{ INVALID_EHANDLE_INDEX };
	static ULONGLONG s_LastSearchTick = 0;

	auto CachedController = [&]() -> C_DOTAPlayerController*
	{
		if ( !s_CachedHandle.IsValid() )
			return nullptr;
		auto* pCached = pGES->GetBaseEntityFromHandle( s_CachedHandle );
		if ( !pCached )
			return nullptr;
		// Class name works for some entities; when it is null, accept the
		// cached entity if it still carries a plausible assigned-hero handle
		// at the controller offset (an unrelated entity reusing the slot is
		// very unlikely to have valid handle bits exactly there).
		if ( const char* className = pCached->GetSchemaClassName();
			className && std::strstr( className , "DOTAPlayerController" ) != nullptr )
			return static_cast<C_DOTAPlayerController*>( pCached );
		if ( hasAssignedHeroOffset )
		{
			CHandle assigned{ INVALID_EHANDLE_INDEX };
			std::memcpy( &assigned , reinterpret_cast<const uint8_t*>( pCached ) + assignedHeroOffset , sizeof( CHandle ) );
			if ( assigned.IsValid() )
				return static_cast<C_DOTAPlayerController*>( pCached );
		}
		return nullptr;
	};

	if ( auto* pCached = CachedController() )
		return pCached;

	s_CachedHandle = CHandle{ INVALID_EHANDLE_INDEX };

	const ULONGLONG now = GetTickCount64();
	if ( s_LastSearchTick != 0 && now - s_LastSearchTick < 250 )
		return nullptr;
	s_LastSearchTick = now;

	uint32_t isLocalControllerOffset = 0;
	bool hasLocalControllerFlag =
		pSchema->TryGetOffset( "CBasePlayerController" , "m_bIsLocalPlayerController" , isLocalControllerOffset ) ||
		pSchema->TryGetOffset( "C_BasePlayerController" , "m_bIsLocalPlayerController" , isLocalControllerOffset ) ||
		pSchema->TryGetOffset( "C_DOTAPlayerController" , "m_bIsLocalPlayerController" , isLocalControllerOffset );
	uint32_t steamIdOffset = 0;
	bool hasControllerSteamId =
		pSchema->TryGetOffset( "CBasePlayerController" , "m_steamID" , steamIdOffset ) ||
		pSchema->TryGetOffset( "C_BasePlayerController" , "m_steamID" , steamIdOffset ) ||
		pSchema->TryGetOffset( "C_DOTAPlayerController" , "m_steamID" , steamIdOffset );

	// CBasePlayerController fields live well below 0x1000. A huge offset is a
	// server-schema mismatch and must not be dereferenced (that crashed Dota).
	constexpr uint32_t kMaxBaseControllerField = 0x1000;
	if ( isLocalControllerOffset == 0 || isLocalControllerOffset >= kMaxBaseControllerField )
		hasLocalControllerFlag = false;
	if ( steamIdOffset == 0 || steamIdOffset >= kMaxBaseControllerField )
		hasControllerSteamId = false;

	uint64_t localSteamId = 0;
	if ( hasControllerSteamId )
	{
		if ( HMODULE steamApi = GetModuleHandleA( "steam_api64.dll" ) )
		{
			using SteamUserAccessorFn = void* ( __cdecl* )();
			using GetSteamIdFn = uint64_t ( __cdecl* )( void* );
			auto getSteamId = reinterpret_cast<GetSteamIdFn>(
				GetProcAddress( steamApi , "SteamAPI_ISteamUser_GetSteamID" ) );
			constexpr const char* userAccessors[] = {
				"SteamAPI_SteamUser_v024" , "SteamAPI_SteamUser_v023" , "SteamAPI_SteamUser_v022" ,
				"SteamAPI_SteamUser_v021" , "SteamAPI_SteamUser_v020"
			};
			if ( getSteamId )
			{
				for ( const char* accessorName : userAccessors )
				{
					auto getSteamUser = reinterpret_cast<SteamUserAccessorFn>( GetProcAddress( steamApi , accessorName ) );
					if ( !getSteamUser )
						continue;
					if ( void* steamUser = getSteamUser() )
					{
						localSteamId = getSteamId( steamUser );
						if ( localSteamId != 0 )
							break;
					}
				}
			}
		}
	}

	auto ControllerSteamId = [=]( const C_BaseEntity* controller ) -> uint64_t
	{
		return controller && hasControllerSteamId ? *reinterpret_cast<const uint64_t*>(
			reinterpret_cast<uintptr_t>( controller ) + steamIdOffset ) : 0;
	};
	auto HasLocalFlag = [=]( const C_BaseEntity* controller ) -> bool
	{
		return controller && hasLocalControllerFlag && *reinterpret_cast<const bool*>(
			reinterpret_cast<uintptr_t>( controller ) + isLocalControllerOffset );
	};

	// Hero-match strategy: the player-id path (see CLocalHeroResolver) is the
	// only local-identity method proven to work on this build. Resolve our
	// hero first, then pick the entity whose m_hAssignedHero points at it -
	// including entities whose class name cannot be read (see the note at the
	// top of this function), which is the common case here.
	C_BaseEntity* localHero = nullptr;
	int localHeroEntIndex = -1;
	const bool haveLocalHero = hasAssignedHeroOffset &&
		CLocalHeroResolver::ResolveByPlayerIdOnly( pGES , localHero , localHeroEntIndex );

	C_DOTAPlayerController* flagFallback = nullptr;
	CHandle flagFallbackHandle{ INVALID_EHANDLE_INDEX };

	// Hero-matching entities that could NOT be confirmed controllers by class
	// name. Accepted after the loop only when the match is unambiguous, since
	// in theory an unrelated entity could carry hero-handle-shaped bytes at
	// the controller's m_hAssignedHero offset.
	C_DOTAPlayerController* unclassifiedMatch = nullptr;
	CHandle unclassifiedMatchHandle{ INVALID_EHANDLE_INDEX };
	int unclassifiedMatchCount = 0;

	for ( int chunkIndex = 0; chunkIndex < MAX_ENTITY_LISTS; ++chunkIndex )
	{
		auto* chunk = pGES->m_pIdentityChunks[chunkIndex];
		if ( !chunk )
			continue;

		for ( int entryIndex = 0; entryIndex < MAX_ENTITIES_IN_LIST; ++entryIndex )
		{
			auto* identity = &chunk->m_pIdentities[entryIndex];
			auto* entity = identity->pBaseEntity();
			if ( !entity )
				continue;

			auto* pController = static_cast<C_DOTAPlayerController*>( entity );

			// Strongest signal, checked on EVERY entity: our own 64-bit Steam
			// ID at the controller's m_steamID offset. A full 64-bit equality
			// cannot realistically false-positive on unrelated entity bytes,
			// so no class-name confirmation is needed.
			if ( localSteamId != 0 && entity != localHero &&
				ControllerSteamId( pController ) == localSteamId )
			{
				s_CachedHandle = identity->Handle();
				return pController;
			}

			bool heroMatch = false;
			if ( haveLocalHero && entity != localHero )
			{
				CHandle assigned{ INVALID_EHANDLE_INDEX };
				std::memcpy( &assigned , reinterpret_cast<const uint8_t*>( entity ) + assignedHeroOffset , sizeof( CHandle ) );
				heroMatch = assigned.IsValid() &&
					static_cast<int>( static_cast<uint16_t>( assigned.GetEntryIndex() ) & ENT_ENTRY_MASK ) == localHeroEntIndex;
			}
			if ( !heroMatch )
				continue;

			const char* className = entity->GetSchemaClassName();
			if ( className && std::strstr( className , "DOTAPlayerController" ) != nullptr )
			{
				// Confirmed controller pointing at our hero.
				s_CachedHandle = identity->Handle();
				return pController;
			}

			// Unconfirmed (class name unreadable). Require a plausible Steam64
			// id (0x01100001 universe/type prefix) at the steam-id offset too,
			// so hero-handle-shaped garbage on unrelated entities can't slip
			// in; the real controller always carries one.
			if ( hasControllerSteamId && ( ControllerSteamId( pController ) >> 32 ) != 0x01100001ull )
				continue;

			++unclassifiedMatchCount;
			if ( !unclassifiedMatch )
			{
				unclassifiedMatch = pController;
				unclassifiedMatchHandle = identity->Handle();
			}

			if ( !flagFallback && HasLocalFlag( pController ) )
			{
				flagFallback = pController;
				flagFallbackHandle = identity->Handle();
			}
		}
	}

	if ( unclassifiedMatch && unclassifiedMatchCount == 1 )
	{
		s_CachedHandle = unclassifiedMatchHandle;
		return unclassifiedMatch;
	}

	if ( flagFallback )
	{
		s_CachedHandle = flagFallbackHandle;
		return flagFallback;
	}

	return nullptr;
}
