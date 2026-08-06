#include "CAndromedaClient.hpp"
#include "CAndromedaGUI.hpp"

#include <Dota2/SDK/SDK.hpp>
#include <Dota2/SDK/Interface/CGameEntitySystem.hpp>

#include <AndromedaClient/Fonts/CFontManager.hpp>
#include <AndromedaClient/GUI/CAndromedaMenu.hpp>
#include <AndromedaClient/Settings/Settings.hpp>
#include <AndromedaClient/Data/HeroData.hpp>
#include <AndromedaClient/Scripting/LuaManager.hpp>
#include <DllLauncher.hpp>
#include <Common/Helpers/StringHelper.hpp>
#include <Common/MemoryEngine.hpp>
#include <filesystem>
#include <cstring>
#include <cmath>
#include <algorithm>

static CAndromedaClient g_CAndromedaClient{};
static CHeroDataLoader g_HeroDataLoader{};

namespace
{
	struct ModuleRange
	{
		uintptr_t base = 0;
		uintptr_t size = 0;
		uintptr_t codeStart = 0;
		uintptr_t codeEnd = 0;
	};

	auto GetModuleRange( const char* moduleName ) -> ModuleRange
	{
		ModuleRange out{};
		auto* hModule = GetModuleHandleA( moduleName );

		if ( !hModule )
			return out;

		auto* dos = reinterpret_cast<PIMAGE_DOS_HEADER>( hModule );

		if ( dos->e_magic != IMAGE_DOS_SIGNATURE )
			return out;

		auto* nt = reinterpret_cast<PIMAGE_NT_HEADERS64>( reinterpret_cast<uintptr_t>( hModule ) + dos->e_lfanew );

		if ( nt->Signature != IMAGE_NT_SIGNATURE )
			return out;

		out.base = reinterpret_cast<uintptr_t>( hModule );
		out.size = nt->OptionalHeader.SizeOfImage;
		out.codeStart = out.base + nt->OptionalHeader.BaseOfCode;
		out.codeEnd = out.codeStart + nt->OptionalHeader.SizeOfCode;

		return out;
	}

	auto IsWritableFloat( const void* ptr ) -> bool
	{
		if ( !ptr )
			return false;

		MEMORY_BASIC_INFORMATION mbi{};

		if ( !VirtualQuery( ptr , &mbi , sizeof( mbi ) ) )
			return false;

		if ( mbi.State != MEM_COMMIT )
			return false;

		const DWORD protect = mbi.Protect & 0xFF;

		return protect == PAGE_READWRITE ||
			protect == PAGE_WRITECOPY ||
			protect == PAGE_EXECUTE_READWRITE ||
			protect == PAGE_EXECUTE_WRITECOPY;
	}

	auto SafeWriteFloat( float* ptr , float value ) -> bool
	{
		if ( !IsWritableFloat( ptr ) )
			return false;

		*ptr = value;
		return true;
	}

	auto FindStringInModule( const ModuleRange& mod , const char* needle ) -> const char*
	{
		if ( !mod.base || !needle )
			return nullptr;

		const size_t needleLen = strlen( needle );

		if ( !needleLen || needleLen >= mod.size )
			return nullptr;

		const auto* begin = reinterpret_cast<const char*>( mod.base );
		const auto* end = begin + mod.size - needleLen;

		for ( const char* p = begin; p < end; ++p )
		{
			if ( memcmp( p , needle , needleLen ) == 0 && p[needleLen] == '\0' )
				return p;
		}

		return nullptr;
	}

	// Only movss STORE: F3 0F 11 /r with RIP-relative destination.
	auto ResolveMovssStoreTarget( uintptr_t insn ) -> float*
	{
		const auto* bytes = reinterpret_cast<const uint8_t*>( insn );

		if ( bytes[0] != 0xF3 || bytes[1] != 0x0F || bytes[2] != 0x11 )
			return nullptr;

		const uint8_t modrm = bytes[3];

		if ( ( modrm & 0xC7 ) != 0x05 )
			return nullptr;

		const auto rel = *reinterpret_cast<const int32_t*>( insn + 4 );
		return reinterpret_cast<float*>( insn + 8 + rel );
	}

