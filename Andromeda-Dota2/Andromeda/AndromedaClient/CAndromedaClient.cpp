#include "CAndromedaClient.hpp"
#include "CAndromedaGUI.hpp"

#include <Dota2/SDK/SDK.hpp>
#include <Dota2/SDK/Interface/CGameEntitySystem.hpp>
#include <Dota2/SDK/Types/CEntityData.hpp>
#include <Dota2/SDK/Types/CHandle.hpp>
#include <Dota2/SDK/CSchemaOffset.hpp>
#include <Dota2/SDK/Interface/CShemaSystemSDK.hpp>

#include <wrl/client.h>
#include <wincodec.h>

#include <AndromedaClient/GUI/CAndromedaMenu.hpp>
#include <AndromedaClient/Settings/Settings.hpp>
#include <AndromedaClient/Data/HeroData.hpp>
#include <AndromedaClient/Data/AbilityDamageData.hpp>
#include <AndromedaClient/Scripting/LuaManager.hpp>
#include <DllLauncher.hpp>
#include <Common/Helpers/StringHelper.hpp>
#include <Common/MemoryEngine.hpp>
#include <filesystem>
#include <cstring>
#include <cctype>
#include <cmath>
#include <algorithm>
#include <array>
#include <cfloat>
#include <cstdio>
#include <string>
#include <future>
#include <chrono>
#include <fstream>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#pragma comment(lib, "windowscodecs.lib")

static CAndromedaClient g_CAndromedaClient{};
static CHeroDataLoader g_HeroDataLoader{};

namespace
{
	enum class TrackedEntryKind : uint8_t
	{
		Ability,
		Item
	};

	struct TrackedEntry
	{
		TrackedEntryKind kind = TrackedEntryKind::Ability;
		std::string name;
		float cooldown = 0.f;
		int level = 0;
		int count = 0;
	};

	enum class TrackedIconState : uint8_t
	{
		Queued,
		Downloading,
		Ready,
		Failed
	};

	struct TrackedIcon
	{
		ID3D11ShaderResourceView* srv = nullptr;
		int width = 0;
		int height = 0;
		TrackedIconState state = TrackedIconState::Queued;
		std::wstring path;
		std::wstring url;
		std::future<bool> download;

		~TrackedIcon()
		{
			if ( srv )
				srv->Release();
		}

		TrackedIcon() = default;
		TrackedIcon( const TrackedIcon& ) = delete;
		TrackedIcon& operator=( const TrackedIcon& ) = delete;
	};

	static std::unordered_map<std::string , TrackedIcon> g_TrackedIcons;
	// Keep the reported Viper case correct even before the metadata cache is
	// available; a successful load replaces this fallback with the complete set.
	static std::unordered_set<std::string> g_InnateAbilityNames{ "viper_predator" };
	static std::unordered_set<std::string> g_HiddenAbilityNames;
	static std::unordered_map<std::string , std::unordered_set<std::string>> g_HeroAbilityNames;
	static std::unordered_map<std::string , int> g_ItemCosts;

	auto LoadInnateAbilityNames( const std::string& path ) -> bool
	{
		std::ifstream input( path );
		if ( !input.is_open() )
			return false;

		std::unordered_set<std::string> parsed;
		std::unordered_set<std::string> hidden;
		std::string currentAbility;
		std::string line;
		int objectDepth = 0;
		while ( std::getline( input , line ) )
		{
			if ( objectDepth == 1 )
			{
				const size_t firstQuote = line.find( '"' );
				const size_t secondQuote = firstQuote == std::string::npos ? std::string::npos : line.find( '"' , firstQuote + 1 );
				const size_t openingBrace = secondQuote == std::string::npos ? std::string::npos : line.find( '{' , secondQuote + 1 );
				if ( firstQuote != std::string::npos && secondQuote != std::string::npos && openingBrace != std::string::npos )
					currentAbility = line.substr( firstQuote + 1 , secondQuote - firstQuote - 1 );
			}

			if ( !currentAbility.empty() && line.find( "\"is_innate\": true" ) != std::string::npos )
				parsed.insert( currentAbility );
			if ( !currentAbility.empty() && line.find( "\"Hidden\"" ) != std::string::npos )
				hidden.insert( currentAbility );

			bool inString = false;
			bool escaped = false;
			for ( const char character : line )
			{
				if ( escaped )
				{
					escaped = false;
					continue;
				}
				if ( character == '\\' && inString )
				{
					escaped = true;
					continue;
				}
				if ( character == '"' )
					inString = !inString;
				else if ( !inString && character == '{' )
					++objectDepth;
				else if ( !inString && character == '}' )
					--objectDepth;
			}

			if ( objectDepth <= 1 )
				currentAbility.clear();
		}

		if ( parsed.empty() )
			return false;
		g_InnateAbilityNames.swap( parsed );
		g_HiddenAbilityNames.swap( hidden );
		return true;
	}

	auto LoadHeroAbilityNames( const std::string& path ) -> bool
	{
		std::ifstream input( path );
		if ( !input.is_open() )
			return false;

		std::unordered_map<std::string , std::unordered_set<std::string>> parsed;
		std::string currentHero;
		std::string line;
		bool readingAbilities = false;
		while ( std::getline( input , line ) )
		{
			const size_t heroMarker = line.find( "\"npc_dota_hero_" );
			if ( heroMarker != std::string::npos )
			{
				const size_t endQuote = line.find( '"' , heroMarker + 1 );
				if ( endQuote != std::string::npos )
				{
					currentHero = line.substr( heroMarker + 1 , endQuote - heroMarker - 1 );
					readingAbilities = false;
				}
				continue;
			}

			if ( currentHero.empty() )
				continue;

			// Capture both the primary ability list and facet-granted abilities.
			// Facet skills (crystal_clone, ofrenda, …) appear on the live skill bar.
			if ( !readingAbilities && line.find( "\"abilities\"" ) != std::string::npos && line.find( '[' ) != std::string::npos )
			{
				readingAbilities = true;
				continue;
			}
			if ( !readingAbilities )
				continue;

			if ( line.find( ']' ) != std::string::npos )
			{
				readingAbilities = false;
				continue;
			}
			const size_t firstQuote = line.find( '"' );
			const size_t secondQuote = firstQuote == std::string::npos ? std::string::npos : line.find( '"' , firstQuote + 1 );
			if ( firstQuote != std::string::npos && secondQuote != std::string::npos )
			{
				const std::string ability = line.substr( firstQuote + 1 , secondQuote - firstQuote - 1 );
				if ( ability != "generic_hidden" && ability.rfind( "special_bonus_" , 0 ) != 0 )
					parsed[currentHero].insert( ability );
			}
		}

		if ( parsed.empty() )
			return false;
		g_HeroAbilityNames.swap( parsed );
		return true;
	}

	// Flat map: {"item_blink":2250,"item_tango":90,...}
	auto LoadItemCosts( const std::string& path ) -> bool
	{
		std::ifstream input( path );
		if ( !input.is_open() )
			return false;

		std::unordered_map<std::string , int> parsed;
		std::string line;
		while ( std::getline( input , line ) )
		{
			size_t pos = 0;
			while ( pos < line.size() )
			{
				const size_t keyStart = line.find( '"' , pos );
				if ( keyStart == std::string::npos )
					break;
				const size_t keyEnd = line.find( '"' , keyStart + 1 );
				if ( keyEnd == std::string::npos )
					break;

				const size_t colon = line.find( ':' , keyEnd + 1 );
				if ( colon == std::string::npos )
					break;

				size_t valueStart = colon + 1;
				while ( valueStart < line.size() && ( line[valueStart] == ' ' || line[valueStart] == '\t' ) )
					++valueStart;
				if ( valueStart >= line.size() || !( std::isdigit( static_cast<unsigned char>( line[valueStart] ) ) ||
					line[valueStart] == '-' ) )
				{
					pos = keyEnd + 1;
					continue;
				}

				size_t valueEnd = valueStart;
				if ( line[valueEnd] == '-' )
					++valueEnd;
				while ( valueEnd < line.size() && std::isdigit( static_cast<unsigned char>( line[valueEnd] ) ) )
					++valueEnd;

				const std::string key = line.substr( keyStart + 1 , keyEnd - keyStart - 1 );
				if ( key.rfind( "item_" , 0 ) == 0 )
				{
					try
					{
						parsed[key] = std::stoi( line.substr( valueStart , valueEnd - valueStart ) );
					}
					catch ( ... )
					{
					}
				}
				pos = valueEnd;
			}
		}

		if ( parsed.empty() )
			return false;
		g_ItemCosts.swap( parsed );
		return true;
	}

	auto LookupItemCost( const std::string& name ) -> int
	{
		if ( name.empty() || g_ItemCosts.empty() )
			return 0;

		auto it = g_ItemCosts.find( name );
		if ( it != g_ItemCosts.end() )
			return (std::max)( 0 , it->second );

		if ( name.rfind( "item_" , 0 ) != 0 )
		{
			it = g_ItemCosts.find( "item_" + name );
			if ( it != g_ItemCosts.end() )
				return (std::max)( 0 , it->second );
		}
		return 0;
	}

	struct HeroVitals
	{
		const char* name = "unknown";
		CHandle handle{};
		int playerId = -1;
		int teamSlot = -1;
		uint8_t team = 0;
		int health = 0;
		int maxHealth = 0;
		float mana = 0.f;
		float maxMana = 0.f;
		int netWorth = 0;
		int officialNetWorth = 0;
		int pocketGold = 0;
		std::array<TrackedEntry , 32> trackedEntries{};
		int trackedEntryCount = 0;
		std::array<TrackedEntry , 27> inventoryEntries{};
		std::array<bool , 27> hasInventoryEntry{};
	};

	auto ComputeInventoryNetWorth( const HeroVitals& vitals ) -> int
	{
		int total = 0;
		for ( size_t index = 0; index < vitals.inventoryEntries.size(); ++index )
		{
			if ( !vitals.hasInventoryEntry[index] )
				continue;
			total += LookupItemCost( vitals.inventoryEntries[index].name );
		}
		return total;
	}

	struct HeroVitalsOffsets
	{
		uint32_t health = 0;
		uint32_t maxHealth = 0;
		uint32_t mana = 0;
		uint32_t maxMana = 0;
		uint32_t team = 0;
		uint32_t assignedHero = 0;
		uint32_t playerId = 0;
		uint32_t playerOwnerId = 0;
		uint32_t heroPlayerId = 0;
		uint32_t playerTeamData = 0;
		uint32_t teamSlot = 0;
		uint32_t selectedHero = 0;
		uint32_t dataTeam = 0;
		uint32_t dataTeamNetWorth = 0;
		uint32_t dataTeamReliableGold = 0;
		uint32_t dataTeamUnreliableGold = 0;
		uint32_t spectatorNetWorth = 0;
		uint32_t isIllusion = 0;
		uint32_t isClone = 0;
		uint32_t abilities = 0;
		uint32_t inventory = 0;
		uint32_t inventoryItems = 0;
		uint32_t abilityLevel = 0;
		uint32_t abilityCooldown = 0;
		uint32_t itemCharges = 0;
		uint32_t isLocalController = 0;
		uint32_t nextOutgoingOrder = 0;
		uint32_t cursor = 0;
		bool hasAbilities = false;
		bool hasInventory = false;
		bool hasAbilityLevel = false;
		bool hasAbilityCooldown = false;
		bool hasItemCharges = false;
		bool hasPlayerId = false;
		bool hasPlayerOwnerId = false;
		bool hasHeroPlayerId = false;
		bool hasTeamSlotMapping = false;
		bool hasSelectedHero = false;
		bool hasDataTeam = false;
		bool hasDataTeamNetWorth = false;
		bool hasDataTeamReliableGold = false;
		bool hasDataTeamUnreliableGold = false;
		bool hasSpectatorNetWorth = false;
		bool hasIsIllusion = false;
		bool hasIsClone = false;
		bool hasIsLocalController = false;
		bool hasNextOutgoingOrder = false;
		bool hasCursor = false;
		bool resolved = false;
	};

	struct PlayerTeamSlotMappings
	{
		std::array<int , 24> slotsByPlayerId{};
		std::array<int , 24> slotsByRecord{};
		std::array<CHandle , 24> selectedHeroes{};
		int recordCount = 0;
	};

	struct NetWorthMappings
	{
		std::array<int , 24> byPlayerId{};
		std::array<int , 24> goldByPlayerId{};
		std::array<int , 5> radiantBySlot{};
		std::array<int , 5> direBySlot{};
		std::array<int , 5> radiantGoldBySlot{};
		std::array<int , 5> direGoldBySlot{};
		bool hasRadiant = false;
		bool hasDire = false;
		bool hasSpectator = false;
	};

	struct HeroVitalsSnapshot
	{
		std::array<HeroVitals , 10> heroes{};
		int controllerCount = 0;
		int assignedHeroCount = 0;
		int fallbackHeroCount = 0;
		int scannedEntityCount = 0;
		int allocatedChunkCount = 0;
		uint8_t localTeam = 0;
		bool offsetsReady = false;
	};

	auto LooksLikePlayerControllerName( const char* name ) -> bool
	{
		return name && std::strstr( name , "DOTAPlayerController" ) != nullptr;
	}

	auto LooksLikeHeroEntityName( const char* name ) -> bool
	{
		return name && ( std::strstr( name , "DOTA_Unit_Hero" ) != nullptr ||
			std::strcmp( name , "C_DOTA_BaseNPC_Hero" ) == 0 );
	}

	auto LooksLikePlayerResourceName( const char* name ) -> bool
	{
		return name && std::strstr( name , "DOTA_PlayerResource" ) != nullptr;
	}

	auto ContainsNoCase( const char* haystack , const char* needle ) -> bool
	{
		if ( !haystack || !needle || !needle[0] )
			return false;

		const size_t haystackLen = std::strlen( haystack );
		const size_t needleLen = std::strlen( needle );
		if ( needleLen > haystackLen )
			return false;

		for ( size_t start = 0; start + needleLen <= haystackLen; ++start )
		{
			bool matched = true;
			for ( size_t index = 0; index < needleLen; ++index )
			{
				const auto left = static_cast<unsigned char>( haystack[start + index] );
				const auto right = static_cast<unsigned char>( needle[index] );
				if ( std::tolower( left ) != std::tolower( right ) )
				{
					matched = false;
					break;
				}
			}
			if ( matched )
				return true;
		}

		return false;
	}

	auto LooksLikeTeamDataName( const char* name , bool radiant ) -> bool
	{
		if ( !name )
			return false;

		if ( radiant )
		{
			return ContainsNoCase( name , "DataRadiant" ) ||
				ContainsNoCase( name , "data_radiant" ) ||
				ContainsNoCase( name , "dota_data_radiant" ) ||
				( ContainsNoCase( name , "radiant" ) && ContainsNoCase( name , "data" ) &&
					!ContainsNoCase( name , "dire" ) && !ContainsNoCase( name , "spectator" ) );
		}

		return ContainsNoCase( name , "DataDire" ) ||
			ContainsNoCase( name , "data_dire" ) ||
			ContainsNoCase( name , "dota_data_dire" ) ||
			( ContainsNoCase( name , "dire" ) && ContainsNoCase( name , "data" ) &&
				!ContainsNoCase( name , "radiant" ) && !ContainsNoCase( name , "spectator" ) );
	}

	auto LooksLikeSpectatorDataName( const char* name ) -> bool
	{
		return name && ( ContainsNoCase( name , "DataSpectator" ) ||
			ContainsNoCase( name , "data_spectator" ) );
	}

	auto FindSchemaClassSize( const char* className ) -> int
	{
		auto* schemaSystem = SDK::Interfaces::SchemaSystem();
		if ( !schemaSystem )
			return 0;

		auto** scopes = schemaSystem->GetAllTypeScope();
		const uint16_t scopeCount = schemaSystem->GetAllTypeScopeSize();
		for ( uint16_t index = 0; scopes && index < scopeCount; ++index )
		{
			if ( !scopes[index] )
				continue;
			if ( auto* binding = scopes[index]->FindRawClassBinding( className ) )
				return binding->m_SizeOf();
		}

		return 0;
	}

	auto ResolveDataTeamPlayerStride() -> int
	{
		static int cached = -1;
		if ( cached >= 0 )
			return cached;

		int stride = FindSchemaClassSize( "DataTeamPlayer_t" );
		if ( stride <= 0 )
		{
			// FindRawClassBinding can miss classes that schema Init found via hash.
			// Estimate from the deepest known field and pad for trailing members.
			auto* schema = GetSchemaOffset();
			uint32_t deepest = 0x90;
			if ( schema )
			{
				static const char* kTailFields[] = {
					"m_iPlayerSteamID" ,
					"m_nCurrentMadstone" ,
					"m_NeutralChoices" ,
					"m_CourierController" ,
					"m_quickBuyController" ,
					"m_nSelectedBlessing" ,
				};
				for ( const char* field : kTailFields )
				{
					uint32_t offset = 0;
					if ( schema->TryGetOffset( "DataTeamPlayer_t" , field , offset ) && offset > deepest )
						deepest = offset;
				}
			}
			stride = static_cast<int>( ( deepest + 0x800 ) & ~0xF );
		}

		// DataTeamPlayer_t is large (suggestion arrays / controllers). Allow up to 1MB.
		if ( stride <= 0x90 || stride > 0x100000 )
			stride = 0;

		cached = stride;
		return cached;
	}

	auto IsReadableMemory( const void* ptr , size_t size ) -> bool
	{
		if ( !ptr || size == 0 )
			return false;

		MEMORY_BASIC_INFORMATION mbi{};
		if ( !VirtualQuery( ptr , &mbi , sizeof( mbi ) ) )
			return false;

		const DWORD protect = mbi.Protect & ~( PAGE_GUARD | PAGE_NOACCESS );
		const bool committed = mbi.State == MEM_COMMIT;
		const bool canRead =
			protect == PAGE_READONLY ||
			protect == PAGE_READWRITE ||
			protect == PAGE_WRITECOPY ||
			protect == PAGE_EXECUTE_READ ||
			protect == PAGE_EXECUTE_READWRITE ||
			protect == PAGE_EXECUTE_WRITECOPY;

		const auto regionEnd = reinterpret_cast<uintptr_t>( mbi.BaseAddress ) + mbi.RegionSize;
		const auto ptrEnd = reinterpret_cast<uintptr_t>( ptr ) + size;
		return committed && canRead && ptrEnd <= regionEnd;
	}

	auto TryReadUtlVector( uintptr_t vectorBase , const void* entityBase , int& outCount , const uint8_t*& outData ) -> bool
	{
		outCount = 0;
		outData = nullptr;
		if ( !vectorBase || !IsReadableMemory( reinterpret_cast<const void*>( vectorBase ) , 0x18 ) )
			return false;

		// Source 2 CUtlVector: m_Size@0x00, pad@0x04, m_pMemory@0x08.
		const int size = *reinterpret_cast<const int32_t*>( vectorBase );
		const auto* data = *reinterpret_cast<const uint8_t* const*>( vectorBase + 0x8 );
		if ( size <= 0 || size > 24 || !data )
			return false;

		// Reject false positives: EmbeddedNetworkVar bookkeeping can look like
		// size=1 with a back-pointer to the entity itself (see crash dump).
		if ( data == entityBase || data == reinterpret_cast<const uint8_t*>( vectorBase ) )
			return false;
		if ( !IsReadableMemory( data , 0xA0 ) )
			return false;

		outCount = size;
		outData = data;
		return true;
	}

	auto ReadDataTeamVector( CEntityInstance* entity , uint32_t fieldOffset ,
		int& outCount , const uint8_t*& outData ) -> bool
	{
		outCount = 0;
		outData = nullptr;
		if ( !entity || fieldOffset == 0 )
			return false;

		const uintptr_t field = reinterpret_cast<uintptr_t>( entity ) + fieldOffset;
		// Live dump of dota_data_radiant::m_vecDataTeam shows CUtlVector at +0x00
		// (size=N, ptr=heap). +0x50 is empty on this build. Still try +0x50 for
		// older EmbeddedNetworkVar layouts used by PlayerResource-style fields.
		static const uint32_t kVectorBases[] = { 0x00 , 0x50 };
		for ( uint32_t base : kVectorBases )
		{
			if ( TryReadUtlVector( field + base , entity , outCount , outData ) )
				return true;
		}

		return false;
	}

	auto ReadTeamDataNetWorthSlots( CEntityInstance* teamData , const HeroVitalsOffsets& offsets ,
		std::array<int , 5>& outSlots , std::array<int , 5>* outGoldSlots = nullptr ) -> int
	{
		outSlots.fill( 0 );
		if ( outGoldSlots )
			outGoldSlots->fill( 0 );
		if ( !teamData || !offsets.hasDataTeam || !offsets.hasDataTeamNetWorth )
			return 0;

		int dataTeamStride = ResolveDataTeamPlayerStride();
		if ( dataTeamStride <= 0 )
			dataTeamStride = 0x1AE0;

		int count = 0;
		const uint8_t* data = nullptr;
		if ( !ReadDataTeamVector( teamData , offsets.dataTeam , count , data ) || !data )
			return 0;

		// size can briefly be 1 while the array is already allocated for the full team.
		const int tryCount = (std::max)( count , 5 );
		if ( !IsReadableMemory( data ,
			static_cast<size_t>( tryCount ) * static_cast<size_t>( dataTeamStride ) ) )
		{
			if ( !IsReadableMemory( data ,
				static_cast<size_t>( count ) * static_cast<size_t>( dataTeamStride ) ) )
				return 0;
		}

		const int limit = IsReadableMemory( data ,
			static_cast<size_t>( tryCount ) * static_cast<size_t>( dataTeamStride ) ) ? tryCount : count;
		const int usable = (std::min)( limit , static_cast<int>( outSlots.size() ) );
		int positive = 0;
		for ( int slot = 0; slot < usable; ++slot )
		{
			const uint8_t* slotBase = data + static_cast<size_t>( slot ) * static_cast<size_t>( dataTeamStride );
			const auto* valuePtr = reinterpret_cast<const int32_t*>( slotBase + offsets.dataTeamNetWorth );
			if ( !IsReadableMemory( valuePtr , sizeof( int32_t ) ) )
				break;

			const int value = *valuePtr;
			if ( value < 0 || value > 300000 )
			{
				if ( slot == 0 )
					return 0;
				break;
			}

			outSlots[slot] = value;
			if ( value > 0 )
				++positive;

			if ( outGoldSlots )
			{
				int gold = 0;
				if ( offsets.hasDataTeamReliableGold )
				{
					const auto* reliablePtr = reinterpret_cast<const int32_t*>(
						slotBase + offsets.dataTeamReliableGold );
					if ( IsReadableMemory( reliablePtr , sizeof( int32_t ) ) )
					{
						const int reliable = *reliablePtr;
						if ( reliable >= 0 && reliable <= 100000 )
							gold += reliable;
					}
				}
				if ( offsets.hasDataTeamUnreliableGold )
				{
					const auto* unreliablePtr = reinterpret_cast<const int32_t*>(
						slotBase + offsets.dataTeamUnreliableGold );
					if ( IsReadableMemory( unreliablePtr , sizeof( int32_t ) ) )
					{
						const int unreliable = *unreliablePtr;
						if ( unreliable >= 0 && unreliable <= 100000 )
							gold += unreliable;
					}
				}
				( *outGoldSlots )[slot] = gold;
			}
		}

		return positive > 0 ? usable : 0;
	}

