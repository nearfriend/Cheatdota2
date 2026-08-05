#include "Hook_CreateSwapChain.hpp"

#include <AndromedaClient/CAndromedaGUI.hpp>
#include <Dota2/CHook_Loader.hpp>

auto WINAPI Hook_CreateSwapChain( IDXGIFactory* pFactory , IUnknown* pDevice , DXGI_SWAP_CHAIN_DESC* pDesc , IDXGISwapChain** ppSwapChain )->HRESULT
{
	GetAndromedaGUI()->ClearRenderTargetView();

	const HRESULT hr = CreateSwapChain_o( pFactory , pDevice , pDesc , ppSwapChain );

	if ( SUCCEEDED( hr ) && ppSwapChain && *ppSwapChain )
		GetHook_Loader()->InstallSwapChainHooks( *ppSwapChain );

	return hr;
}
