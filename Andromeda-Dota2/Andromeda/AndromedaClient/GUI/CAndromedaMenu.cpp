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
#include <cmath>
#include <cstdio>

#pragma comment(lib, "windowscodecs.lib")

static CAndromedaMenu g_CAndromedaMenu{};

static constexpr ImU32 kAccentColor = IM_COL32( 218 , 51 , 62 , 255 );
static constexpr ImU32 kAccentTextColor = IM_COL32( 235 , 77 , 87 , 255 );
static constexpr ImU32 kAccentTrackColor = IM_COL32( 93 , 31 , 38 , 255 );

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

	switch ( vk )
	{
	case VK_MBUTTON:
		return "Middle Mouse";
	case VK_XBUTTON1:
		return "Mouse4";
	case VK_XBUTTON2:
		return "Mouse5";
	case VK_MENU:
		return "Alt";
	case VK_LMENU:
		return "Left Alt";
	case VK_RMENU:
		return "Right Alt";
	case VK_SHIFT:
		return "Shift";
	case VK_CONTROL:
		return "Ctrl";
	case VK_SPACE:
		return "Space";
	default:
		break;
	}

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

static void DrawBrandMark( ImDrawList* drawList , const ImVec2& center )
{
	const ImU32 purple = kAccentColor;
	const ImU32 white = IM_COL32( 236 , 238 , 242 , 255 );

	drawList->AddTriangleFilled( ImVec2( center.x , center.y - 17.f ) , ImVec2( center.x - 14.f , center.y - 8.f ) , ImVec2( center.x - 5.f , center.y ) , purple );
	drawList->AddTriangleFilled( ImVec2( center.x + 17.f , center.y ) , ImVec2( center.x + 8.f , center.y - 14.f ) , ImVec2( center.x , center.y - 5.f ) , purple );
	drawList->AddTriangleFilled( ImVec2( center.x , center.y + 17.f ) , ImVec2( center.x + 14.f , center.y + 8.f ) , ImVec2( center.x + 5.f , center.y ) , purple );
	drawList->AddTriangleFilled( ImVec2( center.x - 17.f , center.y ) , ImVec2( center.x - 8.f , center.y + 14.f ) , ImVec2( center.x , center.y + 5.f ) , purple );
	drawList->AddRectFilled( ImVec2( center.x - 4.f , center.y - 4.f ) , ImVec2( center.x + 4.f , center.y + 4.f ) , white , 1.f );
}

enum class ReferenceIcon
{
	Info, Aggro, Camera, Heroes, Overlay, Bell, Offscreen, Radius, Hidden,
	Visible, Ward, Sparkles, Tools, Code, Cloud, Globe, Gear, Save, Mouse,
	Distance, Smooth, Duration, Speed, Check, Allies, Warning, Choose,
	Items, Network, Glyph
};