	auto TryClassifyTeamDataEntity( CEntityInstance* entity , CEntityIdentity* identity ,
		const HeroVitalsOffsets& offsets , CHandle& radiantOut , CHandle& direOut ,
		CHandle& spectatorOut ) -> bool
	{
		if ( !entity || !identity || !offsets.hasDataTeam )
			return false;

		const char* className = entity->GetSchemaClassName();
		const char* designerName = identity->DesingerName().String();
		const char* entityName = identity->Name().String();

		const bool nameSpectator = LooksLikeSpectatorDataName( className ) ||
			LooksLikeSpectatorDataName( designerName ) || LooksLikeSpectatorDataName( entityName );
		const bool nameRadiant = LooksLikeTeamDataName( className , true ) ||
			LooksLikeTeamDataName( designerName , true ) || LooksLikeTeamDataName( entityName , true );
		const bool nameDire = LooksLikeTeamDataName( className , false ) ||
			LooksLikeTeamDataName( designerName , false ) || LooksLikeTeamDataName( entityName , false );
		const bool nameData = ContainsNoCase( className , "data" ) || ContainsNoCase( designerName , "data" ) ||
			ContainsNoCase( entityName , "data" );

		if ( !nameSpectator && !nameRadiant && !nameDire && !nameData )
			return false;

		int count = 0;
		const uint8_t* data = nullptr;
		const bool hasVector = ReadDataTeamVector( entity , offsets.dataTeam , count , data );
		if ( !hasVector && !nameSpectator && !nameRadiant && !nameDire )
			return false;

		const uint8_t team = offsets.team ?
			*reinterpret_cast<const uint8_t*>( reinterpret_cast<uintptr_t>( entity ) + offsets.team ) : 0;
		const CHandle handle = identity->Handle();

		if ( nameSpectator )
		{
			spectatorOut = handle;
			return true;
		}
		if ( nameRadiant || ( hasVector && team == 2 ) )
		{
			radiantOut = handle;
			return true;
		}
		if ( nameDire || ( hasVector && team == 3 ) )
		{
			direOut = handle;
			return true;
		}
		if ( hasVector && count >= 5 && count <= 10 )
		{
			// Last resort: first unclaimed 5-player data vector goes to radiant, second to dire.
			if ( !radiantOut.IsValid() )
			{
				radiantOut = handle;
				return true;
			}
			if ( !direOut.IsValid() && handle.m_Index != radiantOut.m_Index )
			{
				direOut = handle;
				return true;
			}
		}

		return false;
	}

	auto ResolveHeroVitalsOffsets() -> const HeroVitalsOffsets&
	{
		static HeroVitalsOffsets offsets{};

		if ( offsets.resolved )
			return offsets;

		auto* schema = GetSchemaOffset();
		if ( !schema )
			return offsets;

		const bool hasHealth = schema->TryGetOffset( "C_BaseEntity" , "m_iHealth" , offsets.health );
		const bool hasMaxHealth = schema->TryGetOffset( "C_BaseEntity" , "m_iMaxHealth" , offsets.maxHealth );
		const bool hasMana = schema->TryGetOffset( "C_DOTA_BaseNPC" , "m_flMana" , offsets.mana );
		const bool hasMaxMana = schema->TryGetOffset( "C_DOTA_BaseNPC" , "m_flMaxMana" , offsets.maxMana );
		const bool hasTeam = schema->TryGetOffset( "C_BaseEntity" , "m_iTeamNum" , offsets.team );
		const bool hasAssignedHero = schema->TryGetOffset( "C_DOTAPlayerController" , "m_hAssignedHero" , offsets.assignedHero );
		offsets.hasPlayerId = schema->TryGetOffset( "C_DOTAPlayerController" , "m_nPlayerID" , offsets.playerId ) ||
			schema->TryGetOffset( "C_DOTAPlayerController" , "m_iPlayerID" , offsets.playerId );
		offsets.hasPlayerOwnerId = schema->TryGetOffset( "C_DOTA_BaseNPC" , "m_nPlayerOwnerID" , offsets.playerOwnerId );
		offsets.hasHeroPlayerId = schema->TryGetOffset( "C_DOTA_BaseNPC_Hero" , "m_iPlayerID" , offsets.heroPlayerId );
		const bool hasPlayerTeamData = schema->TryGetOffset( "C_DOTA_PlayerResource" , "m_vecPlayerTeamData" , offsets.playerTeamData );
		const bool hasTeamSlot = schema->TryGetOffset( "PlayerResourcePlayerTeamData_t" , "m_iTeamSlot" , offsets.teamSlot );
		offsets.hasSelectedHero = schema->TryGetOffset( "PlayerResourcePlayerTeamData_t" , "m_hSelectedHero" , offsets.selectedHero );
		// Net worth lives on DataTeamPlayer_t (Radiant/Dire team data), not PlayerResource.
		offsets.hasDataTeam = schema->TryGetOffset( "C_DOTA_DataNonSpectator" , "m_vecDataTeam" , offsets.dataTeam ) ||
			schema->TryGetOffset( "CDOTA_DataNonSpectator" , "m_vecDataTeam" , offsets.dataTeam );
		offsets.hasDataTeamNetWorth = schema->TryGetOffset( "DataTeamPlayer_t" , "m_iNetWorth" , offsets.dataTeamNetWorth );
		offsets.hasDataTeamReliableGold = schema->TryGetOffset( "DataTeamPlayer_t" , "m_iReliableGold" , offsets.dataTeamReliableGold );
		offsets.hasDataTeamUnreliableGold = schema->TryGetOffset( "DataTeamPlayer_t" , "m_iUnreliableGold" , offsets.dataTeamUnreliableGold );
		offsets.hasSpectatorNetWorth = schema->TryGetOffset( "C_DOTA_DataSpectator" , "m_iNetWorth" , offsets.spectatorNetWorth ) ||
			schema->TryGetOffset( "CDOTA_DataSpectator" , "m_iNetWorth" , offsets.spectatorNetWorth );
		offsets.hasTeamSlotMapping = hasPlayerTeamData && hasTeamSlot;
		offsets.hasIsIllusion = schema->TryGetOffset( "C_DOTA_BaseNPC" , "m_bIsIllusion" , offsets.isIllusion );
		offsets.hasIsClone = schema->TryGetOffset( "C_DOTA_BaseNPC" , "m_bIsClone" , offsets.isClone );
		offsets.hasIsLocalController =
			schema->TryGetOffset( "CBasePlayerController" , "m_bIsLocalPlayerController" , offsets.isLocalController ) ||
			schema->TryGetOffset( "C_BasePlayerController" , "m_bIsLocalPlayerController" , offsets.isLocalController ) ||
			schema->TryGetOffset( "C_DOTAPlayerController" , "m_bIsLocalPlayerController" , offsets.isLocalController );
		offsets.hasNextOutgoingOrder = schema->TryGetOffset( "C_DOTAPlayerController" ,
			"m_nNextOutgoingOrderSequenceNumber" , offsets.nextOutgoingOrder );
		offsets.hasCursor = schema->TryGetOffset( "C_DOTAPlayerController" , "m_iCursor" , offsets.cursor );
		offsets.hasAbilities = schema->TryGetOffset( "C_DOTA_BaseNPC" , "m_vecAbilities" , offsets.abilities );
		const bool hasInventoryContainer = schema->TryGetOffset( "C_DOTA_BaseNPC" , "m_Inventory" , offsets.inventory );
		// Prefer the client schema name. Server CDOTA_* bindings can exist with
		// different layouts and would point m_hItems at the wrong place.
		const bool hasNestedItems = schema->TryGetOffset( "C_DOTA_UnitInventory" , "m_hItems" , offsets.inventoryItems ) ||
			schema->TryGetOffset( "CDOTA_UnitInventory" , "m_hItems" , offsets.inventoryItems );
		if ( !hasNestedItems )
		{
			offsets.inventory = 0;
			offsets.hasInventory = schema->TryGetOffset( "C_DOTA_BaseNPC" , "m_hItems" , offsets.inventoryItems );
		}
		else
		{
			offsets.hasInventory = hasInventoryContainer;
		}
		offsets.hasAbilityLevel = schema->TryGetOffset( "C_DOTABaseAbility" , "m_iLevel" , offsets.abilityLevel );
		offsets.hasAbilityCooldown = schema->TryGetOffset( "C_DOTABaseAbility" , "m_flCooldown" , offsets.abilityCooldown ) ||
			schema->TryGetOffset( "C_DOTABaseAbility" , "m_fCooldown" , offsets.abilityCooldown );
		offsets.hasItemCharges = schema->TryGetOffset( "C_DOTA_Item" , "m_iCurrentCharges" , offsets.itemCharges ) ||
			schema->TryGetOffset( "CDOTA_Item" , "m_iCurrentCharges" , offsets.itemCharges );
		offsets.resolved = hasHealth && hasMaxHealth && hasMana && hasMaxMana && hasTeam && hasAssignedHero;

		static bool logged = false;
		if ( !logged )
		{
			DEV_LOG( "[hero-vitals] schema health=%d(0x%X) maxHealth=%d(0x%X) mana=%d(0x%X) maxMana=%d(0x%X) team=%d(0x%X) assignedHero=%d(0x%X) playerId=%d(0x%X) heroPlayerId=%d(0x%X) teamData=%d(0x%X) teamSlot=%d(0x%X) selectedHero=%d(0x%X) dataTeam=%d(0x%X) dataNetWorth=%d(0x%X) gold=%d(0x%X)+%d(0x%X) spectatorNetWorth=%d(0x%X) illusion=%d(0x%X) clone=%d(0x%X) abilities=%d(0x%X) inventory=%d(0x%X+0x%X)\n" ,
				hasHealth , offsets.health , hasMaxHealth , offsets.maxHealth , hasMana , offsets.mana ,
				hasMaxMana , offsets.maxMana , hasTeam , offsets.team , hasAssignedHero , offsets.assignedHero ,
				offsets.hasPlayerId , offsets.playerId , offsets.hasHeroPlayerId , offsets.heroPlayerId ,
				hasPlayerTeamData , offsets.playerTeamData , hasTeamSlot , offsets.teamSlot ,
				offsets.hasSelectedHero , offsets.selectedHero ,
				offsets.hasDataTeam , offsets.dataTeam , offsets.hasDataTeamNetWorth , offsets.dataTeamNetWorth ,
				offsets.hasDataTeamReliableGold , offsets.dataTeamReliableGold ,
				offsets.hasDataTeamUnreliableGold , offsets.dataTeamUnreliableGold ,
				offsets.hasSpectatorNetWorth , offsets.spectatorNetWorth ,
				offsets.hasIsIllusion , offsets.isIllusion , offsets.hasIsClone , offsets.isClone ,
				offsets.hasAbilities , offsets.abilities , offsets.hasInventory , offsets.inventory ,
				offsets.inventoryItems );
			logged = true;
		}

		return offsets;
	}

	// DataTeam / portrait slots are 0-4 per team. Player IDs are usually
	// Radiant 0-4 and Dire 5-9. Never cross-map (e.g. Radiant pid=5 must not
	// steal Dire slot 0 — that caused the Juggernaut gold/NW leak).
	auto InferTeamSlotFromPlayerId( uint8_t team , int playerId ) -> int
	{
		if ( playerId < 0 )
			return -1;
		if ( team == 2 )
			return ( playerId < 5 ) ? playerId : -1;
		if ( team == 3 )
		{
			if ( playerId >= 5 && playerId < 10 )
				return playerId - 5;
			// Dire-only / custom lobbies sometimes keep Dire as 0-4.
			if ( playerId < 5 )
				return playerId;
			return -1;
		}
		return -1;
	}

	auto PlayerIdMatchesTeamConvention( uint8_t team , int playerId ) -> bool
	{
		if ( playerId < 0 )
			return false;
		if ( team == 2 )
			return playerId < 5;
		if ( team == 3 )
			return playerId >= 5 && playerId < 10;
		return false;
	}

	auto ResolvePlayerTeamSlots( CEntityInstance* playerResource , const HeroVitalsOffsets& offsets )
		-> PlayerTeamSlotMappings
	{
		PlayerTeamSlotMappings mappings{};
		mappings.slotsByPlayerId.fill( -1 );
		mappings.slotsByRecord.fill( -1 );
		for ( auto& handle : mappings.selectedHeroes )
			handle.m_Index = INVALID_EHANDLE_INDEX;
		if ( !playerResource || !offsets.hasTeamSlotMapping )
			return mappings;

		static const int teamDataStride = FindSchemaClassSize( "PlayerResourcePlayerTeamData_t" );
		if ( teamDataStride <= 0 || teamDataStride > 0x1000 )
			return mappings;

		// Prefer EmbeddedNetworkVar layout (+0x50); some builds keep a plain CUtlVector at +0x00.
		int count = 0;
		const uint8_t* data = nullptr;
		const uintptr_t field = reinterpret_cast<uintptr_t>( playerResource ) + offsets.playerTeamData;
		static const uint32_t kVectorBases[] = { 0x50 , 0x00 };
		for ( uint32_t base : kVectorBases )
		{
			if ( TryReadUtlVector( field + base , playerResource , count , data ) &&
				count > 0 && count <= static_cast<int>( mappings.slotsByRecord.size() ) )
				break;
			count = 0;
			data = nullptr;
		}
		if ( !data || count <= 0 )
			return mappings;
		mappings.recordCount = count;
		std::array<int , 24> rawSlots{};
		rawSlots.fill( -1 );
		bool sawZeroSlot = false;
		bool sawFiveSlot = false;

		for ( int record = 0; record < count; ++record )
		{
			rawSlots[record] = *reinterpret_cast<const int32_t*>( data + record * teamDataStride + offsets.teamSlot );
			if ( offsets.hasSelectedHero )
				mappings.selectedHeroes[record] = *reinterpret_cast<const CHandle*>(
					data + record * teamDataStride + offsets.selectedHero );
			const bool activeRecord = !offsets.hasSelectedHero || mappings.selectedHeroes[record].IsValid();
			sawZeroSlot = sawZeroSlot || ( activeRecord && rawSlots[record] == 0 );
			sawFiveSlot = sawFiveSlot || ( activeRecord && rawSlots[record] == 5 );
		}

		// Some builds expose lobby slots as 1..5 while the Panorama portrait
		// indices are always 0..4. Detect that representation from the complete
		// record set instead of blindly shifting every build.
		const int slotBase = sawFiveSlot && !sawZeroSlot ? 1 : 0;

		for ( int playerId = 0; playerId < count; ++playerId )
		{
			const int slot = rawSlots[playerId] - slotBase;
			if ( slot >= 0 && slot < 5 )
			{
				mappings.slotsByPlayerId[playerId] = slot;
				mappings.slotsByRecord[playerId] = slot;
			}
		}

		return mappings;
	}

	auto ResolveNetWorthMappings( CEntityInstance* radiantData , CEntityInstance* direData ,
		CEntityInstance* spectatorData , const PlayerTeamSlotMappings& teamSlots ,
		const HeroVitalsOffsets& offsets ) -> NetWorthMappings
	{
		NetWorthMappings mappings{};
		mappings.byPlayerId.fill( 0 );
		mappings.goldByPlayerId.fill( 0 );

		if ( radiantData )
		{
			const int count = ReadTeamDataNetWorthSlots( radiantData , offsets , mappings.radiantBySlot ,
				&mappings.radiantGoldBySlot );
			mappings.hasRadiant = count > 0;
		}
		if ( direData )
		{
			const int count = ReadTeamDataNetWorthSlots( direData , offsets , mappings.direBySlot ,
				&mappings.direGoldBySlot );
			mappings.hasDire = count > 0;
		}

		if ( spectatorData && offsets.hasSpectatorNetWorth )
		{
			const auto* values = reinterpret_cast<const int32_t*>(
				reinterpret_cast<uintptr_t>( spectatorData ) + offsets.spectatorNetWorth );
			for ( int playerId = 0; playerId < static_cast<int>( mappings.byPlayerId.size() ); ++playerId )
			{
				const int value = (std::max)( 0 , values[playerId] );
				if ( value > 0 )
				{
					mappings.byPlayerId[playerId] = value;
					mappings.hasSpectator = true;
				}
			}
		}

		// Map team-slot net worth / gold onto absolute player IDs.
		// Prefer PlayerResource slots; if those are missing, use 0-4 / 5-9.
		// Only attach Radiant DataTeam values to Radiant-convention IDs (0-4) and
		// Dire values to Dire-convention IDs (5-9) so a Radiant hero with pid=5
		// cannot inherit the local Dire slot-0 economy.
		const int mapLimit = teamSlots.recordCount > 0 ? teamSlots.recordCount : 10;
		for ( int playerId = 0; playerId < mapLimit &&
			playerId < static_cast<int>( mappings.byPlayerId.size() ); ++playerId )
		{
			const uint8_t conventionalTeam = playerId < 5 ? 2 : 3;
			int slot = ( teamSlots.recordCount > 0 ) ? teamSlots.slotsByPlayerId[playerId] : -1;
			if ( slot < 0 || slot >= 5 )
				slot = InferTeamSlotFromPlayerId( conventionalTeam , playerId );
			if ( slot < 0 || slot >= 5 )
				continue;

			if ( conventionalTeam == 2 && mappings.hasRadiant )
			{
				if ( mappings.byPlayerId[playerId] <= 0 && mappings.radiantBySlot[slot] > 0 )
					mappings.byPlayerId[playerId] = mappings.radiantBySlot[slot];
				if ( mappings.goldByPlayerId[playerId] <= 0 )
					mappings.goldByPlayerId[playerId] = mappings.radiantGoldBySlot[slot];
			}
			else if ( conventionalTeam == 3 && mappings.hasDire )
			{
				if ( mappings.byPlayerId[playerId] <= 0 && mappings.direBySlot[slot] > 0 )
					mappings.byPlayerId[playerId] = mappings.direBySlot[slot];
				if ( mappings.goldByPlayerId[playerId] <= 0 )
					mappings.goldByPlayerId[playerId] = mappings.direGoldBySlot[slot];
			}
		}

		return mappings;
	}

	auto ResolveHeroNetWorth( const NetWorthMappings& netWorth , uint8_t team , int teamSlot , int playerId ) -> int
	{
		int slot = teamSlot;
		if ( slot < 0 || slot >= 5 )
			slot = InferTeamSlotFromPlayerId( team , playerId );

		// Strict team gate: never read the opposite team's DataTeam slot.
		if ( slot >= 0 && slot < 5 )
		{
			if ( team == 2 && netWorth.hasRadiant )
				return netWorth.radiantBySlot[slot];
			if ( team == 3 && netWorth.hasDire )
				return netWorth.direBySlot[slot];
		}

		// byPlayerId is filled from the 0-4 / 5-9 convention; only use it when
		// the hero's live team matches that convention (blocks Radiant pid=5 leak).
		if ( playerId >= 0 && playerId < static_cast<int>( netWorth.byPlayerId.size() ) &&
			PlayerIdMatchesTeamConvention( team , playerId ) )
			return netWorth.byPlayerId[playerId];

		return 0;
	}

	auto ResolveHeroGold( const NetWorthMappings& netWorth , uint8_t team , int teamSlot , int playerId ) -> int
	{
		int slot = teamSlot;
		if ( slot < 0 || slot >= 5 )
			slot = InferTeamSlotFromPlayerId( team , playerId );

		if ( slot >= 0 && slot < 5 )
		{
			if ( team == 2 && netWorth.hasRadiant )
				return netWorth.radiantGoldBySlot[slot];
			if ( team == 3 && netWorth.hasDire )
				return netWorth.direGoldBySlot[slot];
		}

		if ( playerId >= 0 && playerId < static_cast<int>( netWorth.goldByPlayerId.size() ) &&
			PlayerIdMatchesTeamConvention( team , playerId ) )
			return netWorth.goldByPlayerId[playerId];

		return 0;
	}

	auto ResolveHeroTeamSlot( const PlayerTeamSlotMappings& mappings , CHandle heroHandle ,
		int playerId , uint8_t team = 0 ) -> int
	{
		if ( heroHandle.IsValid() )
		{
			for ( int record = 0; record < mappings.recordCount; ++record )
			{
				if ( mappings.slotsByRecord[record] >= 0 && mappings.selectedHeroes[record].IsValid() &&
					mappings.selectedHeroes[record] == heroHandle )
					return mappings.slotsByRecord[record];
			}
		}

		// Only trust playerId→slot when the ID matches the hero's live team.
		if ( playerId >= 0 && playerId < static_cast<int>( mappings.slotsByPlayerId.size() ) &&
			mappings.slotsByPlayerId[playerId] >= 0 && PlayerIdMatchesTeamConvention( team , playerId ) )
			return mappings.slotsByPlayerId[playerId];

		return InferTeamSlotFromPlayerId( team , playerId );
	}

