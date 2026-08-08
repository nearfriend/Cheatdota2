#include "CHook_Loader.hpp"

#include <Common/MemoryEngine.hpp>
#include <MinHook/MinHook.h>
#include <d3d11.h>

#include <Dota2/Hook/Hook_CreateSwapChain.hpp>
#include <Dota2/Hook/Hook_Present.hpp>
#include <Dota2/Hook/Hook_ResizeBuffers.hpp>

#include <Dota2/Hook/Hook_GetProtoCDOTAGameAccountPlus.hpp>

#include <Dota2/Hook/Hook_OnCreateMove.hpp>

#include <AndromedaClient/CAndromedaGUI.hpp>

static CHook_Loader g_CHook_Loader{};

namespace
{
	auto FindPatternInList( const char* dll , const char** patterns ) -> PVOID
	{
		for ( int i = 0; patterns[i]; ++i )
		{
			auto* result = FindPattern( dll , patterns[i] );

			if ( result )
				return result;
		}

		return nullptr;
	}

	auto GetCallPtrSlot( uintptr_t insn ) -> uintptr_t
	{
		const auto disp = *reinterpret_cast<int32_t*>( insn + 2 );
		return insn + 6 + disp;
	}

	static bool s_PresentEnabled = false;

	auto EnableAllHooks() -> void
	{
		const MH_STATUS status = MH_EnableHook( MH_ALL_HOOKS );

		if ( status != MH_OK && status != MH_ERROR_ENABLED )
			DEV_LOG( "[error] MH_EnableHook failed: %i\n" , status );
	}

	// Most reliable path: hook IDXGISwapChain::Present from a temporary swapchain vtable.
	auto InstallDxgiPresentVTableHook() -> bool
	{
		if ( Present_o )
			return true;

		using D3D11CreateDeviceAndSwapChain_t = HRESULT( WINAPI* )(
			IDXGIAdapter* ,
			D3D_DRIVER_TYPE ,
			HMODULE ,
			UINT ,
			const D3D_FEATURE_LEVEL* ,
			UINT ,
			UINT ,
			const DXGI_SWAP_CHAIN_DESC* ,
			IDXGISwapChain** ,
			ID3D11Device** ,
			D3D_FEATURE_LEVEL* ,
			ID3D11DeviceContext** );

		HMODULE hD3D11 = GetModuleHandleA( "d3d11.dll" );

		if ( !hD3D11 )
			hD3D11 = LoadLibraryA( "d3d11.dll" );

		if ( !hD3D11 )
		{
			DEV_LOG( "[warn] d3d11.dll not available\n" );
			return false;
		}

		auto pCreate = reinterpret_cast<D3D11CreateDeviceAndSwapChain_t>(
			GetProcAddress( hD3D11 , "D3D11CreateDeviceAndSwapChain" ) );

		if ( !pCreate )
		{
			DEV_LOG( "[warn] D3D11CreateDeviceAndSwapChain export not found\n" );
			return false;
		}

		WNDCLASSEXA wc{};
		wc.cbSize = sizeof( wc );
		wc.style = CS_CLASSDC;
		wc.lpfnWndProc = DefWindowProcA;
		wc.hInstance = GetModuleHandleA( nullptr );
		wc.lpszClassName = "AndromedaDummyDX";

		RegisterClassExA( &wc );

		HWND hwnd = CreateWindowA(
			wc.lpszClassName ,
			"" ,
			WS_OVERLAPPEDWINDOW ,
			0 , 0 , 100 , 100 ,
			nullptr , nullptr , wc.hInstance , nullptr );

		if ( !hwnd )
		{
			UnregisterClassA( wc.lpszClassName , wc.hInstance );
			return false;
		}

		DXGI_SWAP_CHAIN_DESC desc{};
		desc.BufferCount = 1;
		desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
		desc.OutputWindow = hwnd;
		desc.SampleDesc.Count = 1;
		desc.Windowed = TRUE;
		desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

		D3D_FEATURE_LEVEL featureLevel{};
		IDXGISwapChain* pSwapChain = nullptr;
		ID3D11Device* pDevice = nullptr;
		ID3D11DeviceContext* pContext = nullptr;

		const HRESULT hr = pCreate(
			nullptr ,
			D3D_DRIVER_TYPE_HARDWARE ,
			nullptr ,
			0 ,
			nullptr ,
			0 ,
			D3D11_SDK_VERSION ,
			&desc ,
			&pSwapChain ,
			&pDevice ,
			&featureLevel ,
			&pContext );

		bool ok = false;

		if ( SUCCEEDED( hr ) && pSwapChain )
		{
			void** vtable = *reinterpret_cast<void***>( pSwapChain );
			void* pPresent = vtable[8];
			void* pResize = vtable[13];
			void* pPresent1 = vtable[22];

			if ( pPresent && MH_CreateHook( pPresent , &Hook_Present , reinterpret_cast<LPVOID*>( &Present_o ) ) == MH_OK )
			{
				ok = true;
				DEV_LOG( "[+] Hooked IDXGISwapChain::Present @ %p\n" , pPresent );
			}
			else
			{
				DEV_LOG( "[warn] Failed to hook IDXGISwapChain::Present @ %p (status may already be hooked)\n" , pPresent );
			}

			if ( pPresent1 && MH_CreateHook( pPresent1 , &Hook_Present1 , reinterpret_cast<LPVOID*>( &Present1_o ) ) == MH_OK )
			{
				ok = true;
				DEV_LOG( "[+] Hooked IDXGISwapChain1::Present1 @ %p\n" , pPresent1 );
			}

			if ( pResize && !ResizeBuffers_o )
			{
				if ( MH_CreateHook( pResize , &Hook_ResizeBuffers , reinterpret_cast<LPVOID*>( &ResizeBuffers_o ) ) == MH_OK )
					DEV_LOG( "[+] Hooked IDXGISwapChain::ResizeBuffers @ %p\n" , pResize );
			}

			if ( pContext )
				pContext->Release();

			if ( pDevice )
				pDevice->Release();

			pSwapChain->Release();
		}
		else
		{
			DEV_LOG( "[warn] Dummy D3D11CreateDeviceAndSwapChain failed: 0x%08X\n" , hr );
		}

		DestroyWindow( hwnd );
		UnregisterClassA( wc.lpszClassName , wc.hInstance );

		return ok;
	}

