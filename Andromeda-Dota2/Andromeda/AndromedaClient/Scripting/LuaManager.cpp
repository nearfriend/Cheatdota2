#include "LuaManager.hpp"

#include <filesystem>
#include <chrono>
#include <algorithm>
#include <cctype>
#include <vector>
#include <Windows.h>

// We avoid linking against lua directly: dynamically load lua54.dll / lua53.dll / lua.dll next to Assets\\Lua\\ or in PATH.
static CLuaManager g_LuaManager{};

static constexpr int LUA_OK = 0;
static constexpr int LUA_TFUNCTION = 6;
static constexpr int LUA_MULTRET = -1;

static std::string ToLowerKey( const std::string& in )
{
	std::string out = in;
	std::transform( out.begin() , out.end() , out.begin() , []( unsigned char c ) { return static_cast<char>( std::tolower( c ) ); } );
	return out;
}

static bool IsExecutableCode( const void* ptr )
{
	if ( !ptr )
		return false;

	MEMORY_BASIC_INFORMATION mbi{};
	if ( !VirtualQuery( ptr , &mbi , sizeof( mbi ) ) )
		return false;

	if ( mbi.State != MEM_COMMIT )
		return false;

	const DWORD protect = mbi.Protect;
	return protect == PAGE_EXECUTE ||
		protect == PAGE_EXECUTE_READ ||
		protect == PAGE_EXECUTE_READWRITE ||
		protect == PAGE_EXECUTE_WRITECOPY;
}

static bool IsCodeInModule( HMODULE module , const void* ptr )
{
	if ( !module || !ptr || !IsExecutableCode( ptr ) )
		return false;

	auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>( module );
	if ( dos->e_magic != IMAGE_DOS_SIGNATURE )
		return false;

	auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>( reinterpret_cast<const uint8_t*>( module ) + dos->e_lfanew );
	if ( nt->Signature != IMAGE_NT_SIGNATURE )
		return false;

	const auto base = reinterpret_cast<uintptr_t>( module );
	const auto addr = reinterpret_cast<uintptr_t>( ptr );
	return addr >= base && addr < base + nt->OptionalHeader.SizeOfImage;
}

// Isolated SEH wrappers — no C++ objects with destructors in these functions.
static lua_State* SafeLuaNewState( lua_State* ( __cdecl* fn )() )
{
	if ( !fn )
		return nullptr;

	__try
	{
		return fn();
	}
	__except ( EXCEPTION_EXECUTE_HANDLER )
	{
		return nullptr;
	}
}

static void SafeLuaClose( void ( __cdecl* fn )( lua_State* ) , lua_State* L )
{
	if ( !fn || !L )
		return;

	__try
	{
		fn( L );
	}
	__except ( EXCEPTION_EXECUTE_HANDLER )
	{
	}
}

static int SafeLuaPCallK( int ( __cdecl* fn )( lua_State* , int , int , int , intptr_t , void* ) ,
	lua_State* L , int nargs , int nresults , int errfunc )
{
	if ( !fn || !L )
		return -1;

	__try
	{
		return fn( L , nargs , nresults , errfunc , 0 , nullptr );
	}
	__except ( EXCEPTION_EXECUTE_HANDLER )
	{
		return -1;
	}
}

static bool SafeLuaOpenLibs( void ( __cdecl* fn )( lua_State* ) , lua_State* L )
{
	if ( !fn || !L )
		return false;

	__try
	{
		fn( L );
		return true;
	}
	__except ( EXCEPTION_EXECUTE_HANDLER )
	{
		return false;
	}
}

static int SafeLuaLoadFileX( int ( __cdecl* fn )( lua_State* , const char* , const char* ) ,
	lua_State* L , const char* path )
{
	if ( !fn || !L || !path )
		return -1;

	__try
	{
		return fn( L , path , nullptr );
	}
	__except ( EXCEPTION_EXECUTE_HANDLER )
	{
		return -1;
	}
}