static void DrawReferenceIcon( ImDrawList* dl , const ImVec2& c , ReferenceIcon icon , ImU32 color , float s = 1.f )
{
	const float r = 7.f * s;
	const float t = (std::max)( 1.f , 1.35f * s );
	switch ( icon )
	{
	case ReferenceIcon::Info:
		dl->AddCircle( c , r , color , 16 , t );
		dl->AddCircleFilled( ImVec2( c.x , c.y - 3.2f * s ) , 1.1f * s , color );
		dl->AddLine( ImVec2( c.x , c.y - 0.5f * s ) , ImVec2( c.x , c.y + 4.2f * s ) , color , t );
		break;
	case ReferenceIcon::Aggro:
		dl->AddCircle( c , 6.5f * s , color , 16 , t );
		dl->AddCircle( c , 2.2f * s , color , 12 , t );
		dl->AddLine( ImVec2( c.x + 3.f * s , c.y - 3.f * s ) , ImVec2( c.x + 8.f * s , c.y - 8.f * s ) , color , t );
		dl->AddTriangleFilled( ImVec2( c.x + 8.f * s , c.y - 8.f * s ) , ImVec2( c.x + 4.f * s , c.y - 7.f * s ) , ImVec2( c.x + 7.f * s , c.y - 4.f * s ) , color );
		break;
	case ReferenceIcon::Camera:
		dl->AddRect( ImVec2( c.x - 8.f * s , c.y - 5.f * s ) , ImVec2( c.x + 8.f * s , c.y + 6.f * s ) , color , 2.f * s , 0 , t );
		dl->AddRectFilled( ImVec2( c.x - 4.5f * s , c.y - 7.f * s ) , ImVec2( c.x + 1.f * s , c.y - 5.f * s ) , color , 1.f * s );
		dl->AddCircle( ImVec2( c.x + 1.5f * s , c.y + 0.5f * s ) , 3.3f * s , color , 14 , t );
		break;
	case ReferenceIcon::Heroes:
		dl->AddCircle( ImVec2( c.x - 3.8f * s , c.y - 3.8f * s ) , 2.8f * s , color , 12 , t );
		dl->AddCircle( ImVec2( c.x + 4.2f * s , c.y - 2.5f * s ) , 2.3f * s , color , 12 , t );
		dl->AddLine( ImVec2( c.x - 9.f * s , c.y + 7.f * s ) , ImVec2( c.x - 7.f * s , c.y + 2.f * s ) , color , t );
		dl->AddLine( ImVec2( c.x - 7.f * s , c.y + 2.f * s ) , ImVec2( c.x , c.y + 2.f * s ) , color , t );
		dl->AddLine( ImVec2( c.x , c.y + 2.f * s ) , ImVec2( c.x + 2.f * s , c.y + 7.f * s ) , color , t );
		dl->AddLine( ImVec2( c.x + 2.f * s , c.y + 3.f * s ) , ImVec2( c.x + 7.f * s , c.y + 3.f * s ) , color , t );
		dl->AddLine( ImVec2( c.x + 7.f * s , c.y + 3.f * s ) , ImVec2( c.x + 9.f * s , c.y + 7.f * s ) , color , t );
		break;
	case ReferenceIcon::Overlay:
		dl->AddRect( ImVec2( c.x - 8.f * s , c.y - 7.f * s ) , ImVec2( c.x + 8.f * s , c.y + 7.f * s ) , color , 2.f * s , 0 , t );
		dl->AddLine( ImVec2( c.x - 4.f * s , c.y - 3.f * s ) , ImVec2( c.x + 5.f * s , c.y - 3.f * s ) , color , t );
		dl->AddLine( ImVec2( c.x - 4.f * s , c.y + 1.f * s ) , ImVec2( c.x + 3.f * s , c.y + 1.f * s ) , color , t );
		dl->AddLine( ImVec2( c.x - 4.f * s , c.y + 5.f * s ) , ImVec2( c.x + 6.f * s , c.y + 5.f * s ) , color , t );
		break;
	case ReferenceIcon::Bell:
		dl->AddLine( ImVec2( c.x - 6.f * s , c.y + 4.f * s ) , ImVec2( c.x - 5.f * s , c.y - 2.f * s ) , color , t );
		dl->AddLine( ImVec2( c.x - 5.f * s , c.y - 2.f * s ) , ImVec2( c.x , c.y - 6.f * s ) , color , t );
		dl->AddLine( ImVec2( c.x , c.y - 6.f * s ) , ImVec2( c.x + 5.f * s , c.y - 2.f * s ) , color , t );
		dl->AddLine( ImVec2( c.x + 5.f * s , c.y - 2.f * s ) , ImVec2( c.x + 6.f * s , c.y + 4.f * s ) , color , t );
		dl->AddLine( ImVec2( c.x - 7.f * s , c.y + 4.f * s ) , ImVec2( c.x + 7.f * s , c.y + 4.f * s ) , color , t );
		dl->AddCircleFilled( ImVec2( c.x , c.y + 6.f * s ) , 1.5f * s , color );
		break;
	case ReferenceIcon::Offscreen:
		dl->AddCircle( c , 7.f * s , color , 16 , t );
		dl->AddLine( ImVec2( c.x - 3.f * s , c.y + 3.f * s ) , ImVec2( c.x + 5.f * s , c.y - 5.f * s ) , color , t );
		dl->AddLine( ImVec2( c.x + 1.f * s , c.y - 5.f * s ) , ImVec2( c.x + 5.f * s , c.y - 5.f * s ) , color , t );
		dl->AddLine( ImVec2( c.x + 5.f * s , c.y - 5.f * s ) , ImVec2( c.x + 5.f * s , c.y - 1.f * s ) , color , t );
		break;
	case ReferenceIcon::Radius:
		dl->AddCircle( c , 7.5f * s , color , 20 , t );
		dl->AddCircle( c , 4.f * s , color , 16 , t );
		dl->AddCircleFilled( c , 1.4f * s , color );
		break;
	case ReferenceIcon::Hidden:
	case ReferenceIcon::Visible:
		dl->AddLine( ImVec2( c.x - 8.f * s , c.y ) , ImVec2( c.x - 3.f * s , c.y - 4.f * s ) , color , t );
		dl->AddLine( ImVec2( c.x - 3.f * s , c.y - 4.f * s ) , ImVec2( c.x + 3.f * s , c.y - 4.f * s ) , color , t );
		dl->AddLine( ImVec2( c.x + 3.f * s , c.y - 4.f * s ) , ImVec2( c.x + 8.f * s , c.y ) , color , t );
		dl->AddLine( ImVec2( c.x + 8.f * s , c.y ) , ImVec2( c.x + 3.f * s , c.y + 4.f * s ) , color , t );
		dl->AddLine( ImVec2( c.x + 3.f * s , c.y + 4.f * s ) , ImVec2( c.x - 3.f * s , c.y + 4.f * s ) , color , t );
		dl->AddLine( ImVec2( c.x - 3.f * s , c.y + 4.f * s ) , ImVec2( c.x - 8.f * s , c.y ) , color , t );
		dl->AddCircleFilled( c , 2.3f * s , color );
		if ( icon == ReferenceIcon::Hidden )
			dl->AddLine( ImVec2( c.x - 8.f * s , c.y + 7.f * s ) , ImVec2( c.x + 8.f * s , c.y - 7.f * s ) , color , 2.f * s );
		break;
	case ReferenceIcon::Ward:
		dl->AddCircle( ImVec2( c.x , c.y - 4.f * s ) , 4.f * s , color , 14 , t );
		dl->AddCircleFilled( ImVec2( c.x , c.y - 4.f * s ) , 1.4f * s , color );
		dl->AddLine( ImVec2( c.x , c.y ) , ImVec2( c.x , c.y + 7.f * s ) , color , t );
		dl->AddLine( ImVec2( c.x - 5.f * s , c.y + 7.f * s ) , ImVec2( c.x + 5.f * s , c.y + 7.f * s ) , color , t );
		break;
	case ReferenceIcon::Sparkles:
		dl->AddLine( ImVec2( c.x , c.y - 8.f * s ) , ImVec2( c.x , c.y + 8.f * s ) , color , t );
		dl->AddLine( ImVec2( c.x - 8.f * s , c.y ) , ImVec2( c.x + 8.f * s , c.y ) , color , t );
		dl->AddLine( ImVec2( c.x + 5.f * s , c.y - 7.f * s ) , ImVec2( c.x + 9.f * s , c.y - 3.f * s ) , color , t );
		dl->AddLine( ImVec2( c.x + 9.f * s , c.y - 7.f * s ) , ImVec2( c.x + 5.f * s , c.y - 3.f * s ) , color , t );
		break;
	case ReferenceIcon::Tools:
		dl->AddLine( ImVec2( c.x - 6.f * s , c.y - 7.f * s ) , ImVec2( c.x + 6.f * s , c.y + 7.f * s ) , color , 2.f * s );
		dl->AddLine( ImVec2( c.x + 6.f * s , c.y - 7.f * s ) , ImVec2( c.x - 6.f * s , c.y + 7.f * s ) , color , 2.f * s );
		dl->AddCircle( ImVec2( c.x - 6.f * s , c.y - 7.f * s ) , 2.5f * s , color , 12 , t );
		break;
	case ReferenceIcon::Code:
		dl->AddLine( ImVec2( c.x - 2.f * s , c.y - 7.f * s ) , ImVec2( c.x - 8.f * s , c.y ) , color , t );
		dl->AddLine( ImVec2( c.x - 8.f * s , c.y ) , ImVec2( c.x - 2.f * s , c.y + 7.f * s ) , color , t );
		dl->AddLine( ImVec2( c.x + 2.f * s , c.y - 7.f * s ) , ImVec2( c.x + 8.f * s , c.y ) , color , t );
		dl->AddLine( ImVec2( c.x + 8.f * s , c.y ) , ImVec2( c.x + 2.f * s , c.y + 7.f * s ) , color , t );
		break;
	case ReferenceIcon::Cloud:
		dl->AddCircle( ImVec2( c.x - 4.f * s , c.y + 1.f * s ) , 4.f * s , color , 12 , t );
		dl->AddCircle( ImVec2( c.x + 1.f * s , c.y - 2.f * s ) , 5.f * s , color , 14 , t );
		dl->AddCircle( ImVec2( c.x + 6.f * s , c.y + 2.f * s ) , 3.5f * s , color , 12 , t );
		dl->AddLine( ImVec2( c.x - 7.f * s , c.y + 5.f * s ) , ImVec2( c.x + 8.f * s , c.y + 5.f * s ) , color , t );
		break;
	case ReferenceIcon::Globe:
		dl->AddCircle( c , 8.f * s , color , 20 , t );
		dl->AddLine( ImVec2( c.x - 8.f * s , c.y ) , ImVec2( c.x + 8.f * s , c.y ) , color , t );
		dl->AddCircle( c , 4.f * s , color , 16 , t );
		break;
	case ReferenceIcon::Gear:
		dl->AddCircle( c , 6.f * s , color , 12 , 2.5f * s );
		dl->AddCircleFilled( c , 2.f * s , IM_COL32( 13 , 14 , 16 , 255 ) );
		for ( int i = 0; i < 8; ++i )
		{
			const float a = static_cast<float>( i ) * 0.785398f;
			dl->AddLine( ImVec2( c.x + cosf( a ) * 6.f * s , c.y + sinf( a ) * 6.f * s ) , ImVec2( c.x + cosf( a ) * 9.f * s , c.y + sinf( a ) * 9.f * s ) , color , 2.f * s );
		}
		break;
	case ReferenceIcon::Save:
		dl->AddRect( ImVec2( c.x - 7.f * s , c.y - 8.f * s ) , ImVec2( c.x + 7.f * s , c.y + 8.f * s ) , color , 1.f * s , 0 , t );
		dl->AddRect( ImVec2( c.x - 4.f * s , c.y - 6.f * s ) , ImVec2( c.x + 3.f * s , c.y - 1.f * s ) , color , 0.f , 0 , t );
		dl->AddCircle( ImVec2( c.x , c.y + 4.f * s ) , 2.5f * s , color , 12 , t );
		break;
	case ReferenceIcon::Mouse:
		dl->AddRect( ImVec2( c.x - 5.f * s , c.y - 8.f * s ) , ImVec2( c.x + 5.f * s , c.y + 8.f * s ) , color , 5.f * s , 0 , t );
		dl->AddLine( ImVec2( c.x , c.y - 8.f * s ) , ImVec2( c.x , c.y - 2.f * s ) , color , t );
		dl->AddCircleFilled( ImVec2( c.x , c.y - 4.5f * s ) , 1.f * s , color );
		break;
	case ReferenceIcon::Distance:
		dl->AddLine( ImVec2( c.x - 8.f * s , c.y ) , ImVec2( c.x + 8.f * s , c.y ) , color , t );
		dl->AddTriangleFilled( ImVec2( c.x - 8.f * s , c.y ) , ImVec2( c.x - 3.f * s , c.y - 3.f * s ) , ImVec2( c.x - 3.f * s , c.y + 3.f * s ) , color );
		dl->AddTriangleFilled( ImVec2( c.x + 8.f * s , c.y ) , ImVec2( c.x + 3.f * s , c.y - 3.f * s ) , ImVec2( c.x + 3.f * s , c.y + 3.f * s ) , color );
		break;
	case ReferenceIcon::Smooth:
		dl->AddLine( ImVec2( c.x - 8.f * s , c.y + 2.f * s ) , ImVec2( c.x - 4.f * s , c.y - 3.f * s ) , color , t );
		dl->AddLine( ImVec2( c.x - 4.f * s , c.y - 3.f * s ) , ImVec2( c.x , c.y + 3.f * s ) , color , t );
		dl->AddLine( ImVec2( c.x , c.y + 3.f * s ) , ImVec2( c.x + 4.f * s , c.y - 2.f * s ) , color , t );
		dl->AddLine( ImVec2( c.x + 4.f * s , c.y - 2.f * s ) , ImVec2( c.x + 8.f * s , c.y + 1.f * s ) , color , t );
		break;
	case ReferenceIcon::Duration:
		dl->AddCircle( c , 7.f * s , color , 18 , t );
		dl->AddLine( ImVec2( c.x , c.y ) , ImVec2( c.x , c.y - 4.f * s ) , color , t );
		dl->AddLine( ImVec2( c.x , c.y ) , ImVec2( c.x + 4.f * s , c.y + 2.f * s ) , color , t );
		dl->AddLine( ImVec2( c.x - 3.f * s , c.y - 9.f * s ) , ImVec2( c.x + 3.f * s , c.y - 9.f * s ) , color , t );
		break;
	case ReferenceIcon::Speed:
		dl->AddLine( ImVec2( c.x - 8.f * s , c.y + 5.f * s ) , ImVec2( c.x - 2.f * s , c.y - 6.f * s ) , color , 2.f * s );
		dl->AddLine( ImVec2( c.x - 2.f * s , c.y - 6.f * s ) , ImVec2( c.x + 1.f * s , c.y ) , color , 2.f * s );
		dl->AddLine( ImVec2( c.x + 1.f * s , c.y ) , ImVec2( c.x + 8.f * s , c.y - 1.f * s ) , color , 2.f * s );
		break;
	case ReferenceIcon::Check:
		dl->AddLine( ImVec2( c.x - 7.f * s , c.y ) , ImVec2( c.x - 2.f * s , c.y + 5.f * s ) , color , 2.f * s );
		dl->AddLine( ImVec2( c.x - 2.f * s , c.y + 5.f * s ) , ImVec2( c.x + 8.f * s , c.y - 6.f * s ) , color , 2.f * s );
		break;
	case ReferenceIcon::Allies:
		dl->AddCircleFilled( ImVec2( c.x - 2.f * s , c.y - 4.f * s ) , 3.5f * s , color );
		dl->AddTriangleFilled( ImVec2( c.x - 8.f * s , c.y + 7.f * s ) , ImVec2( c.x + 4.f * s , c.y + 7.f * s ) , ImVec2( c.x - 2.f * s , c.y ) , color );
		dl->AddLine( ImVec2( c.x + 5.f * s , c.y - 5.f * s ) , ImVec2( c.x + 9.f * s , c.y - 1.f * s ) , color , 1.7f * s );
		dl->AddLine( ImVec2( c.x + 9.f * s , c.y - 5.f * s ) , ImVec2( c.x + 5.f * s , c.y - 1.f * s ) , color , 1.7f * s );
		break;
	case ReferenceIcon::Warning:
		dl->AddCircleFilled( c , 7.f * s , color , 18 );
		dl->AddLine( ImVec2( c.x , c.y - 4.f * s ) , ImVec2( c.x , c.y + 1.5f * s ) , IM_COL32( 16 , 17 , 19 , 255 ) , 1.5f * s );
		dl->AddCircleFilled( ImVec2( c.x , c.y + 4.f * s ) , 1.f * s , IM_COL32( 16 , 17 , 19 , 255 ) );
		break;
	case ReferenceIcon::Choose:
		dl->AddCircle( ImVec2( c.x - 4.f * s , c.y - 4.f * s ) , 2.5f * s , color , 10 , 1.5f * s );
		dl->AddCircle( ImVec2( c.x + 5.f * s , c.y - 2.f * s ) , 2.5f * s , color , 10 , 1.5f * s );
		dl->AddCircle( ImVec2( c.x - 1.f * s , c.y + 5.f * s ) , 2.5f * s , color , 10 , 1.5f * s );
		dl->AddLine( ImVec2( c.x - 2.f * s , c.y - 3.f * s ) , ImVec2( c.x + 3.f * s , c.y - 2.f * s ) , color , 1.5f * s );
		dl->AddLine( ImVec2( c.x + 3.f * s , c.y ) , ImVec2( c.x , c.y + 3.f * s ) , color , 1.5f * s );
		break;
	case ReferenceIcon::Items:
		dl->AddRect( ImVec2( c.x - 6.f * s , c.y - 5.f * s ) , ImVec2( c.x + 6.f * s , c.y + 7.f * s ) , color , 2.f * s , 0 , 1.5f * s );
		dl->AddLine( ImVec2( c.x - 3.f * s , c.y - 5.f * s ) , ImVec2( c.x - 2.f * s , c.y - 8.f * s ) , color , 1.5f * s );
		dl->AddLine( ImVec2( c.x - 2.f * s , c.y - 8.f * s ) , ImVec2( c.x + 2.f * s , c.y - 8.f * s ) , color , 1.5f * s );
		dl->AddLine( ImVec2( c.x + 2.f * s , c.y - 8.f * s ) , ImVec2( c.x + 3.f * s , c.y - 5.f * s ) , color , 1.5f * s );
		break;
	case ReferenceIcon::Network:
		for ( int i = 0; i < 3; ++i )
			dl->AddRectFilled( ImVec2( c.x - 8.f * s , c.y + ( i * 4.f - 6.f ) * s ) , ImVec2( c.x + ( 7.f - i * 3.f ) * s , c.y + ( i * 4.f - 4.f ) * s ) , color , 1.f * s );
		break;
	case ReferenceIcon::Glyph:
		dl->AddCircle( c , 7.f * s , color , 18 , 1.5f * s );
		dl->AddLine( ImVec2( c.x - 6.f * s , c.y + 5.f * s ) , ImVec2( c.x + 6.f * s , c.y - 5.f * s ) , color , 1.5f * s );
		dl->AddCircleFilled( c , 2.f * s , color );
		break;
	}
}

