#include "Hook_OnCreateMove.hpp"

#include <Dota2/SDK/Update/CDOTAInput.hpp>
#include <Dota2/SDK/Update/CUserCmd.hpp>
#include <Dota2/SDK/Interface/CGameEntitySystem.hpp>
#include <GameClient/CL_DOTAPlayerController.hpp>
#include <AndromedaClient/CAndromedaClient.hpp>

auto Hook_OnCreateMove( CDOTAInput* pCDOTAInput , uint32_t split_screen_index , bool a3 ) -> void
{
	OnCreateMove_o( pCDOTAInput , split_screen_index , a3 );

	// Try to get CUserCmd, but always call OnCreateMove even if it's null
	CUserCmd* pCUserCmd = nullptr;
	
	if ( auto pLocalPlayerController = GetCL_DOTAPlayerController()->GetLocal(); pLocalPlayerController )
	{
		pCUserCmd = pCDOTAInput->GetUserCmd( pLocalPlayerController );
	}

	// Always call OnCreateMove (even if pCUserCmd is null - the function handles it)
	GetAndromedaClient()->OnCreateMove( pCDOTAInput , pCUserCmd );
}
