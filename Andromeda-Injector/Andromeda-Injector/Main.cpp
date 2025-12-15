#include "Main.h"

#include "CInjector.h"
#include <filesystem>
#include <string>
#include <vector>
#include <fstream>
#include <regex>
#include <winhttp.h>
#include <winreg.h>

#pragma comment(lib, "winhttp.lib")

auto __forceinline PrintMessage( const char* fmt , ... ) -> void
{
	char buff[4096] = { 0 };

	va_list args;
	va_start( args , fmt );
	vsnprintf( buff , sizeof( buff ) - 1 , fmt , args );
	va_end( args );

	printf( "%s" , buff );
};

static bool DownloadFileWinHttp( const std::wstring& url , const std::wstring& outPath )
{
	URL_COMPONENTSW components = {};
	wchar_t hostName[256] = {};
	wchar_t urlPath[2048] = {};

	components.dwStructSize = sizeof( components );
	components.lpszHostName = hostName;
	components.dwHostNameLength = _countof( hostName );
	components.lpszUrlPath = urlPath;
	components.dwUrlPathLength = _countof( urlPath );

	if ( !WinHttpCrackUrl( url.c_str() , 0 , 0 , &components ) )
		return false;

	const bool isHttps = components.nScheme == INTERNET_SCHEME_HTTPS;
	const INTERNET_PORT port = components.nPort ? components.nPort : ( isHttps ? INTERNET_DEFAULT_HTTPS_PORT : INTERNET_DEFAULT_HTTP_PORT );

	HINTERNET hSession = WinHttpOpen( L"AndromedaInjectorDownloader/1.0" , WINHTTP_ACCESS_TYPE_DEFAULT_PROXY , WINHTTP_NO_PROXY_NAME , WINHTTP_NO_PROXY_BYPASS , 0 );
	if ( !hSession ) return false;

	HINTERNET hConnect = WinHttpConnect( hSession , hostName , port , 0 );
	if ( !hConnect ) { WinHttpCloseHandle( hSession ); return false; }

	DWORD flags = isHttps ? WINHTTP_FLAG_SECURE : 0;
	HINTERNET hRequest = WinHttpOpenRequest( hConnect , L"GET" , urlPath , nullptr , WINHTTP_NO_REFERER , WINHTTP_DEFAULT_ACCEPT_TYPES , flags );
	if ( !hRequest )
	{
		WinHttpCloseHandle( hConnect );
		WinHttpCloseHandle( hSession );
		return false;
	}

	bool ok = WinHttpSendRequest( hRequest , WINHTTP_NO_ADDITIONAL_HEADERS , 0 , WINHTTP_NO_REQUEST_DATA , 0 , 0 , 0 );
	if ( ok ) ok = WinHttpReceiveResponse( hRequest , nullptr );

	std::vector<uint8_t> buffer;
	if ( ok )
	{
		DWORD dwSize = 0;
		do
		{
			if ( !WinHttpQueryDataAvailable( hRequest , &dwSize ) || dwSize == 0 )
				break;

			const size_t offset = buffer.size();
			buffer.resize( offset + dwSize );

			DWORD dwDownloaded = 0;
			if ( !WinHttpReadData( hRequest , buffer.data() + offset , dwSize , &dwDownloaded ) )
			{
				ok = false;
				break;
			}
			buffer.resize( offset + dwDownloaded );
		} while ( dwSize > 0 );
	}

	WinHttpCloseHandle( hRequest );
	WinHttpCloseHandle( hConnect );
	WinHttpCloseHandle( hSession );

	if ( !ok || buffer.empty() )
		return false;

	std::filesystem::create_directories( std::filesystem::path( outPath ).parent_path() );
	std::ofstream out( outPath , std::ios::binary | std::ios::trunc );
	if ( !out.is_open() )
		return false;

	out.write( reinterpret_cast<const char*>( buffer.data() ) , static_cast<std::streamsize>( buffer.size() ) );
	return out.good();
}

