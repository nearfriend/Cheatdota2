#pragma once

#include <Common/Common.hpp>
#include <Common/MemoryEngine.hpp>
#include <cstring>
#include <Dota2/SDK/SDK.hpp>
#include <Dota2/SDK/Interface/IVEngineClient2.hpp>

#include <Dota2/SDK/Update/Offsets.hpp>
#include <Dota2/SDK/Types/CHandle.hpp>
#include <Dota2/SDK/Types/CEntityData.hpp>
#include <Dota2/SDK/FunctionListSDK.hpp>

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
	if ( auto pFn = GetFunctionList()->CGameEntitySystem_GetLocalPlayerController.GetFunction() )
	{
		using Fn = C_DOTAPlayerController* ( __fastcall* )( int );

		if ( auto* pController = reinterpret_cast<Fn>( pFn )( 0 ) )
			return pController;
	}

	return ResolveLocalPlayerControllerBySchema();
}

inline auto CGameEntitySystem::ResolveLocalPlayerControllerBySchema() -> C_DOTAPlayerController*
{
	auto* pEngine = SDK::Interfaces::EngineToClient();
	auto* pGES = SDK::Interfaces::GameEntitySystem();

	if ( !pEngine || !pGES )
		return nullptr;

	int localPlayerSlot = -1;
	pEngine->GetLocalPlayer( localPlayerSlot , 0 );

	if ( localPlayerSlot < 0 )
		return nullptr;

	static CHandle s_CachedHandle{ INVALID_EHANDLE_INDEX };
	static int s_CachedSlot = -1;

	if ( s_CachedHandle.IsValid() && s_CachedSlot == localPlayerSlot )
	{
		if ( auto* pCached = pGES->GetBaseEntityFromHandle( s_CachedHandle ) )
			return static_cast<C_DOTAPlayerController*>( pCached );
	}

	const int highestIndex = pGES->GetHighestEntityIndex();

	for ( int idx = 0; idx <= highestIndex; ++idx )
	{
		auto* pController = pGES->GetBaseEntity<C_DOTAPlayerController>( idx );

		if ( !pController )
			continue;

		auto* pBinding = pController->GetSchemaClassBinding();

		if ( !pBinding )
			continue;

		const char* className = pBinding->m_bindingName();

		if ( !className || std::strcmp( className , "C_DOTAPlayerController" ) != 0 )
			continue;

		if ( pController->m_iPlayerID() != localPlayerSlot )
			continue;

		if ( auto* pIdentity = pController->pEntityIdentity() )
		{
			s_CachedHandle = pIdentity->Handle();
			s_CachedSlot = localPlayerSlot;
		}

		return pController;
	}

	return nullptr;
}
