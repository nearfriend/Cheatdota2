#include "CAndromedaMenu.hpp"

#include <ImGui/imgui.h>

#include <AndromedaClient/CAndromedaClient.hpp>
#include <AndromedaClient/CAndromedaGUI.hpp>
#include <AndromedaClient/Settings/Settings.hpp>
#include <AndromedaClient/Data/HeroData.hpp>
#include <AndromedaClient/Scripting/LuaManager.hpp>
#include <AndromedaClient/Heroes/Meepo/CMeepoController.hpp>
#include <Dota2/Hook/Hook_GetProtoCDOTAGameAccountPlus.hpp>
#include <DllLauncher.hpp>
#include <Common/Helpers/StringHelper.hpp>

#include <d3d11.h>
#include <wrl/client.h>
#include <wincodec.h>
#include <filesystem>
#include <winreg.h>
#include <vector>
#include <algorithm>
#include <cstdio>

#pragma comment(lib, "windowscodecs.lib")

static CAndromedaMenu g_CAndromedaMenu{};

struct GuiTexture
{
	ID3D11ShaderResourceView* srv = nullptr;
	int width = 0;
	int height = 0;
};

static bool LoadTextureFromFile( const std::wstring& filePath , GuiTexture& outTexture )
{
	auto pDevice = GetAndromedaGUI()->GetDevice();
	auto pContext = GetAndromedaGUI()->GetDeviceContext();

	if ( !pDevice || !pContext )
		return false;

	static Microsoft::WRL::ComPtr<IWICImagingFactory> g_WICFactory;

	if ( !g_WICFactory )
	{
		if ( FAILED( CoCreateInstance( CLSID_WICImagingFactory , nullptr , CLSCTX_INPROC_SERVER , IID_PPV_ARGS( &g_WICFactory ) ) ) )
			return false;
	}

	Microsoft::WRL::ComPtr<IWICBitmapDecoder> decoder;
	if ( FAILED( g_WICFactory->CreateDecoderFromFilename( filePath.c_str() , nullptr , GENERIC_READ , WICDecodeMetadataCacheOnLoad , &decoder ) ) )
		return false;

	Microsoft::WRL::ComPtr<IWICBitmapFrameDecode> frame;
	if ( FAILED( decoder->GetFrame( 0 , &frame ) ) )
		return false;

	Microsoft::WRL::ComPtr<IWICFormatConverter> converter;
	if ( FAILED( g_WICFactory->CreateFormatConverter( &converter ) ) )
		return false;

	if ( FAILED( converter->Initialize( frame.Get() , GUID_WICPixelFormat32bppRGBA , WICBitmapDitherTypeNone , nullptr , 0.f , WICBitmapPaletteTypeCustom ) ) )
		return false;

	UINT w = 0 , h = 0;
	converter->GetSize( &w , &h );

	const UINT stride = w * 4;
	const UINT imageSize = stride * h;
	std::vector<BYTE> pixels( imageSize );

	if ( FAILED( converter->CopyPixels( nullptr , stride , imageSize , pixels.data() ) ) )
		return false;

	D3D11_TEXTURE2D_DESC desc = {};
	desc.Width = w;
	desc.Height = h;
	desc.MipLevels = 1;
	desc.ArraySize = 1;
	desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	desc.SampleDesc.Count = 1;
	desc.Usage = D3D11_USAGE_DEFAULT;
	desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

	D3D11_SUBRESOURCE_DATA initData = {};
	initData.pSysMem = pixels.data();
	initData.SysMemPitch = stride;

	Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
	if ( FAILED( pDevice->CreateTexture2D( &desc , &initData , &texture ) ) )
		return false;

	if ( FAILED( pDevice->CreateShaderResourceView( texture.Get() , nullptr , &outTexture.srv ) ) )
		return false;

	outTexture.width = static_cast<int>( w );
	outTexture.height = static_cast<int>( h );

	return true;
}

static bool LoadTextureFromPaths( const std::vector<std::wstring>& paths , GuiTexture& outTexture , std::string& usedPath )
{
	for ( const auto& path : paths )
	{
		if ( LoadTextureFromFile( path , outTexture ) )
		{
			usedPath = unicode_to_ansi( path );
			return true;
		}
	}

	return false;
}

static void ReleaseTexture( GuiTexture& tex )
{
	if ( tex.srv )
	{
		tex.srv->Release();
		tex.srv = nullptr;
	}

	tex.width = 0;
	tex.height = 0;
}

struct MeepoTextures
{
	bool attempted = false;
	GuiTexture hero;
	GuiTexture spells[4];
	bool loadedHero = false;
	bool loadedSpells[4] = { false,false,false,false };
	std::string heroPathUsed;
	std::string spellPathsUsed[4];
};