static bool DrawRailButton( const char* id , ReferenceIcon icon , bool selected )
{
	const ImVec2 pos = ImGui::GetCursorScreenPos();
	const ImVec2 size( 38.f , 36.f );
	ImGui::InvisibleButton( id , size );
	const bool hovered = ImGui::IsItemHovered();
	ImDrawList* drawList = ImGui::GetWindowDrawList();

	if ( selected || hovered )
		drawList->AddRectFilled( pos , ImVec2( pos.x + size.x , pos.y + size.y ) , selected ? IM_COL32( 29 , 34 , 38 , 255 ) : IM_COL32( 25 , 28 , 32 , 220 ) , 5.f );
	if ( selected )
		drawList->AddRectFilled( ImVec2( pos.x - 6.f , pos.y + 9.f ) , ImVec2( pos.x - 3.f , pos.y + 27.f ) , kAccentColor , 2.f );

	DrawReferenceIcon( drawList , ImVec2( pos.x + size.x * 0.5f , pos.y + size.y * 0.5f ) , icon , selected ? kAccentColor : IM_COL32( 181 , 183 , 197 , 255 ) );
	return ImGui::IsItemClicked();
}

static bool DrawIconButton( const char* id , ReferenceIcon icon , const ImVec2& size )
{
	const bool clicked = ImGui::Button( id , size );
	const ImVec2 min = ImGui::GetItemRectMin();
	const ImVec2 max = ImGui::GetItemRectMax();
	DrawReferenceIcon( ImGui::GetWindowDrawList() , ImVec2( ( min.x + max.x ) * 0.5f , ( min.y + max.y ) * 0.5f ) , icon , IM_COL32( 166 , 168 , 178 , 255 ) , 0.72f );
	return clicked;
}