static HMODULE TryLoadLuaModule( const std::string& baseDir )
{
	const std::string candidates[] =
	{
		baseDir + "lua54.dll",
		baseDir + "lua53.dll",
		baseDir + "lua.dll"
	};

	for ( const auto& path : candidates )
	{
		if ( auto h = LoadLibraryA( path.c_str() ) )
			return h;
	}

	return nullptr;
}

CLuaManager* GetLuaManager()
{
	return &g_LuaManager;
}

bool CLuaManager::Init( const std::string& scriptsRoot )
{
	m_ScriptsRoot = scriptsRoot;
	if ( !m_ScriptsRoot.empty() && m_ScriptsRoot.back() != '\\' && m_ScriptsRoot.back() != '/' )
		m_ScriptsRoot.push_back( '\\' );

	std::error_code ec;
	std::filesystem::create_directories( m_ScriptsRoot , ec );

	m_Initialized = EnsureApi();
	if ( !m_Initialized )
	{
		DEV_LOG( "[lua] failed to load lua runtime (place lua54.dll next to Assets\\\\Lua)\n" );
	}
	else
	{
		DEV_LOG( "[lua] runtime initialized, root: %s\n" , m_ScriptsRoot.c_str() );
	}

	return m_Initialized;
}

void CLuaManager::Shutdown()
{
	for ( auto& [_, script] : m_HeroScripts )
	{
		CloseHero( script );
	}
	m_HeroScripts.clear();

	if ( m_Api.module )
	{
		FreeLibrary( m_Api.module );
		m_Api = {};
	}

	m_Initialized = false;
}

bool CLuaManager::EnsureApi()
{
	if ( m_Api.loaded && ValidateApiPointers() )
		return true;

	// Stale / corrupted pointers — force a clean reload.
	if ( m_Api.module )
	{
		DEV_LOG( "[lua] API pointers invalid — reloading lua runtime\n" );
		FreeLibrary( m_Api.module );
		m_Api = {};
	}

	if ( !LoadApi() )
		return false;

	return m_Api.loaded && ValidateApiPointers();
}

bool CLuaManager::ValidateApiPointers() const
{
	if ( !m_Api.module || !m_Api.loaded )
		return false;

	const void* fns[] = {
		reinterpret_cast<const void*>( m_Api.luaL_newstate ),
		reinterpret_cast<const void*>( m_Api.lua_close ),
		reinterpret_cast<const void*>( m_Api.luaL_openlibs ),
		reinterpret_cast<const void*>( m_Api.luaL_loadfilex ),
		reinterpret_cast<const void*>( m_Api.lua_gettop ),
		reinterpret_cast<const void*>( m_Api.lua_settop ),
		reinterpret_cast<const void*>( m_Api.lua_getglobal ),
		reinterpret_cast<const void*>( m_Api.lua_type ),
		reinterpret_cast<const void*>( m_Api.lua_pushnumber ),
		reinterpret_cast<const void*>( m_Api.lua_pcallk ),
		reinterpret_cast<const void*>( m_Api.lua_tolstring ),
	};

	for ( const void* fn : fns )
	{
		if ( !IsCodeInModule( m_Api.module , fn ) )
			return false;
	}

	return true;
}

bool CLuaManager::LoadApi()
{
	const std::string base = m_ScriptsRoot.empty() ? std::string{} : m_ScriptsRoot;
	m_Api.module = TryLoadLuaModule( base );
	if ( !m_Api.module )
		return false;

	auto load = [&]( auto& fn , const char* name )
	{
		fn = reinterpret_cast<decltype( fn )>( GetProcAddress( m_Api.module , name ) );
		return fn != nullptr && IsCodeInModule( m_Api.module , reinterpret_cast<const void*>( fn ) );
	};

	bool ok = true;
	ok &= load( m_Api.luaL_newstate , "luaL_newstate" );
	ok &= load( m_Api.lua_close , "lua_close" );
	ok &= load( m_Api.luaL_openlibs , "luaL_openlibs" );
	ok &= load( m_Api.luaL_loadfilex , "luaL_loadfilex" );
	ok &= load( m_Api.lua_gettop , "lua_gettop" );
	ok &= load( m_Api.lua_settop , "lua_settop" );
	ok &= load( m_Api.lua_getglobal , "lua_getglobal" );
	ok &= load( m_Api.lua_type , "lua_type" );
	ok &= load( m_Api.lua_pushnumber , "lua_pushnumber" );
	ok &= load( m_Api.lua_pcallk , "lua_pcallk" );
	ok &= load( m_Api.lua_tolstring , "lua_tolstring" );

	m_Api.loaded = ok;
	if ( !m_Api.loaded )
	{
		DEV_LOG( "[lua] failed to resolve required exports from lua runtime\n" );
		FreeLibrary( m_Api.module );
		m_Api = {};
	}

	return m_Api.loaded;
}

