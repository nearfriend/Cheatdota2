#include "Hook_OnRemoveEntity.hpp"

#include <Dota2/SDK/Types/CEntityData.hpp>

auto Hook_OnRemoveEntity( CGameEntitySystem* pCGameEntitySystem , CEntityInstance* pInst , CHandle handle ) -> void
{
	auto* pIdentity = pInst ? pInst->pEntityIdentity() : nullptr;
	auto* pBinding = pInst ? pInst->GetSchemaClassBinding() : nullptr;
	const char* className = ( pBinding && pBinding->m_bindingName() ) ? pBinding->m_bindingName() : "<unknown>";

	DEV_LOG( "Hook_OnRemoveEntity: %p , %s\n" , pIdentity , className );

	return OnRemoveEntity_o( pCGameEntitySystem , pInst , handle );
}
