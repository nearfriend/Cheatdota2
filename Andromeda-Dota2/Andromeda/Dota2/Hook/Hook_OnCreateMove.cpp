#include "Hook_OnCreateMove.hpp"

#include <Common/DevLog.hpp>
#include <Dota2/SDK/Update/CDOTAInput.hpp>
#include <Dota2/SDK/Update/CUserCmd.hpp>
#include <Dota2/SDK/Interface/CGameEntitySystem.hpp>
#include <GameClient/CL_DOTAPlayerController.hpp>
#include <AndromedaClient/CAndromedaClient.hpp>

auto Hook_OnCreateMove( CDOTAInput* pCDOTAInput , uint32_t split_screen_index , bool a3 ) -> void
{
	if ( !OnCreateMove_o )
		return;

	OnCreateMove_o( pCDOTAInput , split_screen_index , a3 );

	if ( !pCDOTAInput )
		return;

	auto* pLocalPlayerController = GetCL_DOTAPlayerController()->GetLocal();
	CUserCmd* pCUserCmd = pLocalPlayerController ? pCDOTAInput->GetUserCmd( pLocalPlayerController ) : nullptr;

	// The usercmd gates every hero controller downstream (see
	// CAndromedaClient::OnCreateMove), so make its two failure stages -
	// controller unresolved vs. controller found but no usercmd - visible.
	// Logs the acquisition transitions once and the stuck states every 5s.
	static bool hadUserCmd = false;
	static ULONGLONG lastStuckLog = 0;
	const ULONGLONG nowTick = GetTickCount64();
	if ( pCUserCmd && !hadUserCmd )
	{
		hadUserCmd = true;
		DEV_LOG( "[createmove] usercmd acquired (controller=%p)\n" , pLocalPlayerController );
	}
	else if ( !pCUserCmd )
	{
		hadUserCmd = false;
		if ( !lastStuckLog || nowTick - lastStuckLog >= 5000 )
		{
			lastStuckLog = nowTick;
			DEV_LOG( "[createmove] no usercmd (controller=%p)\n" , pLocalPlayerController );
		}
	}

	GetAndromedaClient()->OnCreateMove( pCDOTAInput , pCUserCmd );
}