CLuaManager::HeroScript& CLuaManager::EnsureHeroEntry( const std::string& heroName )
{
	auto lower = ToLowerKey( heroName );
	auto it = m_HeroScripts.find( lower );
	if ( it == m_HeroScripts.end() )
	{
		HeroScript hs;
		hs.name = lower;
		hs.folder = BuildHeroPath( lower );
		m_HeroScripts.emplace( lower , hs );
		it = m_HeroScripts.find( lower );
	}
	return it->second;
}

bool CLuaManager::LoadHeroScript( HeroScript& script )
{
	if ( !EnsureApi() )
		return false;

	const std::string scriptPath = script.folder + "main.lua";
	std::error_code ec;
	if ( !std::filesystem::exists( scriptPath , ec ) )
	{
		script.lastError = "main.lua not found";
		script.loaded = false;
		return false;
	}

	CloseHero( script );

	if ( !ValidateApiPointers() )
	{
		script.lastError = "lua API pointers invalid";
		DEV_LOG( "[lua] refusing LoadHeroScript — invalid API pointers\n" );
		return false;
	}

	script.L = SafeLuaNewState( m_Api.luaL_newstate );
	if ( !script.L )
	{
		script.lastError = "luaL_newstate failed";
		DEV_LOG( "[lua] luaL_newstate failed or faulted for %s\n" , scriptPath.c_str() );
		return false;
	}

	if ( !SafeLuaOpenLibs( m_Api.luaL_openlibs , script.L ) )
	{
		script.lastError = "luaL_openlibs faulted";
		DEV_LOG( "[lua] luaL_openlibs faulted for %s\n" , scriptPath.c_str() );
		SafeLuaClose( m_Api.lua_close , script.L );
		script.L = nullptr;
		return false;
	}

	if ( SafeLuaLoadFileX( m_Api.luaL_loadfilex , script.L , scriptPath.c_str() ) != LUA_OK ||
		SafeLuaPCallK( m_Api.lua_pcallk , script.L , 0 , LUA_MULTRET , 0 ) != LUA_OK )
	{
		const char* err = m_Api.lua_tolstring ? m_Api.lua_tolstring( script.L , -1 , nullptr ) : "lua_dofile failed";
		script.lastError = err ? err : "unknown lua error";
		DEV_LOG( "[lua] load error (%s): %s\n" , scriptPath.c_str() , script.lastError.c_str() );
		CloseHero( script );
		return false;
	}

	const int top = m_Api.lua_gettop( script.L );
	m_Api.lua_getglobal( script.L , "on_tick" );
	script.hasOnTick = m_Api.lua_type && m_Api.lua_type( script.L , -1 ) == LUA_TFUNCTION;
	m_Api.lua_settop( script.L , top );

	script.loaded = true;
	script.lastError.clear();
	script.lastWrite = std::filesystem::last_write_time( scriptPath , ec );

	DEV_LOG( "[lua] loaded hero script: %s\n" , scriptPath.c_str() );
	return true;
}

void CLuaManager::CloseHero( HeroScript& script )
{
	if ( script.L )
		SafeLuaClose( m_Api.lua_close , script.L );

	script.L = nullptr;
	script.loaded = false;
	script.hasOnTick = false;
}

bool CLuaManager::ReloadHero( const std::string& heroName )
{
	auto& hs = EnsureHeroEntry( heroName );
	return LoadHeroScript( hs );
}