	template <typename T>
	auto ReadEntityField( const CEntityInstance* entity , uint32_t offset ) -> T
	{
		return *reinterpret_cast<const T*>( reinterpret_cast<uintptr_t>( entity ) + offset );
	}

	auto ScoreLocalController( const C_DOTAPlayerController* controller , const HeroVitalsOffsets& offsets ) -> int
	{
		if ( !controller )
			return 0;
		if ( !offsets.hasIsLocalController || offsets.isLocalController == 0 || offsets.isLocalController >= 0x1000 )
			return 0;
		return ReadEntityField<bool>( controller , offsets.isLocalController ) ? 500 : 0;
	}

	auto TeamFromController( C_DOTAPlayerController* controller , const HeroVitalsSnapshot& snapshot ,
		const HeroVitalsOffsets& offsets ) -> uint8_t
	{
		if ( !controller || !offsets.resolved )
			return 0;

		const CHandle assignedHero = ReadEntityField<CHandle>( controller , offsets.assignedHero );
		if ( assignedHero.IsValid() )
		{
			for ( const auto& hero : snapshot.heroes )
			{
				if ( hero.handle == assignedHero && ( hero.team == 2 || hero.team == 3 ) )
					return hero.team;
			}

			if ( auto* entitySystem = SDK::Interfaces::GameEntitySystem() )
			{
				if ( auto* localHero = entitySystem->GetBaseEntityFromHandle( assignedHero ) )
				{
					const uint8_t heroTeam = ReadEntityField<uint8_t>( localHero , offsets.team );
					if ( heroTeam == 2 || heroTeam == 3 )
						return heroTeam;
				}
			}
		}

		const uint8_t controllerTeam = ReadEntityField<uint8_t>( controller , offsets.team );
		if ( controllerTeam == 2 || controllerTeam == 3 )
			return controllerTeam;
		return 0;
	}

	auto InferLocalTeamFromRoster( const HeroVitalsSnapshot& snapshot ) -> uint8_t
	{
		int radiantOfficial = 0;
		int direOfficial = 0;
		int radiantGold = 0;
		int direGold = 0;
		for ( const auto& hero : snapshot.heroes )
		{
			if ( hero.playerId < 0 || hero.maxHealth <= 0 || ( hero.team != 2 && hero.team != 3 ) )
				continue;
			if ( hero.officialNetWorth > 0 )
			{
				if ( hero.team == 2 )
					++radiantOfficial;
				else
					++direOfficial;
			}
			if ( hero.pocketGold > 0 )
			{
				if ( hero.team == 2 )
					++radiantGold;
				else
					++direGold;
			}
		}
		if ( radiantOfficial > 0 && direOfficial == 0 )
			return 2;
		if ( direOfficial > 0 && radiantOfficial == 0 )
			return 3;
		if ( radiantGold > 0 && direGold == 0 )
			return 2;
		if ( direGold > 0 && radiantGold == 0 )
			return 3;
		return 0;
	}

	auto BuildTrackedEntry( CEntityInstance* entity , TrackedEntryKind kind ,
		const HeroVitalsOffsets& offsets , TrackedEntry& entry , const char* resolvedName = nullptr ) -> bool
	{
		if ( !entity )
			return false;

		auto* identity = entity->pEntityIdentity();
		if ( !identity )
			return false;
		const char* rawName = resolvedName;
		if ( !rawName || !rawName[0] )
			rawName = identity->Name().String();
		if ( !rawName || !rawName[0] )
			rawName = identity->DesingerName().String();
		if ( !rawName || !rawName[0] )
			return false;

		entry = {};
		entry.kind = kind;
		entry.name = rawName;
		if ( offsets.hasAbilityCooldown )
		{
			const float cooldown = ReadEntityField<float>( entity , offsets.abilityCooldown );
			entry.cooldown = std::isfinite( cooldown ) && cooldown > 0.f && cooldown < 10000.f ? cooldown : 0.f;
		}
		if ( kind == TrackedEntryKind::Ability && offsets.hasAbilityLevel )
			entry.level = (std::clamp)( ReadEntityField<int>( entity , offsets.abilityLevel ) , 0 , 30 );
		if ( kind == TrackedEntryKind::Item && offsets.hasItemCharges )
			entry.count = (std::clamp)( ReadEntityField<int>( entity , offsets.itemCharges ) , 0 , 9999 );
		return true;
	}

	auto AddTrackedEntry( HeroVitals& vitals , CEntityInstance* entity , TrackedEntryKind kind ,
		const HeroVitalsOffsets& offsets , const char* resolvedName = nullptr ) -> void
	{
		if ( vitals.trackedEntryCount >= static_cast<int>( vitals.trackedEntries.size() ) )
			return;

		TrackedEntry candidate{};
		if ( !BuildTrackedEntry( entity , kind , offsets , candidate , resolvedName ) )
			return;

		for ( int index = 0; index < vitals.trackedEntryCount; ++index )
		{
			if ( vitals.trackedEntries[index].kind == kind && vitals.trackedEntries[index].name == candidate.name )
				return;
		}

		vitals.trackedEntries[vitals.trackedEntryCount++] = std::move( candidate );
	}

	auto ResolveHeroAbilityName( CEntityInstance* entity , const std::string& heroName ) -> std::string
	{
		if ( !entity || heroName.empty() )
			return {};
		auto* identity = entity->pEntityIdentity();
		if ( !identity )
			return {};

		std::string heroPrefix = heroName;
		constexpr const char* heroNamePrefix = "npc_dota_hero_";
		if ( heroPrefix.rfind( heroNamePrefix , 0 ) == 0 )
			heroPrefix.erase( 0 , std::strlen( heroNamePrefix ) );
		const std::string prefixWithSep = heroPrefix + "_";
		const auto heroAbilities = g_HeroAbilityNames.find( heroName );
		// Prefer designer name — it matches hero_abilities.json / CDN icon keys.
		const char* candidates[] = {
			identity->DesingerName().String() ,
			identity->Name().String() ,
			entity->GetSchemaClassName()
		};
		for ( const char* rawName : candidates )
		{
			if ( !rawName || !rawName[0] )
				continue;
			std::string abilityName = rawName;

			// Schema class names look like C_DOTA_Ability_muerta_dead_shot /
			// CDOTA_Ability_Muerta_DeadShot — normalize to ability key form.
			auto normalizeClassName = []( std::string value ) -> std::string
			{
				constexpr const char* prefixes[] = {
					"C_DOTA_Ability_" , "CDOTA_Ability_" , "C_DOTAAbility_" , "CDOTAAbility_"
				};
				for ( const char* prefix : prefixes )
				{
					if ( value.rfind( prefix , 0 ) == 0 )
					{
						value.erase( 0 , std::strlen( prefix ) );
						break;
					}
				}
				std::string normalized;
				normalized.reserve( value.size() + 8 );
				for ( size_t index = 0; index < value.size(); ++index )
				{
					const unsigned char character = static_cast<unsigned char>( value[index] );
					if ( character == ':' )
					{
						normalized.push_back( '_' );
						continue;
					}
					if ( std::isupper( character ) && !normalized.empty() && normalized.back() != '_' )
						normalized.push_back( '_' );
					normalized.push_back( static_cast<char>( std::tolower( character ) ) );
				}
				return normalized;
			};
			if ( abilityName.find( "Ability" ) != std::string::npos || abilityName.find( "ability" ) != std::string::npos )
				abilityName = normalizeClassName( abilityName );

			if ( abilityName == "generic_hidden" || abilityName.rfind( "special_bonus_" , 0 ) == 0 )
				continue;
			// Innates stay off the tracker tiles; they are not skill-bar slots.
			if ( g_InnateAbilityNames.find( abilityName ) != g_InnateAbilityNames.end() )
				continue;

			const bool onHeroList = heroAbilities != g_HeroAbilityNames.end() &&
				heroAbilities->second.find( abilityName ) != heroAbilities->second.end();
			const bool matchesHeroPrefix = abilityName.rfind( prefixWithSep , 0 ) == 0;
			if ( !onHeroList && !matchesHeroPrefix )
				continue;

			// Hidden metadata should not drop live skill-bar abilities (facet
			// swaps like crystal_clone / ofrenda are often marked Hidden).
			if ( !onHeroList && g_HiddenAbilityNames.find( abilityName ) != g_HiddenAbilityNames.end() )
				continue;

			return abilityName;
		}
		return {};
	}

	// Source 2 C_NetworkUtlVectorBase / CUtlVector layout (see schema explorer):
	// m_Size @0x00, m_pMemory @0x08, m_nAllocationCount @0x10, m_nGrowSize @0x14.
	struct NetworkHandleVector
	{
		int32_t size = 0;
		int32_t pad = 0;
		const CHandle* data = nullptr;
		int32_t allocationCount = 0;
		int32_t growSize = 0;
	};

	auto CollectTrackedEntries( HeroVitals& vitals , C_DOTA_BaseNPC_Hero* hero ,
		const HeroVitalsOffsets& offsets ) -> void
	{
		auto* entitySystem = SDK::Interfaces::GameEntitySystem();
		if ( !entitySystem )
			return;
		const std::string heroName = vitals.name ? vitals.name : "";

		if ( offsets.hasAbilities )
		{
			// Ability bars include talents/facets and often exceed 24 entries.
			constexpr int kMaxAbilityVectorSize = 48;
			const auto* networkVector = reinterpret_cast<const NetworkHandleVector*>(
				reinterpret_cast<uintptr_t>( hero ) + offsets.abilities );

			const CHandle* handles = nullptr;
			int count = 0;
			// Primary path: networked utl vector (current client schema).
			if ( networkVector && networkVector->data &&
				networkVector->size > 0 && networkVector->size <= kMaxAbilityVectorSize )
			{
				handles = networkVector->data;
				count = networkVector->size;
			}

			if ( handles )
			{
				for ( int index = 0; index < count; ++index )
				{
					if ( !handles[index].IsValid() )
						continue;
					auto* ability = entitySystem->GetBaseEntityFromHandle( handles[index] );
					const std::string abilityName = ResolveHeroAbilityName( ability , heroName );
					if ( abilityName.empty() )
						continue;
					AddTrackedEntry( vitals , ability , TrackedEntryKind::Ability , offsets , abilityName.c_str() );
				}
			}
		}

		if ( offsets.hasInventory )
		{
			// m_hItems is schema-typed as C_NetworkUtlVectorBase, but the live
			// client stores: int32 size @+0, then an INLINE CHandle[25] @+4.
			// (m_Inventory+m_hItems .. m_bItemQueried is exactly 0x68 bytes =
			// 4 + 25*4.) Reading from +8 skips the first real item and used to
			// crash when +8..+F was treated as a heap pointer.
			constexpr int kMaxInventorySlots = 27;
			const uintptr_t itemsBase = reinterpret_cast<uintptr_t>( hero ) +
				offsets.inventory + offsets.inventoryItems;
			const int32_t reportedSize = *reinterpret_cast<const int32_t*>( itemsBase );
			const CHandle* handles = nullptr;
			int count = 0;

			if ( reportedSize > 0 && reportedSize <= kMaxInventorySlots )
			{
				handles = reinterpret_cast<const CHandle*>( itemsBase + sizeof( int32_t ) );
				count = reportedSize;
			}
			else
			{
				// Older builds exposed a bare CHandle[N] with no size header.
				handles = reinterpret_cast<const CHandle*>( itemsBase );
				count = 19;
			}

			const int slotLimit = (std::min)( count , static_cast<int>( vitals.inventoryEntries.size() ) );
			for ( int index = 0; index < slotLimit; ++index )
			{
				if ( !handles[index].IsValid() )
					continue;
				auto* item = entitySystem->GetBaseEntityFromHandle( handles[index] );
				if ( !item )
					continue;
				const char* className = item->GetSchemaClassName();
				const char* designer = nullptr;
				const char* entityName = nullptr;
				if ( auto* identity = item->pEntityIdentity() )
				{
					designer = identity->DesingerName().String();
					entityName = identity->Name().String();
				}
				const bool looksLikeItem =
					( designer && std::strncmp( designer , "item_" , 5 ) == 0 ) ||
					( entityName && std::strncmp( entityName , "item_" , 5 ) == 0 ) ||
					( className && std::strstr( className , "Item" ) != nullptr );
				if ( !looksLikeItem )
					continue;

				vitals.hasInventoryEntry[index] = BuildTrackedEntry( item , TrackedEntryKind::Item , offsets ,
					vitals.inventoryEntries[index] );
				if ( vitals.hasInventoryEntry[index] )
					AddTrackedEntry( vitals , item , TrackedEntryKind::Item , offsets );
			}
		}
	}

	auto StoreHeroVitals( HeroVitalsSnapshot& snapshot , C_DOTA_BaseNPC_Hero* hero ,
		CHandle heroHandle , int preferredPlayerId , const PlayerTeamSlotMappings& teamSlots ,
		const NetWorthMappings& netWorth , const HeroVitalsOffsets& offsets ) -> bool
	{
		if ( !hero )
			return false;

		const uint8_t team = ReadEntityField<uint8_t>( hero , offsets.team );
		const int maxHealth = ReadEntityField<int>( hero , offsets.maxHealth );
		if ( ( team != 2 && team != 3 ) || maxHealth <= 0 || maxHealth > 1000000 )
			return false;

		if ( preferredPlayerId >= 0 )
		{
			for ( const auto& existing : snapshot.heroes )
			{
				if ( existing.playerId == preferredPlayerId )
					return false;
			}
		}

		int resultIndex = preferredPlayerId >= 0 && preferredPlayerId < static_cast<int>( snapshot.heroes.size() ) ? preferredPlayerId : -1;
		if ( resultIndex < 0 || snapshot.heroes[resultIndex].playerId >= 0 )
		{
			resultIndex = -1;
			for ( int index = 0; index < static_cast<int>( snapshot.heroes.size() ); ++index )
			{
				if ( snapshot.heroes[index].playerId < 0 )
				{
					resultIndex = index;
					break;
				}
			}
		}

		if ( resultIndex < 0 )
			return false;

		auto& vitals = snapshot.heroes[resultIndex];
		if ( auto* identity = hero->pEntityIdentity() )
		{
			const char* designerName = identity->DesingerName().String();
			const char* entityName = identity->Name().String();
			vitals.name = designerName && designerName[0] ? designerName :
				( entityName && entityName[0] ? entityName : "unknown" );
		}

		vitals.playerId = preferredPlayerId >= 0 ? preferredPlayerId : resultIndex;
		vitals.handle = heroHandle;
		vitals.team = team;
		vitals.teamSlot = ResolveHeroTeamSlot( teamSlots , heroHandle , vitals.playerId , team );
		vitals.health = (std::max)( 0 , ReadEntityField<int>( hero , offsets.health ) );
		vitals.maxHealth = maxHealth;
		vitals.mana = (std::max)( 0.f , ReadEntityField<float>( hero , offsets.mana ) );
		vitals.maxMana = (std::max)( 0.f , ReadEntityField<float>( hero , offsets.maxMana ) );
		CollectTrackedEntries( vitals , hero , offsets );

		const int officialNetWorth = ResolveHeroNetWorth( netWorth , team , vitals.teamSlot , vitals.playerId );
		const int inventoryValue = ComputeInventoryNetWorth( vitals );
		const int pocketGold = ResolveHeroGold( netWorth , team , vitals.teamSlot , vitals.playerId );
		const int computedNetWorth = inventoryValue + pocketGold;
		// Live m_iNetWorth is often only populated for one local slot in-match.
		// Fall back to inventory (+ pocket gold when readable) for everyone else.
		vitals.officialNetWorth = officialNetWorth;
		vitals.pocketGold = pocketGold;
		vitals.netWorth = officialNetWorth > 0 ? officialNetWorth : computedNetWorth;
		return true;
	}

	auto CollectInPlayHeroVitals() -> HeroVitalsSnapshot
	{
		HeroVitalsSnapshot snapshot{};
		auto* entitySystem = SDK::Interfaces::GameEntitySystem();
		const auto& offsets = ResolveHeroVitalsOffsets();
		snapshot.offsetsReady = offsets.resolved;

		if ( !entitySystem || !offsets.resolved )
			return snapshot;

		// Player controllers are the authoritative roster. Walk the allocated
		// chunks directly because GetHighestEntityIndex is build-dependent in Dota.
		static auto controllerHandles = []
		{
			std::array<CHandle , 24> handles{};
			for ( auto& handle : handles )
				handle.m_Index = INVALID_EHANDLE_INDEX;
			return handles;
		}();
		static auto fallbackHeroHandles = []
		{
			std::array<CHandle , 64> handles{};
			for ( auto& handle : handles )
				handle.m_Index = INVALID_EHANDLE_INDEX;
			return handles;
		}();
		static int cachedControllerCount = 0;
		static int cachedFallbackHeroCount = 0;
		static int cachedScannedEntityCount = 0;
		static int cachedAllocatedChunkCount = 0;
		static CHandle playerResourceHandle{};
		static CHandle radiantDataHandle{};
		static CHandle direDataHandle{};
		static CHandle spectatorDataHandle{};
		static ULONGLONG nextControllerRefresh = 0;
		const ULONGLONG now = GetTickCount64();

		if ( now >= nextControllerRefresh )
		{
			for ( auto& handle : controllerHandles )
				handle.m_Index = INVALID_EHANDLE_INDEX;
			for ( auto& handle : fallbackHeroHandles )
				handle.m_Index = INVALID_EHANDLE_INDEX;
			cachedControllerCount = 0;
			cachedFallbackHeroCount = 0;
			cachedScannedEntityCount = 0;
			cachedAllocatedChunkCount = 0;
			playerResourceHandle.m_Index = INVALID_EHANDLE_INDEX;
			radiantDataHandle.m_Index = INVALID_EHANDLE_INDEX;
			direDataHandle.m_Index = INVALID_EHANDLE_INDEX;
			spectatorDataHandle.m_Index = INVALID_EHANDLE_INDEX;

			for ( int chunkIndex = 0; chunkIndex < MAX_ENTITY_LISTS; ++chunkIndex )
			{
				auto* chunk = entitySystem->m_pIdentityChunks[chunkIndex];
				if ( !chunk )
					continue;
				++cachedAllocatedChunkCount;

				for ( int entryIndex = 0; entryIndex < MAX_ENTITIES_IN_LIST; ++entryIndex )
				{
					auto* identity = &chunk->m_pIdentities[entryIndex];
					auto* entity = identity->pBaseEntity();
					if ( !entity )
						continue;
					++cachedScannedEntityCount;

					const char* className = entity->GetSchemaClassName();
					const char* designerName = identity->DesingerName().String();
					const char* entityName = identity->Name().String();
					if ( LooksLikePlayerResourceName( className ) )
						playerResourceHandle = identity->Handle();
					else if ( LooksLikePlayerControllerName( className ) && cachedControllerCount < static_cast<int>( controllerHandles.size() ) )
						controllerHandles[cachedControllerCount++] = identity->Handle();
					else if ( ( LooksLikeHeroEntityName( className ) ||
						( designerName && std::strstr( designerName , "npc_dota_hero_" ) != nullptr ) ) &&
						cachedFallbackHeroCount < static_cast<int>( fallbackHeroHandles.size() ) )
						fallbackHeroHandles[cachedFallbackHeroCount++] = identity->Handle();

					TryClassifyTeamDataEntity( entity , identity , offsets ,
						radiantDataHandle , direDataHandle , spectatorDataHandle );
				}
			}

			nextControllerRefresh = now + 1000;
		}
		snapshot.controllerCount = cachedControllerCount;
		snapshot.fallbackHeroCount = cachedFallbackHeroCount;
		snapshot.scannedEntityCount = cachedScannedEntityCount;
		snapshot.allocatedChunkCount = cachedAllocatedChunkCount;
		auto* playerResource = playerResourceHandle.IsValid() ?
			entitySystem->GetBaseEntityFromHandle( playerResourceHandle ) : nullptr;
		auto* radiantData = radiantDataHandle.IsValid() ?
			entitySystem->GetBaseEntityFromHandle( radiantDataHandle ) : nullptr;
		auto* direData = direDataHandle.IsValid() ?
			entitySystem->GetBaseEntityFromHandle( direDataHandle ) : nullptr;
		auto* spectatorData = spectatorDataHandle.IsValid() ?
			entitySystem->GetBaseEntityFromHandle( spectatorDataHandle ) : nullptr;
		const auto teamSlots = ResolvePlayerTeamSlots( playerResource , offsets );
		const auto netWorth = ResolveNetWorthMappings( radiantData , direData , spectatorData , teamSlots , offsets );
		int bestLocalScore = 0;
		uint8_t bestLocalTeam = 0;

		for ( int controllerIndex = 0; controllerIndex < cachedControllerCount; ++controllerIndex )
		{
			const CHandle controllerHandle = controllerHandles[controllerIndex];
			if ( !controllerHandle.IsValid() )
				continue;

			auto* controller = static_cast<C_DOTAPlayerController*>( entitySystem->GetBaseEntityFromHandle( controllerHandle ) );
			if ( !controller )
				continue;

			const CHandle heroHandle = ReadEntityField<CHandle>( controller , offsets.assignedHero );
			if ( !heroHandle.IsValid() )
				continue;

			auto* hero = static_cast<C_DOTA_BaseNPC_Hero*>( entitySystem->GetBaseEntityFromHandle( heroHandle ) );
			if ( !hero )
				continue;
			++snapshot.assignedHeroCount;

			const int playerId = offsets.hasPlayerId ? ReadEntityField<int32_t>( controller , offsets.playerId ) : -1;
			StoreHeroVitals( snapshot , hero , heroHandle , playerId , teamSlots , netWorth , offsets );

			const int localScore = ScoreLocalController( controller , offsets );
			if ( localScore > bestLocalScore )
			{
				const uint8_t team = ReadEntityField<uint8_t>( hero , offsets.team );
				if ( team == 2 || team == 3 )
				{
					bestLocalScore = localScore;
					bestLocalTeam = team;
				}
			}
		}

		// Some Dota builds do not expose controllers through the client entity
		// chunks. Fall back to real hero entities and reject illusions/clones.
		for ( int heroIndex = 0; heroIndex < cachedFallbackHeroCount; ++heroIndex )
		{
			const CHandle heroHandle = fallbackHeroHandles[heroIndex];
			auto* hero = static_cast<C_DOTA_BaseNPC_Hero*>( entitySystem->GetBaseEntityFromHandle( heroHandle ) );
			if ( !hero )
				continue;
			if ( offsets.hasIsIllusion && ReadEntityField<bool>( hero , offsets.isIllusion ) )
				continue;
			if ( offsets.hasIsClone && ReadEntityField<bool>( hero , offsets.isClone ) )
				continue;

			const int playerId = offsets.hasHeroPlayerId ? ReadEntityField<int32_t>( hero , offsets.heroPlayerId ) :
				( offsets.hasPlayerOwnerId ? ReadEntityField<int32_t>( hero , offsets.playerOwnerId ) : -1 );
			StoreHeroVitals( snapshot , hero , heroHandle , playerId , teamSlots , netWorth , offsets );
		}

		if ( auto* localController = CGameEntitySystem::GetLocalPlayerController() )
			snapshot.localTeam = TeamFromController( localController , snapshot , offsets );
		if ( snapshot.localTeam != 2 && snapshot.localTeam != 3 && bestLocalTeam != 0 )
			snapshot.localTeam = bestLocalTeam;
		if ( snapshot.localTeam != 2 && snapshot.localTeam != 3 )
		{
			if ( netWorth.hasRadiant && !netWorth.hasDire )
				snapshot.localTeam = 2;
			else if ( netWorth.hasDire && !netWorth.hasRadiant )
				snapshot.localTeam = 3;
			else
				snapshot.localTeam = InferLocalTeamFromRoster( snapshot );
		}

		return snapshot;
	}

