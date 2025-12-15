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

	// Подгрузка hero data из кеша: Assets\data\npc_heroes.json
	const std::string baseDir = GetDllDir();
	const std::string heroJsonPath = baseDir + "Assets\\data\\npc_heroes.json";
	const std::string heroJsonUrl = "https://raw.githubusercontent.com/odota/dotaconstants/master/build/npc_heroes.json";

	// Если файла нет, попробуем скачать; если есть — просто загружаем. forceDownload=false.
	if ( g_HeroDataLoader.EnsureCacheAndLoad( heroJsonUrl , heroJsonPath , false ) )
	{
		const char* src = g_HeroDataLoader.GetSourcePath().c_str();
		DEV_LOG( "[heroes] loaded %zu heroes from %s\n" , g_HeroDataLoader.GetAll().size() , src );
	}
	else
	{
		DEV_LOG( "[heroes] failed to load hero data (url: %s, path: %s)\n" , heroJsonUrl.c_str() , heroJsonPath.c_str() );
	}

	// Initialize Lua runtime (scripts live in Assets\\Lua\\<hero>\\main.lua)
	GetLuaManager()->Init( baseDir + "Assets\\Lua\\" );
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
	// TODO: bind to the actual local hero; for now drive the Meepo script tick and combo.
	auto* lua = GetLuaManager();
	if ( lua )
	{
		lua->TickHero( "meepo" , 1.f / 60.f );

		if ( Settings::Heroes::Meepo::ComboKey > 0 && ( GetAsyncKeyState( Settings::Heroes::Meepo::ComboKey ) & 0x8000 ) )
			lua->TriggerHeroCombo( "meepo" );

		// Placeholder: here we would translate pending combo to actual orders (need IssueOrder + ability offsets).
		// For now just clear the pending flag to avoid spamming.
		if ( lua->GetComboTarget( "meepo" ) >= 0 )
		{
			// Future: use target ent index to execute ability sequence.
		}
	}
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