	auto AcceptCameraCandidate( float* candidate , float expectedApprox , float tolerance ) -> bool
	{
		if ( !candidate || !IsWritableFloat( candidate ) )
			return false;

		const float value = *candidate;

		if ( !std::isfinite( value ) )
			return false;

		return fabsf( value - expectedApprox ) <= tolerance;
	}

	// Find writable float written near code that references the ConVar name.
	auto FindWritableFloatNearStringRef( const ModuleRange& mod , const char* name , float expectedApprox , float tolerance ) -> float*
	{
		const char* str = FindStringInModule( mod , name );

		if ( !str )
		{
			DEV_LOG( "[camera] string '%s' not found\n" , name );
			return nullptr;
		}

		const uintptr_t strAddr = reinterpret_cast<uintptr_t>( str );
		float* best = nullptr;
		float bestDelta = tolerance + 1.f;

		for ( uintptr_t addr = mod.codeStart; addr + 7 < mod.codeEnd; ++addr )
		{
			const auto* b = reinterpret_cast<const uint8_t*>( addr );

			if ( b[0] != 0x48 || b[1] != 0x8D )
				continue;

			if ( ( b[2] & 0xC7 ) != 0x05 )
				continue;

			const auto rel = *reinterpret_cast<const int32_t*>( addr + 3 );

			if ( addr + 7 + rel != strAddr )
				continue;

			const uintptr_t windowStart = ( addr > 0x100 ) ? ( addr - 0x100 ) : mod.codeStart;
			const uintptr_t windowEnd = (std::min)( addr + 0x200 , mod.codeEnd );

			for ( uintptr_t insn = windowStart; insn + 8 < windowEnd; ++insn )
			{
				float* candidate = ResolveMovssStoreTarget( insn );

				if ( !AcceptCameraCandidate( candidate , expectedApprox , tolerance ) )
					continue;

				const float delta = fabsf( *candidate - expectedApprox );

				if ( delta < bestDelta )
				{
					bestDelta = delta;
					best = candidate;
				}
			}
		}

		if ( best )
			DEV_LOG( "[camera] '%s' writable -> %p (value=%.1f)\n" , name , best , *best );
		else
			DEV_LOG( "[camera] '%s' writable store target not found\n" , name );

		return best;
	}

	auto FindWritableFloatByValueNear( const ModuleRange& mod , float expected , float* nearPtr , size_t maxDistance ) -> float*
	{
		if ( !mod.base )
			return nullptr;

		float* best = nullptr;
		size_t bestDist = maxDistance ? maxDistance + 1 : SIZE_MAX;

		auto* dos = reinterpret_cast<PIMAGE_DOS_HEADER>( mod.base );
		auto* nt = reinterpret_cast<PIMAGE_NT_HEADERS64>( mod.base + dos->e_lfanew );
		auto* section = IMAGE_FIRST_SECTION( nt );

		for ( unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i , ++section )
		{
			if ( ( section->Characteristics & IMAGE_SCN_MEM_WRITE ) == 0 )
				continue;

			uintptr_t start = mod.base + section->VirtualAddress;
			uintptr_t end = start + section->Misc.VirtualSize;

			for ( uintptr_t addr = start; addr + sizeof( float ) <= end; addr += sizeof( float ) )
			{
				auto* f = reinterpret_cast<float*>( addr );

				if ( !IsWritableFloat( f ) || fabsf( *f - expected ) > 0.01f )
					continue;

				if ( !nearPtr )
					return f;

				const size_t dist = static_cast<size_t>( llabs( static_cast<long long>( addr ) - reinterpret_cast<long long>( nearPtr ) ) );

				if ( dist < bestDist )
				{
					bestDist = dist;
					best = f;
				}
			}
		}

		if ( best && maxDistance && bestDist > maxDistance )
			return nullptr;

		return best;
	}

	auto ValidateOrNull( float*& ptr , const char* name ) -> void
	{
		if ( !ptr )
			return;

		if ( !IsWritableFloat( ptr ) )
		{
			DEV_LOG( "[camera] rejecting read-only %s @ %p\n" , name , ptr );
			ptr = nullptr;
		}
	}
}