void CLuaManager::TickHero( const std::string& heroName , float deltaSeconds )
{
	if ( !m_Initialized )
		return;

	if ( !EnsureApi() )
		return;

	auto& hs = EnsureHeroEntry( heroName );

	// Lazy load if not loaded yet.
	if ( !hs.loaded )
	{
		if ( !LoadHeroScript( hs ) )
			return;
	}

	if ( !hs.hasOnTick || !hs.L )
		return;

	const int top = m_Api.lua_gettop( hs.L );
	m_Api.lua_getglobal( hs.L , "on_tick" );
	if ( m_Api.lua_type && m_Api.lua_type( hs.L , -1 ) == LUA_TFUNCTION )
	{
		m_Api.lua_pushnumber( hs.L , static_cast<double>( deltaSeconds ) );
		if ( SafeLuaPCallK( m_Api.lua_pcallk , hs.L , 1 , 0 , 0 ) != LUA_OK )
		{
			const char* err = m_Api.lua_tolstring ? m_Api.lua_tolstring( hs.L , -1 , nullptr ) : "pcall failed";
			DEV_LOG( "[lua] on_tick error (%s): %s\n" , hs.name.c_str() , err ? err : "unknown" );
		}
	}
	m_Api.lua_settop( hs.L , top );
}

void CLuaManager::TriggerHeroCombo( const std::string& heroName )
{
	if ( !m_Initialized )
		return;

	auto& hs = EnsureHeroEntry( heroName );

	if ( !hs.loaded )
	{
		if ( !LoadHeroScript( hs ) )
			return;
	}

	hs.comboPending = true;
	CallFunction( hs , "on_combo" , { static_cast<double>( hs.targetEntIndex ) } );
}

void CLuaManager::SetComboTarget( const std::string& heroName , int targetEntIndex )
{
	auto& hs = EnsureHeroEntry( heroName );
	hs.targetEntIndex = targetEntIndex;
}

int CLuaManager::GetComboTarget( const std::string& heroName ) const
{
	auto lower = ToLowerKey( heroName );
	auto it = m_HeroScripts.find( lower );
	if ( it == m_HeroScripts.end() )
		return -1;
	return it->second.targetEntIndex;
}

bool CLuaManager::HasScript( const std::string& heroName ) const
{
	auto lower = ToLowerKey( heroName );
	const std::string path = BuildHeroPath( lower ) + "main.lua";
	return std::filesystem::exists( path );
}

std::string CLuaManager::GetStatus( const std::string& heroName ) const
{
	auto lower = ToLowerKey( heroName );
	auto it = m_HeroScripts.find( lower );
	if ( it == m_HeroScripts.end() )
	{
		return "Not loaded";
	}

	const auto& hs = it->second;
	if ( hs.loaded )
		return "Loaded";
	if ( !hs.lastError.empty() )
		return "Error: " + hs.lastError;
	return "Not loaded";
}

std::string CLuaManager::BuildHeroPath( const std::string& heroName ) const
{
	std::string path = m_ScriptsRoot + heroName;
	if ( !path.empty() && path.back() != '\\' && path.back() != '/' )
		path.push_back( '\\' );
	return path;
}

void CLuaManager::CallFunction( HeroScript& script , const char* funcName , const std::vector<double>& args )
{
	if ( !script.L || !m_Api.lua_getglobal )
		return;

	const int top = m_Api.lua_gettop( script.L );
	m_Api.lua_getglobal( script.L , funcName );
	if ( m_Api.lua_type && m_Api.lua_type( script.L , -1 ) == LUA_TFUNCTION )
	{
		for ( double v : args )
			m_Api.lua_pushnumber( script.L , v );

		const int argCount = static_cast<int>( args.size() );
		if ( SafeLuaPCallK( m_Api.lua_pcallk , script.L , argCount , 0 , 0 ) != LUA_OK )
		{
			const char* err = m_Api.lua_tolstring ? m_Api.lua_tolstring( script.L , -1 , nullptr ) : "pcall failed";
			DEV_LOG( "[lua] %s error (%s): %s\n" , funcName , script.name.c_str() , err ? err : "unknown" );
		}
	}
	m_Api.lua_settop( script.L , top );
}