static bool DrawNavigationItem( const char* label , ReferenceIcon icon , bool selected )
{
	const ImVec2 pos = ImGui::GetCursorScreenPos();
	const ImVec2 size( ImGui::GetContentRegionAvail().x , 32.f );
	ImGui::InvisibleButton( label , size );
	const bool hovered = ImGui::IsItemHovered();
	ImDrawList* drawList = ImGui::GetWindowDrawList();

	if ( selected || hovered )
		drawList->AddRectFilled( pos , ImVec2( pos.x + size.x , pos.y + size.y ) , selected ? IM_COL32( 25 , 31 , 34 , 255 ) : IM_COL32( 22 , 25 , 28 , 220 ) , 5.f );
	if ( selected )
		drawList->AddRectFilled( ImVec2( pos.x - 7.f , pos.y + 6.f ) , ImVec2( pos.x - 4.f , pos.y + 26.f ) , kAccentColor , 2.f );

	const ImU32 iconColor = selected ? kAccentColor : IM_COL32( 154 , 156 , 170 , 255 );
	const ImU32 textColor = selected ? kAccentTextColor : IM_COL32( 166 , 168 , 181 , 255 );
	DrawReferenceIcon( drawList , ImVec2( pos.x + 16.f , pos.y + 16.f ) , icon , iconColor , 0.78f );

	// Keep every label inside the navigation column. Long labels scroll from side
	// to side while hovered so their hidden portion remains readable.
	const float textStartX = pos.x + 35.f;
	const float textEndX = pos.x + size.x - 5.f;
	const float availableTextWidth = (std::max)( 0.f , textEndX - textStartX );
	const float textWidth = ImGui::CalcTextSize( label ).x;
	const float overflow = (std::max)( 0.f , textWidth - availableTextWidth );
	float scrollOffset = 0.f;

	ImGuiStorage* storage = ImGui::GetStateStorage();
	const ImGuiID hoverStartKey = ImGui::GetID( "##navigationMarqueeStart" );
	if ( hovered && overflow > 0.f )
	{
		float hoverStart = storage->GetFloat( hoverStartKey , -1.f );
		if ( hoverStart < 0.f )
		{
			hoverStart = static_cast<float>( ImGui::GetTime() );
			storage->SetFloat( hoverStartKey , hoverStart );
		}

		const float pauseDuration = 0.45f;
		const float travelDuration = overflow / 38.f;
		const float cycleDuration = pauseDuration * 2.f + travelDuration * 2.f;
		const float phase = fmodf( static_cast<float>( ImGui::GetTime() ) - hoverStart , cycleDuration );
		if ( phase > pauseDuration && phase <= pauseDuration + travelDuration )
			scrollOffset = overflow * ( phase - pauseDuration ) / travelDuration;
		else if ( phase > pauseDuration + travelDuration && phase <= pauseDuration * 2.f + travelDuration )
			scrollOffset = overflow;
		else if ( phase > pauseDuration * 2.f + travelDuration )
			scrollOffset = overflow * ( 1.f - ( phase - pauseDuration * 2.f - travelDuration ) / travelDuration );
	}
	else
	{
		storage->SetFloat( hoverStartKey , -1.f );
	}

	drawList->PushClipRect( ImVec2( textStartX , pos.y ) , ImVec2( textEndX , pos.y + size.y ) , true );
	drawList->AddText( ImVec2( textStartX - scrollOffset , pos.y + 7.f ) , textColor , label );
	drawList->PopClipRect();
	return ImGui::IsItemClicked();
}

struct ReferenceNavigationItem
{
	const char* label;
	ReferenceIcon icon;
	const char* section;
};

struct ReferenceNavigationCategory
{
	const char* title;
	ReferenceIcon railIcon;
	const ReferenceNavigationItem* items;
	int itemCount;
};

static const ReferenceNavigationItem g_GeneralNavigation[] =
{
	{ "Auto Control Renewal", ReferenceIcon::Sparkles, nullptr },
	{ "Auto Disabler", ReferenceIcon::Bell, nullptr },
	{ "Builds Helper", ReferenceIcon::Info, nullptr },
	{ "Dodger", ReferenceIcon::Hidden, nullptr },
	{ "FailSwitch", ReferenceIcon::Offscreen, nullptr },
	{ "Heal and Restore", ReferenceIcon::Info, nullptr },
	{ "Illusion Shuffle", ReferenceIcon::Smooth, nullptr },
	{ "Items Manager", ReferenceIcon::Tools, nullptr },
	{ "Kill Stealer", ReferenceIcon::Aggro, nullptr },
	{ "Overwolf", ReferenceIcon::Visible, nullptr },
	{ "Procast Damage", ReferenceIcon::Sparkles, nullptr },
	{ "Shop Manager", ReferenceIcon::Overlay, nullptr },
	{ "Snatcher", ReferenceIcon::Mouse, nullptr },
};

static const ReferenceNavigationItem g_HeroesNavigation[] =
{
	{ "Settings", ReferenceIcon::Gear, nullptr },
	{ "Abaddon", ReferenceIcon::Heroes, "Hero List" },
	{ "Alchemist", ReferenceIcon::Heroes, nullptr },
	{ "Ancient Apparition", ReferenceIcon::Heroes, nullptr },
	{ "Anti Mage", ReferenceIcon::Heroes, nullptr },
	{ "Arc Warden", ReferenceIcon::Heroes, nullptr },
	{ "Axe", ReferenceIcon::Heroes, nullptr },
	{ "Bane", ReferenceIcon::Heroes, nullptr },
	{ "Batrider", ReferenceIcon::Heroes, nullptr },
	{ "Beastmaster", ReferenceIcon::Heroes, nullptr },
	{ "Bloodseeker", ReferenceIcon::Heroes, nullptr },
	{ "Bounty Hunter", ReferenceIcon::Heroes, nullptr },
	{ "Brewmaster", ReferenceIcon::Heroes, nullptr },
};

static const ReferenceNavigationItem g_InfoNavigation[] =
{
	{ "Aggro Drawer", ReferenceIcon::Aggro, nullptr },
	{ "Camera", ReferenceIcon::Camera, nullptr },
	{ "Heroes Overlay", ReferenceIcon::Heroes, nullptr },
	{ "Info Overlay", ReferenceIcon::Overlay, nullptr },
	{ "Notifications", ReferenceIcon::Bell, nullptr },
	{ "Offscreen", ReferenceIcon::Offscreen, nullptr },
	{ "Radius", ReferenceIcon::Radius, nullptr },
	{ "Show Me More", ReferenceIcon::Hidden, nullptr },
	{ "Visible Settings", ReferenceIcon::Visible, nullptr },
	{ "Ward Helper", ReferenceIcon::Ward, nullptr },
};

static const ReferenceNavigationItem g_CreepsNavigation[] =
{
	{ "Aggro Deaggro", ReferenceIcon::Aggro, nullptr },
	{ "Auto Stack", ReferenceIcon::Overlay, nullptr },
	{ "Camps Bounty", ReferenceIcon::Info, nullptr },
	{ "Creep Blocker", ReferenceIcon::Heroes, nullptr },
	{ "Creep Waves", ReferenceIcon::Smooth, nullptr },
	{ "Illusion Control", ReferenceIcon::Heroes, nullptr },
	{ "Jungle Bot", ReferenceIcon::Sparkles, nullptr },
	{ "[v2] Last Hit Helper", ReferenceIcon::Info, nullptr },
};

static const ReferenceNavigationItem g_ChangerNavigation[] =
{
	{ "Better UI", ReferenceIcon::Overlay, nullptr },
	{ "Changer", ReferenceIcon::Mouse, nullptr },
	{ "Color Changer", ReferenceIcon::Info, nullptr },
	{ "Cursor Trail", ReferenceIcon::Mouse, nullptr },
	{ "Profile Changer", ReferenceIcon::Heroes, nullptr },
	{ "Runes Changer", ReferenceIcon::Sparkles, nullptr },
	{ "Sounds", ReferenceIcon::Bell, nullptr },
	{ "Tree Changer", ReferenceIcon::Sparkles, nullptr },
	{ "Unlocker", ReferenceIcon::Gear, nullptr },
	{ "Walk Effects", ReferenceIcon::Mouse, nullptr },
	{ "Watermark", ReferenceIcon::Overlay, nullptr },
	{ "World", ReferenceIcon::Globe, nullptr },
};