static MeepoTextures g_MeepoTextures{};
static GuiTexture g_LogoTexture{};
static bool g_LogoAttempted = false;
static bool g_LogoLoaded = false;
static std::string g_LogoPathUsed;

static void ResetMeepoTextures()
{
	ReleaseTexture( g_MeepoTextures.hero );
	for ( auto& tex : g_MeepoTextures.spells )
		ReleaseTexture( tex );

	g_MeepoTextures = {};
}

static std::string VkToName( int vk )
{
	if ( vk <= 0 )
		return "None";

	UINT scan = MapVirtualKeyA( static_cast<UINT>( vk ) , MAPVK_VK_TO_VSC );
	LONG lParam = static_cast<LONG>( scan ) << 16;

	char name[64] = {};
	if ( GetKeyNameTextA( lParam , name , static_cast<int>( sizeof( name ) ) ) > 0 )
		return std::string( name );

	char fallback[16] = {};
	snprintf( fallback , sizeof( fallback ) , "VK_%d" , vk );
	return std::string( fallback );
}

static void AppendIfPanoramaExists( const std::wstring& baseDir , std::vector<std::wstring>& out )
{
	if ( baseDir.empty() )
		return;

	auto panoImages = std::filesystem::path( baseDir ) / L"game" / L"dota" / L"panorama" / L"images";
	if ( std::filesystem::exists( panoImages ) )
		out.push_back( baseDir );
}

static std::vector<std::wstring> GetDotaBaseCandidates()
{
	std::vector<std::wstring> result;

	// 1) Р‘Р°Р·Р° РёР· GetDota2Dir (РµСЃР»Рё РјС‹ РІРЅСѓС‚СЂРё Dota).
	AppendIfPanoramaExists( ansi_to_unicode( GetDota2Dir() ) , result );

	// 2) Р‘Р°Р·Р° РѕС‚ DLL (РµСЃР»Рё СЃС‚СЂСѓРєС‚СѓСЂР° СЂР°Р·РІРµСЂРЅСѓС‚Р° СЂСЏРґРѕРј).
	AppendIfPanoramaExists( ansi_to_unicode( GetDllDir() ) , result );

	// 3) Р РµРµСЃС‚СЂ Steam (HKCU\Software\Valve\Steam\SteamPath)
	HKEY hKey = nullptr;
	if ( RegOpenKeyExA( HKEY_CURRENT_USER , "Software\\Valve\\Steam" , 0 , KEY_READ , &hKey ) == ERROR_SUCCESS )
	{
		char steamPath[512] = {};
		DWORD type = REG_SZ;
		DWORD size = sizeof( steamPath );
		if ( RegQueryValueExA( hKey , "SteamPath" , nullptr , &type , reinterpret_cast<LPBYTE>( steamPath ) , &size ) == ERROR_SUCCESS )
		{
			std::string base = steamPath;
			if ( !base.empty() && base.back() != '\\' && base.back() != '/' )
				base.push_back( '\\' );
			base += "steamapps\\common\\dota 2 beta\\";
			AppendIfPanoramaExists( ansi_to_unicode( base ) , result );
		}
		RegCloseKey( hKey );
	}

	// 4) Р”РµС„РѕР»С‚РЅС‹Рµ РїСѓС‚Рё Steam.
	AppendIfPanoramaExists( L"C:\\Program Files (x86)\\Steam\\steamapps\\common\\dota 2 beta\\" , result );
	AppendIfPanoramaExists( L"C:\\Program Files\\Steam\\steamapps\\common\\dota 2 beta\\" , result );

	return result;
}

static void TryLoadLogoTexture()
{
	if ( g_LogoAttempted )
		return;

	g_LogoAttempted = true;

	const std::wstring dllBase = ansi_to_unicode( GetDllDir() );
	std::vector<std::wstring> logoPaths =
	{
		dllBase + L"Assets\\image-Photoroom (1).png",
		dllBase + L"Assets\\logo.png",
		dllBase + L"Assets\\Icons\\logo.png",
	};

	g_LogoLoaded = LoadTextureFromPaths( logoPaths , g_LogoTexture , g_LogoPathUsed );
	if ( g_LogoLoaded )
		DEV_LOG( "[ui] logo loaded: %s\n" , g_LogoPathUsed.c_str() );
	else
		DEV_LOG( "[ui] logo not found (checked %zu paths)\n" , logoPaths.size() );
}

