#pragma once

#include <Common/Common.hpp>

#include <unordered_map>
#include <string>
#include <regex>
#include <fstream>
#include <sstream>
#include <vector>
#include <filesystem>
#include <winhttp.h>

#pragma comment(lib, "winhttp.lib")

// Простая загрузка npc_heroes.json без сторонних JSON библиотек.
// Ищет пары id/localized_name и кеширует в unordered_map.
class CHeroDataLoader
{
public:
	bool LoadFromFile( const std::string& path )
	{
		std::ifstream file( path , std::ios::binary );
		if ( !file.is_open() )
			return false;

		std::ostringstream ss;
		ss << file.rdbuf();
		const std::string content = ss.str();

		if ( content.empty() )
			return false;

		std::unordered_map<int , std::string> parsed;

		// Простейший парсер: ищем "id": <num> ... "localized_name": "<name>"
		const std::regex re( R"regex("id"\s*:\s*(\d+)[^{}]*?"localized_name"\s*:\s*"([^"]+)")regex" );
		for ( std::sregex_iterator it( content.begin() , content.end() , re ) , end; it != end; ++it )
		{
			const int heroId = std::stoi( ( *it )[1].str() );
			const std::string heroName = ( *it )[2].str();
			parsed[heroId] = heroName;
		}

		if ( parsed.empty() )
			return false;

		m_HeroIdToName.swap( parsed );
		m_SourcePath = path;
		return true;
	}

	// Скачивает файл по URL (https/http) в указанный путь. Возвращает true при успехе.
	static inline bool DownloadFileWinHttp( const std::wstring& url , const std::wstring& outPath )
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

		HINTERNET hSession = WinHttpOpen( L"AndromedaHeroDownloader/1.0" , WINHTTP_ACCESS_TYPE_DEFAULT_PROXY , WINHTTP_NO_PROXY_NAME , WINHTTP_NO_PROXY_BYPASS , 0 );
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

				size_t offset = buffer.size();
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

	// Проверяет наличие кеша и при необходимости скачивает. Если загрузили — сразу парсим.
	bool EnsureCacheAndLoad( const std::string& url , const std::string& cachePath , bool forceDownload = false )
	{
		const bool cacheExists = std::filesystem::exists( cachePath );
		bool downloaded = false;

		if ( forceDownload || !cacheExists )
		{
			if ( DownloadFileWinHttp( std::wstring( url.begin() , url.end() ) , std::wstring( cachePath.begin() , cachePath.end() ) ) )
				downloaded = true;
		}

		return LoadFromFile( cachePath ) || downloaded;
	}

	auto GetHeroName( int id ) const -> std::string
	{
		if ( auto it = m_HeroIdToName.find( id ); it != m_HeroIdToName.end() )
			return it->second;
		return {};
	}

	auto GetAll() const -> const std::unordered_map<int , std::string>& { return m_HeroIdToName; }
	auto IsLoaded() const -> bool { return !m_HeroIdToName.empty(); }
	auto GetSourcePath() const -> const std::string& { return m_SourcePath; }

private:
	std::unordered_map<int , std::string> m_HeroIdToName;
	std::string m_SourcePath;
};

auto GetHeroDataLoader() -> CHeroDataLoader*; // см. реализацию в CAndromedaClient.cpp
