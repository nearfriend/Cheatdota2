#include "DllLauncher.hpp"

#include <string>
#include <winternl.h>

#include <Common/CrashLog.hpp>
#include <Common/Helpers/StringHelper.hpp>

#include <Dota2/CHook_Loader.hpp>
#include <Dota2/CSDK_Loader.hpp>
#include <Dota2/SDK/CFunctionList.hpp>

#include <AndromedaClient/CAndromedaClient.hpp>

static CDllLauncher g_CDllLauncher{};

auto CDllLauncher::OnDllMain( LPVOID lpReserved , HINSTANCE hInstace ) -> void
{
	if ( lpReserved )
	{
		ManualMapParam_t* pParam = reinterpret_cast<ManualMapParam_t*>( lpReserved );

		if ( pParam )
		{
			m_DllDir = pParam->DllPath;
			m_DllDir += "\\";
			m_DllDir = m_DllDir.substr( 0 , m_DllDir.find_last_of( '\\' ) + 1 );
		}
	}
	else
	{
		char szDllDir[MAX_PATH];

		GetModuleFileNameA( hInstace , szDllDir , MAX_PATH );

		m_DllDir = szDllDir;
		m_DllDir = m_DllDir.substr( 0 , m_DllDir.find_last_of( '\\' ) );
		m_DllDir += '\\';
	}

	m_hDllImage = hInstace;

	m_SizeofImage = GetSizeOfImageInternal();
	m_BaseOfCode = GetBaseOfCodeInternal();

	char szGameFile[MAX_PATH] = { 0 };
	GetModuleFileNameA( 0 , szGameFile , MAX_PATH );

	m_Dota2Dir = szGameFile;
	m_Dota2Dir = m_Dota2Dir.substr( 0 , m_Dota2Dir.find_last_of( "\\/" ) );
	m_Dota2Dir += '\\';

	memset( szGameFile , 0 , MAX_PATH );

	CreateThread( 0 , 0 , StartCheatTheard , lpReserved , 0 , 0 );
}

auto CDllLauncher::OnDestroy() -> void
{
	if ( !m_bDestroyed )
	{
		GetDevLog()->Destroy();
		GetHook_Loader()->DestroyHooks();
		GetCrashLog()->DestroyVectorExceptionHandler();
		
		m_bDestroyed = true;
	}
}

// Helper function to safely initialize client without C++ objects in SEH block
static void SafeInitClient()
{
	// Use SetUnhandledExceptionFilter or rely on vectored exception handler
	// We can't use __try/__except here because it conflicts with C++ objects
	GetAndromedaClient()->OnInit();
}

auto WINAPI CDllLauncher::StartCheatTheard( LPVOID lpThreadParameter ) -> DWORD
{
	// Initialize crash handler FIRST - this sets up vectored exception handler
	// which will catch exceptions without needing __try/__except
	GetDevLog()->Init();
	GetCrashLog()->InitVectorExceptionHandler();

#if ENABLE_CONSOLE_DEBUG == 1
	DEV_LOG( "[+] StartCheatThread: %s\n" , ansi_to_utf8( GetDllDir() ).c_str() );
#endif

	// Wait a bit for game to fully initialize
	Sleep( 2000 );

	// Initialize MinHook
	if ( !GetHook_Loader()->InitalizeMH() )
	{
		DEV_LOG( "[error] Hook_Loader::InitalizeMH\n" );
		return 0;
	}

	// Install first hook (critical for GUI)
	if ( !GetHook_Loader()->InstallFirstHook() )
	{
		DEV_LOG( "[error] Hook_Loader::InstallFirstHook\n" );
		// Don't return - continue anyway, some hooks might work
	}

	// Wait for game modules to load
	Sleep( 1000 );

	// Initialize function list (pattern scanning)
	if ( !GetFunctionList()->OnInit() )
	{
		DEV_LOG( "[error] FunctionList::OnInit\n" );
		// Continue anyway - some functions might still work
	}

	// Wait a bit more before SDK loading
	Sleep( 1000 );

	// Load SDK (interfaces, schema system)
	if ( !GetSDK_Loader()->LoadSDK() )
	{
		DEV_LOG( "[error] CSDK_Loader::LoadSDK\n" );
		// Continue anyway - GUI might still work
	}

	// Wait before installing second hooks
	Sleep( 500 );

	// Install second hooks (gameplay hooks)
	if ( !GetHook_Loader()->InstallSecondHook() )
	{
		DEV_LOG( "[error] Hook_Loader::InstallSecondHook\n" );
		// Continue anyway - GUI should still work
	}

	// Wait before client initialization
	Sleep( 500 );

	// Initialize client (this might trigger schema dump)
	// Vectored exception handler will catch any exceptions
	SafeInitClient();

	DEV_LOG( "[success] Cheat initialization complete\n" );

	return 0;
}

auto GetDllDir()->std::string&
{
	return GetDllLauncher()->m_DllDir;
}

auto GetDota2Dir() -> std::string&
{
	return GetDllLauncher()->m_Dota2Dir;
}

auto GetDllLauncher() -> CDllLauncher*
{
	return &g_CDllLauncher;
}