auto CAndromedaClient::SearchCameraConvar( CBasePattern& pattern , const char** fallbackPatterns ) -> bool
{
	if ( pattern.Search( true ) )
		return true;

	for ( int i = 0; fallbackPatterns[i]; ++i )
	{
		if ( FindPattern( CLIENT_DLL , fallbackPatterns[i] ) )
		{
			pattern = CBasePattern(
				pattern.GetPatternName() ,
				fallbackPatterns[i] ,
				CLIENT_DLL ,
				0 ,
				eBasePatternSearchType::SEARCH_TYPE_MOV_PTR );

			if ( pattern.Search( true ) )
			{
				DEV_LOG( "[+] %s found via fallback pattern %d\n" , pattern.GetPatternName() , i );
				return true;
			}
		}
	}

	return false;
}

auto CAndromedaClient::ResolveCameraPointers() -> void
{
	static bool s_Resolved = false;

	if ( s_Resolved )
		return;

	s_Resolved = true;

	static const char* distanceFallbacks[] =
	{
		"F3 0F 11 05 ? ? ? ? 48 8D 0D ? ? ? ? E8 ? ? ? ? BA ? ? ? ? F3 0F 11 05",
		"F3 0F 11 05 ? ? ? ? F3 0F 11 05 ? ? ? ? F3 0F 11 05",
		nullptr
	};

	static const char* fogFallbacks[] =
	{
		"F3 0F 11 05 ? ? ? ? 48 8D 0D ? ? ? ? E8 ? ? ? ? F3 0F 11 05",
		"F3 0F 11 05 ? ? ? ? F3 0F 11 05 ? ? ? ? 48 83 C4",
		nullptr
	};

	if ( dota_camera_farplane.Search( true ) )
		m_pCameraFarplane = reinterpret_cast<float*>( dota_camera_farplane.GetFunction() );

	if ( SearchCameraConvar( dota_camera_distance , distanceFallbacks ) )
		m_pCameraDistance = reinterpret_cast<float*>( dota_camera_distance.GetFunction() );

	if ( SearchCameraConvar( dota_camera_fog_end , fogFallbacks ) )
		m_pCameraFogEnd = reinterpret_cast<float*>( dota_camera_fog_end.GetFunction() );

	// Pattern results must be writable — old string search often hit .rdata constants.
	ValidateOrNull( m_pCameraDistance , "distance" );
	ValidateOrNull( m_pCameraFogEnd , "fog_end" );
	ValidateOrNull( m_pCameraFarplane , "farplane" );

	const auto mod = GetModuleRange( CLIENT_DLL );

	if ( !m_pCameraDistance )
		m_pCameraDistance = FindWritableFloatNearStringRef( mod , "dota_camera_distance" , 1200.f , 50.f );

	if ( !m_pCameraFogEnd )
		m_pCameraFogEnd = FindWritableFloatNearStringRef( mod , "dota_camera_fog_end" , 4500.f , 6000.f );

	if ( !m_pCameraFarplane )
		m_pCameraFarplane = FindWritableFloatNearStringRef( mod , "dota_camera_farplane" , 1800.f , 9000.f );

	if ( !m_pCameraDistance && m_pCameraFarplane )
		m_pCameraDistance = FindWritableFloatByValueNear( mod , 1200.f , m_pCameraFarplane , 0x8000 );

	if ( !m_pCameraFogEnd && m_pCameraFarplane )
		m_pCameraFogEnd = FindWritableFloatByValueNear( mod , 4500.f , m_pCameraFarplane , 0x8000 );

	ValidateOrNull( m_pCameraDistance , "distance" );
	ValidateOrNull( m_pCameraFogEnd , "fog_end" );
	ValidateOrNull( m_pCameraFarplane , "farplane" );

	DEV_LOG( "[camera] pointers: distance=%p fog_end=%p farplane=%p\n" ,
		m_pCameraDistance , m_pCameraFogEnd , m_pCameraFarplane );
}

