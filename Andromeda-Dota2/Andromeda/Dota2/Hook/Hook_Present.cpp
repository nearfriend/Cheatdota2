#include "Hook_Present.hpp"

#include <AndromedaClient/CAndromedaGUI.hpp>

static auto PresentCallback( IDXGISwapChain* pSwapChain ) -> void
{
	if ( !pSwapChain )
		return;

	GetAndromedaGUI()->OnPresent( pSwapChain );
}

auto Hook_PresentOverlay( IDXGISwapChain* pSwapChain , uint64_t SyncInterval , uintptr_t Flags ) -> uintptr_t
{
	PresentCallback( pSwapChain );

	if ( PresentOverlay_o )
		return PresentOverlay_o( pSwapChain , SyncInterval , Flags );

	return 0;
}

auto Hook_Present( IDXGISwapChain* pSwapChain , UINT SyncInterval , UINT Flags ) -> HRESULT
{
	PresentCallback( pSwapChain );

	if ( Present_o )
		return Present_o( pSwapChain , SyncInterval , Flags );

	return DXGI_ERROR_INVALID_CALL;
}

auto Hook_Present1( IDXGISwapChain* pSwapChain , UINT SyncInterval , UINT Flags , const DXGI_PRESENT_PARAMETERS* pPresentParameters ) -> HRESULT
{
	PresentCallback( pSwapChain );

	if ( Present1_o )
		return Present1_o( pSwapChain , SyncInterval , Flags , pPresentParameters );

	return DXGI_ERROR_INVALID_CALL;
}