	auto DrawVitalsBar( ImDrawList* drawList , const ImVec2& min , const ImVec2& size ,
		float value , float maximum , ImU32 fillColor ) -> void
	{
		const ImVec2 bottomRight( min.x + size.x , min.y + size.y );
		drawList->AddRectFilled( min , bottomRight , IM_COL32( 12 , 14 , 20 , 230 ) );

		const float ratio = maximum > 0.f ? std::clamp( value / maximum , 0.f , 1.f ) : 0.f;
		if ( ratio > 0.f )
			drawList->AddRectFilled( ImVec2( min.x + 1.f , min.y + 1.f ) ,
				ImVec2( min.x + 1.f + ( size.x - 2.f ) * ratio , bottomRight.y - 1.f ) , fillColor );

		drawList->AddRect( min , bottomRight , IM_COL32( 4 , 5 , 8 , 255 ) );

		char text[16]{};
		std::snprintf( text , sizeof( text ) , "%d" , static_cast<int>( std::round( value ) ) );
		const float fontSize = (std::max)( 9.f , size.y * 0.94f );
		const ImVec2 textSize = ImGui::GetFont()->CalcTextSizeA( fontSize , FLT_MAX , 0.f , text );
		const ImVec2 textPos( min.x + ( size.x - textSize.x ) * 0.5f , min.y + ( size.y - textSize.y ) * 0.5f );

		// Outline every side of the glyphs so the values remain readable over
		// both the bright filled portion and the nearly black empty portion.
		const ImU32 outlineColor = IM_COL32( 0 , 0 , 0 , 255 );
		drawList->AddText( ImGui::GetFont() , fontSize , ImVec2( textPos.x - 1.f , textPos.y ) , outlineColor , text );
		drawList->AddText( ImGui::GetFont() , fontSize , ImVec2( textPos.x + 1.f , textPos.y ) , outlineColor , text );
		drawList->AddText( ImGui::GetFont() , fontSize , ImVec2( textPos.x , textPos.y - 1.f ) , outlineColor , text );
		drawList->AddText( ImGui::GetFont() , fontSize , ImVec2( textPos.x , textPos.y + 1.f ) , outlineColor , text );
		drawList->AddText( ImGui::GetFont() , fontSize , textPos , IM_COL32( 245 , 245 , 245 , 255 ) , text );
	}

	auto FindTrackedEntry( const HeroVitals& hero , const std::string& name ) -> const TrackedEntry*
	{
		for ( int index = 0; index < hero.trackedEntryCount; ++index )
		{
			if ( hero.trackedEntries[index].name == name )
				return &hero.trackedEntries[index];
		}
		return nullptr;
	}

	auto LoadTrackedIconTexture( const std::wstring& path , TrackedIcon& icon ) -> bool
	{
		auto* device = GetAndromedaGUI()->GetDevice();
		if ( !device )
			return false;

		static Microsoft::WRL::ComPtr<IWICImagingFactory> factory;
		if ( !factory && FAILED( CoCreateInstance( CLSID_WICImagingFactory , nullptr , CLSCTX_INPROC_SERVER ,
			IID_PPV_ARGS( &factory ) ) ) )
			return false;

		Microsoft::WRL::ComPtr<IWICBitmapDecoder> decoder;
		if ( FAILED( factory->CreateDecoderFromFilename( path.c_str() , nullptr , GENERIC_READ ,
			WICDecodeMetadataCacheOnLoad , &decoder ) ) )
			return false;

		Microsoft::WRL::ComPtr<IWICBitmapFrameDecode> frame;
		Microsoft::WRL::ComPtr<IWICFormatConverter> converter;
		if ( FAILED( decoder->GetFrame( 0 , &frame ) ) || FAILED( factory->CreateFormatConverter( &converter ) ) ||
			FAILED( converter->Initialize( frame.Get() , GUID_WICPixelFormat32bppRGBA , WICBitmapDitherTypeNone ,
				nullptr , 0.f , WICBitmapPaletteTypeCustom ) ) )
			return false;

		UINT width = 0;
		UINT height = 0;
		if ( FAILED( converter->GetSize( &width , &height ) ) || width == 0 || height == 0 )
			return false;

		const UINT stride = width * 4;
		std::vector<BYTE> pixels( static_cast<size_t>( stride ) * height );
		if ( FAILED( converter->CopyPixels( nullptr , stride , static_cast<UINT>( pixels.size() ) , pixels.data() ) ) )
			return false;

		D3D11_TEXTURE2D_DESC description{};
		description.Width = width;
		description.Height = height;
		description.MipLevels = 1;
		description.ArraySize = 1;
		description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		description.SampleDesc.Count = 1;
		description.Usage = D3D11_USAGE_DEFAULT;
		description.BindFlags = D3D11_BIND_SHADER_RESOURCE;

		D3D11_SUBRESOURCE_DATA initialData{};
		initialData.pSysMem = pixels.data();
		initialData.SysMemPitch = stride;
		Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
		if ( FAILED( device->CreateTexture2D( &description , &initialData , &texture ) ) ||
			FAILED( device->CreateShaderResourceView( texture.Get() , nullptr , &icon.srv ) ) )
			return false;

		icon.width = static_cast<int>( width );
		icon.height = static_cast<int>( height );
		return true;
	}

	auto TrackedIconAssetName( const TrackedEntry& entry ) -> std::string
	{
		std::string name = entry.name;
		if ( entry.kind == TrackedEntryKind::Item && name.rfind( "item_" , 0 ) == 0 )
			name.erase( 0 , 5 );
		name.erase( std::remove_if( name.begin() , name.end() , []( const unsigned char value )
		{
			return !std::isalnum( value ) && value != '_';
		} ) , name.end() );
		return name;
	}

	auto GetAssetIcon( const char* folder , const std::string& assetName ,
		const std::string& remoteAssetName = {} ) -> TrackedIcon*
	{
		if ( assetName.empty() )
			return nullptr;

		const std::string key = std::string( folder ) + "/" + assetName;
		auto [iterator , inserted] = g_TrackedIcons.try_emplace( key );
		auto& icon = iterator->second;
		if ( inserted )
		{
			const std::string path = GetDllDir() + "Assets\\icons\\" + folder + "\\" + assetName + ".png";
			const std::string urlName = remoteAssetName.empty() ? assetName : remoteAssetName;
			const std::string url = "https://cdn.cloudflare.steamstatic.com/apps/dota2/images/dota_react/" +
				std::string( folder ) + "/" + urlName + ".png";
			icon.path.assign( path.begin() , path.end() );
			icon.url.assign( url.begin() , url.end() );
			if ( std::filesystem::exists( icon.path ) && LoadTrackedIconTexture( icon.path , icon ) )
				icon.state = TrackedIconState::Ready;
		}

		if ( icon.state == TrackedIconState::Ready && icon.srv )
			return &icon;
		return nullptr;
	}

	auto GetTrackedIcon( const TrackedEntry& entry ) -> TrackedIcon*
	{
		return GetAssetIcon( entry.kind == TrackedEntryKind::Item ? "items" : "abilities" ,
			TrackedIconAssetName( entry ) );
	}

	auto GetHeroIcon( const HeroVitals& hero ) -> TrackedIcon*
	{
		std::string assetName = hero.name ? hero.name : "";
		assetName.erase( std::remove_if( assetName.begin() , assetName.end() , []( const unsigned char value )
		{
			return !std::isalnum( value ) && value != '_';
		} ) , assetName.end() );
		std::string remoteAssetName = assetName;
		constexpr const char* heroPrefix = "npc_dota_hero_";
		if ( remoteAssetName.rfind( heroPrefix , 0 ) == 0 )
			remoteAssetName.erase( 0 , std::strlen( heroPrefix ) );
		return GetAssetIcon( "heroes" , assetName , remoteAssetName );
	}

	auto UpdateTrackedIconDownloads() -> void
	{
		int activeDownloads = 0;
		for ( auto& [key , icon] : g_TrackedIcons )
		{
			if ( icon.state != TrackedIconState::Downloading )
				continue;
			if ( icon.download.valid() && icon.download.wait_for( std::chrono::seconds( 0 ) ) == std::future_status::ready )
			{
				const bool downloaded = icon.download.get();
				icon.state = downloaded && LoadTrackedIconTexture( icon.path , icon ) ?
					TrackedIconState::Ready : TrackedIconState::Failed;
			}
			else
			{
				++activeDownloads;
			}
		}

		for ( auto& [key , icon] : g_TrackedIcons )
		{
			if ( activeDownloads >= 4 )
				break;
			if ( icon.state != TrackedIconState::Queued )
				continue;

			const std::wstring url = icon.url;
			const std::wstring path = icon.path;
			icon.state = TrackedIconState::Downloading;
			icon.download = std::async( std::launch::async , [url , path]
			{
				return CHeroDataLoader::DownloadFileWinHttp( url , path );
			} );
			++activeDownloads;
		}
	}

	auto DrawTrackedEntryTile( const char* id , const TrackedEntry* entry , const ImVec2& size ) -> bool
	{
		const ImVec2 min = ImGui::GetCursorScreenPos();
		const bool clicked = ImGui::InvisibleButton( id , size );
		const ImVec2 bottomRight( min.x + size.x , min.y + size.y );
		ImDrawList* drawList = ImGui::GetWindowDrawList();

		ImU32 color = IM_COL32( 30 , 34 , 43 , 245 );
		if ( entry )
		{
			uint32_t hash = 2166136261u;
			for ( const unsigned char character : entry->name )
				hash = ( hash ^ character ) * 16777619u;
			const int red = 45 + static_cast<int>( hash & 0x3f );
			const int green = 55 + static_cast<int>( ( hash >> 8 ) & 0x4f );
			const int blue = 70 + static_cast<int>( ( hash >> 16 ) & 0x5f );
			color = IM_COL32( red , green , blue , 255 );
		}
		drawList->AddRectFilled( min , bottomRight , color , 3.f );
		drawList->AddRect( min , bottomRight , ImGui::IsItemHovered() ? IM_COL32( 115 , 232 , 124 , 255 ) :
			IM_COL32( 128 , 139 , 157 , 230 ) , 3.f , 0 , ImGui::IsItemHovered() ? 2.f : 1.f );

		if ( entry )
		{
			if ( auto* icon = GetTrackedIcon( *entry ); icon && icon->srv )
			{
				drawList->AddImage( reinterpret_cast<ImTextureID>( icon->srv ) , min , bottomRight );
			}

			if ( entry->cooldown > 0.05f )
			{
				drawList->AddRectFilled( min , bottomRight , IM_COL32( 3 , 5 , 9 , 155 ) , 3.f );
				char cooldown[16]{};
				std::snprintf( cooldown , sizeof( cooldown ) , entry->cooldown < 10.f ? "%.1f" : "%.0f" , entry->cooldown );
				const ImVec2 cooldownSize = ImGui::CalcTextSize( cooldown );
				drawList->AddText( ImVec2( min.x + ( size.x - cooldownSize.x ) * 0.5f ,
					min.y + ( size.y - cooldownSize.y ) * 0.5f ) , IM_COL32( 255 , 245 , 225 , 255 ) , cooldown );
			}

			if ( entry->kind == TrackedEntryKind::Item )
			{
				char count[16]{};
				std::snprintf( count , sizeof( count ) , "%d" , (std::max)( 1 , entry->count ) );
				drawList->AddText( ImVec2( min.x + 2.f , min.y + 1.f ) , IM_COL32( 255 , 255 , 255 , 255 ) , count );
			}
		}
		drawList->AddRect( min , bottomRight , ImGui::IsItemHovered() ? IM_COL32( 115 , 232 , 124 , 255 ) :
			IM_COL32( 128 , 139 , 157 , 230 ) , 3.f , 0 , ImGui::IsItemHovered() ? 2.f : 1.f );

		return clicked;
	}

	auto DrawHeroTrackedEntries( const HeroVitals& hero , int stateIndex , const ImVec2& anchor , float portraitWidth ) -> void
	{
		if ( stateIndex < 0 || stateIndex >= 24 || hero.trackedEntryCount <= 0 )
			return;

		static std::array<std::array<std::string , 2> , 24> selected{};
		static std::array<int , 24> openSlot = [] { std::array<int , 24> value{}; value.fill( -1 ); return value; }();

		for ( int slot = 0; slot < 2; ++slot )
		{
			if ( FindTrackedEntry( hero , selected[stateIndex][slot] ) )
				continue;
			const TrackedEntryKind preferred = slot == 0 ? TrackedEntryKind::Ability : TrackedEntryKind::Item;
			selected[stateIndex][slot].clear();
			for ( int index = 0; index < hero.trackedEntryCount; ++index )
			{
				if ( hero.trackedEntries[index].kind == preferred )
				{
					selected[stateIndex][slot] = hero.trackedEntries[index].name;
					break;
				}
			}
			if ( selected[stateIndex][slot].empty() && hero.trackedEntryCount > slot )
				selected[stateIndex][slot] = hero.trackedEntries[slot].name;
		}

		const float tileSize = (std::clamp)( portraitWidth * 0.43f , 18.f , 30.f );
		const float gap = 2.f;
		const float selectedWidth = tileSize * 2.f + gap;
		const bool expanded = openSlot[stateIndex] >= 0;
		const float listWidth = tileSize;
		const int visibleRows = hero.trackedEntryCount;
		const float listOriginX = expanded ? openSlot[stateIndex] * ( tileSize + gap ) : 0.f;
		const float windowWidth = expanded ? (std::max)( selectedWidth , listOriginX + listWidth ) : selectedWidth;
		const float windowHeight = expanded ? tileSize + gap + visibleRows * ( tileSize + gap ) : tileSize;

		char windowName[48]{};
		std::snprintf( windowName , sizeof( windowName ) , "##hero-tracker-%d" , stateIndex );
		ImGui::SetNextWindowPos( anchor );
		ImGui::SetNextWindowSize( ImVec2( windowWidth , windowHeight ) );
		const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
			ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoBackground;
		if ( !ImGui::Begin( windowName , nullptr , flags ) )
		{
			ImGui::End();
			return;
		}

		for ( int slot = 0; slot < 2; ++slot )
		{
			ImGui::SetCursorPos( ImVec2( slot * ( tileSize + gap ) , 0.f ) );
			char id[24]{};
			std::snprintf( id , sizeof( id ) , "##slot-%d" , slot );
			if ( DrawTrackedEntryTile( id , FindTrackedEntry( hero , selected[stateIndex][slot] ) , ImVec2( tileSize , tileSize ) ) )
				openSlot[stateIndex] = openSlot[stateIndex] == slot ? -1 : slot;
		}

		if ( expanded )
		{
			for ( int index = 0; index < visibleRows; ++index )
			{
				const auto& entry = hero.trackedEntries[index];
				const float rowY = tileSize + gap + index * ( tileSize + gap );
				ImGui::SetCursorPos( ImVec2( listOriginX , rowY ) );
				char id[24]{};
				std::snprintf( id , sizeof( id ) , "##candidate-%d" , index );
				if ( DrawTrackedEntryTile( id , &entry , ImVec2( tileSize , tileSize ) ) )
				{
					selected[stateIndex][openSlot[stateIndex]] = entry.name;
					openSlot[stateIndex] = -1;
					break;
				}
			}
		}

		ImGui::End();
	}

	auto ResolveLocalTeam( const HeroVitalsSnapshot& snapshot ) -> uint8_t
	{
		static uint8_t lastKnownTeam = 0;
		const auto& offsets = ResolveHeroVitalsOffsets();
		uint8_t localTeam = snapshot.localTeam;

		if ( localTeam != 2 && localTeam != 3 && offsets.resolved )
			localTeam = TeamFromController( CGameEntitySystem::GetLocalPlayerController() , snapshot , offsets );

		if ( localTeam == 2 || localTeam == 3 )
		{
			if ( lastKnownTeam != localTeam )
			{
				DEV_LOG( "[side-panels] localTeam=%u (snapshot=%u last=%u)\n" ,
					static_cast<unsigned>( localTeam ) , static_cast<unsigned>( snapshot.localTeam ) ,
					static_cast<unsigned>( lastKnownTeam ) );
			}
			lastKnownTeam = localTeam;
			return localTeam;
		}

		if ( lastKnownTeam == 2 || lastKnownTeam == 3 )
			return lastKnownTeam;

		static bool loggedMissing = false;
		if ( !loggedMissing )
		{
			DEV_LOG( "[side-panels] local team unresolved controllers=%d assigned=%d\n" ,
				snapshot.controllerCount , snapshot.assignedHeroCount );
			loggedMissing = true;
		}
		return 0;
	}