auto CAndromedaClient::OnInit() -> void
{
	ResolveCameraPointers();

	if ( m_pCameraDistance )
		DEV_LOG( "[dota_camera_distance] Found (writable)!\n" );
	else
		DEV_LOG( "[error] dota_camera_distance not found — camera slider disabled\n" );

	if ( m_pCameraFogEnd )
		DEV_LOG( "[dota_camera_fog_end] Found (writable)!\n" );
	else
		DEV_LOG( "[warn] dota_camera_fog_end not found\n" );

	if ( m_pCameraFarplane )
		DEV_LOG( "[dota_camera_farplane] Found (writable)!\n" );
	else
		DEV_LOG( "[warn] dota_camera_farplane not found\n" );

	const std::string baseDir = GetDllDir();
	const std::string heroJsonPath = baseDir + "Assets\\data\\npc_heroes.json";
	constexpr const char* kHeroJsonUrl = "https://raw.githubusercontent.com/odota/dotaconstants/master/build/heroes.json";

	if ( g_HeroDataLoader.EnsureCacheAndLoad( kHeroJsonUrl , heroJsonPath ) )
		DEV_LOG( "[heroes] loaded %zu heroes from %s\n" , g_HeroDataLoader.GetAll().size() , g_HeroDataLoader.GetSourcePath().c_str() );
	else
		DEV_LOG( "[heroes] skip hero data load (missing/invalid file: %s)\n" , heroJsonPath.c_str() );

	const std::string scriptsRoot = baseDir + "Assets\\Lua\\";
	GetLuaManager()->Init( scriptsRoot );
}

auto CAndromedaClient::SetCameraDistance( float Distance ) -> void
{
	if ( !m_pCameraDistance && !m_pCameraFogEnd && !m_pCameraFarplane )
		ResolveCameraPointers();

	if ( m_pCameraDistance )
	{
		if ( !SafeWriteFloat( m_pCameraDistance , Distance ) )
		{
			DEV_LOG( "[error] camera distance @ %p is not writable — clearing\n" , m_pCameraDistance );
			m_pCameraDistance = nullptr;
		}
		else
		{
			static int s_LogCount = 0;
			if ( s_LogCount < 3 )
			{
				DEV_LOG( "[camera] set distance=%.1f @ %p\n" , Distance , m_pCameraDistance );
				s_LogCount++;
			}
		}
	}

	if ( m_pCameraFogEnd && !SafeWriteFloat( m_pCameraFogEnd , 10000.f ) )
	{
		DEV_LOG( "[error] camera fog_end @ %p is not writable — clearing\n" , m_pCameraFogEnd );
		m_pCameraFogEnd = nullptr;
	}

	if ( m_pCameraFarplane && !SafeWriteFloat( m_pCameraFarplane , 10000.f ) )
	{
		DEV_LOG( "[error] camera farplane @ %p is not writable — clearing\n" , m_pCameraFarplane );
		m_pCameraFarplane = nullptr;
	}
}

auto CAndromedaClient::OnRender() -> void
{
	// Only force camera once pointers are confirmed writable.
	if ( Settings::Camera::Enable && ( m_pCameraDistance || m_pCameraFarplane ) )
		SetCameraDistance( Settings::Camera::Distance );

	if ( GetAndromedaGUI()->IsVisible() )
		GetAndromedaMenu()->OnRenderMenu();

	GetFontManager()->FirstInitFonts();
	GetFontManager()->m_VerdanaFont.DrawString( 1 , 1 , ImColor( 255 , 255 , 0 ) , FW1_LEFT , XorStr( CHEAT_NAME ) );
}

auto CAndromedaClient::OnCreateMove( CDOTAInput* pCDOTAInput , CUserCmd* pCUserCmd ) -> void
{
	GetAndromedaGUI()->ProcessHotkeys();

	static int s_nCallCount = 0;
	s_nCallCount++;

	if ( s_nCallCount <= 3 )
		DEV_LOG( "[andromeda] OnCreateMove called (call #%d, pCUserCmd=%p)\n" , s_nCallCount , pCUserCmd );

	if ( Settings::Camera::Enable && ( m_pCameraDistance || m_pCameraFarplane ) )
		SetCameraDistance( Settings::Camera::Distance );

	// Skip hero/Lua work until we have a real usercmd (match fully loaded).
	// Running Lua during load crashed the client (debug.log AV in LoadHeroScript).
	if ( !pCUserCmd )
		return;

	m_InvokerController.OnCreateMove( pCDOTAInput , pCUserCmd );
	m_MeepoController.OnCreateMove( pCDOTAInput , pCUserCmd );
}

auto CAndromedaClient::GetHeroData() -> CHeroDataLoader*
{
	return &g_HeroDataLoader;
}

auto GetAndromedaClient() -> CAndromedaClient*
{
	return &g_CAndromedaClient;
}

auto GetHeroDataLoader() -> CHeroDataLoader*
{
	return &g_HeroDataLoader;
}
