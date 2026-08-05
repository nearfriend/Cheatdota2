#include "Hook_OnAddEntity.hpp"

#include <Dota2/SDK/Types/CEntityData.hpp>
#include <Dota2/SDK/Interface/CGameEntitySystem.hpp>
#include <Dota2/SDK/Types/CHandle.hpp>
#include <Dota2/SDK/SDK.hpp>
#include <AndromedaClient/CAndromedaClient.hpp>
#include <cstring>
#include <string>
#include <algorithm>
#include <Windows.h>

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

auto Hook_OnAddEntity( CGameEntitySystem* pCGameEntitySystem , CEntityInstance* pInst , CHandle handle ) -> void
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

	if ( pInst && IsReadable( pInst ) && className != "<unknown>" )
	{
		const bool isHero =
			strstr( className , "C_DOTA_BaseNPC_Hero" ) != nullptr ||
			strstr( className , "npc_dota_hero" ) != nullptr;

		if ( isHero )
		{
			s_nHeroCount++;
			DEV_LOG( "[OnAddEntity] HERO FOUND #%d: class='%s', ptr=%p\n" , s_nHeroCount , className , pInst );

			auto* pHero = static_cast<C_DOTA_BaseNPC_Hero*>( pInst );

			if ( pHero && IsReadable( pHero ) )
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
					}
					catch ( ... )
					{
					}
				}

				if ( auto* pClient = GetAndromedaClient() )
				{
					pClient->GetMeepoController().OnEntityAdded( pInst );
					DEV_LOG( "[meepo] Hero captured via OnAddEntity: %p (class: %s, name: %s)\n" , pHero , className , entityName.empty() ? "<unknown>" : entityName.c_str() );
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
				
				// Check if it's a Meepo ability (by name if available, or capture all abilities)
				bool isMeepoAbility = false;
				if ( !entityName.empty() )
				{
					std::string lowerName = entityName;
					std::transform( lowerName.begin() , lowerName.end() , lowerName.begin() , ::tolower );
					isMeepoAbility = ( lowerName.find( "meepo" ) != std::string::npos );
				}
				else
				{
					// If name unavailable, capture all abilities and let MeepoController filter
					// In Demo mode with Meepo, all abilities should be Meepo's
					isMeepoAbility = true;
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

	return OnAddEntity_o( pCGameEntitySystem , pInst , handle );
}
