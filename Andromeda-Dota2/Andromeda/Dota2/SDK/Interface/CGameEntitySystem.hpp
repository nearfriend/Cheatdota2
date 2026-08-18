#pragma once

#include <Common/Common.hpp>
#include <Common/MemoryEngine.hpp>
#include <cstring>
#include <Dota2/SDK/SDK.hpp>
#include <Dota2/SDK/Interface/IVEngineClient2.hpp>

#include <Dota2/SDK/Update/Offsets.hpp>
#include <Dota2/SDK/Types/CHandle.hpp>
#include <Dota2/SDK/Types/CEntityData.hpp>

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

	static CHandle s_CachedHandle{ INVALID_EHANDLE_INDEX };
	static ULONGLONG s_LastSearchTick = 0;

	auto CachedController = [&]() -> C_DOTAPlayerController*
	{
		if ( !s_CachedHandle.IsValid() )
			return nullptr;
		auto* pCached = pGES->GetBaseEntityFromHandle( s_CachedHandle );
		if ( !pCached )
			return nullptr;
		const char* className = pCached->GetSchemaClassName();
		if ( !className || std::strstr( className , "DOTAPlayerController" ) == nullptr )
			return nullptr;
		return static_cast<C_DOTAPlayerController*>( pCached );
	};

	if ( auto* pCached = CachedController() )
		return pCached;

	s_CachedHandle = CHandle{ INVALID_EHANDLE_INDEX };

	auto* pSchema = GetSchemaOffset();
	if ( !pSchema )
		return nullptr;

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

	C_DOTAPlayerController* flagFallback = nullptr;
	CHandle flagFallbackHandle{ INVALID_EHANDLE_INDEX };
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

			const char* className = entity->GetSchemaClassName();
			if ( !className || std::strstr( className , "DOTAPlayerController" ) == nullptr )
				continue;

			auto* pController = static_cast<C_DOTAPlayerController*>( entity );
			if ( localSteamId != 0 )
			{
				const uint64_t steamId = ControllerSteamId( pController );
				if ( steamId != 0 && steamId == localSteamId )
				{
					s_CachedHandle = identity->Handle();
					return pController;
				}
			}
			if ( !flagFallback && HasLocalFlag( pController ) )
			{
				flagFallback = pController;
				flagFallbackHandle = identity->Handle();
			}
		}
	}

	if ( flagFallback )
	{
		s_CachedHandle = flagFallbackHandle;
		return flagFallback;
	}

	return nullptr;
}