	auto DrawHeroSidePanels() -> void
	{
		if ( !Settings::InfoOverlay::SidePanelsEnabled ||
			( !Settings::InfoOverlay::ShowNetworth && !Settings::InfoOverlay::ShowItems ) )
			return;

		const auto snapshot = CollectInPlayHeroVitals();
		UpdateTrackedIconDownloads();
		const uint8_t localTeam = ResolveLocalTeam( snapshot );
		const ImVec2 display = ImGui::GetIO().DisplaySize;
		if ( display.x <= 0.f || display.y <= 0.f )
			return;

		std::array<const HeroVitals* , 10> ordered{};
		int heroCount = 0;
		for ( const auto& hero : snapshot.heroes )
		{
			if ( hero.playerId >= 0 && hero.maxHealth > 0 && ( hero.team == 2 || hero.team == 3 ) )
				ordered[heroCount++] = &hero;
		}
		// Match Health/Mana slider timing: keep these panels hidden in menu/lobby
		// until in-play heroes exist (the same gate that makes vitals bars appear).
		if ( heroCount == 0 )
			return;

		std::sort( ordered.begin() , ordered.begin() + heroCount , []( const HeroVitals* left , const HeroVitals* right )
		{
			return left->netWorth > right->netWorth;
		} );

		const float initialX = 8.f;
		const float initialY = (std::max)( 110.f , display.y * 0.22f );
		constexpr float netWidth = 178.f;
		constexpr float rowHeight = 25.f;
		const ImGuiWindowFlags panelFlags = ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoResize |
			ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoNav |
			ImGuiWindowFlags_NoFocusOnAppearing;

		ImGui::PushStyleVar( ImGuiStyleVar_WindowPadding , ImVec2( 4.f , 4.f ) );
		ImGui::PushStyleVar( ImGuiStyleVar_WindowRounding , 2.f );
		ImGui::PushStyleColor( ImGuiCol_WindowBg , IM_COL32( 13 , 16 , 24 , 225 ) );
		ImGui::PushStyleColor( ImGuiCol_TitleBg , IM_COL32( 19 , 22 , 31 , 238 ) );
		ImGui::PushStyleColor( ImGuiCol_TitleBgActive , IM_COL32( 28 , 33 , 45 , 248 ) );
		ImGui::PushStyleColor( ImGuiCol_TitleBgCollapsed , IM_COL32( 19 , 22 , 31 , 238 ) );
		ImGui::PushStyleColor( ImGuiCol_Border , IM_COL32( 49 , 55 , 68 , 230 ) );

		if ( Settings::InfoOverlay::ShowNetworth )
		{
			ImGui::SetNextWindowPos( ImVec2( initialX , initialY ) , ImGuiCond_FirstUseEver );
			if ( ImGui::Begin( "Net Worth##andromeda-side-networth" , nullptr , panelFlags ) )
			{
				ImDrawList* drawList = ImGui::GetWindowDrawList();
				const ImVec2 contentMin = ImGui::GetCursorScreenPos();
				const int maximum = (std::max)( 1 , ordered[0]->netWorth );
				for ( int index = 0; index < heroCount; ++index )
				{
					const auto& hero = *ordered[index];
					const ImVec2 rowMin( contentMin.x , contentMin.y + index * rowHeight );
					drawList->AddRectFilled( rowMin , ImVec2( rowMin.x + netWidth , rowMin.y + rowHeight - 1.f ) ,
						IM_COL32( 13 , 16 , 24 , 220 ) );
					const ImVec2 portraitMax( rowMin.x + 36.f , rowMin.y + rowHeight - 1.f );
					if ( auto* icon = GetHeroIcon( hero ); icon && icon->srv )
						drawList->AddImage( reinterpret_cast<ImTextureID>( icon->srv ) , rowMin , portraitMax );
					else
						drawList->AddRectFilled( rowMin , portraitMax , IM_COL32( 47 , 53 , 66 , 255 ) );

					const float barLeft = rowMin.x + 40.f;
					const float barRight = rowMin.x + netWidth - 7.f;
					const float ratio = (std::clamp)( static_cast<float>( hero.netWorth ) / maximum , 0.f , 1.f );
					const ImU32 teamColor = hero.team == localTeam ?
						IM_COL32( 91 , 166 , 54 , 235 ) : IM_COL32( 179 , 48 , 43 , 235 );
					drawList->AddRectFilled( ImVec2( barLeft , rowMin.y + 9.f ) , ImVec2( barRight , rowMin.y + 15.f ) ,
						IM_COL32( 39 , 43 , 52 , 245 ) , 3.f );
					drawList->AddRectFilled( ImVec2( barLeft , rowMin.y + 9.f ) ,
						ImVec2( barLeft + ( barRight - barLeft ) * ratio , rowMin.y + 15.f ) , teamColor , 3.f );
					char value[24]{};
					std::snprintf( value , sizeof( value ) , "%d" , hero.netWorth );
					drawList->AddText( ImVec2( barLeft + 1.f , rowMin.y + 3.f ) , IM_COL32( 0 , 0 , 0 , 255 ) , value );
					drawList->AddText( ImVec2( barLeft , rowMin.y + 2.f ) , IM_COL32( 245 , 245 , 245 , 255 ) , value );
				}
				ImGui::Dummy( ImVec2( netWidth , heroCount * rowHeight ) );
			}
			ImGui::End();
		}

		if ( Settings::InfoOverlay::ShowItems )
		{
			std::array<const HeroVitals* , 5> enemies{};
			int enemyCount = 0;
			if ( localTeam == 2 || localTeam == 3 )
			{
				for ( int index = 0; index < heroCount &&
					enemyCount < static_cast<int>( enemies.size() ); ++index )
				{
					if ( ordered[index]->team != localTeam )
						enemies[enemyCount++] = ordered[index];
				}
			}
			constexpr float itemWidth = 218.f;
			const float inventoryY = initialY + 34.f + (std::max)( 1 , heroCount ) * rowHeight;
			ImGui::SetNextWindowPos( ImVec2( initialX , inventoryY ) , ImGuiCond_FirstUseEver );
			if ( ImGui::Begin( "Enemy Items##andromeda-side-items" , nullptr , panelFlags ) )
			{
				if ( enemyCount > 0 )
				{
					ImDrawList* drawList = ImGui::GetWindowDrawList();
					const ImVec2 contentMin = ImGui::GetCursorScreenPos();
					for ( int row = 0; row < enemyCount; ++row )
					{
						const auto& hero = *enemies[row];
						const ImVec2 rowMin( contentMin.x , contentMin.y + row * rowHeight );
						drawList->AddRectFilled( rowMin , ImVec2( rowMin.x + itemWidth , rowMin.y + rowHeight - 1.f ) ,
							IM_COL32( 13 , 16 , 24 , 225 ) );
						if ( auto* icon = GetHeroIcon( hero ); icon && icon->srv )
							drawList->AddImage( reinterpret_cast<ImTextureID>( icon->srv ) , rowMin ,
								ImVec2( rowMin.x + 36.f , rowMin.y + rowHeight - 1.f ) );
						for ( int slot = 0; slot < 6; ++slot )
						{
							const float itemX = rowMin.x + 39.f + slot * 29.5f;
							const ImVec2 itemMin( itemX , rowMin.y + 1.f );
							const ImVec2 itemMax( itemX + 27.f , rowMin.y + rowHeight - 2.f );
							drawList->AddRectFilled( itemMin , itemMax , IM_COL32( 34 , 39 , 49 , 255 ) , 2.f );
							if ( hero.hasInventoryEntry[slot] )
							{
								const auto& entry = hero.inventoryEntries[slot];
								if ( auto* icon = GetTrackedIcon( entry ); icon && icon->srv )
									drawList->AddImage( reinterpret_cast<ImTextureID>( icon->srv ) , itemMin , itemMax );
								if ( entry.count > 1 )
								{
									char count[12]{};
									std::snprintf( count , sizeof( count ) , "%d" , entry.count );
									drawList->AddText( ImVec2( itemMin.x + 2.f , itemMin.y ) ,
										IM_COL32( 255 , 255 , 255 , 255 ) , count );
								}
							}
							drawList->AddRect( itemMin , itemMax , IM_COL32( 91 , 101 , 119 , 230 ) , 2.f );
						}
					}
					ImGui::Dummy( ImVec2( itemWidth , enemyCount * rowHeight ) );
				}
				else
				{
					ImGui::TextDisabled( localTeam == 0 ? "Waiting for local team..." : "Waiting for enemy data..." );
					ImGui::Dummy( ImVec2( itemWidth , 1.f ) );
				}
			}
			ImGui::End();
		}

		ImGui::PopStyleColor( 5 );
		ImGui::PopStyleVar( 2 );
	}

	auto DrawHeroVitalsOverlay() -> void
	{
		// The master Info Overlay switch owns all HP/Mana bars. Avoid collecting
		// hero data as well as drawing it while the feature is disabled.
		if ( !Settings::InfoOverlay::TopOverlayEnabled )
			return;

		const auto snapshot = CollectInPlayHeroVitals();
		UpdateTrackedIconDownloads();
		const auto& heroes = snapshot.heroes;
		const ImVec2 display = ImGui::GetIO().DisplaySize;

		if ( display.x <= 0.f || display.y <= 0.f )
			return;

		// These proportions follow Dota's centered top-bar layout and scale with
		// resolution: five portraits, a scoreboard gap, then five portraits.
		const float portraitWidth = display.x * 0.03275f;
		const float teamWidth = portraitWidth * 5.f;
		const float scoreboardGap = display.x * 0.107f;
		// Dota's compact top bar ends at roughly 1/16 of the render height for
		// this HUD layout. Keep the vitals attached to that edge, not the old
		// lower screen-relative estimate that left a large vertical gap.
		const float portraitBottom = display.y * 0.0625f;
		const float barHeight = (std::max)( 9.f , display.y * 0.0105f );
		// Pull the bars up to the portrait edge. The previous +1 anchor left a
		// visible strip of world/HUD background between the two elements.
		// The compact top-bar artwork ends substantially above the old estimated
		// portraitBottom coordinate. Account for that built-in HUD padding so the
		// first bar begins directly beneath the visible portrait frame.
		const float portraitOverlapCorrection = (std::max)( 16.f , display.y * 0.0225f );
		const float barTop = portraitBottom - portraitOverlapCorrection;
		const float leftStart = display.x * 0.5f - scoreboardGap * 0.5f - teamWidth;
		const float rightStart = display.x * 0.5f + scoreboardGap * 0.5f;
		ImDrawList* drawList = ImGui::GetForegroundDrawList();
		const uint8_t localTeam = ResolveLocalTeam( snapshot );

		// Reserve every valid team slot first. Then place unresolved or duplicate
		// entries into the remaining holes. This prevents a fallback hero from
		// colliding with a later resolved hero and does not assume faction-specific
		// player-ID ranges.
		std::array<int , 10> assignedPortraitSlots{};
		assignedPortraitSlots.fill( -1 );
		std::array<bool , 5> radiantSlotsUsed{};
		std::array<bool , 5> direSlotsUsed{};
		for ( int index = 0; index < static_cast<int>( heroes.size() ); ++index )
		{
			const auto& hero = heroes[index];
			if ( hero.playerId < 0 || hero.maxHealth <= 0 || hero.teamSlot < 0 || hero.teamSlot >= 5 )
				continue;

			auto* usedSlots = hero.team == 2 ? &radiantSlotsUsed : hero.team == 3 ? &direSlotsUsed : nullptr;
			if ( usedSlots && !( *usedSlots )[hero.teamSlot] )
			{
				assignedPortraitSlots[index] = hero.teamSlot;
				( *usedSlots )[hero.teamSlot] = true;
			}
		}

		for ( int index = 0; index < static_cast<int>( heroes.size() ); ++index )
		{
			const auto& hero = heroes[index];
			if ( hero.playerId < 0 || hero.maxHealth <= 0 || assignedPortraitSlots[index] >= 0 )
				continue;

			auto* usedSlots = hero.team == 2 ? &radiantSlotsUsed : hero.team == 3 ? &direSlotsUsed : nullptr;
			if ( !usedSlots )
				continue;
			for ( int slot = 0; slot < 5; ++slot )
			{
				if ( !( *usedSlots )[slot] )
				{
					assignedPortraitSlots[index] = slot;
					( *usedSlots )[slot] = true;
					break;
				}
			}
		}

		for ( int index = 0; index < static_cast<int>( heroes.size() ); ++index )
		{
			const auto& hero = heroes[index];
			if ( hero.playerId < 0 || hero.maxHealth <= 0 )
				continue;
			// Show On Allies is a child of the master switch. The early return above
			// handles the parent state; this filter handles the child state.
			if ( !Settings::InfoOverlay::ShowOnAllies && localTeam >= 2 && localTeam <= 3 && hero.team == localTeam )
				continue;

			const int portraitSlot = assignedPortraitSlots[index];
			float groupStart = 0.f;
			if ( hero.team == 2 )
				groupStart = leftStart;
			else if ( hero.team == 3 )
				groupStart = rightStart;
			else
				continue;

			if ( portraitSlot < 0 || portraitSlot >= 5 )
				continue;

			const float barInset = 1.f;
			const float x = groupStart + portraitWidth * portraitSlot + barInset;
			const ImVec2 barSize( portraitWidth - barInset * 2.f , barHeight );
			DrawVitalsBar( drawList , ImVec2( x , barTop ) , barSize ,
				static_cast<float>( hero.health ) , static_cast<float>( hero.maxHealth ) , IM_COL32( 205 , 55 , 55 , 255 ) );
			DrawVitalsBar( drawList , ImVec2( x , barTop + barHeight ) , barSize ,
				hero.mana , hero.maxMana , IM_COL32( 65 , 112 , 205 , 255 ) );

			const int trackerStateIndex = hero.playerId >= 0 && hero.playerId < 24 ? hero.playerId :
				( hero.team == 2 ? portraitSlot : portraitSlot + 5 );
			const float trackerTileSize = (std::clamp)( portraitWidth * 0.43f , 18.f , 30.f );
			const float trackerWidth = trackerTileSize * 2.f + 2.f;
			DrawHeroTrackedEntries( hero , trackerStateIndex ,
				ImVec2( x + ( barSize.x - trackerWidth ) * 0.5f ,
					barTop + barHeight * 2.f + 2.f ) , portraitWidth );
		}
	}

	auto TryReadEntityOrigin( C_BaseEntity* entity , Vector3& outOrigin ) -> bool
	{
		static uint32_t sceneNodeOffset = 0;
		static uint32_t absOriginOffset = 0;
		static bool offsetsResolved = false;
		static bool offsetsValid = false;

		if ( !offsetsResolved )
		{
			auto* schema = GetSchemaOffset();
			offsetsValid = schema &&
				schema->TryGetOffset( "C_BaseEntity" , "m_pGameSceneNode" , sceneNodeOffset ) &&
				schema->TryGetOffset( "CGameSceneNode" , "m_vecAbsOrigin" , absOriginOffset );
			offsetsResolved = true;
			DEV_LOG( "[visible-by-enemy] origin offsets valid=%d sceneNode=0x%X absOrigin=0x%X\n" ,
				offsetsValid ? 1 : 0 , sceneNodeOffset , absOriginOffset );
		}

		if ( !offsetsValid || !entity )
			return false;

		auto* sceneNode = *reinterpret_cast<void**>( reinterpret_cast<uintptr_t>( entity ) + sceneNodeOffset );
		if ( !sceneNode )
			return false;

		outOrigin = *reinterpret_cast<Vector3*>( reinterpret_cast<uintptr_t>( sceneNode ) + absOriginOffset );
		return std::isfinite( outOrigin.m_x ) && std::isfinite( outOrigin.m_y ) && std::isfinite( outOrigin.m_z );
	}

	constexpr float kTrueSightRadiusGem = 900.f;
	constexpr float kTrueSightRadiusSentry = 1050.f;
	constexpr float kTrueSightRadiusTower = 700.f;

	enum class TrueSightKind : uint8_t
	{
		Gem ,
		Sentry ,
		Tower
	};

	struct TrueSightSource
	{
		CHandle handle{};
		Vector3 origin{};
		float radius = 0.f;
		TrueSightKind kind = TrueSightKind::Tower;
		uint8_t team = 0;
		ULONGLONG expiresAt = 0; // sticky remembered sentries only
	};

	auto NameEqualsNoCase( const char* left , const char* right ) -> bool
	{
		if ( !left || !right )
			return false;
		while ( *left && *right )
		{
			if ( std::tolower( static_cast<unsigned char>( *left ) ) !=
				std::tolower( static_cast<unsigned char>( *right ) ) )
				return false;
			++left;
			++right;
		}
		return *left == *right;
	}

	auto IsEnemyTrueSightDebuffName( const char* name ) -> bool
	{
		if ( !name || !name[0] )
			return false;
		// Debuff applied TO us by enemy true sight. Do not match our own gem/ward item buffs.
		if ( ContainsNoCase( name , "item_" ) )
			return false;
		return NameEqualsNoCase( name , "modifier_truesight" ) ||
			( ContainsNoCase( name , "modifier_truesight" ) && !ContainsNoCase( name , "immune" ) );
	}

	auto IsReadablePointer( const void* ptr , size_t size = 8 ) -> bool
	{
		if ( !ptr )
			return false;
		MEMORY_BASIC_INFORMATION mbi{};
		if ( !VirtualQuery( ptr , &mbi , sizeof( mbi ) ) )
			return false;
		if ( mbi.State != MEM_COMMIT )
			return false;
		const DWORD protect = mbi.Protect & 0xFF;
		const bool readable =
			protect == PAGE_READONLY ||
			protect == PAGE_READWRITE ||
			protect == PAGE_WRITECOPY ||
			protect == PAGE_EXECUTE_READ ||
			protect == PAGE_EXECUTE_READWRITE ||
			protect == PAGE_EXECUTE_WRITECOPY;
		const auto start = reinterpret_cast<uintptr_t>( ptr );
		const auto end = reinterpret_cast<uintptr_t>( mbi.BaseAddress ) + mbi.RegionSize;
		return readable && start + size <= end;
	}

	// Invisible enemy sentries are not networked until revealed (e.g. by Gem).
	// Being inside their aura still applies modifier_truesight on us client-side.
	auto LocalHeroHasTrueSightDebuff( C_BaseEntity* hero ) -> bool
	{
		if ( !hero )
			return false;

		static uint32_t modifierManagerOffset = 0;
		static uint32_t buffNameOffset = 0;
		static bool offsetsResolved = false;
		static bool loggedLayout = false;
		static int workingLayout = -1;

		if ( !offsetsResolved )
		{
			auto* schema = GetSchemaOffset();
			if ( schema )
			{
				schema->TryGetOffset( "C_DOTA_BaseNPC" , "m_ModifierManager" , modifierManagerOffset );
				if ( !schema->TryGetOffset( "CDOTA_Buff" , "m_name" , buffNameOffset ) )
					schema->TryGetOffset( "CDOTA_Modifier" , "m_name" , buffNameOffset );
			}
			offsetsResolved = true;
			DEV_LOG( "[visible-by-enemy] modifier offsets manager=0x%X buffName=0x%X\n" ,
				modifierManagerOffset , buffNameOffset );
		}

		if ( !modifierManagerOffset )
			return false;

		auto* manager = reinterpret_cast<uint8_t*>( hero ) + modifierManagerOffset;
		if ( !IsReadablePointer( manager , 0x40 ) )
			return false;

		auto tryLayout = [&]( int layoutId , int vectorBase , int sizeOff , int dataOff ) -> bool
		{
			if ( !IsReadablePointer( manager + vectorBase + sizeOff , sizeof( int32_t ) ) ||
				!IsReadablePointer( manager + vectorBase + dataOff , sizeof( void* ) ) )
				return false;

			const int32_t size = *reinterpret_cast<const int32_t*>( manager + vectorBase + sizeOff );
			auto* data = *reinterpret_cast<void***>( manager + vectorBase + dataOff );
			if ( size <= 0 || size > 128 || !data || !IsReadablePointer( data , sizeof( void* ) * static_cast<size_t>( size ) ) )
				return false;

			const uint32_t nameOff = buffNameOffset ? buffNameOffset : 0x10;
			int readableNames = 0;
			for ( int index = 0; index < size; ++index )
			{
				auto* buff = data[index];
				if ( !buff || !IsReadablePointer( buff , nameOff + sizeof( CUtlSymbolLarge ) ) )
					continue;

				const auto& symbol = *reinterpret_cast<const CUtlSymbolLarge*>(
					reinterpret_cast<const uint8_t*>( buff ) + nameOff );
				const char* name = symbol.String();
				if ( !name || !name[0] )
					continue;
				++readableNames;
				if ( IsEnemyTrueSightDebuffName( name ) )
				{
					if ( !loggedLayout )
					{
						DEV_LOG( "[visible-by-enemy] true-sight debuff via layout=%d name=%s\n" ,
							layoutId , name );
						loggedLayout = true;
						workingLayout = layoutId;
					}
					return true;
				}
			}

			// Layout looks valid if we could read several buff names.
			if ( readableNames >= 2 && workingLayout < 0 )
				workingLayout = layoutId;
			return false;
		};

		// Prefer a previously working layout, then probe common CUtlVector shapes at +0x10.
		if ( workingLayout == 0 && tryLayout( 0 , 0x10 , 0x0 , 0x8 ) )
			return true;
		if ( workingLayout == 1 && tryLayout( 1 , 0x10 , 0x8 , 0x0 ) )
			return true;
		if ( workingLayout == 2 && tryLayout( 2 , 0x10 , 0x0 , 0x10 ) )
			return true;
		if ( workingLayout == 3 && tryLayout( 3 , 0x0 , 0x0 , 0x8 ) )
			return true;

		if ( tryLayout( 0 , 0x10 , 0x0 , 0x8 ) )
			return true;
		if ( tryLayout( 1 , 0x10 , 0x8 , 0x0 ) )
			return true;
		if ( tryLayout( 2 , 0x10 , 0x0 , 0x10 ) )
			return true;
		if ( tryLayout( 3 , 0x0 , 0x0 , 0x8 ) )
			return true;

		return false;
	}

	auto InventoryHasGem( const HeroVitals& vitals ) -> bool
	{
		for ( size_t index = 0; index < vitals.inventoryEntries.size(); ++index )
		{
			if ( !vitals.hasInventoryEntry[index] )
				continue;
			const std::string& name = vitals.inventoryEntries[index].name;
			if ( name == "item_gem" || name == "gem" ||
				name.find( "item_gem" ) != std::string::npos )
				return true;
		}
		for ( int index = 0; index < vitals.trackedEntryCount; ++index )
		{
			const auto& entry = vitals.trackedEntries[index];
			if ( entry.kind != TrackedEntryKind::Item )
				continue;
			if ( entry.name == "item_gem" || entry.name == "gem" ||
				entry.name.find( "item_gem" ) != std::string::npos )
				return true;
		}
		return false;
	}

	auto LooksLikeSentryWardName( const char* name ) -> bool
	{
		if ( !name || !name[0] )
			return false;
		// Observer wards without true sight are not detectors.
		if ( ContainsNoCase( name , "observer" ) &&
			!ContainsNoCase( name , "truesight" ) &&
			!ContainsNoCase( name , "true_sight" ) )
			return false;
		return ContainsNoCase( name , "ward_base_truesight" ) ||
			ContainsNoCase( name , "sentry" ) ||
			ContainsNoCase( name , "Observer_Ward_TrueSight" ) ||
			ContainsNoCase( name , "ward_truesight" ) ||
			ContainsNoCase( name , "npc_dota_sentry" ) ||
			( ContainsNoCase( name , "ward" ) &&
				( ContainsNoCase( name , "truesight" ) || ContainsNoCase( name , "true_sight" ) ) );
	}

	auto LooksLikeTowerName( const char* name ) -> bool
	{
		if ( !name || !name[0] )
			return false;
		return ContainsNoCase( name , "tower" ) ||
			ContainsNoCase( name , "BaseNPC_Tower" ) ||
			ContainsNoCase( name , "DOTA_BaseNPC_Tower" );
	}

	auto HorizontalDistance( const Vector3& left , const Vector3& right ) -> float
	{
		const float dx = left.m_x - right.m_x;
		const float dy = left.m_y - right.m_y;
		return std::sqrt( dx * dx + dy * dy );
	}

