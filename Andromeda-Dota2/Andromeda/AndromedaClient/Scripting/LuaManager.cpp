#include "LuaManager.hpp"

#include <filesystem>
#include <chrono>
#include <algorithm>
#include <cctype>
#include <vector>

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
	if ( m_Api.loaded )
		return true;

	if ( !LoadApi() )
		return false;

	return m_Api.loaded;
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
		return fn != nullptr;
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
	if ( !std::filesystem::exists( scriptPath ) )
	{
		script.lastError = "main.lua not found";
		script.loaded = false;
		return false;
	}

	CloseHero( script );

	script.L = m_Api.luaL_newstate ? m_Api.luaL_newstate() : nullptr;
	if ( !script.L )
	{
		script.lastError = "luaL_newstate failed";
		return false;
	}

	m_Api.luaL_openlibs( script.L );

	if ( !m_Api.luaL_loadfilex || !m_Api.lua_pcallk ||
		m_Api.luaL_loadfilex( script.L , scriptPath.c_str() , nullptr ) != LUA_OK ||
		m_Api.lua_pcallk( script.L , 0 , LUA_MULTRET , 0 , 0 , nullptr ) != LUA_OK )
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
	std::error_code ec;
	script.lastWrite = std::filesystem::last_write_time( scriptPath , ec );

	DEV_LOG( "[lua] loaded hero script: %s\n" , scriptPath.c_str() );
	return true;
}

void CLuaManager::CloseHero( HeroScript& script )
{
	if ( script.L && m_Api.lua_close )
		m_Api.lua_close( script.L );

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
		if ( m_Api.lua_pcallk( hs.L , 1 , 0 , 0 , 0 , nullptr ) != LUA_OK )
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
		if ( m_Api.lua_pcallk( script.L , argCount , 0 , 0 , 0 , nullptr ) != LUA_OK )
		{
			const char* err = m_Api.lua_tolstring ? m_Api.lua_tolstring( script.L , -1 , nullptr ) : "pcall failed";
			DEV_LOG( "[lua] %s error (%s): %s\n" , funcName , script.name.c_str() , err ? err : "unknown" );
		}
	}
	m_Api.lua_settop( script.L , top );
}
