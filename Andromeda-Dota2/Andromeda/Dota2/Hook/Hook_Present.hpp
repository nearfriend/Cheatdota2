#pragma once

#include <Common/Common.hpp>
#include <d3d11.h>
#include <dxgi1_2.h>

// Steam overlay PresentOverlay wrapper (__fastcall, returns uintptr_t).
auto Hook_PresentOverlay( IDXGISwapChain* pSwapChain , uint64_t SyncInterval , uintptr_t Flags ) -> uintptr_t;

using PresentOverlay_t = decltype( &Hook_PresentOverlay );
inline PresentOverlay_t PresentOverlay_o = nullptr;

// IDXGISwapChain::Present (vtable index 8).
auto Hook_Present( IDXGISwapChain* pSwapChain , UINT SyncInterval , UINT Flags ) -> HRESULT;

using Present_t = decltype( &Hook_Present );
inline Present_t Present_o = nullptr;

// IDXGISwapChain1::Present1 (vtable index 22) — used by modern Source 2.
auto Hook_Present1( IDXGISwapChain* pSwapChain , UINT SyncInterval , UINT Flags , const DXGI_PRESENT_PARAMETERS* pPresentParameters ) -> HRESULT;

using Present1_t = decltype( &Hook_Present1 );
inline Present1_t Present1_o = nullptr;