	auto RefreshTrueSightSources( CGameEntitySystem* entitySystem , const HeroVitalsSnapshot& snapshot ,
		uint8_t localTeam , CHandle localHeroHandle , std::vector<TrueSightSource>& outSources ,
		std::vector<TrueSightSource>& stickySentries ) -> void
	{
		outSources.clear();
		const HeroVitalsOffsets& offsets = ResolveHeroVitalsOffsets();
		const ULONGLONG now = GetTickCount64();

		for ( const auto& candidate : snapshot.heroes )
		{
			if ( !candidate.handle.IsValid() || candidate.maxHealth <= 0 || candidate.health <= 0 )
				continue;
			if ( candidate.team == localTeam || ( candidate.team != 2 && candidate.team != 3 ) )
				continue;
			if ( candidate.handle.Get() == localHeroHandle.Get() )
				continue;
			if ( !InventoryHasGem( candidate ) )
				continue;

			auto* enemy = entitySystem->GetBaseEntityFromHandle( candidate.handle );
			Vector3 enemyOrigin{};
			if ( !enemy || !TryReadEntityOrigin( enemy , enemyOrigin ) )
				continue;

			TrueSightSource source{};
			source.handle = candidate.handle;
			source.origin = enemyOrigin;
			source.radius = kTrueSightRadiusGem;
			source.kind = TrueSightKind::Gem;
			source.team = candidate.team;
			outSources.push_back( source );
		}

		for ( int chunkIndex = 0; chunkIndex < MAX_ENTITY_LISTS; ++chunkIndex )
		{
			auto* chunk = entitySystem->m_pIdentityChunks[chunkIndex];
			if ( !chunk )
				continue;

			for ( int entryIndex = 0; entryIndex < MAX_ENTITIES_IN_LIST; ++entryIndex )
			{
				auto* identity = &chunk->m_pIdentities[entryIndex];
				auto* entity = identity->pBaseEntity();
				if ( !entity )
					continue;

				const char* designerName = identity->DesingerName().String();
				const char* entityName = identity->Name().String();
				const char* className = entity->GetSchemaClassName();

				const bool isSentry = LooksLikeSentryWardName( designerName ) ||
					LooksLikeSentryWardName( entityName ) ||
					LooksLikeSentryWardName( className );
				const bool isTower = LooksLikeTowerName( designerName ) ||
					LooksLikeTowerName( entityName ) ||
					LooksLikeTowerName( className );
				if ( !isSentry && !isTower )
					continue;

				const uint8_t team = offsets.team
					? ReadEntityField<uint8_t>( entity , offsets.team )
					: entity->m_iTeamNum();
				if ( team == localTeam || ( team != 2 && team != 3 ) )
					continue;

				if ( isTower && offsets.health && offsets.maxHealth )
				{
					const int health = ReadEntityField<int>( entity , offsets.health );
					const int maxHealth = ReadEntityField<int>( entity , offsets.maxHealth );
					if ( maxHealth > 0 && maxHealth <= 1000000 && health <= 0 )
						continue;
				}

				Vector3 sourceOrigin{};
				if ( !TryReadEntityOrigin( static_cast<C_BaseEntity*>( entity ) , sourceOrigin ) )
					continue;
				if ( !std::isfinite( sourceOrigin.m_x ) || !std::isfinite( sourceOrigin.m_y ) )
					continue;
				if ( std::fabs( sourceOrigin.m_x ) < 1.f && std::fabs( sourceOrigin.m_y ) < 1.f )
					continue;

				TrueSightSource source{};
				source.handle = identity->Handle();
				source.origin = sourceOrigin;
				source.radius = isSentry ? kTrueSightRadiusSentry : kTrueSightRadiusTower;
				source.kind = isSentry ? TrueSightKind::Sentry : TrueSightKind::Tower;
				source.team = team;
				outSources.push_back( source );

				// Remember enemy sentries after they are briefly revealed (Gem/dust/etc).
				if ( isSentry )
				{
					constexpr ULONGLONG kStickySentryMs = 7ull * 60ull * 1000ull;
					bool updated = false;
					for ( auto& sticky : stickySentries )
					{
						const float dx = sticky.origin.m_x - sourceOrigin.m_x;
						const float dy = sticky.origin.m_y - sourceOrigin.m_y;
						if ( ( dx * dx + dy * dy ) <= ( 80.f * 80.f ) )
						{
							sticky = source;
							sticky.handle.m_Index = INVALID_EHANDLE_INDEX;
							sticky.expiresAt = now + kStickySentryMs;
							updated = true;
							break;
						}
					}
					if ( !updated )
					{
						TrueSightSource sticky = source;
						sticky.handle.m_Index = INVALID_EHANDLE_INDEX;
						sticky.expiresAt = now + kStickySentryMs;
						stickySentries.push_back( sticky );
					}
				}
			}
		}

		for ( size_t index = 0; index < stickySentries.size(); )
		{
			if ( stickySentries[index].expiresAt && now > stickySentries[index].expiresAt )
			{
				stickySentries.erase( stickySentries.begin() + static_cast<std::ptrdiff_t>( index ) );
				continue;
			}

			bool alreadyListed = false;
			for ( const auto& live : outSources )
			{
				if ( live.kind != TrueSightKind::Sentry )
					continue;
				const float dx = live.origin.m_x - stickySentries[index].origin.m_x;
				const float dy = live.origin.m_y - stickySentries[index].origin.m_y;
				if ( ( dx * dx + dy * dy ) <= ( 80.f * 80.f ) )
				{
					alreadyListed = true;
					break;
				}
			}
			if ( !alreadyListed )
				outSources.push_back( stickySentries[index] );
			++index;
		}
	}

	auto UpdateTrueSightSourceOrigins( CGameEntitySystem* entitySystem , std::vector<TrueSightSource>& sources ) -> void
	{
		for ( auto& source : sources )
		{
			if ( !source.handle.IsValid() )
				continue;
			auto* entity = entitySystem->GetBaseEntityFromHandle( source.handle );
			Vector3 origin{};
			if ( entity && TryReadEntityOrigin( entity , origin ) )
				source.origin = origin;
		}
	}

	auto IsDetectedByEnemyTrueSight( const std::vector<TrueSightSource>& sources , const Vector3& localOrigin ,
		float& nearestSourceOut , int& sourceChecksOut , TrueSightKind& nearestKindOut ) -> bool
	{
		nearestSourceOut = 99999.f;
		sourceChecksOut = static_cast<int>( sources.size() );
		nearestKindOut = TrueSightKind::Tower;
		bool detected = false;

		for ( const auto& source : sources )
		{
			const float dist = HorizontalDistance( localOrigin , source.origin );
			if ( dist < nearestSourceOut )
			{
				nearestSourceOut = dist;
				nearestKindOut = source.kind;
			}
			if ( dist <= source.radius )
				detected = true;
		}

		return detected;
	}

	auto ResolveHealthBarLift( C_BaseEntity* hero ) -> float
	{
		static uint32_t healthBarOffset = 0;
		static bool resolved = false;
		if ( !resolved )
		{
			auto* schema = GetSchemaOffset();
			if ( schema )
				schema->TryGetOffset( "C_DOTA_BaseNPC" , "m_iHealthBarOffset" , healthBarOffset );
			resolved = true;
		}

		float lift = 180.f;
		if ( hero && healthBarOffset )
		{
			const int barOffset = ReadEntityField<int>( hero , healthBarOffset );
			if ( barOffset > 40 && barOffset < 500 )
				lift = static_cast<float>( barOffset );
		}
		return lift;
	}

	auto DrawVisibleByEnemyEyeIcon( C_BaseEntity* hero , const Vector3& origin ) -> bool
	{
		const float lift = ResolveHealthBarLift( hero );

		ImVec2 screenHead{};
		Vector3 head = origin;
		head.m_z += lift;
		if ( !Math::WorldToScreen( head , screenHead ) )
		{
			// Prefer a reliable feet projection + screen-space lift over failing high-Z W2S.
			if ( !Math::WorldToScreen( origin , screenHead ) )
				return false;
			screenHead.y -= 48.f;
		}

		ImVec2 screenSide{};
		float halfBar = 48.f;
		if ( Math::WorldToScreen( Vector3( origin.m_x + 70.f , origin.m_y , origin.m_z + lift ) , screenSide ) )
			halfBar = std::hypot( screenSide.x - screenHead.x , screenSide.y - screenHead.y );
		halfBar = (std::clamp)( halfBar , 34.f , 78.f );

		const float iconSize = (std::clamp)( halfBar * 0.55f , 22.f , 34.f );
		// Snap every frame — previous lerp froze when DeltaTime was 0.
		const ImVec2 iconPos( screenHead.x + halfBar + iconSize * 0.95f , screenHead.y );

		static bool loggedOnce = false;
		if ( !loggedOnce )
		{
			DEV_LOG( "[visible-by-enemy] eye screen=(%.0f,%.0f) size=%.1f origin=(%.0f,%.0f,%.0f)\n" ,
				iconPos.x , iconPos.y , iconSize , origin.m_x , origin.m_y , origin.m_z );
			loggedOnce = true;
		}

		ImDrawList* drawList = ImGui::GetForegroundDrawList();
		const float outer = iconSize;
		const ImU32 blueFill = IM_COL32( 40 , 150 , 255 , 255 );
		const ImU32 blueDark = IM_COL32( 12 , 78 , 190 , 255 );
		const ImU32 white = IM_COL32( 255 , 255 , 255 , 255 );
		const ImU32 black = IM_COL32( 0 , 0 , 0 , 220 );

		drawList->AddCircleFilled( iconPos , outer + 5.f , IM_COL32( 40 , 150 , 255 , 70 ) , 28 );
		drawList->AddCircleFilled( iconPos , outer , blueFill , 28 );
		drawList->AddCircle( iconPos , outer , black , 28 , 2.2f );
		drawList->AddCircle( iconPos , outer - 1.2f , blueDark , 28 , 1.4f );

		for ( int tooth = 0; tooth < 12; ++tooth )
		{
			const float angle = ( static_cast<float>( tooth ) / 12.f ) * 6.2831853f;
			const float cosA = std::cos( angle );
			const float sinA = std::sin( angle );
			const ImVec2 tip( iconPos.x + cosA * ( outer + 3.6f ) , iconPos.y + sinA * ( outer + 3.6f ) );
			const ImVec2 base( iconPos.x + cosA * ( outer - 0.5f ) , iconPos.y + sinA * ( outer - 0.5f ) );
			drawList->AddLine( base , tip , blueFill , 3.1f );
			drawList->AddLine( base , tip , black , 1.1f );
		}

		const ImVec2 eyeRadius( outer * 0.70f , outer * 0.42f );
		drawList->AddEllipseFilled( iconPos , eyeRadius , white , 0.f , 24 );
		drawList->AddEllipse( iconPos , eyeRadius , black , 0.f , 24 , 1.5f );
		drawList->AddCircleFilled( iconPos , outer * 0.22f , blueDark , 16 );
		drawList->AddCircleFilled( iconPos , outer * 0.11f , white , 12 );
		return true;
	}

	auto UpdateVisibleByEnemyIndicator() -> void
	{
		static uint32_t assignedHeroOffset = 0;
		static bool loggedOffsets = false;
		static ULONGLONG lastW2SFailLog = 0;
		static ULONGLONG nextSourceRefresh = 0;
		static std::vector<TrueSightSource> cachedSources{};
		static std::vector<TrueSightSource> stickySentries{};
		static int lastSourceCount = -1;

		if ( !Settings::VisibleByEnemy::Enable )
			return;

		auto* entitySystem = SDK::Interfaces::GameEntitySystem();
		if ( !entitySystem )
			return;

		auto* schema = GetSchemaOffset();
		if ( !schema ||
			( !assignedHeroOffset && !schema->TryGetOffset( "C_DOTAPlayerController" , "m_hAssignedHero" , assignedHeroOffset ) ) )
		{
			return;
		}

		if ( !loggedOffsets )
		{
			DEV_LOG( "[visible-by-enemy] offsets assignedHero=0x%X (true-sight: gem/sentry/tower)\n" ,
				assignedHeroOffset );
			loggedOffsets = true;
		}

		CHandle heroHandle{};
		auto* hero = static_cast<C_BaseEntity*>( nullptr );
		uint8_t localTeam = 0;
		int localPlayerId = -1;

		if ( auto* engine = SDK::Interfaces::EngineToClient() )
			engine->GetLocalPlayer( localPlayerId , 0 );

		if ( auto* controller = CGameEntitySystem::GetLocalPlayerController() )
		{
			heroHandle = ReadEntityField<CHandle>( controller , assignedHeroOffset );
			if ( heroHandle.IsValid() )
				hero = entitySystem->GetBaseEntityFromHandle( heroHandle );
		}

		const auto snapshot = CollectInPlayHeroVitals();
		if ( !hero )
		{
			for ( const auto& candidate : snapshot.heroes )
			{
				if ( !candidate.handle.IsValid() || candidate.maxHealth <= 0 )
					continue;
				if ( localPlayerId >= 0 && candidate.playerId != localPlayerId )
					continue;

				heroHandle = candidate.handle;
				hero = entitySystem->GetBaseEntityFromHandle( heroHandle );
				localTeam = candidate.team;
				if ( hero )
					break;
			}
		}

		if ( !heroHandle.IsValid() || !hero )
			return;

		if ( localTeam != 2 && localTeam != 3 )
			localTeam = hero->m_iTeamNum();

		Vector3 localOrigin{};
		if ( !TryReadEntityOrigin( hero , localOrigin ) )
			return;

		const ULONGLONG now = GetTickCount64();
		if ( now >= nextSourceRefresh )
		{
			RefreshTrueSightSources( entitySystem , snapshot , localTeam , heroHandle ,
				cachedSources , stickySentries );
			nextSourceRefresh = now + 100;
			if ( static_cast<int>( cachedSources.size() ) != lastSourceCount )
			{
				int gems = 0 , sentries = 0 , towers = 0;
				for ( const auto& source : cachedSources )
				{
					if ( source.kind == TrueSightKind::Gem )
						++gems;
					else if ( source.kind == TrueSightKind::Sentry )
						++sentries;
					else
						++towers;
				}
				DEV_LOG( "[visible-by-enemy] sources refreshed total=%d gem=%d sentry=%d tower=%d\n" ,
					static_cast<int>( cachedSources.size() ) , gems , sentries , towers );
				lastSourceCount = static_cast<int>( cachedSources.size() );
			}
		}

		UpdateTrueSightSourceOrigins( entitySystem , cachedSources );

		float nearestSource = 99999.f;
		int sourceChecks = 0;
		TrueSightKind nearestKind = TrueSightKind::Tower;
		const bool detectedByRange = IsDetectedByEnemyTrueSight( cachedSources , localOrigin ,
			nearestSource , sourceChecks , nearestKind );
		const bool detectedByDebuff = LocalHeroHasTrueSightDebuff( hero );
		const bool detected = detectedByRange || detectedByDebuff;
		if ( detectedByDebuff && !detectedByRange )
			nearestKind = TrueSightKind::Sentry;

		if ( !detected )
			return;

		if ( !DrawVisibleByEnemyEyeIcon( hero , localOrigin ) && now - lastW2SFailLog >= 3000 )
		{
			DEV_LOG( "[visible-by-enemy] eye icon unavailable (view/w2s not ready)\n" );
			lastW2SFailLog = now;
		}
	}

	auto LooksLikeFogControllerName( const char* name ) -> bool
	{
		if ( !name || !name[0] )
			return false;

		bool sawFog = false;
		bool sawController = false;
		const size_t length = strlen( name );

		for ( size_t index = 0; index < length; ++index )
		{
			auto matchesAt = [&]( const char* word )
			{
				const size_t wordLength = strlen( word );
				if ( index + wordLength > length )
					return false;

				for ( size_t offset = 0; offset < wordLength; ++offset )
				{
					const unsigned char value = static_cast<unsigned char>( name[index + offset] );
					if ( static_cast<char>( std::tolower( value ) ) != word[offset] )
						return false;
				}

				return true;
			};

			sawFog = sawFog || matchesAt( "fog" );
			sawController = sawController || matchesAt( "controller" );
		}

		return sawFog && sawController;
	}

	struct ModuleRange
	{
		uintptr_t base = 0;
		uintptr_t size = 0;
		uintptr_t codeStart = 0;
		uintptr_t codeEnd = 0;
	};

	auto GetModuleRange( const char* moduleName ) -> ModuleRange
	{
		ModuleRange out{};
		auto* hModule = GetModuleHandleA( moduleName );

		if ( !hModule )
			return out;

		auto* dos = reinterpret_cast<PIMAGE_DOS_HEADER>( hModule );

		if ( dos->e_magic != IMAGE_DOS_SIGNATURE )
			return out;

		auto* nt = reinterpret_cast<PIMAGE_NT_HEADERS64>( reinterpret_cast<uintptr_t>( hModule ) + dos->e_lfanew );

		if ( nt->Signature != IMAGE_NT_SIGNATURE )
			return out;

		out.base = reinterpret_cast<uintptr_t>( hModule );
		out.size = nt->OptionalHeader.SizeOfImage;
		out.codeStart = out.base + nt->OptionalHeader.BaseOfCode;
		out.codeEnd = out.codeStart + nt->OptionalHeader.SizeOfCode;

		return out;
	}

	auto IsWritableRange( const void* ptr , size_t size ) -> bool
	{
		if ( !ptr || size == 0 )
			return false;

		MEMORY_BASIC_INFORMATION mbi{};

		if ( !VirtualQuery( ptr , &mbi , sizeof( mbi ) ) )
			return false;

		if ( mbi.State != MEM_COMMIT )
			return false;

		const DWORD protect = mbi.Protect & 0xFF;
		const auto address = reinterpret_cast<uintptr_t>( ptr );
		const auto regionEnd = reinterpret_cast<uintptr_t>( mbi.BaseAddress ) + mbi.RegionSize;

		const bool writable = protect == PAGE_READWRITE ||
			protect == PAGE_WRITECOPY ||
			protect == PAGE_EXECUTE_READWRITE ||
			protect == PAGE_EXECUTE_WRITECOPY;

		return writable && address + size <= regionEnd;
	}

	auto IsWritableFloat( const void* ptr ) -> bool
	{
		return IsWritableRange( ptr , sizeof( float ) );
	}

	auto SafeWriteFloat( float* ptr , float value ) -> bool
	{
		if ( !IsWritableFloat( ptr ) )
			return false;

		*ptr = value;
		return true;
	}

	auto FindStringInModule( const ModuleRange& mod , const char* needle ) -> const char*
	{
		if ( !mod.base || !needle )
			return nullptr;

		const size_t needleLen = strlen( needle );

		if ( !needleLen || needleLen >= mod.size )
			return nullptr;

		const auto* begin = reinterpret_cast<const char*>( mod.base );
		const auto* end = begin + mod.size - needleLen;

		for ( const char* p = begin; p < end; ++p )
		{
			if ( memcmp( p , needle , needleLen ) == 0 && p[needleLen] == '\0' )
				return p;
		}

		return nullptr;
	}

	// Only movss STORE: F3 0F 11 /r with RIP-relative destination.
	auto ResolveMovssStoreTarget( uintptr_t insn ) -> float*
	{
		const auto* bytes = reinterpret_cast<const uint8_t*>( insn );

		if ( bytes[0] != 0xF3 || bytes[1] != 0x0F || bytes[2] != 0x11 )
			return nullptr;

		const uint8_t modrm = bytes[3];

		if ( ( modrm & 0xC7 ) != 0x05 )
			return nullptr;

		const auto rel = *reinterpret_cast<const int32_t*>( insn + 4 );
		return reinterpret_cast<float*>( insn + 8 + rel );
	}

	auto AcceptCameraCandidate( float* candidate , float expectedApprox , float tolerance ) -> bool
	{
		if ( !candidate || !IsWritableFloat( candidate ) )
			return false;

		const float value = *candidate;

		if ( !std::isfinite( value ) )
			return false;

		return fabsf( value - expectedApprox ) <= tolerance;
	}

	// Find writable float written near code that references the ConVar name.
	auto FindWritableFloatNearStringRef( const ModuleRange& mod , const char* name , float expectedApprox , float tolerance ) -> float*
	{
		const char* str = FindStringInModule( mod , name );

		if ( !str )
		{
			DEV_LOG( "[camera] string '%s' not found\n" , name );
			return nullptr;
		}

		const uintptr_t strAddr = reinterpret_cast<uintptr_t>( str );
		float* best = nullptr;
		float bestDelta = tolerance + 1.f;

		for ( uintptr_t addr = mod.codeStart; addr + 7 < mod.codeEnd; ++addr )
		{
			const auto* b = reinterpret_cast<const uint8_t*>( addr );

			if ( b[0] != 0x48 || b[1] != 0x8D )
				continue;

			if ( ( b[2] & 0xC7 ) != 0x05 )
				continue;

			const auto rel = *reinterpret_cast<const int32_t*>( addr + 3 );

			if ( addr + 7 + rel != strAddr )
				continue;

			const uintptr_t windowStart = ( addr > 0x100 ) ? ( addr - 0x100 ) : mod.codeStart;
			const uintptr_t windowEnd = (std::min)( addr + 0x200 , mod.codeEnd );

			for ( uintptr_t insn = windowStart; insn + 8 < windowEnd; ++insn )
			{
				float* candidate = ResolveMovssStoreTarget( insn );

				if ( !AcceptCameraCandidate( candidate , expectedApprox , tolerance ) )
					continue;

				const float delta = fabsf( *candidate - expectedApprox );

				if ( delta < bestDelta )
				{
					bestDelta = delta;
					best = candidate;
				}
			}
		}

		if ( best )
			DEV_LOG( "[camera] '%s' writable -> %p (value=%.1f)\n" , name , best , *best );
		else
			DEV_LOG( "[camera] '%s' writable store target not found\n" , name );

		return best;
	}

	auto FindWritableFloatByValueNear( const ModuleRange& mod , float expected , float* nearPtr , size_t maxDistance ) -> float*
	{
		if ( !mod.base || !nearPtr )
			return nullptr;

		float* best = nullptr;
		size_t bestDist = maxDistance + 1;

		auto* dos = reinterpret_cast<PIMAGE_DOS_HEADER>( mod.base );
		auto* nt = reinterpret_cast<PIMAGE_NT_HEADERS64>( mod.base + dos->e_lfanew );
		auto* section = IMAGE_FIRST_SECTION( nt );

		for ( unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i , ++section )
		{
			if ( ( section->Characteristics & IMAGE_SCN_MEM_WRITE ) == 0 )
				continue;

			const uintptr_t start = mod.base + section->VirtualAddress;
			const uintptr_t end = start + section->Misc.VirtualSize;
			const uintptr_t nearAddr = reinterpret_cast<uintptr_t>( nearPtr );

			const uintptr_t windowStart = ( nearAddr > start + maxDistance ) ? ( nearAddr - maxDistance ) : start;
			const uintptr_t windowEnd = (std::min)( nearAddr + maxDistance , end );
			uintptr_t addr = windowStart;

			if ( addr % sizeof( float ) )
				addr += sizeof( float ) - ( addr % sizeof( float ) );

			for ( ; addr + sizeof( float ) <= windowEnd; addr += sizeof( float ) )
			{
				auto* f = reinterpret_cast<float*>( addr );

				if ( !IsWritableFloat( f ) || fabsf( *f - expected ) > 0.01f )
					continue;

				const size_t dist = ( addr > nearAddr ) ? ( addr - nearAddr ) : ( nearAddr - addr );

				if ( dist < bestDist )
				{
					bestDist = dist;
					best = f;
				}
			}
		}

		return best;
	}