static std::wstring UnescapeBackslashes( const std::string& in )
{
	std::wstring out;
	out.reserve( in.size() );
	for ( size_t i = 0; i < in.size(); ++i )
	{
		if ( in[i] == '\\' && i + 1 < in.size() && in[i + 1] == '\\' )
		{
			out.push_back( L'\\' );
			++i;
		}
		else
		{
			out.push_back( static_cast<wchar_t>( in[i] ) );
		}
	}
	return out;
}

static void CollectDotaCandidatesFromLibrary( const std::wstring& steamRoot , std::vector<std::wstring>& out )
{
	const std::wstring libFile = steamRoot + L"steamapps\\libraryfolders.vdf";
	if ( !std::filesystem::exists( libFile ) )
		return;

	std::ifstream in( libFile );
	if ( !in.is_open() )
		return;

	std::string line;
	std::regex pathRegex( R"(\"path\"\s*\"([^\"]+)\")" );
	while ( std::getline( in , line ) )
	{
		std::smatch m;
		if ( std::regex_search( line , m , pathRegex ) && m.size() > 1 )
		{
			const std::wstring libPath = UnescapeBackslashes( m[1].str() );
			if ( !libPath.empty() )
			{
				std::wstring dotaPath = libPath;
				if ( dotaPath.back() != L'\\' && dotaPath.back() != L'/' )
					dotaPath.push_back( L'\\' );
				dotaPath += L"steamapps\\common\\dota 2 beta\\";
				out.push_back( dotaPath );
			}
		}
	}
}

static std::wstring FindDotaBaseDir()
{
	std::vector<std::wstring> candidates;

	// SteamPath из реестра + все библиотеки
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
			const std::wstring steamRoot( base.begin() , base.end() );
			candidates.push_back( steamRoot + L"steamapps\\common\\dota 2 beta\\" );
			CollectDotaCandidatesFromLibrary( steamRoot , candidates );
		}
		RegCloseKey( hKey );
	}

	// стандартные пути
	const wchar_t* defaults[] = {
		L"C:\\Program Files (x86)\\Steam\\steamapps\\common\\dota 2 beta\\",
		L"C:\\Program Files\\Steam\\steamapps\\common\\dota 2 beta\\",
	};

	for ( auto path : defaults )
	{
		candidates.push_back( path );
	}

	for ( const auto& cand : candidates )
	{
		if ( std::filesystem::exists( cand ) )
			return cand;
	}

	return L"";
}

static bool CopyIfExists( const std::wstring& src , const std::wstring& dst )
{
	if ( !std::filesystem::exists( src ) )
		return false;
	std::filesystem::create_directories( std::filesystem::path( dst ).parent_path() );
	try
	{
		std::filesystem::copy_file( src , dst , std::filesystem::copy_options::overwrite_existing );
		return true;
	}
	catch ( ... )
	{
		return false;
	}
}

