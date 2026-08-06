#pragma once

#include <Common/Common.hpp>

#include <string>
#include <unordered_map>
#include <filesystem>
#include <cstdint>
#include <cstddef>

struct lua_State;

class CLuaManager final
{
public:
	bool Init( const std::string& scriptsRoot );
	void Shutdown();

	// Reload or lazily load a specific hero script (Assets\\Lua\\<heroName>\\main.lua).
	bool ReloadHero( const std::string& heroName );

	// Invoke per-frame tick for the given hero script (if loaded and exposes on_tick(dt)).
	void TickHero( const std::string& heroName , float deltaSeconds );

	// Invoke optional combo handler on_combo() for the hero.
	void TriggerHeroCombo( const std::string& heroName );

	// Target selection for combo (ent index from UI/Lua).
	void SetComboTarget( const std::string& heroName , int targetEntIndex );
	int GetComboTarget( const std::string& heroName ) const;

	// Status helpers for UI.
	bool HasScript( const std::string& heroName ) const;
	std::string GetStatus( const std::string& heroName ) const;
	std::string GetRoot() const { return m_ScriptsRoot; }

private:
	struct LuaApi
	{
		HMODULE module = nullptr;
		bool loaded = false;

		// Only bind symbols that are actually exported by standard lua54.dll.
		// Macros like luaL_dofile / lua_isfunction / lua_tostring are resolved in code.
		lua_State* ( __cdecl* luaL_newstate )() = nullptr;
		void      ( __cdecl* lua_close )( lua_State* ) = nullptr;
		void      ( __cdecl* luaL_openlibs )( lua_State* ) = nullptr;
		int       ( __cdecl* luaL_loadfilex )( lua_State* , const char* , const char* ) = nullptr;
		int       ( __cdecl* lua_gettop )( lua_State* ) = nullptr;
		void      ( __cdecl* lua_settop )( lua_State* , int ) = nullptr;
		int       ( __cdecl* lua_getglobal )( lua_State* , const char* ) = nullptr;
		int       ( __cdecl* lua_type )( lua_State* , int ) = nullptr;
		void      ( __cdecl* lua_pushnumber )( lua_State* , double ) = nullptr;
		int       ( __cdecl* lua_pcallk )( lua_State* , int , int , int , intptr_t , void* ) = nullptr;
		const char* ( __cdecl* lua_tolstring )( lua_State* , int , size_t* ) = nullptr;
	};

	struct HeroScript
	{
		std::string name;
		std::string folder;
		lua_State* L = nullptr;
		bool loaded = false;
		bool hasOnTick = false;
		std::string lastError;
		std::filesystem::file_time_type lastWrite{};
		int targetEntIndex = -1;
		bool comboPending = false;
	};

	bool LoadApi();
	bool EnsureApi();
	HeroScript& EnsureHeroEntry( const std::string& heroName );
	bool LoadHeroScript( HeroScript& script );
	void CloseHero( HeroScript& script );
	std::string BuildHeroPath( const std::string& heroName ) const;
	void CallFunction( HeroScript& script , const char* funcName , const std::vector<double>& args );

private:
	std::string m_ScriptsRoot;
	LuaApi m_Api;
	std::unordered_map<std::string , HeroScript> m_HeroScripts;
	bool m_Initialized = false;
};

CLuaManager* GetLuaManager();