	auto ValidateOrNull( float*& ptr , const char* name ) -> void
	{
		if ( !ptr )
			return;

		if ( !IsWritableFloat( ptr ) )
		{
			DEV_LOG( "[camera] rejecting read-only %s @ %p\n" , name , ptr );
			ptr = nullptr;
		}
	}

	// r_farz defaults to -1; clip/fog distances are large positives.
	auto LooksLikeClipDistance( float value ) -> bool
	{
		return std::isfinite( value ) && value >= 1000.f && value <= 25000.f;
	}

	auto LooksLikeRFarz( float value ) -> bool
	{
		return std::isfinite( value ) && fabsf( value + 1.f ) < 0.01f;
	}

	// Like FindWritableFloatNearStringRef, but accepts any finite float in [minV, maxV].
	auto FindWritableFloatNearStringInRange( const ModuleRange& mod , const char* name , float minV , float maxV ) -> float*
	{
		const char* str = FindStringInModule( mod , name );

		if ( !str )
			return nullptr;

		const uintptr_t strAddr = reinterpret_cast<uintptr_t>( str );
		float* best = nullptr;
		float bestScore = 1.0e12f;

		for ( uintptr_t addr = mod.codeStart; addr + 7 < mod.codeEnd; ++addr )
		{
			const auto* b = reinterpret_cast<const uint8_t*>( addr );

			if ( b[0] != 0x48 || b[1] != 0x8D )
				continue;

			if ( ( b[2] & 0xC7 ) != 0x05 )
				continue;

			const auto rel = *reinterpret_cast<const int32_t*>( addr + 3 );

			if ( addr + 7 + rel != strAddr )
				continue;

			const uintptr_t windowStart = ( addr > 0x100 ) ? ( addr - 0x100 ) : mod.codeStart;
			const uintptr_t windowEnd = (std::min)( addr + 0x200 , mod.codeEnd );

			for ( uintptr_t insn = windowStart; insn + 8 < windowEnd; ++insn )
			{
				float* candidate = ResolveMovssStoreTarget( insn );

				if ( !candidate || !IsWritableFloat( candidate ) )
					continue;

				const float value = *candidate;

				if ( !std::isfinite( value ) || value < minV || value > maxV )
					continue;

				const float score = fabsf( value );
				if ( score < bestScore )
				{
					bestScore = score;
					best = candidate;
				}
			}
		}

		if ( best )
			DEV_LOG( "[fog] '%s' float in-range -> %p (value=%.1f)\n" , name , best , *best );

		return best;
	}

	auto FindBoolConVarByNamePointer( const ModuleRange& mod , const char* name ) -> bool*
	{
		const char* str = FindStringInModule( mod , name );

		if ( !str )
		{
			DEV_LOG( "[fog] string '%s' not found in %p\n" , name , reinterpret_cast<void*>( mod.base ) );
			return nullptr;
		}

		const uintptr_t strAddr = reinterpret_cast<uintptr_t>( str );
		auto* dos = reinterpret_cast<PIMAGE_DOS_HEADER>( mod.base );
		auto* nt = reinterpret_cast<PIMAGE_NT_HEADERS64>( mod.base + dos->e_lfanew );
		auto* section = IMAGE_FIRST_SECTION( nt );

		// Source 2 ConVar: name pointer field, type @ +0x28, values @ +0x40 (bool in values[0]).
		static const int kNameFieldOffs[] = { 0 , 8 , 16 , 24 };
		static const int16_t kBoolType = 0;

		for ( unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i , ++section )
		{
			if ( ( section->Characteristics & IMAGE_SCN_MEM_WRITE ) == 0 )
				continue;

			const uintptr_t start = mod.base + section->VirtualAddress;
			const uintptr_t end = start + section->Misc.VirtualSize;
			uintptr_t addr = ( start + 7 ) & ~uintptr_t( 7 );

			for ( ; addr + 0x48 <= end; addr += 8 )
			{
				if ( *reinterpret_cast<uintptr_t*>( addr ) != strAddr )
					continue;

				for ( int nameOff : kNameFieldOffs )
				{
					if ( addr < static_cast<uintptr_t>( nameOff ) )
						continue;

					const uintptr_t base = addr - static_cast<uintptr_t>( nameOff );
					auto* typePtr = reinterpret_cast<int16_t*>( base + 0x28 );
					auto* valuePtr = reinterpret_cast<bool*>( base + 0x40 );

					if ( !IsWritableRange( typePtr , sizeof( int16_t ) ) || !IsWritableRange( valuePtr , sizeof( bool ) ) )
						continue;

					if ( *typePtr != kBoolType )
						continue;

					DEV_LOG( "[fog] '%s' bool ConVar -> %p (value=%d, base=%p nameOff=%d)\n" ,
						name , valuePtr , *valuePtr ? 1 : 0 , reinterpret_cast<void*>( base ) , nameOff );
					return valuePtr;
				}
			}
		}

		DEV_LOG( "[fog] '%s' bool ConVar not located via name pointer\n" , name );
		return nullptr;
	}
}

auto CAndromedaClient::SearchCameraConvar( CBasePattern& pattern , const char** fallbackPatterns ) -> bool
{
	if ( pattern.Search( true ) )
		return true;

	for ( int i = 0; fallbackPatterns[i]; ++i )
	{
		if ( FindPattern( CLIENT_DLL , fallbackPatterns[i] ) )
		{
			pattern = CBasePattern(
				pattern.GetPatternName() ,
				fallbackPatterns[i] ,
				CLIENT_DLL ,
				0 ,
				eBasePatternSearchType::SEARCH_TYPE_MOV_PTR );

			if ( pattern.Search( true ) )
			{
				DEV_LOG( "[+] %s found via fallback pattern %d\n" , pattern.GetPatternName() , i );
				return true;
			}
		}
	}

	return false;
}

auto CAndromedaClient::ResolveFogOffsets() -> void
{
	static bool s_Logged = false;

	if ( m_FogParamsResolved && m_FogControllerResolved && m_CameraFogResolved )
		return;

	auto* schema = GetSchemaOffset();

	if ( !schema )
		return;

	schema->TryGetOffset( "fogparams_t" , "enable" , m_FogEnableOffset );
	schema->TryGetOffset( "fogparams_t" , "blend" , m_FogBlendOffset );
	schema->TryGetOffset( "fogparams_t" , "start" , m_FogStartOffset );
	schema->TryGetOffset( "fogparams_t" , "end" , m_FogEndOffset );
	schema->TryGetOffset( "fogparams_t" , "farz" , m_FogFarZOffset );
	schema->TryGetOffset( "fogparams_t" , "maxdensity" , m_FogMaxDensityOffset );
	schema->TryGetOffset( "fogparams_t" , "startLerpTo" , m_FogStartLerpOffset );
	schema->TryGetOffset( "fogparams_t" , "endLerpTo" , m_FogEndLerpOffset );
	schema->TryGetOffset( "fogparams_t" , "maxdensityLerpTo" , m_FogMaxDensityLerpOffset );
	schema->TryGetOffset( "fogparams_t" , "skyboxFogFactor" , m_FogSkyboxFactorOffset );
	schema->TryGetOffset( "fogparams_t" , "skyboxFogFactorLerpTo" , m_FogSkyboxFactorLerpOffset );
	schema->TryGetOffset( "fogparams_t" , "blendtobackground" , m_FogBlendToBackgroundOffset );
	schema->TryGetOffset( "fogparams_t" , "scattering" , m_FogScatteringOffset );

	schema->TryGetOffset( "CBasePlayerController" , "m_hPawn" , m_PlayerControllerPawnOffset );
	schema->TryGetOffset( "C_BasePlayerPawn" , "m_pCameraServices" , m_PlayerPawnCameraServicesOffset );
	schema->TryGetOffset( "CPlayer_CameraServices" , "m_CurrentFog" , m_CameraCurrentFogOffset );
	schema->TryGetOffset( "CPlayer_CameraServices" , "m_bOverrideFogColor" , m_CameraOverrideFogColorEnabledOffset );
	schema->TryGetOffset( "CPlayer_CameraServices" , "m_OverrideFogColor" , m_CameraOverrideFogColorOffset );
	schema->TryGetOffset( "CPlayer_CameraServices" , "m_bOverrideFogStartEnd" , m_CameraOverrideFogStartEndEnabledOffset );
	schema->TryGetOffset( "CPlayer_CameraServices" , "m_fOverrideFogStart" , m_CameraOverrideFogStartOffset );
	schema->TryGetOffset( "CPlayer_CameraServices" , "m_fOverrideFogEnd" , m_CameraOverrideFogEndOffset );
	if ( !m_FogControllerFogOffset )
	{
		static const char* controllerClasses[] =
		{
			"C_FogController" ,
			"C_EnvFogController" ,
			"C_FogControllerBase" ,
			nullptr
		};

		for ( int index = 0; controllerClasses[index]; ++index )
		{
			if ( schema->TryGetOffset( controllerClasses[index] , "m_fog" , m_FogControllerFogOffset ) )
			{
				schema->TryGetOffset( controllerClasses[index] , "m_iChangedVariables" , m_FogChangedVariablesOffset );
				break;
			}
		}
	}

	if ( !m_FogEnableOffset )
		m_FogEnableOffset = 0x64;
	if ( !m_FogMaxDensityOffset )
		m_FogMaxDensityOffset = 0x30;
	if ( !m_FogStartOffset )
		m_FogStartOffset = 0x24;
	if ( !m_FogEndOffset )
		m_FogEndOffset = 0x28;
	if ( !m_FogFarZOffset )
		m_FogFarZOffset = 0x2C;
	if ( !m_FogBlendOffset )
		m_FogBlendOffset = 0x65;
	if ( !m_FogSkyboxFactorOffset )
		m_FogSkyboxFactorOffset = 0x3C;
	if ( !m_FogSkyboxFactorLerpOffset )
		m_FogSkyboxFactorLerpOffset = 0x40;
	if ( !m_FogMaxDensityLerpOffset )
		m_FogMaxDensityLerpOffset = 0x4C;
	if ( !m_FogStartLerpOffset )
		m_FogStartLerpOffset = 0x44;
	if ( !m_FogEndLerpOffset )
		m_FogEndLerpOffset = 0x48;
	if ( !m_FogBlendToBackgroundOffset )
		m_FogBlendToBackgroundOffset = 0x58;
	if ( !m_FogScatteringOffset )
		m_FogScatteringOffset = 0x5C;

	// Linked Windows schema fallbacks. Runtime schema values above take priority.
	if ( !m_PlayerControllerPawnOffset )
		m_PlayerControllerPawnOffset = 0x6A4;
	if ( !m_PlayerPawnCameraServicesOffset )
		m_PlayerPawnCameraServicesOffset = 0xB98;
	if ( !m_CameraCurrentFogOffset )
		m_CameraCurrentFogOffset = 0x130;
	if ( !m_CameraOverrideFogColorEnabledOffset )
		m_CameraOverrideFogColorEnabledOffset = 0x19C;
	if ( !m_CameraOverrideFogColorOffset )
		m_CameraOverrideFogColorOffset = 0x1A1;
	if ( !m_CameraOverrideFogStartEndEnabledOffset )
		m_CameraOverrideFogStartEndEnabledOffset = 0x1B5;
	if ( !m_CameraOverrideFogStartOffset )
		m_CameraOverrideFogStartOffset = 0x1BC;
	if ( !m_CameraOverrideFogEndOffset )
		m_CameraOverrideFogEndOffset = 0x1D0;

	m_FogParamsResolved = m_FogEnableOffset > 0 && m_FogMaxDensityOffset > 0;
	m_FogControllerResolved = m_FogControllerFogOffset > 0 && m_FogParamsResolved;
	m_CameraFogResolved = m_FogParamsResolved && m_PlayerControllerPawnOffset > 0 &&
		m_PlayerPawnCameraServicesOffset > 0 && m_CameraCurrentFogOffset > 0 &&
		m_CameraOverrideFogColorEnabledOffset > 0 && m_CameraOverrideFogColorOffset > 0 &&
		m_CameraOverrideFogStartEndEnabledOffset > 0 && m_CameraOverrideFogStartOffset > 0 &&
		m_CameraOverrideFogEndOffset > 0;

	if ( !s_Logged || ( m_FogControllerResolved && m_CameraFogResolved ) )
	{
		DEV_LOG( "[fog] schema paths: params=%d controller=%d camera=%d enable=0x%X dens=0x%X ctrl_fog=0x%X current=0x%X\n" ,
			m_FogParamsResolved ? 1 : 0 , m_FogControllerResolved ? 1 : 0 ,
			m_CameraFogResolved ? 1 : 0 , m_FogEnableOffset , m_FogMaxDensityOffset ,
			m_FogControllerFogOffset , m_CameraCurrentFogOffset );
		s_Logged = true;
	}
}

auto CAndromedaClient::FogPlanesReady() const -> bool
{
	return m_pFogStartZoomedOut && m_pFogEndZoomedOut;
}

auto CAndromedaClient::ResolveFogConVars() -> void
{
	// Keep trying fog_enable even after planes resolve; stop only when both paths are done
	// or attempts are exhausted.
	if ( FogPlanesReady() && m_pFogEnable )
		return;

	if ( m_FogConVarResolveAttempts >= 12 )
		return;

	static DWORD s_LastAttempt = 0;
	const DWORD now = GetTickCount();

	if ( m_FogConVarResolveAttempts > 0 && ( now - s_LastAttempt ) < 1000 )
		return;

	s_LastAttempt = now;
	++m_FogConVarResolveAttempts;

	const auto mod = GetModuleRange( CLIENT_DLL );

	if ( !mod.base )
	{
		DEV_LOG( "[fog] client.dll not ready (attempt %d)\n" , m_FogConVarResolveAttempts );
		return;
	}

	// Primary: Dota camera fog planes. These wash the map when zoomed out.
	if ( !m_pFogStartZoomedOut )
	{
		m_pFogStartZoomedOut = FindWritableFloatNearStringRef( mod , "dota_camera_fog_start_zoomed_out" , 4500.f , 500.f );
		if ( !m_pFogStartZoomedOut )
			m_pFogStartZoomedOut = FindWritableFloatNearStringInRange( mod , "dota_camera_fog_start_zoomed_out" , 1000.f , 100000.f );
	}
	if ( !m_pFogEndZoomedOut )
	{
		m_pFogEndZoomedOut = FindWritableFloatNearStringRef( mod , "dota_camera_fog_end_zoomed_out" , 6000.f , 500.f );
		if ( !m_pFogEndZoomedOut )
			m_pFogEndZoomedOut = FindWritableFloatNearStringInRange( mod , "dota_camera_fog_end_zoomed_out" , 1000.f , 100000.f );
	}
	if ( !m_pFogStartZoomedIn )
	{
		m_pFogStartZoomedIn = FindWritableFloatNearStringRef( mod , "dota_camera_fog_start_zoomed_in" , 2000.f , 500.f );
		if ( !m_pFogStartZoomedIn )
			m_pFogStartZoomedIn = FindWritableFloatNearStringInRange( mod , "dota_camera_fog_start_zoomed_in" , 500.f , 100000.f );
	}
	if ( !m_pFogEndZoomedIn )
	{
		m_pFogEndZoomedIn = FindWritableFloatNearStringRef( mod , "dota_camera_fog_end_zoomed_in" , 2500.f , 500.f );
		if ( !m_pFogEndZoomedIn )
			m_pFogEndZoomedIn = FindWritableFloatNearStringInRange( mod , "dota_camera_fog_end_zoomed_in" , 500.f , 100000.f );
	}

	// Fallback: known defaults near the working camera-distance ConVar cluster.
	if ( m_pCameraDistance )
	{
		if ( !m_pFogStartZoomedOut )
			m_pFogStartZoomedOut = FindWritableFloatByValueNear( mod , 4500.f , m_pCameraDistance , 0x4000 );
		if ( !m_pFogEndZoomedOut )
			m_pFogEndZoomedOut = FindWritableFloatByValueNear( mod , 6000.f , m_pCameraDistance , 0x4000 );
		if ( !m_pFogStartZoomedIn )
			m_pFogStartZoomedIn = FindWritableFloatByValueNear( mod , 2000.f , m_pCameraDistance , 0x4000 );
		if ( !m_pFogEndZoomedIn )
			m_pFogEndZoomedIn = FindWritableFloatByValueNear( mod , 2500.f , m_pCameraDistance , 0x4000 );
	}

	if ( !m_pFogEnable )
		m_pFogEnable = FindBoolConVarByNamePointer( mod , "fog_enable" );

	DEV_LOG( "[fog] convars attempt %d: enable=%p startOut=%p endOut=%p startIn=%p endIn=%p\n" ,
		m_FogConVarResolveAttempts , m_pFogEnable ,
		m_pFogStartZoomedOut , m_pFogEndZoomedOut , m_pFogStartZoomedIn , m_pFogEndZoomedIn );
}

auto CAndromedaClient::ApplyFogConVars() -> bool
{
	constexpr float kFar = 50000.f;
	bool wrote = false;

	auto writePlane = [&]( float*& ptr , const char* name )
	{
		if ( !ptr )
			return;

		if ( !SafeWriteFloat( ptr , kFar ) )
		{
			DEV_LOG( "[fog] %s @ %p unwritable — clearing\n" , name , ptr );
			ptr = nullptr;
			return;
		}

		wrote = true;
	};

	writePlane( m_pFogStartZoomedOut , "dota_camera_fog_start_zoomed_out" );
	writePlane( m_pFogEndZoomedOut , "dota_camera_fog_end_zoomed_out" );
	writePlane( m_pFogStartZoomedIn , "dota_camera_fog_start_zoomed_in" );
	writePlane( m_pFogEndZoomedIn , "dota_camera_fog_end_zoomed_in" );

	if ( m_pFogEnable )
	{
		if ( !IsWritableRange( m_pFogEnable , sizeof( bool ) ) )
		{
			DEV_LOG( "[fog] fog_enable @ %p unwritable — clearing\n" , m_pFogEnable );
			m_pFogEnable = nullptr;
		}
		else
		{
			*m_pFogEnable = false;
			wrote = true;
		}
	}

	static bool s_Logged = false;

	if ( wrote && !s_Logged )
	{
		DEV_LOG( "[fog] applied camera fog ConVars (enable=%d startOut=%.0f endOut=%.0f)\n" ,
			m_pFogEnable ? ( *m_pFogEnable ? 1 : 0 ) : -1 ,
			m_pFogStartZoomedOut ? *m_pFogStartZoomedOut : -1.f ,
			m_pFogEndZoomedOut ? *m_pFogEndZoomedOut : -1.f );
		s_Logged = true;
	}

	return FogPlanesReady() || m_pFogEnable != nullptr;
}

auto CAndromedaClient::ApplyNoFogParams( uintptr_t fog ) -> void
{
	if ( !fog || !m_FogParamsResolved )
		return;

	uint32_t highest = m_FogEnableOffset;
	highest = (std::max)( highest , m_FogBlendOffset );
	highest = (std::max)( highest , m_FogStartOffset );
	highest = (std::max)( highest , m_FogEndOffset );
	highest = (std::max)( highest , m_FogFarZOffset );
	highest = (std::max)( highest , m_FogMaxDensityOffset );
	highest = (std::max)( highest , m_FogStartLerpOffset );
	highest = (std::max)( highest , m_FogEndLerpOffset );
	highest = (std::max)( highest , m_FogMaxDensityLerpOffset );
	highest = (std::max)( highest , m_FogSkyboxFactorOffset );
	highest = (std::max)( highest , m_FogSkyboxFactorLerpOffset );
	highest = (std::max)( highest , m_FogBlendToBackgroundOffset );
	highest = (std::max)( highest , m_FogScatteringOffset );

	if ( !IsWritableRange( reinterpret_cast<void*>( fog ) , highest + sizeof( float ) ) )
		return;

	constexpr float kNoFogDistance = 50000.f;
	*reinterpret_cast<bool*>( fog + m_FogEnableOffset ) = false;

	if ( m_FogBlendOffset )
		*reinterpret_cast<bool*>( fog + m_FogBlendOffset ) = false;
	if ( m_FogStartOffset )
		*reinterpret_cast<float*>( fog + m_FogStartOffset ) = kNoFogDistance;
	if ( m_FogEndOffset )
		*reinterpret_cast<float*>( fog + m_FogEndOffset ) = kNoFogDistance;
	if ( m_FogFarZOffset )
		*reinterpret_cast<float*>( fog + m_FogFarZOffset ) = kNoFogDistance;
	if ( m_FogMaxDensityOffset )
		*reinterpret_cast<float*>( fog + m_FogMaxDensityOffset ) = 0.f;
	if ( m_FogStartLerpOffset )
		*reinterpret_cast<float*>( fog + m_FogStartLerpOffset ) = kNoFogDistance;
	if ( m_FogEndLerpOffset )
		*reinterpret_cast<float*>( fog + m_FogEndLerpOffset ) = kNoFogDistance;
	if ( m_FogMaxDensityLerpOffset )
		*reinterpret_cast<float*>( fog + m_FogMaxDensityLerpOffset ) = 0.f;
	if ( m_FogSkyboxFactorOffset )
		*reinterpret_cast<float*>( fog + m_FogSkyboxFactorOffset ) = 0.f;
	if ( m_FogSkyboxFactorLerpOffset )
		*reinterpret_cast<float*>( fog + m_FogSkyboxFactorLerpOffset ) = 0.f;
	if ( m_FogBlendToBackgroundOffset )
		*reinterpret_cast<float*>( fog + m_FogBlendToBackgroundOffset ) = 0.f;
	if ( m_FogScatteringOffset )
		*reinterpret_cast<float*>( fog + m_FogScatteringOffset ) = 0.f;
}

