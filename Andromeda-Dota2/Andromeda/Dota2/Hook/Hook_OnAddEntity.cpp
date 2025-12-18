#include "Hook_OnAddEntity.hpp"

#include <Dota2/SDK/Types/CEntityData.hpp>

auto Hook_OnAddEntity( CGameEntitySystem* pCGameEntitySystem , CEntityInstance* pInst , CHandle handle ) -> void
{
	auto* pIdentity = pInst ? pInst->pEntityIdentity() : nullptr;
	auto* pBinding = pInst ? pInst->GetSchemaClassBinding() : nullptr;
	const char* className = ( pBinding && pBinding->m_bindingName() ) ? pBinding->m_bindingName() : "<unknown>";

	DEV_LOG( "Hook_OnAddEntity: %p , %s\n" , pIdentity , className );

	return OnAddEntity_o( pCGameEntitySystem , pInst , handle );
}
