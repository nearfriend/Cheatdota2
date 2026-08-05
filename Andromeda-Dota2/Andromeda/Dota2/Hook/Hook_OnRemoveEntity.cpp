#include "Hook_OnRemoveEntity.hpp"

#include <Dota2/SDK/Types/CEntityData.hpp>

auto Hook_OnRemoveEntity( CGameEntitySystem* pCGameEntitySystem , CEntityInstance* pInst , CHandle handle ) -> void
{
	return OnRemoveEntity_o( pCGameEntitySystem , pInst , handle );
}