static void TryLoadMeepoTextures()
{
	if ( g_MeepoTextures.attempted )
		return;

	g_MeepoTextures.attempted = true;

	const std::wstring dllBase = ansi_to_unicode( GetDllDir() );
	const auto dotaBases = GetDotaBaseCandidates();

	// РџСѓС‚Рё, РєРѕС‚РѕСЂС‹Рµ РїСЂРѕР±СѓРµРј: СЃРЅР°С‡Р°Р»Р° СЂСЏРґРѕРј СЃ DLL, РїРѕС‚РѕРј РІ СѓСЃС‚Р°РЅРѕРІР»РµРЅРЅРѕР№ Dota (РЅРµРїР°РєРѕРІР°РЅРЅС‹Рµ С„Р°Р№Р»С‹).
	std::vector<std::wstring> heroPaths =
	{
		dllBase + L"Assets\\Icons\\Heroes\\meepo.png",
		dllBase + L"Assets\\Icons\\Heroes\\npc_dota_hero_meepo.png",
	};

	for ( const auto& base : dotaBases )
	{
		heroPaths.push_back( base + L"game\\dota\\panorama\\images\\heroes\\meepo.png" );
		heroPaths.push_back( base + L"game\\dota\\panorama\\images\\heroes\\npc_dota_hero_meepo.png" );
	}

	const wchar_t* spellFiles[4] = { L"meepo_earthbind.png" , L"meepo_poof.png" , L"meepo_geostrike.png" , L"meepo_divided_we_stand.png" };

	g_MeepoTextures.loadedHero = LoadTextureFromPaths( heroPaths , g_MeepoTextures.hero , g_MeepoTextures.heroPathUsed );
	if ( g_MeepoTextures.loadedHero )
		DEV_LOG( "[heroes] hero icon loaded: %s\n" , g_MeepoTextures.heroPathUsed.c_str() );
	else
		DEV_LOG( "[heroes] hero icon not found (checked %zu paths)\n" , heroPaths.size() );

	for ( int i = 0; i < 4; ++i )
	{
		std::vector<std::wstring> spellPaths =
		{
			dllBase + L"Assets\\Icons\\Spells\\" + spellFiles[i],
		};

		for ( const auto& base : dotaBases )
			spellPaths.push_back( base + L"game\\dota\\panorama\\images\\spellicons\\" + spellFiles[i] );

		g_MeepoTextures.loadedSpells[i] = LoadTextureFromPaths( spellPaths , g_MeepoTextures.spells[i] , g_MeepoTextures.spellPathsUsed[i] );
		if ( g_MeepoTextures.loadedSpells[i] )
			DEV_LOG( "[heroes] spell icon %d loaded: %s\n" , i + 1 , g_MeepoTextures.spellPathsUsed[i].c_str() );
		else
			DEV_LOG( "[heroes] spell icon %d not found (checked %zu paths)\n" , i + 1 , spellPaths.size() );
	}
}