static const ReferenceNavigationItem g_MiscNavigation[] =
{
	{ "Abuse Mini Games", ReferenceIcon::Info, "Other" },
	{ "Auto Pick", ReferenceIcon::Sparkles, nullptr },
	{ "Auto Queue", ReferenceIcon::Speed, nullptr },
	{ "MMR Tracker", ReferenceIcon::Overlay, nullptr },
	{ "Match Accept", ReferenceIcon::Info, nullptr },
	{ "Meta Tracker", ReferenceIcon::Info, nullptr },
	{ "MpHp Abuse", ReferenceIcon::Smooth, nullptr },
	{ "Players Notes", ReferenceIcon::Overlay, nullptr },
	{ "Show Roles", ReferenceIcon::Visible, nullptr },
	{ "Social Feed", ReferenceIcon::Heroes, nullptr },
	{ "Anti Illusion", ReferenceIcon::Hidden, "In Game" },
	{ "Armlet Abuse", ReferenceIcon::Aggro, nullptr },
	{ "Auto Glyph", ReferenceIcon::Gear, nullptr },
};

static const ReferenceNavigationItem g_ScriptsNavigation[] =
{
	{ "List", ReferenceIcon::Code, nullptr },
	{ "Security", ReferenceIcon::Hidden, nullptr },
};

static const ReferenceNavigationItem g_CloudNavigation[] =
{
	{ "Configs", ReferenceIcon::Save, nullptr },
};

static const ReferenceNavigationCategory g_NavigationCategories[] =
{
	{ "General", ReferenceIcon::Globe, g_GeneralNavigation, IM_ARRAYSIZE( g_GeneralNavigation ) },
	{ "Heroes", ReferenceIcon::Heroes, g_HeroesNavigation, IM_ARRAYSIZE( g_HeroesNavigation ) },
	{ "Info Screen", ReferenceIcon::Overlay, g_InfoNavigation, IM_ARRAYSIZE( g_InfoNavigation ) },
	{ "Creeps", ReferenceIcon::Aggro, g_CreepsNavigation, IM_ARRAYSIZE( g_CreepsNavigation ) },
	{ "Changer", ReferenceIcon::Sparkles, g_ChangerNavigation, IM_ARRAYSIZE( g_ChangerNavigation ) },
	{ "Miscellaneous", ReferenceIcon::Tools, g_MiscNavigation, IM_ARRAYSIZE( g_MiscNavigation ) },
	{ "Scripts", ReferenceIcon::Code, g_ScriptsNavigation, IM_ARRAYSIZE( g_ScriptsNavigation ) },
	{ "Cloud", ReferenceIcon::Cloud, g_CloudNavigation, IM_ARRAYSIZE( g_CloudNavigation ) },
};

static bool DrawSwitchRow( const char* label , const char* id , bool& value , ReferenceIcon icon )
{
	const ImVec2 rowPos = ImGui::GetCursorScreenPos();
	const float rowWidth = ImGui::GetContentRegionAvail().x;
	const float rowHeight = 38.f;
	ImDrawList* drawList = ImGui::GetWindowDrawList();
	drawList->AddText( ImVec2( rowPos.x + 27.f , rowPos.y + 10.f ) , IM_COL32( 218 , 219 , 224 , 255 ) , label );
	DrawReferenceIcon( drawList , ImVec2( rowPos.x + 9.f , rowPos.y + 18.f ) , icon , kAccentColor , 0.65f );

	const ImVec2 switchPos( rowPos.x + rowWidth - 31.f , rowPos.y + 9.f );
	ImGui::SetCursorScreenPos( switchPos );
	ImGui::InvisibleButton( id , ImVec2( 30.f , 18.f ) );
	const bool clicked = ImGui::IsItemClicked();
	if ( clicked )
		value = !value;

	drawList->AddRectFilled( switchPos , ImVec2( switchPos.x + 30.f , switchPos.y + 18.f ) , value ? kAccentTrackColor : IM_COL32( 48 , 50 , 56 , 255 ) , 9.f );
	const float knobX = value ? switchPos.x + 21.f : switchPos.x + 9.f;
	drawList->AddCircleFilled( ImVec2( knobX , switchPos.y + 9.f ) , 7.5f , value ? kAccentColor : IM_COL32( 150 , 152 , 160 , 255 ) );
	drawList->AddLine( ImVec2( rowPos.x , rowPos.y + rowHeight - 1.f ) , ImVec2( rowPos.x + rowWidth , rowPos.y + rowHeight - 1.f ) , IM_COL32( 31 , 33 , 37 , 255 ) );
	ImGui::SetCursorScreenPos( ImVec2( rowPos.x , rowPos.y + rowHeight ) );
	return clicked;
}

static bool DrawSettingsSwitchRow( const char* label , const char* id , bool& value , ReferenceIcon icon , bool showGear = false )
{
	const ImVec2 rowPos = ImGui::GetCursorScreenPos();
	const float rowWidth = ImGui::GetContentRegionAvail().x;
	const float rowHeight = 38.f;
	ImDrawList* drawList = ImGui::GetWindowDrawList();
	const ImU32 textColor = ImGui::GetColorU32( ImGuiCol_Text );
	const ImU32 iconColor = value ? kAccentColor : ImGui::GetColorU32( ImGuiCol_TextDisabled );

	drawList->AddText( ImVec2( rowPos.x + 27.f , rowPos.y + 10.f ) , textColor , label );
	DrawReferenceIcon( drawList , ImVec2( rowPos.x + 9.f , rowPos.y + 18.f ) , icon , iconColor , 0.65f );

	const ImVec2 switchPos( rowPos.x + rowWidth - 31.f , rowPos.y + 9.f );
	if ( showGear )
	{
		ImGui::SetCursorScreenPos( ImVec2( switchPos.x - 28.f , rowPos.y + 5.f ) );
		ImGui::PushID( id );
		ImGui::InvisibleButton( "##gear" , ImVec2( 22.f , 26.f ) );
		if ( ImGui::IsItemHovered() )
			ImGui::SetTooltip( "%s settings" , label );
		ImGui::PopID();
		DrawReferenceIcon( drawList , ImVec2( switchPos.x - 17.f , rowPos.y + 18.f ) , ReferenceIcon::Gear , ImGui::GetColorU32( ImGuiCol_TextDisabled ) , 0.58f );
	}

	ImGui::SetCursorScreenPos( switchPos );
	ImGui::InvisibleButton( id , ImVec2( 30.f , 18.f ) );
	const bool clicked = ImGui::IsItemClicked();
	if ( clicked )
		value = !value;

	drawList->AddRectFilled( switchPos , ImVec2( switchPos.x + 30.f , switchPos.y + 18.f ) , value ? kAccentTrackColor : IM_COL32( 38 , 40 , 45 , 255 ) , 9.f );
	const float knobX = value ? switchPos.x + 21.f : switchPos.x + 9.f;
	drawList->AddCircleFilled( ImVec2( knobX , switchPos.y + 9.f ) , 7.5f , value ? kAccentColor : IM_COL32( 142 , 144 , 151 , 255 ) );
	drawList->AddLine( ImVec2( rowPos.x , rowPos.y + rowHeight - 1.f ) , ImVec2( rowPos.x + rowWidth , rowPos.y + rowHeight - 1.f ) , IM_COL32( 31 , 33 , 37 , 255 ) );
	ImGui::SetCursorScreenPos( ImVec2( rowPos.x , rowPos.y + rowHeight ) );
	return clicked;
}

static void DrawSettingsComboRow( const char* label , const char* id , int& value , const char* const* items , int itemCount , ReferenceIcon icon , bool showGear = false )
{
	const ImVec2 rowPos = ImGui::GetCursorScreenPos();
	const float rowWidth = ImGui::GetContentRegionAvail().x;
	const float rowHeight = 43.f;
	const float comboWidth = 122.f;
	ImDrawList* drawList = ImGui::GetWindowDrawList();
	drawList->AddText( ImVec2( rowPos.x + 27.f , rowPos.y + 11.f ) , ImGui::GetColorU32( ImGuiCol_Text ) , label );
	DrawReferenceIcon( drawList , ImVec2( rowPos.x + 9.f , rowPos.y + 19.f ) , icon , kAccentColor , 0.65f );

	const float comboX = rowPos.x + rowWidth - comboWidth;
	if ( showGear )
		DrawReferenceIcon( drawList , ImVec2( comboX - 13.f , rowPos.y + 19.f ) , ReferenceIcon::Gear , ImGui::GetColorU32( ImGuiCol_TextDisabled ) , 0.55f );
	ImGui::SetCursorScreenPos( ImVec2( comboX , rowPos.y + 4.f ) );
	ImGui::SetNextItemWidth( comboWidth );
	ImGui::Combo( id , &value , items , itemCount );
	drawList->AddLine( ImVec2( rowPos.x , rowPos.y + rowHeight - 1.f ) , ImVec2( rowPos.x + rowWidth , rowPos.y + rowHeight - 1.f ) , IM_COL32( 31 , 33 , 37 , 255 ) );
	ImGui::SetCursorScreenPos( ImVec2( rowPos.x , rowPos.y + rowHeight ) );
}

