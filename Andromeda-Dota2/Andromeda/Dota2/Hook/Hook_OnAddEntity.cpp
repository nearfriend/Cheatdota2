#include "Hook_OnAddEntity.hpp"

#include <Dota2/SDK/Types/CEntityData.hpp>
#include <Dota2/SDK/Interface/CGameEntitySystem.hpp>
#include <Dota2/SDK/Types/CHandle.hpp>
#include <Dota2/SDK/SDK.hpp>
#include <AndromedaClient/CAndromedaClient.hpp>
#include <cstring>
#include <string>
#include <algorithm>
#include <array>
#include <deque>
#include <mutex>
#include <Windows.h>

namespace
{
	constexpr size_t kMaxPendingEntityAdds = 4096;
	std::mutex g_PendingEntityAddsLock;
	std::deque<CHandle> g_PendingEntityAdds;

	auto QueueEntityAdd( CHandle handle ) -> void
	{
		if ( !handle.IsValid() )
			return;

		std::lock_guard lock( g_PendingEntityAddsLock );

		// Never let a loading spike allocate without bound. Dropping an event is
		// safe because hero controllers also resolve from the entity system.
		if ( g_PendingEntityAdds.size() < kMaxPendingEntityAdds )
			g_PendingEntityAdds.push_back( handle );
	}
}

// Helper to check if memory is readable
inline bool IsReadable( const void* ptr , size_t size = 1 )
{
	if ( !ptr )
		return false;

	MEMORY_BASIC_INFORMATION mbi{};
	if ( !VirtualQuery( ptr , &mbi , sizeof( mbi ) ) )
		return false;

	const DWORD protect = mbi.Protect & ~( PAGE_GUARD | PAGE_NOACCESS );
	const bool committed = mbi.State == MEM_COMMIT;
	const bool canRead =
		protect == PAGE_READONLY ||
		protect == PAGE_READWRITE ||
		protect == PAGE_WRITECOPY ||
		protect == PAGE_EXECUTE_READ ||
		protect == PAGE_EXECUTE_READWRITE ||
		protect == PAGE_EXECUTE_WRITECOPY;

	const auto regionEnd = reinterpret_cast<uintptr_t>( mbi.BaseAddress ) + mbi.RegionSize;
	const auto ptrEnd = reinterpret_cast<uintptr_t>( ptr ) + size;

	return committed && canRead && ptrEnd <= regionEnd;
}

static bool IsExcludedHeroName( const char* name )
{
	if ( !name || !name[0] )
		return true;

	return strstr( name , "announcer" ) != nullptr ||
		strstr( name , "target_dummy" ) != nullptr ||
		strstr( name , "npc_dota_hero_base" ) != nullptr;
}

static bool IsPlayableHeroName( const char* name )
{
	if ( !name || !name[0] || IsExcludedHeroName( name ) )
		return false;

	if ( std::strcmp( name , "C_DOTA_BaseNPC_Hero" ) == 0 )
		return true;

	// Designer / entity names: npc_dota_hero_meepo (not announcer variants).
	if ( std::strncmp( name , "npc_dota_hero_" , 14 ) == 0 )
		return true;

	return false;
}

