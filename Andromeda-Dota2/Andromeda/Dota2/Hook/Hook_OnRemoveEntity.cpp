#include "Hook_OnRemoveEntity.hpp"

#include <Dota2/SDK/Types/CEntityData.hpp>
#include <AndromedaClient/CAndromedaClient.hpp>

auto Hook_OnRemoveEntity( CGameEntitySystem* pCGameEntitySystem , CEntityInstance* pInst , CHandle handle ) -> void
{
	if ( auto* client = GetAndromedaClient() )
		client->UnregisterFogController( pInst );

	return OnRemoveEntity_o( pCGameEntitySystem , pInst , handle );
}