auto CAndromedaMenu::OnRenderMenu() -> void
{
	float MenuAlpha = static_cast<float>( Settings::Menu::MenuAlpha ) / 255.f;
	MenuAlpha = (std::max)( MenuAlpha , Settings::Menu::MenuAlphaMin );

	ImGui::PushStyleVar( ImGuiStyleVar_Alpha , MenuAlpha );
	ImGui::SetNextWindowSize( ImVec2( 720 , 520 ) , ImGuiCond_FirstUseEver );

	if ( ImGui::Begin( XorStr( CHEAT_NAME ) , 0 ) )
	{
		// draw logo pinned in the top-left of window
		TryLoadLogoTexture();
		if ( g_LogoLoaded && g_LogoTexture.srv )
		{
			ImDrawList* dl = ImGui::GetWindowDrawList();
			const ImVec2 winPos = ImGui::GetWindowPos();
			const float paddingX = 14.f;
			const float paddingY = 10.f;
			const float maxHeight = 64.f;

			float targetWidth = 72.f;
			float targetHeight = targetWidth * static_cast<float>( g_LogoTexture.height ) / static_cast<float>( g_LogoTexture.width );
			if ( targetHeight > maxHeight )
			{
				targetHeight = maxHeight;
				targetWidth = targetHeight * static_cast<float>( g_LogoTexture.width ) / static_cast<float>( g_LogoTexture.height );
			}

			ImVec2 pMin( winPos.x + paddingX , winPos.y + paddingY );
			ImVec2 pMax( pMin.x + targetWidth , pMin.y + targetHeight );
			dl->AddImage( reinterpret_cast<ImTextureID>( g_LogoTexture.srv ) , pMin , pMax );
		}

		static int SelectedTab = 1; // 0: Info, 1: Camera, 2: Menu, 3: Dota+, 4: Heroes
		const ImVec2 sidebarSize = ImVec2( 170.f , 0.f );

		ImGui::BeginChild( "##sidebar" , sidebarSize , true , ImGuiWindowFlags_NoScrollbar );
		{
			const float itemHeight = 32.f;
			const float textYOffset = 6.f;
			const ImVec4 accent = ImGui::GetStyle().Colors[ImGuiCol_SliderGrabActive];

			auto DrawItem = [&]( const char* label , int id , const char* icon )
			{
				ImVec2 pos = ImGui::GetCursorScreenPos();
				ImVec2 avail = ImVec2( sidebarSize.x - 8.f , itemHeight );
				bool hovered = false;

				ImGui::PushID( id );
				ImGui::InvisibleButton( "##navitem" , avail );
				hovered = ImGui::IsItemHovered();
				bool clicked = ImGui::IsItemClicked();

				ImDrawList* dl = ImGui::GetWindowDrawList();
				ImU32 bgCol = 0;

				if ( SelectedTab == id )
					bgCol = ImGui::GetColorU32( ImVec4( accent.x , accent.y , accent.z , 0.25f ) );
				else if ( hovered )
					bgCol = ImGui::GetColorU32( ImVec4( accent.x , accent.y , accent.z , 0.12f ) );

				if ( bgCol )
					dl->AddRectFilled( pos , ImVec2( pos.x + avail.x , pos.y + avail.y ) , bgCol , 6.f );

				ImVec2 textPos = ImVec2( pos.x + 10.f , pos.y + textYOffset );
				ImGui::SetCursorScreenPos( textPos );
				ImGui::Text( "%s %s", icon , label );

				ImVec2 textSize = ImGui::CalcTextSize( label );
				float underlineY = textPos.y + textSize.y + 2.f;
				ImU32 underlineCol = ImGui::GetColorU32( ImVec4( accent.x , accent.y , accent.z , hovered || SelectedTab == id ? 0.8f : 0.0f ) );
				if ( underlineCol )
					dl->AddLine( ImVec2( textPos.x , underlineY ) , ImVec2( textPos.x + textSize.x + 8.f , underlineY ) , underlineCol , 1.5f );

				if ( clicked )
					SelectedTab = id;

				ImGui::Dummy( ImVec2( avail.x , 4.f ) );
				ImGui::PopID();
			};

			DrawItem( XorStr( "Info" ) , 0 , "*" );
			DrawItem( XorStr( "Camera" ) , 1 , "[]" );
			DrawItem( XorStr( "Menu" ) , 2 , "#" );
			DrawItem( XorStr( "Dota+" ) , 3 , "+" );
			DrawItem( XorStr( "Heroes" ) , 4 , "H" );
		}
		ImGui::EndChild();

		ImGui::SameLine();

		ImGui::BeginChild( "##content" , ImVec2( 0 , 0 ) , true );
		{
			// Header: breadcrumbs + actions + search
			{
				const char* tabNames[] = { "Info", "Camera", "Menu", "Dota+", "Heroes" };
				const char* tabDisplay = ( SelectedTab >= 0 && SelectedTab < IM_ARRAYSIZE( tabNames ) ) ? tabNames[SelectedTab] : "Unknown";

				ImVec4 breadcrumbColor = ImGui::GetStyle().Colors[ImGuiCol_TextDisabled];
				ImGui::PushStyleColor( ImGuiCol_Text , breadcrumbColor );
				ImGui::Text( "РћСЃРЅРѕРІРЅРѕР№ / %s" , tabDisplay );
				ImGui::PopStyleColor();

				const float iconWidth = 26.f;
				const float searchWidth = 180.f;
				const float spacing = ImGui::GetStyle().ItemSpacing.x;
				const float rightWidth = iconWidth * 2.f + spacing * 3.f + searchWidth;
				float lineStartX = ImGui::GetCursorPosX();
				float availX = ImGui::GetContentRegionAvail().x;

				if ( availX > rightWidth )
					ImGui::SameLine( lineStartX + availX - rightWidth );
				else
					ImGui::SameLine();

				ImGui::PushStyleVar( ImGuiStyleVar_FramePadding , ImVec2( 6.f , 4.f ) );

				if ( ImGui::Button( "[S]" , ImVec2( iconWidth , 0 ) ) )
				{
					// TODO: hook save action
				}
				if ( ImGui::IsItemHovered() )
					ImGui::SetTooltip( "РЎРѕС…СЂР°РЅРёС‚СЊ" );

				ImGui::SameLine( 0.f , spacing );

				if ( ImGui::Button( "[C]" , ImVec2( iconWidth , 0 ) ) )
				{
					// TODO: hook cloud action
				}
				if ( ImGui::IsItemHovered() )
					ImGui::SetTooltip( "РћР±Р»Р°РєРѕ" );

				ImGui::SameLine( 0.f , spacing * 1.5f );

				static char searchBuf[64] = { 0 };
				ImGui::PushItemWidth( searchWidth );
				ImGui::InputTextWithHint( "##menu.search" , "РџРѕРёСЃРє" , searchBuf , IM_ARRAYSIZE( searchBuf ) );
				ImGui::PopItemWidth();

				ImGui::PopStyleVar();

				ImGui::Separator();
			}

			ImFont* headerFont = GetAndromedaGUI()->GetHeaderFont();

			if ( SelectedTab == 0 )
			{
				if ( headerFont ) ImGui::PushFont( headerFont );
				ImGui::Text( XorStr( "Welcome to Andromeda." ) );
				if ( headerFont ) ImGui::PopFont();
				ImGui::Separator();
				ImGui::TextWrapped( XorStr( "РСЃРїРѕР»СЊР·СѓР№С‚Рµ Р»РµРІРѕРµ РјРµРЅСЋ, С‡С‚РѕР±С‹ РїРµСЂРµР№С‚Рё Рє РЅР°СЃС‚СЂРѕР№РєР°Рј РєР°РјРµСЂС‹, РјРµРЅСЋ Рё Dota+." ) );
			}
			else if ( SelectedTab == 1 )
			{
				if ( headerFont ) ImGui::PushFont( headerFont );
				ImGui::Text( XorStr( "РќР°СЃС‚СЂРѕР№РєРё РєР°РјРµСЂС‹" ) );
				if ( headerFont ) ImGui::PopFont();
				ImGui::Separator();

				if ( RenderSliderFloat( XorStr( "Distance" ) , XorStr( "##Camera.Distance" ) , Settings::Camera::Distance , 1200.f , 3000.f ) )
					GetAndromedaClient()->SetCameraDistance( Settings::Camera::Distance );
			}
			else if ( SelectedTab == 2 )
			{
				if ( headerFont ) ImGui::PushFont( headerFont );
				ImGui::Text( XorStr( "РќР°СЃС‚СЂРѕР№РєРё РјРµРЅСЋ" ) );
				if ( headerFont ) ImGui::PopFont();
				ImGui::Separator();

				RenderSliderInt( XorStr( "Menu Alpha" ) , XorStr( "##Menu.MenuAlpha" ) , Settings::Menu::MenuAlpha , 100 , 255 );
				RenderSliderFloat( "Min alpha clamp" , "##Menu.MinAlpha" , Settings::Menu::MenuAlphaMin , 0.50f , 1.00f , 0.f );

			}
			else if ( SelectedTab == 3 )
			{
				if ( headerFont ) ImGui::PushFont( headerFont );
				ImGui::Text( XorStr( "Dota+" ) );
				if ( headerFont ) ImGui::PopFont();
				ImGui::Separator();

				RenderCheckBox( XorStr( "Enable Dota+ patch" ) , XorStr( "##DotaPlus.Enable" ) , Settings::DotaPlus::Enable );

				const auto statusOffset = static_cast<size_t>( GetDotaPlusStatusOffset() );
				const bool confirmed = IsDotaPlusStatusOffsetConfirmed();

				ImGui::Text( "Status offset: 0x%zX%s", statusOffset , confirmed ? "" : " (default/guessed)" );
			}
			else if ( SelectedTab == 4 )
			{
				if ( headerFont ) ImGui::PushFont( headerFont );
				ImGui::Text( XorStr( "Heroes" ) );
				if ( headerFont ) ImGui::PopFont();
				ImGui::Separator();

				// РџРµСЂРІС‹Р№ РіРµСЂРѕР№ РІ СЃРїРёСЃРєРµ: Meepo.
				if ( headerFont ) ImGui::PushFont( headerFont );
				std::string meepoName = "Meepo";
				if ( auto pHeroData = GetAndromedaClient()->GetHeroData(); pHeroData && pHeroData->IsLoaded() )
				{
					const std::string fromData = pHeroData->GetHeroName( 82 ); // Meepo id РІ dota 2
					if ( !fromData.empty() )
						meepoName = fromData;
				}
				ImGui::Text( "%s", meepoName.c_str() );
				if ( headerFont ) ImGui::PopFont();
				ImGui::TextWrapped( XorStr( "Вђ?ВђГёвЂ?вЂ'вЂ?Вђ?ВђГјВђГіВђГ± Вђ>ВђГ§Вђ?Вђ?ВђГ§ Meepo. ВђГ® Вђ>ВђГ§Вђ?Вђ?ВђГ§ ВђГ§ВђГјВђГіВђ?ВђГіВђГ± ВђГ­Вђ?ВђГёВђ>вЂ?ВђГіВђГ± Вђ?ВђГёвЂ?ВђГјВђ>Вђ?ВђГ± Вђ?ВђГёВђГі Вђ?ВђГёВђ?ВђГ§вЂ?Вђ?ВђГ± ВђГ± Вђ?вЂ?ВђГіВђГ§ВђГјВђГіВђГ± Вђ?ВђГёВђГјВђГі ВђГєВђ?ВђГёВђГ§ВђГј Вђ>ВђГ±ВђГєВђ?ВђГ±." ) );

				TryLoadMeepoTextures();

				


				ImVec2 spellSize( 48.f , 48.f );

				const ImVec4 panelBg = ImGui::GetStyle().Colors[ImGuiCol_FrameBg];
				ImVec4 cardBg = panelBg;
				cardBg.w = std::clamp( cardBg.w * 1.15f , 0.0f , 1.0f );

				int spellsLoadedCount = 0;
				for ( bool loaded : g_MeepoTextures.loadedSpells )
					if ( loaded ) ++spellsLoadedCount;

				ImGui::Separator();
				static char heroSearch[64] = { 0 };
				ImGui::Text( "Hero List / %s", meepoName.c_str() );
				float rowWidth = ImGui::GetContentRegionAvail().x;
				float searchWidth = 200.f;
				if ( rowWidth > searchWidth + 40.f )
					ImGui::SameLine( ImGui::GetCursorPosX() + rowWidth - searchWidth );
				else
					ImGui::SameLine();
				ImGui::SetNextItemWidth( searchWidth );
				ImGui::InputTextWithHint( "##hero.search" , "Search heroes..." , heroSearch , IM_ARRAYSIZE( heroSearch ) );

				if ( ImGui::Button( "Reload icons" ) )
				{
					ResetMeepoTextures();
					TryLoadMeepoTextures();
				}
				ImGui::SameLine();
				const ImVec4 okCol( 0.55f , 0.95f , 0.65f , 1.f );
				const ImVec4 warnCol( 0.95f , 0.45f , 0.45f , 1.f );
				ImGui::TextColored( spellsLoadedCount == 4 ? okCol : warnCol , "Status: %s (%d/4)", spellsLoadedCount == 4 ? "Loaded" : "Missing", spellsLoadedCount );

				auto* luaMgr = GetLuaManager();
				std::string luaStatus = luaMgr ? luaMgr->GetStatus( "meepo" ) : "Lua disabled";
				const bool luaOk = luaMgr && luaStatus.rfind( "Loaded" , 0 ) == 0;
				ImVec4 luaCol = luaOk ? okCol : warnCol;

				if ( ImGui::Button( "Reload Lua" ) && luaMgr )
					luaMgr->ReloadHero( "meepo" );
				ImGui::SameLine();
				ImGui::TextColored( luaCol , "Lua: %s", luaStatus.c_str() );

				std::string luaPath;
				if ( luaMgr )
					luaPath = luaMgr->GetRoot() + "meepo\\main.lua";
				if ( !luaPath.empty() )
				{
					ImGui::SameLine();
					ImGui::TextDisabled( "[path]" );
					if ( ImGui::IsItemHovered() )
					{
						ImGui::BeginTooltip();
						ImGui::TextUnformatted( luaPath.c_str() );
						ImGui::EndTooltip();
					}
				}

				ImGui::Separator();
				ImGui::Text( "Combo key" );
				ImGui::SameLine();
				static bool capturingCombo = false;
				if ( ImGui::Button( capturingCombo ? "Press key..." : "Bind combo" ) )
					capturingCombo = true;

				ImGui::SameLine();
				if ( ImGui::Button( "Clear##combo" ) )
				{
					Settings::Heroes::Meepo::ComboKey = 0;
					capturingCombo = false;
				}

				ImGui::SameLine();
				ImGui::Text( "Current: %s", VkToName( Settings::Heroes::Meepo::ComboKey ).c_str() );
				ImGui::SameLine();
				ImGui::TextDisabled( "(fires on_combo in Lua)" );

				if ( capturingCombo )
				{
					for ( int vk = 1; vk <= 255; ++vk )
					{
						if ( GetAsyncKeyState( vk ) & 0x8000 )
						{
							Settings::Heroes::Meepo::ComboKey = vk;
							capturingCombo = false;
							break;
						}
					}
				}

				ImGui::Text( "Target entindex" );
				ImGui::SameLine();
				static int targetInput = -1;
				if ( targetInput < 0 )
					targetInput = Settings::Heroes::Meepo::TargetEntIndex;
				ImGui::SetNextItemWidth( 120.f );
				if ( ImGui::InputInt( "##meepo.target" , &targetInput ) )
				{
					Settings::Heroes::Meepo::TargetEntIndex = targetInput;
					if ( auto* mgr = GetLuaManager() )
						mgr->SetComboTarget( "meepo" , targetInput );
				}
				ImGui::SameLine();
				ImGui::TextDisabled( "Use entindex from ESP/console" );

				ImGui::PushStyleColor( ImGuiCol_ChildBg , cardBg );
				ImGui::PushStyleVar( ImGuiStyleVar_ChildRounding , 8.f );

				ImGui::Columns( 2 , "heroLayout" , false );

				if ( ImGui::BeginChild( "##automationCard" , ImVec2( 0 , 150.f ) , true ) )
				{
					ImGui::Text( "Auto usage" );
					ImGui::TextDisabled( "Coming soon: automation toggles." );
					ImGui::Separator();
					ImGui::Text( "Bindings" );
					ImGui::TextDisabled( "Coming soon: keybinds and presets." );
				}
				ImGui::EndChild();

				ImGui::NextColumn();
				if ( ImGui::BeginChild( "##placeholderCard" , ImVec2( 0 , 150.f ) , true ) )
				{
					ImGui::TextDisabled( "More settings coming soon." );
				}
				ImGui::EndChild();

				ImGui::Columns( 1 );

				if ( ImGui::BeginChild( "##spellCard" , ImVec2( 0 , 0 ) , true ) )
				{
					ImGui::Text( "Spell icons" );
					ImGui::BeginGroup();
					for ( int i = 0; i < 4; ++i )
					{
						if ( g_MeepoTextures.loadedSpells[i] && g_MeepoTextures.spells[i].srv )
						{
							ImGui::Image( reinterpret_cast<ImTextureID>( g_MeepoTextures.spells[i].srv ) , spellSize );
						}
						else
						{
							ImGui::Dummy( spellSize );
						}

						if ( i != 3 )
							ImGui::SameLine( 0.f , 14.f );
					}
					ImGui::EndGroup();

					if ( spellsLoadedCount == 4 )
					{
						ImGui::TextColored( okCol , "Status: Loaded (4/4)" );
					}
					else
					{
						ImGui::TextColored( warnCol , "Status: Missing (%d/4)", 4 - spellsLoadedCount );
						ImGui::TextDisabled( "Add icons: Assets\\Icons\\Spells\\meepo_*.png or game\\dota\\panorama\\images\\spellicons\\meepo_*.png" );
					}

					ImGui::Separator();
					ImGui::Text( "Ability Information" );
					
					// Get Meepo controller and display ability data
					auto* pClient = GetAndromedaClient();
					if ( pClient )
					{
						const auto& meepoController = pClient->GetMeepoController();
						
						if ( meepoController.IsHeroResolved() )
						{
							const auto& abilities = meepoController.GetAbilities();
							
							if ( abilities.empty() )
							{
								ImGui::TextColored( warnCol , "No abilities found" );
								ImGui::TextDisabled( "Make sure you're playing as Meepo in-game" );
							}
							else
							{
								ImGui::TextColored( okCol , "Found %zu abilities:", abilities.size() );
								ImGui::Spacing();
								
								for ( const auto& ability : abilities )
								{
									ImGui::PushID( ability.name.c_str() );
									
									ImGui::Text( "Name: %s" , ability.name.c_str() );
									
									if ( ability.level > 0 )
										ImGui::Text( "  Level: %d" , ability.level );
									else
										ImGui::TextDisabled( "  Level: Unknown" );
									
									if ( ability.cooldownLength > 0.0f )
									{
										if ( ability.cooldown > 0.0f )
											ImGui::TextColored( warnCol , "  Cooldown: %.1f / %.1f" , ability.cooldown , ability.cooldownLength );
										else
											ImGui::TextColored( okCol , "  Cooldown: Ready (%.1f)" , ability.cooldownLength );
									}
									else
										ImGui::TextDisabled( "  Cooldown: Unknown" );
									
									if ( ability.manaCost > 0 )
										ImGui::Text( "  Mana Cost: %d" , ability.manaCost );
									else
										ImGui::TextDisabled( "  Mana Cost: Unknown" );
									
									if ( ability.castRange > 0.0f )
										ImGui::Text( "  Cast Range: %.0f" , ability.castRange );
									
									ImGui::Text( "  Activated: %s" , ability.isActivated ? "Yes" : "No" );
									
									ImGui::Spacing();
									ImGui::PopID();
								}
							}
						}
						else
						{
							ImGui::TextColored( warnCol , "Hero not resolved" );
							ImGui::TextDisabled( "Start a game as Meepo to see ability information" );
						}
					}
				}
				ImGui::EndChild();

				ImGui::PopStyleVar();
				ImGui::PopStyleColor();

				ImGui::Separator();
				ImGui::Text( "Coming soon: detailed Meepo controls and automation." );

				ImGui::Text( "Coming soon: detailed Meepo controls and automation." );


			}
		}
		ImGui::EndChild();
	}

	ImGui::End();

	ImGui::PopStyleVar();
}

