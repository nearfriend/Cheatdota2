#include "Hook_OnAddEntity.hpp"

#include <AndromedaClient/CAndromedaClient.hpp>
#include <Dota2/SDK/Types/CEntityData.hpp>

#include <cstring>

auto Hook_OnAddEntity( CGameEntitySystem* pCGameEntitySystem , CEntityInstance* pInst , CHandle handle ) -> void
{
	// Entity construction must never wait on application-side classification.
	// Call Dota first, then perform only the constant-time fog-controller check.
	if ( OnAddEntity_o )
		OnAddEntity_o( pCGameEntitySystem , pInst , handle );

	if ( !pInst )
		return;

	const char* className = pInst->GetSchemaClassName();

	if ( className && std::strstr( className , "FogController" ) )
	{
		if ( auto* client = GetAndromedaClient() )
			client->RegisterFogController( pInst );
	}
}
