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
#include <filesystem>

static CAndromedaClient g_CAndromedaClient{};
static CHeroDataLoader g_HeroDataLoader{};

auto CAndromedaClient::OnInit() -> void
{
	if ( dota_camera_distance.Search() )
		DEV_LOG( "[dota_camera_distance] Found !\n" );

	if ( dota_camera_fog_end.Search() )
		DEV_LOG( "[dota_camera_fog_end] Found !\n" );

	if ( dota_camera_farplane.Search() )
		DEV_LOG( "[dota_camera_farplane] Found !\n" );

	// Hero data: load from cache only if file exists and looks valid; otherwise skip to avoid noisy errors.
	const std::string baseDir = GetDllDir();
	const std::string heroJsonPath = baseDir + "Assets\\data\\npc_heroes.json";
	const bool heroFileOk = std::filesystem::exists( heroJsonPath ) && std::filesystem::file_size( heroJsonPath ) > 128u;
	if ( heroFileOk && g_HeroDataLoader.LoadFromFile( heroJsonPath ) )
	{
		const char* src = g_HeroDataLoader.GetSourcePath().c_str();
		DEV_LOG( "[heroes] loaded %zu heroes from %s\n" , g_HeroDataLoader.GetAll().size() , src );
	}
	else
	{
		DEV_LOG( "[heroes] skip hero data load (missing/invalid file: %s)\n" , heroJsonPath.c_str() );
	}

	// Initialize Lua runtime (scripts live in Assets\\Lua\\<hero>\\main.lua) only if lua*.dll is present.
	const std::string scriptsRoot = baseDir + "Assets\\Lua\\";
	const bool hasLuaDll =
		std::filesystem::exists( scriptsRoot + "lua54.dll" ) ||
		std::filesystem::exists( scriptsRoot + "lua53.dll" ) ||
		std::filesystem::exists( scriptsRoot + "lua.dll" );

	if ( hasLuaDll )
	{
		GetLuaManager()->Init( scriptsRoot );
	}
	else
	{
		DEV_LOG( "[lua] skip init: lua*.dll not found in %s\n" , scriptsRoot.c_str() );
	}
}

auto CAndromedaClient::SetCameraDistance( float Distance ) -> void
{
	static float* force_dota_camera_distance = reinterpret_cast<float*>( dota_camera_distance.GetFunction() );
	static float* force_dota_camera_fog_end = reinterpret_cast<float*>( dota_camera_fog_end.GetFunction() );
	static float* force_dota_camera_farplane = reinterpret_cast<float*>( dota_camera_farplane.GetFunction() );

	if( force_dota_camera_distance )
		*force_dota_camera_distance = Distance;

	if ( force_dota_camera_fog_end )
		*force_dota_camera_fog_end = 10000.f;

	if ( force_dota_camera_farplane )
		*force_dota_camera_farplane = 10000.f;
}

auto CAndromedaClient::OnRender() -> void
{
	if ( GetAndromedaGUI()->IsVisible() )
		GetAndromedaMenu()->OnRenderMenu();

	GetFontManager()->FirstInitFonts();
	GetFontManager()->m_VerdanaFont.DrawString( 1 , 1 , ImColor( 255 , 255 , 0 ) , FW1_LEFT , XorStr( CHEAT_NAME ) );
}

auto CAndromedaClient::OnCreateMove( CDOTAInput* pCDOTAInput , CUserCmd* pCUserCmd ) -> void
{
	static int s_nCallCount = 0;
	s_nCallCount++;
	
	// Log first few calls to verify function is being called
	if ( s_nCallCount <= 3 )
	{
		DEV_LOG( "[andromeda] OnCreateMove called (call #%d, pCUserCmd=%p)\n" , s_nCallCount , pCUserCmd );
	}
	
	// Lua scripting for Invoker (on_tick/on_combo).
	m_InvokerController.OnCreateMove( pCDOTAInput , pCUserCmd );
	
	// Meepo ability information display
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