auto CAndromedaMenu::RenderCheckBox( const char* szTitle , const char* szStrID , bool& SettingsItem ) -> bool
{
	if ( szTitle )
	{
		ImGui::AlignTextToFramePadding();
		ImGui::Text( szTitle );
		ImGui::SameLine( ImGui::CalcTextSize( szTitle ).x + 10.f );
	}

	const float trackHeight = 20.f;
	const float trackWidth = 38.f;
	const float knobPadding = 3.f;

	ImDrawList* dl = ImGui::GetWindowDrawList();
	const ImVec4 bgOff = ImGui::GetStyle().Colors[ImGuiCol_FrameBg];
	const ImVec4 bgOn = ImGui::GetStyle().Colors[ImGuiCol_SliderGrabActive];
	const ImVec4 knobCol = ImVec4( 0.95f , 0.95f , 0.97f , 1.0f );
	const float rounding = trackHeight * 0.5f;

	ImVec2 avail = ImGui::GetContentRegionAvail();
	if ( avail.x > trackWidth )
	{
		ImGui::Dummy( ImVec2( avail.x - trackWidth , 0.f ) );
		ImGui::SameLine();
	}

	const ImVec2 cursor = ImGui::GetCursorScreenPos();

	ImGui::InvisibleButton( szStrID , ImVec2( trackWidth , trackHeight ) );
	bool hovered = ImGui::IsItemHovered();
	bool clicked = ImGui::IsItemClicked();

	if ( clicked )
		SettingsItem = !SettingsItem;

	ImVec2 trackMin = cursor;
	ImVec2 trackMax = ImVec2( cursor.x + trackWidth , cursor.y + trackHeight );

	ImU32 trackCol = ImGui::GetColorU32( SettingsItem ? bgOn : bgOff );
	dl->AddRectFilled( trackMin , trackMax , trackCol , rounding );

	float knobRadius = ( trackHeight * 0.5f ) - knobPadding;
	float knobCenterX = SettingsItem ? ( trackMax.x - knobPadding - knobRadius ) : ( trackMin.x + knobPadding + knobRadius );
	float knobCenterY = trackMin.y + trackHeight * 0.5f;
	ImU32 knobColor = ImGui::GetColorU32( knobCol );
	dl->AddCircleFilled( ImVec2( knobCenterX , knobCenterY ) , knobRadius , knobColor );

	if ( hovered )
	{
		ImU32 outlineCol = ImGui::GetColorU32( ImVec4( 1.f , 1.f , 1.f , 0.15f ) );
		dl->AddRect( trackMin , trackMax , outlineCol , rounding , 0 , 1.0f );
	}

	return clicked;
}