static void PreDownloadAssets()
{
	const std::string baseDirA = GetInjector()->szCurrentDir;
	const std::wstring baseDir = std::wstring( baseDirA.begin() , baseDirA.end() );

	// JSON heroes
	const std::wstring heroJsonUrl = L"https://raw.githubusercontent.com/odota/dotaconstants/master/build/npc_heroes.json";
	const std::wstring heroJsonPath = baseDir + L"Assets\\data\\npc_heroes.json";

	bool heroOk = std::filesystem::exists( heroJsonPath );
	if ( !heroOk )
	{
		DEV_LOG( "[info] downloading npc_heroes.json...\n" );
		heroOk = DownloadFileWinHttp( heroJsonUrl , heroJsonPath );
		DEV_LOG( heroOk ? "[info] npc_heroes.json downloaded\n" : "[error] failed download npc_heroes.json\n" );
	}
	else
	{
		DEV_LOG( "[info] npc_heroes.json already exists\n" );
	}

	// тащим иконки из установленных библиотек Dota
	const std::wstring dotaBase = FindDotaBaseDir();
	if ( !dotaBase.empty() )
	{
		DEV_LOG( "[info] found dota base: %ws\n" , dotaBase.c_str() );
		const std::wstring heroSrc = dotaBase + L"game\\dota\\panorama\\images\\heroes\\npc_dota_hero_meepo.png";
		const std::wstring spellSrc[4] = {
			dotaBase + L"game\\dota\\panorama\\images\\spellicons\\meepo_earthbind.png",
			dotaBase + L"game\\dota\\panorama\\images\\spellicons\\meepo_poof.png",
			dotaBase + L"game\\dota\\panorama\\images\\spellicons\\meepo_geostrike.png",
			dotaBase + L"game\\dota\\panorama\\images\\spellicons\\meepo_divided_we_stand.png",
		};

		const std::wstring heroDst = baseDir + L"Assets\\Icons\\Heroes\\npc_dota_hero_meepo.png";
		bool heroCopied = false;
		if ( CopyIfExists( heroSrc , heroDst ) )
		{
			DEV_LOG( "[info] copied hero icon: %ws\n" , heroDst.c_str() );
			heroCopied = true;
		}
		else
		{
			DEV_LOG( "[warn] hero icon not copied (source missing): %ws\n" , heroSrc.c_str() );
			const std::wstring heroCdn = L"https://cdn.cloudflare.steamstatic.com/apps/dota2/images/heroes/meepo_full.png";
			if ( DownloadFileWinHttp( heroCdn , heroDst ) )
			{
				DEV_LOG( "[info] downloaded hero icon from CDN: %ws\n" , heroDst.c_str() );
				heroCopied = true;
			}
			else
			{
				DEV_LOG( "[error] failed to download hero icon from CDN\n" );
			}
		}

		const std::wstring spellDstBase = baseDir + L"Assets\\Icons\\Spells\\";
		for ( int i = 0; i < 4; ++i )
		{
			std::wstring dst = spellDstBase + spellSrc[i].substr( spellSrc[i].find_last_of( L'\\' ) + 1 );
			if ( CopyIfExists( spellSrc[i] , dst ) )
			{
				DEV_LOG( "[info] copied spell icon: %ws\n" , dst.c_str() );
			}
			else
			{
				DEV_LOG( "[warn] spell icon not copied (source missing): %ws\n" , spellSrc[i].c_str() );

				// fallback: download from CDN (medium size icons)
				std::wstring spellName = spellSrc[i].substr( spellSrc[i].find_last_of( L'\\' ) + 1 );
				if ( spellName.size() > 4 && spellName.substr( spellName.size() - 4 ) == L".png" )
					spellName = spellName.substr( 0 , spellName.size() - 4 );
				const std::wstring spellCdn = L"https://cdn.cloudflare.steamstatic.com/apps/dota2/images/abilities/" + spellName + L"_md.png";
				if ( DownloadFileWinHttp( spellCdn , dst ) )
				{
					DEV_LOG( "[info] downloaded spell icon from CDN: %ws\n" , dst.c_str() );
				}
				else
				{
					DEV_LOG( "[error] failed to download spell icon from CDN: %ws\n" , spellName.c_str() );
				}
			}
		}
	}
	else
	{
		DEV_LOG( "[warn] dota base not found, skip copying icons\n" );
	}
}

int main( int argm , char** argv )
{
	PreDownloadAssets();

	if ( !GetInjector()->Init() )
	{
		DEV_LOG( "[error] GetPrivileges Or Dll Not Found !\n" );
		Sleep( 5000 );
		return 0;
	}

	if ( !GetInjector()->InjectManualMap( "dota2.exe" ) )
	{
		DEV_LOG( "[error] Inject #1\n" );
		Sleep( 5000 );
		return 0;
	}

	DEV_LOG( "[success] Injected !\n" );

	Sleep( 5000 );
	return 0;
}