auto CAndromedaClient::ApplyNoFogCameraServices() -> void
{
	if ( !m_CameraFogResolved )
		return;

	auto* entitySystem = SDK::Interfaces::GameEntitySystem();
	auto* controller = CGameEntitySystem::GetLocalPlayerController();
	if ( !entitySystem || !controller )
		return;

	auto* pawnHandle = reinterpret_cast<CHandle*>(
		reinterpret_cast<uintptr_t>( controller ) + m_PlayerControllerPawnOffset );
	if ( !IsWritableRange( pawnHandle , sizeof( *pawnHandle ) ) || !pawnHandle->IsValid() )
		return;

	auto* pawn = entitySystem->GetBaseEntityFromHandle( *pawnHandle );
	if ( !pawn )
		return;

	auto** cameraSlot = reinterpret_cast<void**>(
		reinterpret_cast<uintptr_t>( pawn ) + m_PlayerPawnCameraServicesOffset );
	if ( !IsWritableRange( cameraSlot , sizeof( *cameraSlot ) ) || !*cameraSlot )
		return;

	const uintptr_t camera = reinterpret_cast<uintptr_t>( *cameraSlot );
	ApplyNoFogParams( camera + m_CameraCurrentFogOffset );

	constexpr size_t kFogOverrideCount = 5;
	constexpr float kNoFogDistance = 50000.f;
	auto* colorEnabled = reinterpret_cast<bool*>( camera + m_CameraOverrideFogColorEnabledOffset );
	auto* colors = reinterpret_cast<uint32_t*>( camera + m_CameraOverrideFogColorOffset );
	auto* distanceEnabled = reinterpret_cast<bool*>( camera + m_CameraOverrideFogStartEndEnabledOffset );
	auto* starts = reinterpret_cast<float*>( camera + m_CameraOverrideFogStartOffset );
	auto* ends = reinterpret_cast<float*>( camera + m_CameraOverrideFogEndOffset );

	if ( !IsWritableRange( colorEnabled , kFogOverrideCount * sizeof( bool ) ) ||
		 !IsWritableRange( colors , kFogOverrideCount * sizeof( uint32_t ) ) ||
		 !IsWritableRange( distanceEnabled , kFogOverrideCount * sizeof( bool ) ) ||
		 !IsWritableRange( starts , kFogOverrideCount * sizeof( float ) ) ||
		 !IsWritableRange( ends , kFogOverrideCount * sizeof( float ) ) )
		return;

	for ( size_t index = 0; index < kFogOverrideCount; ++index )
	{
		colorEnabled[index] = true;
		colors[index] = 0u;
		distanceEnabled[index] = true;
		starts[index] = kNoFogDistance;
		ends[index] = kNoFogDistance;
	}
}

auto CAndromedaClient::ApplyNoFog( CEntityInstance* pEntity ) -> void
{
	if ( !pEntity || !m_FogControllerResolved )
		return;

	ApplyNoFogParams( reinterpret_cast<uintptr_t>( pEntity ) + m_FogControllerFogOffset );

	if ( m_FogChangedVariablesOffset )
	{
		auto* changed = reinterpret_cast<int*>(
			reinterpret_cast<uintptr_t>( pEntity ) + m_FogChangedVariablesOffset );

		if ( IsWritableRange( changed , sizeof( *changed ) ) )
			*changed = -1;
	}
}

auto CAndromedaClient::ApplyNoFog() -> void
{
	ResolveFogOffsets();

	// Primary path: Dota camera fog ConVars. Entity fogparams alone cannot clear zoom haze.
	ApplyFogConVars();
	ApplyNoFogCameraServices();

	for ( auto& slot : m_FogControllers )
	{
		if ( auto* entity = slot.load( std::memory_order_acquire ) )
			ApplyNoFog( entity );
	}
}

auto CAndromedaClient::RegisterFogController( CEntityInstance* pEntity ) -> void
{
	if ( !pEntity )
		return;

	for ( auto& slot : m_FogControllers )
	{
		if ( slot.load( std::memory_order_acquire ) == pEntity )
		{
			ApplyNoFog( pEntity );
			return;
		}
	}

	for ( auto& slot : m_FogControllers )
	{
		CEntityInstance* expected = nullptr;

		if ( slot.compare_exchange_strong( expected , pEntity , std::memory_order_acq_rel ) )
		{
			DEV_LOG( "[fog] controller captured: %p\n" , pEntity );
			ApplyNoFog( pEntity );
			return;
		}
	}
}

auto CAndromedaClient::UnregisterFogController( CEntityInstance* pEntity ) -> void
{
	for ( auto& slot : m_FogControllers )
	{
		CEntityInstance* expected = pEntity;
		slot.compare_exchange_strong( expected , nullptr , std::memory_order_acq_rel );
	}
}

auto CAndromedaClient::ResolveCameraPointers() -> void
{
	static int s_Attempts = 0;
	static DWORD s_LastAttemptTick = 0;

	if ( m_pCameraDistance && m_pRFarz )
		return;

	if ( s_Attempts >= 10 )
		return;

	const DWORD now = GetTickCount();

	if ( s_Attempts > 0 && ( now - s_LastAttemptTick ) < 1500 )
		return;

	s_LastAttemptTick = now;
	++s_Attempts;

	static const char* distanceFallbacks[] =
	{
		"F3 0F 11 05 ? ? ? ? 48 8D 0D ? ? ? ? E8 ? ? ? ? BA ? ? ? ? F3 0F 11 05",
		"F3 0F 11 05 ? ? ? ? F3 0F 11 05 ? ? ? ? F3 0F 11 05",
		nullptr
	};

	if ( !m_pRFarz && dota_camera_farplane.Search( true ) )
	{
		auto* candidate = reinterpret_cast<float*>( dota_camera_farplane.GetFunction() );

		if ( candidate && IsWritableFloat( candidate ) && LooksLikeRFarz( *candidate ) )
		{
			m_pRFarz = candidate;
			DEV_LOG( "[camera] r_farz via pattern -> %p (value=%.1f)\n" , m_pRFarz , *m_pRFarz );

			auto* distanceCandidate = reinterpret_cast<float*>(
				reinterpret_cast<uintptr_t>( candidate ) - 0x14 );

			if ( !m_pCameraDistance &&
				IsWritableFloat( distanceCandidate ) &&
				fabsf( *distanceCandidate - 1200.f ) <= 200.f )
			{
				m_pCameraDistance = distanceCandidate;
				DEV_LOG( "[camera] distance via r_farz-0x14 -> %p (value=%.1f)\n" ,
					m_pCameraDistance , *m_pCameraDistance );
			}
		}
		else if ( candidate && IsWritableFloat( candidate ) && LooksLikeClipDistance( *candidate ) )
		{
			// Some builds store zfar here instead.
			m_pRFarz = candidate;
			DEV_LOG( "[camera] zfar-like via pattern -> %p (value=%.1f)\n" , m_pRFarz , *m_pRFarz );
		}
	}

	if ( !m_pCameraDistance && SearchCameraConvar( dota_camera_distance , distanceFallbacks ) )
		m_pCameraDistance = reinterpret_cast<float*>( dota_camera_distance.GetFunction() );

	ValidateOrNull( m_pCameraDistance , "distance" );

	const auto mod = GetModuleRange( CLIENT_DLL );

	if ( !m_pCameraDistance )
		m_pCameraDistance = FindWritableFloatNearStringRef( mod , "dota_camera_distance" , 1200.f , 200.f );

	if ( !m_pCameraDistance && m_pRFarz )
		m_pCameraDistance = FindWritableFloatByValueNear( mod , 1200.f , m_pRFarz , 0x200 );

	ValidateOrNull( m_pCameraDistance , "distance" );

	if ( !m_pRFarz && m_pCameraDistance )
	{
		auto* maybe = reinterpret_cast<float*>( reinterpret_cast<uintptr_t>( m_pCameraDistance ) + 0x14 );

		if ( IsWritableFloat( maybe ) && ( LooksLikeRFarz( *maybe ) || LooksLikeClipDistance( *maybe ) ) )
		{
			m_pRFarz = maybe;
			DEV_LOG( "[camera] r_farz via distance+0x14 -> %p (value=%.1f)\n" , m_pRFarz , *m_pRFarz );
		}
	}

	ValidateOrNull( m_pRFarz , "r_farz" );

	DEV_LOG( "[camera] pointers: distance=%p r_farz=%p (attempt %d)\n" ,
		m_pCameraDistance , m_pRFarz , s_Attempts );
}

auto CAndromedaClient::OnInit() -> void
{
	ResolveFogOffsets();
	ResolveCameraPointers();
	ResolveFogConVars();
	ApplyNoFog();

	if ( FogPlanesReady() || m_pFogEnable )
		DEV_LOG( "[fog] camera fog ConVars ready (enable=%p startOut=%p endOut=%p)\n" ,
			m_pFogEnable , m_pFogStartZoomedOut , m_pFogEndZoomedOut );
	else
		DEV_LOG( "[warn] camera fog ConVars not ready yet — will retry\n" );

	if ( m_pCameraDistance )
		DEV_LOG( "[dota_camera_distance] Found (writable)!\n" );
	else
		DEV_LOG( "[warn] dota_camera_distance not ready yet — will retry\n" );

	if ( m_pRFarz )
		DEV_LOG( "[r_farz] Found (writable)! value=%.1f\n" , *m_pRFarz );
	else
		DEV_LOG( "[warn] r_farz not found yet\n" );

	const std::string baseDir = GetDllDir();
	const std::string heroJsonPath = baseDir + "Assets\\data\\npc_heroes.json";
	const std::string abilitiesJsonPath = baseDir + "Assets\\data\\abilities.json";
	const std::string itemsJsonPath = baseDir + "Assets\\data\\items.json";
	const std::string heroAbilitiesJsonPath = baseDir + "Assets\\data\\hero_abilities.json";
	constexpr const char* kHeroJsonUrl = "https://raw.githubusercontent.com/odota/dotaconstants/master/build/heroes.json";
	constexpr const char* kAbilitiesJsonUrl = "https://raw.githubusercontent.com/odota/dotaconstants/master/build/abilities.json";
	constexpr const char* kItemsJsonUrl = "https://raw.githubusercontent.com/odota/dotaconstants/master/build/items.json";
	constexpr const char* kHeroAbilitiesJsonUrl = "https://raw.githubusercontent.com/odota/dotaconstants/master/build/hero_abilities.json";

	if ( g_HeroDataLoader.EnsureCacheAndLoad( kHeroJsonUrl , heroJsonPath ) )
		DEV_LOG( "[heroes] loaded %zu heroes from %s\n" , g_HeroDataLoader.GetAll().size() , g_HeroDataLoader.GetSourcePath().c_str() );
	else
		DEV_LOG( "[heroes] skip hero data load (missing/invalid file: %s)\n" , heroJsonPath.c_str() );

	if ( !CHeroDataLoader::IsValidHeroCache( abilitiesJsonPath ) )
	{
		CHeroDataLoader::DownloadFileWinHttp( std::wstring( kAbilitiesJsonUrl , kAbilitiesJsonUrl + std::strlen( kAbilitiesJsonUrl ) ) ,
			std::wstring( abilitiesJsonPath.begin() , abilitiesJsonPath.end() ) );
	}
	if ( LoadInnateAbilityNames( abilitiesJsonPath ) )
		DEV_LOG( "[abilities] loaded %zu innate ability names from %s\n" , g_InnateAbilityNames.size() , abilitiesJsonPath.c_str() );
	else
		DEV_LOG( "[abilities] innate metadata unavailable: %s\n" , abilitiesJsonPath.c_str() );

	if ( GetAbilityDamageData()->LoadFromFile( abilitiesJsonPath ) )
		DEV_LOG( "[kill-stealer] loaded %zu damage entries from %s\n" ,
			GetAbilityDamageData()->LoadedCount() , abilitiesJsonPath.c_str() );
	else
		DEV_LOG( "[kill-stealer] damage metadata unavailable: %s\n" , abilitiesJsonPath.c_str() );

	if ( !CHeroDataLoader::IsValidHeroCache( itemsJsonPath ) )
	{
		CHeroDataLoader::DownloadFileWinHttp( std::wstring( kItemsJsonUrl , kItemsJsonUrl + std::strlen( kItemsJsonUrl ) ) ,
			std::wstring( itemsJsonPath.begin() , itemsJsonPath.end() ) );
	}
	if ( GetAbilityDamageData()->LoadFromFile( itemsJsonPath ) )
		DEV_LOG( "[auto-combo] loaded %zu total ability/item entries after items.json (%s)\n" ,
			GetAbilityDamageData()->LoadedCount() , itemsJsonPath.c_str() );
	else
		DEV_LOG( "[auto-combo] item metadata unavailable: %s\n" , itemsJsonPath.c_str() );

	if ( !CHeroDataLoader::IsValidHeroCache( heroAbilitiesJsonPath ) )
	{
		CHeroDataLoader::DownloadFileWinHttp( std::wstring( kHeroAbilitiesJsonUrl ,
			kHeroAbilitiesJsonUrl + std::strlen( kHeroAbilitiesJsonUrl ) ) ,
			std::wstring( heroAbilitiesJsonPath.begin() , heroAbilitiesJsonPath.end() ) );
	}
	if ( LoadHeroAbilityNames( heroAbilitiesJsonPath ) )
		DEV_LOG( "[abilities] loaded canonical skills for %zu heroes from %s\n" ,
			g_HeroAbilityNames.size() , heroAbilitiesJsonPath.c_str() );
	else
		DEV_LOG( "[abilities] hero skill metadata unavailable: %s\n" , heroAbilitiesJsonPath.c_str() );

	if ( GetAbilityDamageData()->LoadHeroSkillBarFromFile( heroAbilitiesJsonPath ) )
		DEV_LOG( "[kill-stealer] loaded %zu preferred skill slots from %s\n" ,
			GetAbilityDamageData()->HeroSkillSlotCount() , heroAbilitiesJsonPath.c_str() );
	else
		DEV_LOG( "[kill-stealer] preferred skill slots unavailable: %s\n" , heroAbilitiesJsonPath.c_str() );

	const std::string itemCostsJsonPath = baseDir + "Assets\\data\\item_costs.json";
	if ( LoadItemCosts( itemCostsJsonPath ) )
		DEV_LOG( "[items] loaded %zu item costs from %s\n" , g_ItemCosts.size() , itemCostsJsonPath.c_str() );
	else
		DEV_LOG( "[items] item cost metadata unavailable: %s\n" , itemCostsJsonPath.c_str() );

	const std::string scriptsRoot = baseDir + "Assets\\Lua\\";
	GetLuaManager()->Init( scriptsRoot );
}

auto CAndromedaClient::SetCameraDistance( float Distance ) -> void
{
	if ( m_pCameraDistance )
	{
		if ( !SafeWriteFloat( m_pCameraDistance , Distance ) )
		{
			DEV_LOG( "[error] camera distance @ %p is not writable — clearing\n" , m_pCameraDistance );
			m_pCameraDistance = nullptr;
		}
		else
		{
			static int s_LogCount = 0;
			if ( s_LogCount < 3 )
			{
				DEV_LOG( "[camera] set distance=%.1f @ %p\n" , Distance , m_pCameraDistance );
				s_LogCount++;
			}
		}
	}

	// Raising r_farz while camera fog planes are still ~4500-6000 paints the whole map pink/red.
	// Only extend the far clip after fog ConVars are pushed out (or fog_enable is off).
	if ( m_pRFarz && ( FogPlanesReady() || m_pFogEnable ) )
	{
		constexpr float kFarZ = 18000.f;

		if ( !SafeWriteFloat( m_pRFarz , kFarZ ) )
		{
			DEV_LOG( "[error] r_farz @ %p is not writable — clearing\n" , m_pRFarz );
			m_pRFarz = nullptr;
		}
		else
		{
			static bool s_LoggedFarZ = false;
			if ( !s_LoggedFarZ )
			{
				DEV_LOG( "[camera] r_farz set to %.0f @ %p (fog planes ready)\n" , kFarZ , m_pRFarz );
				s_LoggedFarZ = true;
			}
		}
	}
	else if ( m_pRFarz )
	{
		static bool s_LoggedSkip = false;
		if ( !s_LoggedSkip )
		{
			DEV_LOG( "[camera] delaying r_farz until fog ConVars resolve\n" );
			s_LoggedSkip = true;
		}
	}
}

auto CAndromedaClient::GetCastableIconSrv( const std::string& name , bool isItem ) -> ID3D11ShaderResourceView*
{
	if ( name.empty() )
		return nullptr;

	// Items are filed on the CDN without the "item_" prefix the entity names
	// carry, exactly as TrackedIconAssetName does for the overlays.
	std::string assetName = name;
	if ( isItem && assetName.rfind( "item_" , 0 ) == 0 )
		assetName.erase( 0 , 5 );
	assetName.erase( std::remove_if( assetName.begin() , assetName.end() , []( const unsigned char value )
	{
		return !std::isalnum( value ) && value != '_';
	} ) , assetName.end() );

	auto* icon = GetAssetIcon( isItem ? "items" : "abilities" , assetName );
	// The overlays normally drive the download queue, but they return early
	// when their own settings are off - so pump it here too, or a menu-only
	// user would wait forever for an icon that never downloads.
	UpdateTrackedIconDownloads();
	return icon ? icon->srv : nullptr;
}

auto CAndromedaClient::OnRender() -> void
{
	// Apply independently of the camera option so fog cannot return during interpolation.
	ApplyNoFog();
	const float mouseWheelDelta = GetAndromedaGUI()->ConsumeMouseWheelDelta();

	if ( Settings::Camera::Enable )
	{
		m_bCameraWasEnabled = true;

		// The first combo entry is "Wheel"; the second explicitly disables wheel zoom.
		if ( Settings::Camera::ZoomUsingWheel == 0 && mouseWheelDelta != 0.f )
		{
			constexpr float kMinCameraDistance = 1200.f;
			constexpr float kMaxCameraDistance = 10000.f;
			Settings::Camera::Distance = std::clamp(
				Settings::Camera::Distance - mouseWheelDelta * Settings::Camera::ZoomSpeed,
				kMinCameraDistance,
				kMaxCameraDistance );
		}

		if ( m_pCameraDistance || m_pRFarz )
			SetCameraDistance( Settings::Camera::Distance );
	}
	else if ( m_bCameraWasEnabled )
	{
		// Restore the game's default without overwriting the user's saved slider value.
		SetCameraDistance( 1200.f );
		m_bCameraWasEnabled = false;
	}

	if ( GetAndromedaGUI()->IsVisible() )
		GetAndromedaMenu()->OnRenderMenu();

	m_KillStealer.OnRender();
	m_LastHitAssistant.OnRender();
	m_AutoCombo.OnRender();
	m_Dodger.OnRender();

	DrawHeroVitalsOverlay();
	DrawHeroSidePanels();
	UpdateVisibleByEnemyIndicator();

	// Use the existing ImGui draw list. Initializing FW1FontWrapper lazily from
	// Present performs synchronous D3D/DirectWrite work on Dota's render thread.
	ImGui::GetForegroundDrawList()->AddText( ImVec2( 1.f , 1.f ) , IM_COL32( 255 , 255 , 0 , 255 ) , XorStr( CHEAT_NAME ) );
}

auto CAndromedaClient::DiscoverFogControllers() -> void
{
	constexpr int kEntitiesPerTick = 96;
	auto* entitySystem = SDK::Interfaces::GameEntitySystem();

	if ( !entitySystem )
		return;

	const int highest = (std::min)( entitySystem->GetHighestEntityIndex() , MAX_TOTAL_ENTITIES - 1 );
	if ( highest < 0 )
		return;

	const ULONGLONG now = GetTickCount64();

	if ( m_FogScanCursor > highest )
	{
		if ( now < m_NextFogRescanTick )
			return;

		m_FogScanCursor = 0;
	}

	const int end = (std::min)( m_FogScanCursor + kEntitiesPerTick , highest + 1 );

	for ( int index = m_FogScanCursor; index < end; ++index )
	{
		auto* entity = entitySystem->GetBaseEntity<CEntityInstance>( index );
		if ( !entity )
			continue;

		const char* className = entity->GetSchemaClassName();
		bool isFogController = LooksLikeFogControllerName( className );

		if ( !isFogController )
		{
			if ( auto* identity = entity->pEntityIdentity() )
			{
				isFogController = LooksLikeFogControllerName( identity->DesingerName().String() ) ||
					LooksLikeFogControllerName( identity->Name().String() );
			}
		}

		if ( isFogController )
			RegisterFogController( entity );
	}

	m_FogScanCursor = end;

	if ( m_FogScanCursor > highest )
		m_NextFogRescanTick = now + 5000;
}

auto CAndromedaClient::OnCreateMove( CDOTAInput* pCDOTAInput , CUserCmd* pCUserCmd ) -> void
{
	GetAndromedaGUI()->ProcessHotkeys();

	static int s_nCallCount = 0;
	s_nCallCount++;

	if ( s_nCallCount <= 3 )
		DEV_LOG( "[andromeda] OnCreateMove called (call #%d, pCUserCmd=%p)\n" , s_nCallCount , pCUserCmd );

	// Apply before render so networked fog state cannot win for the next frame.
	ApplyNoFog();

	if ( Settings::Camera::Enable )
	{
		if ( m_pCameraDistance || m_pRFarz )
			SetCameraDistance( Settings::Camera::Distance );
	}

	// Deliberately NO usercmd gate here: pCUserCmd stayed null for entire
	// sessions on this build (see "[createmove] no usercmd" in debug.log)
	// while the local-controller resolve inside the hook was broken, and
	// neither controller below consumes the usercmd anyway - each resolves
	// the local hero itself (CLocalHeroResolver) and gates on that. Blocking
	// on pCUserCmd silently disabled the Invoker and Meepo pipelines.

	// Pick up fog controllers that existed before the entity hook was installed.
	// Work is bounded so map startup remains smooth.
	DiscoverFogControllers();

	m_InvokerController.OnCreateMove( pCDOTAInput , pCUserCmd );
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