auto CAndromedaMenu::RenderComboBox( const char* szTitle , const char* szStrID , int& v , const char* Items[] , int ItemsCount ) -> bool
{
	if ( szTitle )
	{
		ImGui::AlignTextToFramePadding();
		ImGui::Text( szTitle );
	}

	ImGui::SameLine();

	ImGui::PushItemWidth( -1.f );
	const auto Ret = ImGui::Combo( szStrID , &v , Items , ItemsCount );
	ImGui::PopItemWidth();

	return Ret;
}

auto CAndromedaMenu::RenderSliderInt( const char* szTitle , const char* szStrID , int& Value , int Min , int Max ) -> bool
{
	if ( szTitle )
	{
		ImGui::AlignTextToFramePadding();
		ImGui::Text( szTitle );
	}

	ImGui::SameLine();

	ImGui::PushStyleVar( ImGuiStyleVar_FramePadding , ImVec2( 10.f , 5.f ) );
	ImGui::PushStyleVar( ImGuiStyleVar_FrameRounding , 6.f );
	ImGui::PushStyleVar( ImGuiStyleVar_GrabMinSize , 12.f );
	ImGui::PushItemWidth( -1.f );
	const auto Ret = ImGui::SliderInt( szStrID , &Value , Min , Max );
	ImGui::PopItemWidth();
	ImGui::PopStyleVar( 3 );

	return Ret;
}

auto CAndromedaMenu::RenderSliderFloat( const char* szTitle , const char* szStrID , float& v , float min , float max , float left_padding ) -> bool
{
	if ( szTitle )
	{
		ImGui::AlignTextToFramePadding();
		ImGui::Text( szTitle );

		if ( left_padding <= 0.f )
			ImGui::SameLine( ImGui::CalcTextSize( szTitle ).x + 10.f );
		else
			ImGui::SameLine( left_padding );
	}

	ImGui::PushStyleVar( ImGuiStyleVar_FramePadding , ImVec2( 10.f , 5.f ) );
	ImGui::PushStyleVar( ImGuiStyleVar_FrameRounding , 6.f );
	ImGui::PushStyleVar( ImGuiStyleVar_GrabMinSize , 12.f );
	ImGui::PushItemWidth( -1.f );
	const auto Ret = ImGui::SliderFloat( szStrID , &v , min , max , "%.3f" , ImGuiSliderFlags_AlwaysClamp );
	ImGui::PopItemWidth();
	ImGui::PopStyleVar( 3 );

	return Ret;
}

auto GetAndromedaMenu() -> CAndromedaMenu*
{
	return &g_CAndromedaMenu;
}