static void DrawTopOverlayElementsRow()
{
	static const char* elementNames[] = { "MP/HP Bars", "Ultimates", "Rune Timers", "Creep Stat", "Buyback", "Team NetWorth" };
	const ImVec2 rowPos = ImGui::GetCursorScreenPos();
	const float rowWidth = ImGui::GetContentRegionAvail().x;
	const float rowHeight = 43.f;
	const float comboWidth = 132.f;
	ImDrawList* drawList = ImGui::GetWindowDrawList();
	drawList->AddText( ImVec2( rowPos.x + 27.f , rowPos.y + 11.f ) , ImGui::GetColorU32( ImGuiCol_Text ) , "What to Show" );
	DrawReferenceIcon( drawList , ImVec2( rowPos.x + 9.f , rowPos.y + 19.f ) , ReferenceIcon::Choose , kAccentColor , 0.65f );

	const float comboX = rowPos.x + rowWidth - comboWidth;
	DrawReferenceIcon( drawList , ImVec2( comboX - 13.f , rowPos.y + 19.f ) , ReferenceIcon::Gear , ImGui::GetColorU32( ImGuiCol_TextDisabled ) , 0.55f );
	const char* preview = "None";
	for ( int i = 0; i < IM_ARRAYSIZE( elementNames ); ++i )
	{
		if ( Settings::InfoOverlay::TopOverlayElements[i] )
		{
			preview = elementNames[i];
			break;
		}
	}

	ImGui::SetCursorScreenPos( ImVec2( comboX , rowPos.y + 4.f ) );
	ImGui::SetNextItemWidth( comboWidth );
	if ( ImGui::BeginCombo( "##topOverlayElements" , preview ) )
	{
		for ( int i = 0; i < IM_ARRAYSIZE( elementNames ); ++i )
		{
			bool& selected = Settings::InfoOverlay::TopOverlayElements[i];
			if ( ImGui::Selectable( elementNames[i] , selected , ImGuiSelectableFlags_DontClosePopups ) )
				selected = !selected;
		}
		ImGui::EndCombo();
	}
	drawList->AddLine( ImVec2( rowPos.x , rowPos.y + rowHeight - 1.f ) , ImVec2( rowPos.x + rowWidth , rowPos.y + rowHeight - 1.f ) , IM_COL32( 31 , 33 , 37 , 255 ) );
	ImGui::SetCursorScreenPos( ImVec2( rowPos.x , rowPos.y + rowHeight ) );
}

static bool DrawSliderRow( const char* label , const char* id , float& value , float minValue , float maxValue , const char* valueFormat , ReferenceIcon icon )
{
	const ImVec2 rowPos = ImGui::GetCursorScreenPos();
	const float rowWidth = ImGui::GetContentRegionAvail().x;
	char valueText[32] = {};
	snprintf( valueText , sizeof( valueText ) , valueFormat , value );
	ImDrawList* drawList = ImGui::GetWindowDrawList();
	drawList->AddText( ImVec2( rowPos.x + 27.f , rowPos.y + 7.f ) , IM_COL32( 205 , 207 , 214 , 255 ) , label );
	DrawReferenceIcon( drawList , ImVec2( rowPos.x + 9.f , rowPos.y + 15.f ) , icon , kAccentColor , 0.65f );
	const ImVec2 valueSize = ImGui::CalcTextSize( valueText );
	drawList->AddText( ImVec2( rowPos.x + rowWidth - valueSize.x , rowPos.y + 7.f ) , IM_COL32( 186 , 187 , 193 , 255 ) , valueText );

	// Use an invisible interaction area and render the slider ourselves. Mixing the
	// native ImGui grab with a custom circle creates the double/pill-shaped handle.
	const float handleRadius = 6.f;
	const float trackStartX = rowPos.x + handleRadius;
	const float trackEndX = rowPos.x + rowWidth - handleRadius;
	const float trackWidth = trackEndX - trackStartX;
	const float trackY = rowPos.y + 31.f;
	ImGui::SetCursorScreenPos( ImVec2( rowPos.x , trackY - 10.f ) );
	ImGui::InvisibleButton( id , ImVec2( rowWidth , 20.f ) );
	const bool hovered = ImGui::IsItemHovered();
	bool changed = false;

	if ( ImGui::IsItemActive() )
	{
		const float normalized = std::clamp( ( ImGui::GetIO().MousePos.x - trackStartX ) / trackWidth , 0.f , 1.f );
		const float newValue = minValue + normalized * ( maxValue - minValue );
		changed = newValue != value;
		value = newValue;
	}

	const float fraction = std::clamp( ( value - minValue ) / ( maxValue - minValue ) , 0.f , 1.f );
	const float grabX = trackStartX + trackWidth * fraction;
	drawList->AddRectFilled( ImVec2( trackStartX , trackY - 2.5f ) , ImVec2( trackEndX , trackY + 2.5f ) , IM_COL32( 35 , 37 , 43 , 255 ) , 2.5f );
	if ( grabX > trackStartX )
		drawList->AddRectFilled( ImVec2( trackStartX , trackY - 2.5f ) , ImVec2( grabX , trackY + 2.5f ) , kAccentColor , 2.5f );
	drawList->AddCircleFilled( ImVec2( grabX , trackY ) , handleRadius + 1.f , IM_COL32( 15 , 16 , 18 , 230 ) );
	drawList->AddCircleFilled( ImVec2( grabX , trackY ) , hovered || ImGui::IsItemActive() ? handleRadius : handleRadius - 0.5f , IM_COL32( 238 , 239 , 242 , 255 ) );
	ImGui::SetCursorScreenPos( ImVec2( rowPos.x , rowPos.y + 48.f ) );
	return changed;
}