	auto TryHookSteamOverlayPresentPointer() -> bool
	{
		if ( Present_o )
			return true;

		static const char* patterns[] =
		{
			"FF 15 ? ? ? ? 8B F8 48 85 FF 74",
			"FF 15 ? ? ? ? 8B F8 85 DB 74",
			"FF 15 ? ? ? ? 8B F0 48 85 FF 74",
			"FF 15 ? ? ? ? 8B D8 48 85",
			nullptr
		};

		auto* insn = FindPatternInList( GAMEOVERLAYRENDER64_DLL , patterns );

		if ( !insn )
		{
			DEV_LOG( "[warn] Steam overlay Present pointer pattern not found\n" );
			return false;
		}

		const auto slot = GetCallPtrSlot( reinterpret_cast<uintptr_t>( insn ) );
		auto** ppPresent = reinterpret_cast<Present_t*>( slot );

		if ( ppPresent && *ppPresent )
		{
			Present_o = *ppPresent;
			DWORD oldProtect = 0;

			if ( VirtualProtect( ppPresent , sizeof( void* ) , PAGE_EXECUTE_READWRITE , &oldProtect ) )
			{
				*ppPresent = &Hook_Present;
				VirtualProtect( ppPresent , sizeof( void* ) , oldProtect , &oldProtect );
				DEV_LOG( "[+] Steam overlay Present pointer hook at %p -> %p\n" , reinterpret_cast<void*>( slot ) , Present_o );
				return true;
			}
		}

		DEV_LOG( "[warn] Steam overlay Present pointer invalid at %p\n" , reinterpret_cast<void*>( slot ) );
		return false;
	}

	auto InstallGraphicsHooks() -> int
	{
		int installed = 0;

		// 1) Direct DXGI Present (works even when Steam overlay wrapper pattern is dead code)
		if ( InstallDxgiPresentVTableHook() )
			installed++;

		// 2) Steam overlay IAT/pointer patch (classic overlay rendering path)
		if ( TryHookSteamOverlayPresentPointer() )
			installed++;

		// 3) Optional: Steam PresentOverlay body (may not be on the hot path)
		static const char* presentPatterns[] =
		{
			"48 89 6C 24 ? 48 89 74 24 ? 41 56 48 83 EC 20 41 8B E8",
			nullptr
		};

		if ( !PresentOverlay_o )
		{
			if ( auto* presentFn = FindPatternInList( GAMEOVERLAYRENDER64_DLL , presentPatterns ) )
			{
				if ( MH_CreateHook( presentFn , &Hook_PresentOverlay , reinterpret_cast<LPVOID*>( &PresentOverlay_o ) ) == MH_OK )
				{
					installed++;
					DEV_LOG( "[+] CBasePattern: Hook::PresentOverlay -> %p\n" , presentFn );
				}
			}
		}

		EnableAllHooks();
		s_PresentEnabled = ( Present_o != nullptr ) || ( Present1_o != nullptr ) || ( PresentOverlay_o != nullptr );

		if ( !s_PresentEnabled )
			DEV_LOG( "[error] No Present hook installed — menu will not render\n" );
		else
			DEV_LOG( "[+] Present hook ready (Present=%p Present1=%p Overlay=%p)\n" , Present_o , Present1_o , PresentOverlay_o );

		return installed;
	}
}