static auto ProcessAddedEntity( CEntityInstance* pInst ) -> void
{
	const char* className = nullptr;
	auto* pIdentity = pInst ? pInst->pEntityIdentity() : nullptr;

	if ( pInst )
		className = pInst->GetSchemaClassName();

	if ( !className && pIdentity && IsReadable( pIdentity ) )
	{
		const auto& designerName = pIdentity->DesingerName();

		if ( designerName.String() && IsReadable( designerName.String() , 128 ) )
			className = designerName.String();
	}

	if ( !className )
		className = "<unknown>";

	static int s_nHeroCount = 0;

	if ( pInst )
	{
		bool isFogController = className && std::strstr( className , "FogController" );

		if ( !isFogController && pIdentity && IsReadable( pIdentity ) )
		{
			const auto& designerName = pIdentity->DesingerName();
			const char* designer = designerName.String();

			if ( designer && IsReadable( designer , 32 ) )
			{
				isFogController =
					std::strstr( designer , "fog_controller" ) != nullptr ||
					std::strstr( designer , "FogController" ) != nullptr;
			}
		}

		if ( isFogController )
		{
			if ( auto* client = GetAndromedaClient() )
				client->RegisterFogController( pInst );
		}
	}

	if ( pInst && IsReadable( pInst ) && className != "<unknown>" )
	{
		std::string entityName;

		if ( pIdentity && IsReadable( pIdentity ) )
		{
			try
			{
				if ( IsReadable( &pIdentity->Name() ) )
				{
					auto nameSymbol = pIdentity->Name();

					if ( IsReadable( nameSymbol.String() , 128 ) )
						entityName = nameSymbol.String();
				}

				if ( entityName.empty() )
				{
					const auto& designerName = pIdentity->DesingerName();

					if ( designerName.String() && IsReadable( designerName.String() , 128 ) )
						entityName = designerName.String();
				}
			}
			catch ( ... )
			{
			}
		}

		const bool isHero =
			IsPlayableHeroName( className ) ||
			( !entityName.empty() && IsPlayableHeroName( entityName.c_str() ) );

		if ( isHero )
		{
			s_nHeroCount++;
			DEV_LOG( "[OnAddEntity] HERO FOUND #%d: class='%s', ptr=%p\n" , s_nHeroCount , className , pInst );

			auto* pHero = static_cast<C_DOTA_BaseNPC_Hero*>( pInst );

			if ( pHero && IsReadable( pHero ) )
			{
				const bool looksLikeMeepo = [&]() -> bool
				{
					auto hasMeepo = []( const char* s ) -> bool
					{
						if ( !s )
							return false;
						std::string lower( s );
						std::transform( lower.begin() , lower.end() , lower.begin() , []( unsigned char c ) { return static_cast<char>( std::tolower( c ) ); } );
						return lower.find( "meepo" ) != std::string::npos && lower.find( "announcer" ) == std::string::npos;
					};
					return hasMeepo( className ) || hasMeepo( entityName.c_str() );
				}();

				if ( looksLikeMeepo )
				{
					if ( auto* pClient = GetAndromedaClient() )
					{
						pClient->GetMeepoController().OnEntityAdded( pInst );
						DEV_LOG( "[meepo] Hero captured via OnAddEntity: %p (class: %s, name: %s)\n" , pHero , className , entityName.empty() ? "<unknown>" : entityName.c_str() );
					}
				}
			}
		}
		// Check if it's an ability
		else if ( strstr( className , "C_DOTABaseAbility" ) != nullptr || 
		          strstr( className , "CDOTABaseAbility" ) != nullptr ||
		          strstr( className , "Ability" ) != nullptr )
		{
			// Try to cast to ability
			auto* ability = static_cast<C_DOTABaseAbility*>( pInst );
			if ( ability && IsReadable( ability ) )
			{
				// Get entity name if possible
				std::string entityName;
				if ( pIdentity && IsReadable( pIdentity ) )
				{
					try
					{
						if ( IsReadable( &pIdentity->Name() ) )
						{
							auto nameSymbol = pIdentity->Name();
							if ( IsReadable( nameSymbol.String() , 128 ) )
							{
								entityName = nameSymbol.String();
							}
						}
					}
					catch ( ... )
					{
						// Name reading failed
					}
				}
				
				// Only capture abilities that are clearly Meepo (avoid grabbing every ability in lobby).
				bool isMeepoAbility = false;
				if ( !entityName.empty() )
				{
					std::string lowerName = entityName;
					std::transform( lowerName.begin() , lowerName.end() , lowerName.begin() , ::tolower );
					isMeepoAbility = ( lowerName.find( "meepo" ) != std::string::npos );
				}
				
				if ( isMeepoAbility )
				{
					if ( auto* pClient = GetAndromedaClient() )
					{
						pClient->GetMeepoController().OnAbilityAdded( pInst );
						DEV_LOG( "[meepo] Ability captured via OnAddEntity: %p (class: %s, name: %s)\n" , ability , className , entityName.empty() ? "<unknown>" : entityName.c_str() );
					}
				}
			}
		}
	}

	return;
}

auto Hook_OnAddEntity( CGameEntitySystem* pCGameEntitySystem , CEntityInstance* pInst , CHandle handle ) -> void
{
	// Let Dota finish constructing the entity immediately. All classification,
	// string work and controller bookkeeping is deferred and bounded per tick.
	if ( OnAddEntity_o )
		OnAddEntity_o( pCGameEntitySystem , pInst , handle );

	QueueEntityAdd( handle );
}

auto ProcessPendingEntityAdds( size_t maxEvents ) -> void
{
	if ( maxEvents == 0 )
		return;

	constexpr size_t kMaxBatchSize = 32;
	std::array<CHandle , kMaxBatchSize> batch{};
	const size_t requested = (std::min)( maxEvents , kMaxBatchSize );
	size_t count = 0;

	{
		std::lock_guard lock( g_PendingEntityAddsLock );

		while ( count < requested && !g_PendingEntityAdds.empty() )
		{
			batch[count++] = g_PendingEntityAdds.front();
			g_PendingEntityAdds.pop_front();
		}
	}

	auto* entitySystem = SDK::Interfaces::GameEntitySystem();

	if ( !entitySystem )
		return;

	for ( size_t index = 0; index < count; ++index )
	{
		auto* entity = reinterpret_cast<CEntityInstance*>(
			entitySystem->GetBaseEntityFromHandle( batch[index] ) );

		if ( entity )
			ProcessAddedEntity( entity );
	}
}