auto CAndromedaMenu::OnRenderMenu() -> void
{
	float menuAlpha = static_cast<float>( Settings::Menu::MenuAlpha ) / 255.f;
	menuAlpha = (std::max)( menuAlpha , Settings::Menu::MenuAlphaMin );
	const ImVec2 display = ImGui::GetIO().DisplaySize;
	ImGui::SetNextWindowPos( ImVec2( display.x * 0.5f , display.y * 0.5f ) , ImGuiCond_FirstUseEver , ImVec2( 0.5f , 0.5f ) );
	ImGui::SetNextWindowSize( ImVec2( 840.f , 560.f ) , ImGuiCond_Always );
	ImGui::PushStyleVar( ImGuiStyleVar_Alpha , menuAlpha );
	ImGui::PushStyleVar( ImGuiStyleVar_WindowPadding , ImVec2( 0.f , 0.f ) );
	ImGui::PushStyleColor( ImGuiCol_WindowBg , IM_COL32( 8 , 9 , 10 , 250 ) );
	const ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;

	if ( ImGui::Begin( "##AndromedaReferenceMenu" , nullptr , flags ) )
	{
		static int selectedCategory = 2;
		static int selectedItems[IM_ARRAYSIZE( g_NavigationCategories )] = { 0, 0, 3 };

		ImGui::PushStyleColor( ImGuiCol_ChildBg , IM_COL32( 13 , 14 , 16 , 247 ) );
		ImGui::BeginChild( "##iconRail" , ImVec2( 50.f , 0.f ) , false , ImGuiWindowFlags_NoScrollbar );
		DrawBrandMark( ImGui::GetWindowDrawList() , ImVec2( ImGui::GetWindowPos().x + 25.f , ImGui::GetWindowPos().y + 25.f ) );
		ImGui::SetCursorPosY( 58.f );
		for ( int i = 0; i < IM_ARRAYSIZE( g_NavigationCategories ); ++i )
		{
			ImGui::SetCursorPosX( 6.f );
			ImGui::PushID( i );
			if ( DrawRailButton( "##railCategory" , g_NavigationCategories[i].railIcon , i == selectedCategory ) )
				selectedCategory = i;
			ImGui::PopID();
			ImGui::SetCursorPosY( ImGui::GetCursorPosY() + 4.f );
		}
		ImGui::SetCursorPos( ImVec2( 6.f , ImGui::GetWindowHeight() - 43.f ) );
		DrawRailButton( "##settingsRail" , ReferenceIcon::Gear , false );
		ImGui::EndChild();
		ImGui::PopStyleColor();

		const ReferenceNavigationCategory& category = g_NavigationCategories[selectedCategory];
		int& selectedItem = selectedItems[selectedCategory];
		const ReferenceNavigationItem& page = category.items[selectedItem];

		ImGui::SameLine( 0.f , 0.f );
		ImGui::PushStyleColor( ImGuiCol_ChildBg , IM_COL32( 15 , 16 , 18 , 244 ) );
		ImGui::PushStyleVar( ImGuiStyleVar_WindowPadding , ImVec2( 12.f , 12.f ) );
		ImGui::BeginChild( "##navigation" , ImVec2( 164.f , 0.f ) , true , ImGuiWindowFlags_NoScrollbar );
		ImGui::SetCursorPosY( 21.f );
		ImGui::SetCursorPosX( 16.f );
		ImGui::TextColored( ImVec4( 0.66f , 0.67f , 0.72f , 1.f ) , "%s" , category.title );
		ImGui::SetCursorPosY( 63.f );
		for ( int i = 0; i < category.itemCount; ++i )
		{
			const ReferenceNavigationItem& item = category.items[i];
			if ( item.section )
			{
				ImGui::SetCursorPosX( 5.f );
				ImGui::TextColored( ImVec4( 0.56f , 0.57f , 0.62f , 1.f ) , "%s" , item.section );
				ImGui::SetCursorPosY( ImGui::GetCursorPosY() + 3.f );
			}
			ImGui::PushID( i );
			if ( DrawNavigationItem( item.label , item.icon , selectedItem == i ) )
				selectedItem = i;
			ImGui::PopID();
			ImGui::SetCursorPosY( ImGui::GetCursorPosY() + 1.f );
		}
		ImGui::EndChild();
		ImGui::PopStyleVar();
		ImGui::PopStyleColor();

		ImGui::SameLine( 0.f , 0.f );
		ImGui::PushStyleColor( ImGuiCol_ChildBg , IM_COL32( 9 , 10 , 11 , 242 ) );
		const float mainContentMargin = 20.f;
		ImGui::PushStyleVar( ImGuiStyleVar_WindowPadding , ImVec2( 0.f , 0.f ) );
		ImGui::BeginChild( "##mainContent" , ImVec2( 0.f , 0.f ) , false , ImGuiWindowFlags_NoScrollbar );
		ImGui::SetCursorPos( ImVec2( mainContentMargin , 20.f ) );
		ImGui::TextDisabled( "Main  /" );
		ImGui::SameLine();
		ImGui::TextColored( ImVec4( 0.92f , 0.30f , 0.34f , 1.f ) , "%s" , page.label );

		static char search[64] = {};
		const float headerControlsWidth = 28.f + 4.f + 28.f + 7.f + 190.f;
		const float rightStart = ImGui::GetWindowWidth() - mainContentMargin - headerControlsWidth;
		ImGui::SameLine( rightStart );
		ImGui::PushStyleVar( ImGuiStyleVar_FramePadding , ImVec2( 6.f , 4.f ) );
		DrawIconButton( "##save" , ReferenceIcon::Save , ImVec2( 28.f , 27.f ) );
		ImGui::SameLine( 0.f , 4.f );
		DrawIconButton( "##cloud" , ReferenceIcon::Cloud , ImVec2( 28.f , 27.f ) );
		ImGui::SameLine( 0.f , 7.f );
		ImGui::SetNextItemWidth( 190.f );
		ImGui::InputTextWithHint( "##referenceSearch" , "Search" , search , IM_ARRAYSIZE( search ) );
		ImGui::PopStyleVar();

		ImGui::SetCursorPos( ImVec2( mainContentMargin , 67.f ) );
		ImGui::PushStyleColor( ImGuiCol_ChildBg , IM_COL32( 16 , 17 , 19 , 250 ) );
		ImGui::PushStyleColor( ImGuiCol_Border , IM_COL32( 31 , 32 , 35 , 255 ) );
		ImGui::PushStyleVar( ImGuiStyleVar_WindowPadding , ImVec2( 12.f , 9.f ) );
		ImGui::PushStyleVar( ImGuiStyleVar_ChildRounding , 5.f );
		const bool killStealerPage = selectedCategory == 0 && selectedItem == 8;
		const bool lastHitPage = selectedCategory == 3 && selectedItem == 7;
		const bool cameraPage = selectedCategory == 2 && selectedItem == 1;
		const bool infoOverlayPage = selectedCategory == 2 && selectedItem == 3;
		const bool visibleByEnemyPage = selectedCategory == 2 && selectedItem == 8;
		const float settingsCardWidth = (std::max)( 1.f , ImGui::GetWindowWidth() - mainContentMargin * 2.f );

		if ( infoOverlayPage )
		{
			const float cardGap = 12.f;
			const float columnWidth = ( settingsCardWidth - cardGap ) * 0.5f;
			const float topCardHeight = 185.f;

			ImGui::BeginChild( "##topOverlayCard" , ImVec2( columnWidth , topCardHeight ) , true , ImGuiWindowFlags_NoScrollbar );
			ImGui::TextColored( ImVec4( 0.58f , 0.59f , 0.62f , 1.f ) , "Top Overlay Settings" );
			ImGui::SetCursorPosY( ImGui::GetCursorPosY() + 2.f );
			DrawSettingsSwitchRow( "Enable" , "##topOverlayEnable" , Settings::InfoOverlay::TopOverlayEnabled , ReferenceIcon::Check );
			ImGui::BeginDisabled( !Settings::InfoOverlay::TopOverlayEnabled );
			DrawSettingsSwitchRow( "Show On Allies" , "##showOnAllies" , Settings::InfoOverlay::ShowOnAllies , ReferenceIcon::Allies );
			DrawSettingsSwitchRow( "Show Dangerous Ability Timer" , "##dangerousAbilityTimer" , Settings::InfoOverlay::ShowDangerousAbilityTimer , ReferenceIcon::Warning );
			DrawTopOverlayElementsRow();
			ImGui::EndDisabled();
			ImGui::EndChild();

			ImGui::SameLine( 0.f , cardGap );
			ImGui::BeginChild( "##sidePanelsCard" , ImVec2( columnWidth , topCardHeight ) , true , ImGuiWindowFlags_NoScrollbar );
			ImGui::TextColored( ImVec4( 0.58f , 0.59f , 0.62f , 1.f ) , "Side Panels Settings" );
			ImGui::SetCursorPosY( ImGui::GetCursorPosY() + 2.f );
			DrawSettingsSwitchRow( "Enable" , "##sidePanelsEnable" , Settings::InfoOverlay::SidePanelsEnabled , ReferenceIcon::Check );
			ImGui::BeginDisabled( !Settings::InfoOverlay::SidePanelsEnabled );
			DrawSettingsSwitchRow( "Items" , "##sidePanelItems" , Settings::InfoOverlay::ShowItems , ReferenceIcon::Items , true );
			DrawSettingsSwitchRow( "Networth" , "##sidePanelNetworth" , Settings::InfoOverlay::ShowNetworth , ReferenceIcon::Network , true );
			DrawSettingsSwitchRow( "Scan Glyph Info" , "##scanGlyphInfo" , Settings::InfoOverlay::ScanGlyphInfo , ReferenceIcon::Glyph , true );
			ImGui::EndDisabled();
			ImGui::EndChild();

			ImGui::SetCursorPos( ImVec2( mainContentMargin , 67.f + topCardHeight + cardGap ) );
			ImGui::BeginChild( "##wardTrackerCard" , ImVec2( settingsCardWidth , 160.f ) , true , ImGuiWindowFlags_NoScrollbar );
			ImGui::TextColored( ImVec4( 0.58f , 0.59f , 0.62f , 1.f ) , "Ward Tracker Settings" );
			ImGui::SetCursorPosY( ImGui::GetCursorPosY() + 2.f );
			DrawSettingsSwitchRow( "Enable" , "##wardTrackerEnable" , Settings::InfoOverlay::WardTrackerEnabled , ReferenceIcon::Check , true );
			ImGui::BeginDisabled( !Settings::InfoOverlay::WardTrackerEnabled );
			static const char* wardShowModes[] = { "Panel" , "On Map" , "Panel, On Map" };
			static const char* wardWorldRenderModes[] = { "Timer" , "Image" , "Timer, Image" };
			DrawSettingsComboRow( "Show" , "##wardShowMode" , Settings::InfoOverlay::WardShowMode , wardShowModes , IM_ARRAYSIZE( wardShowModes ) , ReferenceIcon::Visible );
			DrawSettingsComboRow( "World Render" , "##wardWorldRenderMode" , Settings::InfoOverlay::WardWorldRenderMode , wardWorldRenderModes , IM_ARRAYSIZE( wardWorldRenderModes ) , ReferenceIcon::Radius );
			ImGui::EndDisabled();
			ImGui::EndChild();
		}
		else
		{
			const float settingsCardHeight = killStealerPage ? 420.f : ( lastHitPage ? 260.f : ( cameraPage ? 322.f : 180.f ) );
			ImGui::BeginChild( "##settingsCard" , ImVec2( settingsCardWidth , settingsCardHeight ) , true , ImGuiWindowFlags_NoScrollbar );
			ImGui::TextColored( ImVec4( 0.55f , 0.56f , 0.59f , 1.f ) , "%s Settings" , page.label );
			ImGui::SetCursorPosY( ImGui::GetCursorPosY() + 2.f );

			if ( killStealerPage )
			{
				DrawSwitchRow( "Enable" , "##killStealerEnable" , Settings::KillStealer::Enable , ReferenceIcon::Aggro );
				ImGui::BeginDisabled( !Settings::KillStealer::Enable );
				DrawSwitchRow( "Use Abilities" , "##killStealerAbilities" , Settings::KillStealer::UseAbilities , ReferenceIcon::Sparkles );
				DrawSwitchRow( "Use Items" , "##killStealerItems" , Settings::KillStealer::UseItems , ReferenceIcon::Items );
				DrawSwitchRow( "Use Auto Attack" , "##killStealerAttack" , Settings::KillStealer::UseAutoAttack , ReferenceIcon::Mouse );
				DrawSwitchRow( "Quick Cast Mode" , "##killStealerQuickCast" , Settings::KillStealer::QuickCast , ReferenceIcon::Speed );
				DrawSwitchRow( "Draw Killable Markers" , "##killStealerMarkers" , Settings::KillStealer::DrawKillableMarkers , ReferenceIcon::Visible );
				DrawSwitchRow( "Debug Logs" , "##killStealerDebug" , Settings::KillStealer::DrawDebugInfo , ReferenceIcon::Code );
				DrawSliderRow( "Health Buffer" , "##killStealerHealthBuffer" , Settings::KillStealer::HealthBuffer , 0.f , 250.f , "%.0f hp" , ReferenceIcon::Warning );
				ImGui::EndDisabled();
				ImGui::Spacing();
				ImGui::TextDisabled( "Debug mode writes [kill-stealer] target and damage lines to debug.log." );
				ImGui::TextDisabled( "Ability hotkeys assume default Q/W/E/D/F/R and Dota quick-cast settings." );
			}
			else if ( lastHitPage )
			{
				DrawSwitchRow( "Enable" , "##lastHitEnable" , Settings::LastHitAssistant::Enable , ReferenceIcon::Info );
				ImGui::BeginDisabled( !Settings::LastHitAssistant::Enable );
				DrawSliderRow( "Health Buffer" , "##lastHitHealthBuffer" , Settings::LastHitAssistant::HealthBuffer , 0.f , 250.f , "%.0f hp" , ReferenceIcon::Warning );
				DrawSliderRow( "Detect Range" , "##lastHitDetectRange" , Settings::LastHitAssistant::DetectRange , 150.f , 1600.f , "%.0f" , ReferenceIcon::Radius );
				ImGui::EndDisabled();
				ImGui::Spacing();
				ImGui::TextDisabled( "When enabled, Last Hit Helper is always active." );
				ImGui::TextDisabled( "The circle shows enemy creep scan range around your hero." );
				ImGui::TextDisabled( "Gold stars mark enemy creeps killable by one attack. Visual-only, no input is sent." );
			}
			else if ( cameraPage )
			{
				DrawSwitchRow( "Enable" , "##cameraEnable" , Settings::Camera::Enable , ReferenceIcon::Camera );
				ImGui::BeginDisabled( !Settings::Camera::Enable );
				if ( DrawSliderRow( "Camera Distance" , "##cameraDistanceReference" , Settings::Camera::Distance , 1200.f , 10000.f , "%.0f" , ReferenceIcon::Distance ) )
					GetAndromedaClient()->SetCameraDistance( Settings::Camera::Distance );
				DrawSwitchRow( "Smooth Zoom" , "##smoothZoom" , Settings::Camera::SmoothZoom , ReferenceIcon::Smooth );
				DrawSliderRow( "Smoothness Duration" , "##smoothDuration" , Settings::Camera::SmoothnessDuration , 0.1f , 3.0f , "%.2f sec" , ReferenceIcon::Duration );

				const ImVec2 comboRow = ImGui::GetCursorScreenPos();
				const float comboWidth = ImGui::GetContentRegionAvail().x;
				ImDrawList* drawList = ImGui::GetWindowDrawList();
				drawList->AddText( ImVec2( comboRow.x + 27.f , comboRow.y + 9.f ) , IM_COL32( 205 , 207 , 214 , 255 ) , "Zoom using Wheel" );
				DrawReferenceIcon( drawList , ImVec2( comboRow.x + 9.f , comboRow.y + 17.f ) , ReferenceIcon::Mouse , kAccentColor , 0.65f );
				ImGui::SetCursorScreenPos( ImVec2( comboRow.x + comboWidth - 120.f , comboRow.y + 3.f ) );
				const char* wheelModes[] = { "Wheel", "Disabled" };
				ImGui::SetNextItemWidth( 120.f );
				ImGui::Combo( "##wheelMode" , &Settings::Camera::ZoomUsingWheel , wheelModes , IM_ARRAYSIZE( wheelModes ) );
				drawList->AddLine( ImVec2( comboRow.x , comboRow.y + 38.f ) , ImVec2( comboRow.x + comboWidth , comboRow.y + 38.f ) , IM_COL32( 31 , 33 , 37 , 255 ) );
				ImGui::SetCursorScreenPos( ImVec2( comboRow.x , comboRow.y + 39.f ) );
				DrawSliderRow( "Zoom Speed" , "##zoomSpeed" , Settings::Camera::ZoomSpeed , 1.f , 100.f , "%.0f" , ReferenceIcon::Speed );
				ImGui::EndDisabled();
			}
			else if ( visibleByEnemyPage )
			{
				DrawSwitchRow( "Visible By Enemy" , "##visibleByEnemyEnable" , Settings::VisibleByEnemy::Enable , ReferenceIcon::Visible );
				ImGui::Spacing();
				ImGui::TextDisabled( "Shows an eye next to your health bar when True Sight detects you." );
				ImGui::TextDisabled( "Triggers from enemy Gem of True Sight, Sentry Wards, or Towers." );
			}
			else
			{
				ImGui::Spacing();
				ImGui::TextColored( ImVec4( 0.76f , 0.77f , 0.80f , 1.f ) , "%s" , page.label );
				ImGui::TextDisabled( "This module has no configurable options yet." );
			}
			ImGui::EndChild();
		}
		ImGui::PopStyleVar( 2 );
		ImGui::PopStyleColor( 2 );
		ImGui::EndChild();
		ImGui::PopStyleVar();
		ImGui::PopStyleColor();
	}
	ImGui::End();
	ImGui::PopStyleColor();
	ImGui::PopStyleVar( 2 );
}

auto CAndromedaMenu::OnRenderLegacyMenu() -> void
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

				if ( RenderSliderFloat( XorStr( "Distance" ) , XorStr( "##Camera.Distance" ) , Settings::Camera::Distance , 1200.f , 10000.f ) )
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