auto CHook_Loader::InitalizeMH() -> bool
{
	return MH_Initialize() == MH_OK;
}

auto CHook_Loader::InstallFirstHook() -> bool
{
	m_Hooks =
	{
		{ { XorStr( "Hook::GetProtoCDOTAGameAccountPlus" ) , XorStr( "48 83 EC ? 48 8B 89 ? ? ? ? 48 85 C9 74 ? BA ? ? ? ? E8 ? ? ? ? 48 85 C0 74 ? 8B 48 ? 85 C9 74 ? 48 8B 40 ? 4C 8D 0D" ) , CLIENT_DLL } , &Hook_GetProtoCDOTAGameAccountPlus , reinterpret_cast<LPVOID*>( &GetProtoCDOTAGameAccountPlus_o ) , true , true } ,
	};

	return InstallHooks();
}

auto CHook_Loader::InstallSecondHook() -> bool
{
	int waitCount = 0;

	while ( !GetModuleHandleA( GAMEOVERLAYRENDER64_DLL ) && waitCount < 15000 )
	{
		Sleep( 1 );
		waitCount++;
	}

	if ( !GetModuleHandleA( GAMEOVERLAYRENDER64_DLL ) )
		DEV_LOG( "[warn] gameoverlayrenderer64.dll not loaded yet — installing DXGI Present anyway\n" );

	const int graphicsInstalled = InstallGraphicsHooks();

	m_Hooks =
	{
		{ { XorStr( "Hook::GetProtoCDOTAGameAccountPlus" ) , XorStr( "48 83 EC ? 48 8B 89 ? ? ? ? 48 85 C9 74 ? BA ? ? ? ? E8 ? ? ? ? 48 85 C0 74 ? 8B 48 ? 85 C9 74 ? 48 8B 40 ? 4C 8D 0D" ) , CLIENT_DLL } , &Hook_GetProtoCDOTAGameAccountPlus , reinterpret_cast<LPVOID*>( &GetProtoCDOTAGameAccountPlus_o ) , true , true } ,
		{ { XorStr( "Hook::OnCreateMove" ) , XorStr( "85 D2 0F 85 ? ? ? ? 48 8B C4 44 88 40" ) , CLIENT_DLL } , &Hook_OnCreateMove , reinterpret_cast<LPVOID*>( &OnCreateMove_o ) , true , false } ,
	};

	const bool gameplayHooks = InstallHooks();

	return graphicsInstalled > 0 || gameplayHooks;
}

auto CHook_Loader::InstallSwapChainHooks( IDXGISwapChain* /*pSwapChain*/ ) -> bool
{
	// Present is hooked globally via DXGI vtable address.
	return Present_o != nullptr || PresentOverlay_o != nullptr;
}

auto CHook_Loader::InstallHooks() -> bool
{
	int successCount = 0;

	for ( auto& Hook : m_Hooks )
	{
		int waitCount = 0;

		while ( !GetModuleHandleA( Hook.m_Pattern.GetDllName() ) && waitCount < 8000 )
		{
			Sleep( 1 );
			waitCount++;
		}

		if ( waitCount >= 8000 )
		{
			DEV_LOG( "[warn] Timeout waiting for module: %s\n" , Hook.m_Pattern.GetDllName() );

			if ( !Hook.m_bSkipIfNotFound )
				continue;
			else
				continue;
		}

		if ( !Hook.m_Pattern.Search( Hook.m_bSkipError ) )
		{
			if ( !Hook.m_bSkipError )
				DEV_LOG( "[warn] Hook pattern not found: '%s'\n" , Hook.m_Pattern.GetPatternName() );
			continue;
		}

		auto Status = MH_CreateHook( Hook.m_Pattern.GetFunction() , Hook.m_pDetour , Hook.m_pOriginal );

		if ( Status != MH_OK )
		{
			DEV_LOG( "[warn] Hook creation failed [%i]: '%s'\n" , Status , Hook.m_Pattern.GetPatternName() );
			continue;
		}

		successCount++;
	}

	if ( successCount > 0 )
	{
		MH_STATUS status = MH_EnableHook( MH_ALL_HOOKS );

		if ( status != MH_OK && status != MH_ERROR_ENABLED )
			DEV_LOG( "[error] Failed to enable hooks: %i\n" , status );
		else
			DEV_LOG( "[success] Enabled %d/%zu hooks\n" , successCount , m_Hooks.size() );
	}

	m_Hooks.clear();

	return successCount > 0;
}

auto CHook_Loader::DestroyHooks() -> void
{
	MH_DisableHook( MH_ALL_HOOKS );
	MH_RemoveHook( MH_ALL_HOOKS );
	MH_Uninitialize();
}

auto GetHook_Loader() -> CHook_Loader*
{
	return &g_CHook_Loader;
}
