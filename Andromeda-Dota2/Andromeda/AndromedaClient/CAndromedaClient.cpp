#include "CAndromedaClient.hpp"
#include "CAndromedaGUI.hpp"

#include <Dota2/SDK/SDK.hpp>
#include <Dota2/SDK/Interface/CGameEntitySystem.hpp>
#include <Dota2/SDK/Types/CEntityData.hpp>
#include <Dota2/SDK/Types/CHandle.hpp>
#include <Dota2/SDK/CSchemaOffset.hpp>
#include <Dota2/SDK/Interface/CShemaSystemSDK.hpp>

#include <AndromedaClient/GUI/CAndromedaMenu.hpp>
#include <AndromedaClient/Settings/Settings.hpp>
#include <AndromedaClient/Data/HeroData.hpp>
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

static CAndromedaClient g_CAndromedaClient{};
static CHeroDataLoader g_HeroDataLoader{};

namespace
{
	struct HeroVitals
	{
		const char* name = "unknown";
		int playerId = -1;
		int teamSlot = -1;
		uint8_t team = 0;
		int health = 0;
		int maxHealth = 0;
		float mana = 0.f;
		float maxMana = 0.f;
	};

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
		uint32_t isIllusion = 0;
		uint32_t isClone = 0;
		bool hasPlayerId = false;
		bool hasPlayerOwnerId = false;
		bool hasHeroPlayerId = false;
		bool hasTeamSlotMapping = false;
		bool hasIsIllusion = false;
		bool hasIsClone = false;
		bool resolved = false;
	};

	struct HeroVitalsSnapshot
	{
		std::array<HeroVitals , 10> heroes{};
		int controllerCount = 0;
		int assignedHeroCount = 0;
		int fallbackHeroCount = 0;
		int scannedEntityCount = 0;
		int allocatedChunkCount = 0;
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
		offsets.hasTeamSlotMapping = hasPlayerTeamData && hasTeamSlot;
		offsets.hasIsIllusion = schema->TryGetOffset( "C_DOTA_BaseNPC" , "m_bIsIllusion" , offsets.isIllusion );
		offsets.hasIsClone = schema->TryGetOffset( "C_DOTA_BaseNPC" , "m_bIsClone" , offsets.isClone );
		offsets.resolved = hasHealth && hasMaxHealth && hasMana && hasMaxMana && hasTeam && hasAssignedHero;

		static bool logged = false;
		if ( !logged )
		{
			DEV_LOG( "[hero-vitals] schema health=%d(0x%X) maxHealth=%d(0x%X) mana=%d(0x%X) maxMana=%d(0x%X) team=%d(0x%X) assignedHero=%d(0x%X) playerId=%d(0x%X) heroPlayerId=%d(0x%X) teamData=%d(0x%X) teamSlot=%d(0x%X) illusion=%d(0x%X) clone=%d(0x%X)\n" ,
				hasHealth , offsets.health , hasMaxHealth , offsets.maxHealth , hasMana , offsets.mana ,
				hasMaxMana , offsets.maxMana , hasTeam , offsets.team , hasAssignedHero , offsets.assignedHero ,
				offsets.hasPlayerId , offsets.playerId , offsets.hasHeroPlayerId , offsets.heroPlayerId ,
				hasPlayerTeamData , offsets.playerTeamData , hasTeamSlot , offsets.teamSlot ,
				offsets.hasIsIllusion , offsets.isIllusion , offsets.hasIsClone , offsets.isClone );
			logged = true;
		}

		return offsets;
	}

	auto ResolvePlayerTeamSlots( CEntityInstance* playerResource , const HeroVitalsOffsets& offsets )
		-> std::array<int , 24>
	{
		std::array<int , 24> slots{};
		slots.fill( -1 );
		if ( !playerResource || !offsets.hasTeamSlotMapping )
			return slots;

		static const int teamDataStride = FindSchemaClassSize( "PlayerResourcePlayerTeamData_t" );
		if ( teamDataStride <= 0 || teamDataStride > 0x1000 )
			return slots;

		// C_UtlVectorEmbeddedNetworkVar<T> stores its CUtlVector<T> in the final
		// 0x18 bytes of its 0x68-byte wrapper (at +0x50).
		const uintptr_t vector = reinterpret_cast<uintptr_t>( playerResource ) + offsets.playerTeamData + 0x50;
		const int count = *reinterpret_cast<const int*>( vector );
		const auto* data = *reinterpret_cast<const uint8_t* const*>( vector + 0x8 );
		if ( count <= 0 || count > static_cast<int>( slots.size() ) || !data )
			return slots;

		for ( int playerId = 0; playerId < count; ++playerId )
		{
			const int slot = *reinterpret_cast<const int32_t*>( data + playerId * teamDataStride + offsets.teamSlot );
			if ( slot >= 0 && slot < 5 )
				slots[playerId] = slot;
		}

		return slots;
	}

	template <typename T>
	auto ReadEntityField( const CEntityInstance* entity , uint32_t offset ) -> T
	{
		return *reinterpret_cast<const T*>( reinterpret_cast<uintptr_t>( entity ) + offset );
	}

	auto StoreHeroVitals( HeroVitalsSnapshot& snapshot , C_DOTA_BaseNPC_Hero* hero ,
		int preferredPlayerId , const std::array<int , 24>& teamSlots , const HeroVitalsOffsets& offsets ) -> bool
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
		vitals.teamSlot = vitals.playerId >= 0 && vitals.playerId < static_cast<int>( teamSlots.size() ) ?
			teamSlots[vitals.playerId] : -1;
		vitals.team = team;
		vitals.health = (std::max)( 0 , ReadEntityField<int>( hero , offsets.health ) );
		vitals.maxHealth = maxHealth;
		vitals.mana = (std::max)( 0.f , ReadEntityField<float>( hero , offsets.mana ) );
		vitals.maxMana = (std::max)( 0.f , ReadEntityField<float>( hero , offsets.maxMana ) );
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
					if ( LooksLikePlayerResourceName( className ) )
						playerResourceHandle = identity->Handle();
					else if ( LooksLikePlayerControllerName( className ) && cachedControllerCount < static_cast<int>( controllerHandles.size() ) )
						controllerHandles[cachedControllerCount++] = identity->Handle();
					else if ( ( LooksLikeHeroEntityName( className ) ||
						std::strstr( identity->DesingerName().String() , "npc_dota_hero_" ) != nullptr ) &&
						cachedFallbackHeroCount < static_cast<int>( fallbackHeroHandles.size() ) )
						fallbackHeroHandles[cachedFallbackHeroCount++] = identity->Handle();
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
		const auto teamSlots = ResolvePlayerTeamSlots( playerResource , offsets );

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
			StoreHeroVitals( snapshot , hero , playerId , teamSlots , offsets );
		}

		// Some Dota builds do not expose controllers through the client entity
		// chunks. Fall back to real hero entities and reject illusions/clones.
		for ( int heroIndex = 0; heroIndex < cachedFallbackHeroCount; ++heroIndex )
		{
			auto* hero = static_cast<C_DOTA_BaseNPC_Hero*>( entitySystem->GetBaseEntityFromHandle( fallbackHeroHandles[heroIndex] ) );
			if ( !hero )
				continue;
			if ( offsets.hasIsIllusion && ReadEntityField<bool>( hero , offsets.isIllusion ) )
				continue;
			if ( offsets.hasIsClone && ReadEntityField<bool>( hero , offsets.isClone ) )
				continue;

			const int playerId = offsets.hasHeroPlayerId ? ReadEntityField<int32_t>( hero , offsets.heroPlayerId ) :
				( offsets.hasPlayerOwnerId ? ReadEntityField<int32_t>( hero , offsets.playerOwnerId ) : -1 );
			StoreHeroVitals( snapshot , hero , playerId , teamSlots , offsets );
		}

		return snapshot;
	}

	auto LogHeroVitals( const HeroVitalsSnapshot& snapshot ) -> void
	{
		static ULONGLONG nextLogTick = 0;
		const ULONGLONG now = GetTickCount64();

		if ( now < nextLogTick )
			return;

		nextLogTick = now + 1000;

		int loggedHeroes = 0;
		for ( const auto& hero : snapshot.heroes )
		{
			if ( hero.playerId < 0 || hero.maxHealth <= 0 )
				continue;

			DEV_LOG( "[hero-vitals] playerId=%d teamSlot=%d team=%u %s | HP %d/%d | Mana %.0f/%.0f\n" ,
				hero.playerId , hero.teamSlot , static_cast<unsigned>( hero.team ) , hero.name ,
				hero.health , hero.maxHealth , hero.mana , hero.maxMana );
			++loggedHeroes;
		}

		if ( loggedHeroes == 0 )
			DEV_LOG( "[hero-vitals] no active heroes: offsets=%d chunks=%d entities=%d controllers=%d assignedHeroes=%d heroCandidates=%d\n" ,
				snapshot.offsetsReady , snapshot.allocatedChunkCount , snapshot.scannedEntityCount ,
				snapshot.controllerCount , snapshot.assignedHeroCount , snapshot.fallbackHeroCount );
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
		const float fontSize = (std::max)( 8.f , size.y * 0.82f );
		const ImVec2 textSize = ImGui::GetFont()->CalcTextSizeA( fontSize , FLT_MAX , 0.f , text );
		const ImVec2 textPos( min.x + ( size.x - textSize.x ) * 0.5f , min.y + ( size.y - textSize.y ) * 0.5f );

		// A one-pixel outline keeps the values readable over both empty and full bars.
		drawList->AddText( ImGui::GetFont() , fontSize , ImVec2( textPos.x + 1.f , textPos.y + 1.f ) , IM_COL32( 0 , 0 , 0 , 235 ) , text );
		drawList->AddText( ImGui::GetFont() , fontSize , textPos , IM_COL32( 245 , 245 , 245 , 255 ) , text );
	}

	auto DrawHeroVitalsOverlay() -> void
	{
		const auto snapshot = CollectInPlayHeroVitals();
		LogHeroVitals( snapshot );
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
		const float barHeight = (std::max)( 7.f , display.y * 0.0095f );
		const float barTop = portraitBottom + 1.f;
		const float leftStart = display.x * 0.5f - scoreboardGap * 0.5f - teamWidth;
		const float rightStart = display.x * 0.5f + scoreboardGap * 0.5f;
		ImDrawList* drawList = ImGui::GetForegroundDrawList();
		int radiantFallbackSlot = 0;
		int direFallbackSlot = 0;

		for ( const auto& hero : heroes )
		{
			if ( hero.playerId < 0 || hero.maxHealth <= 0 )
				continue;

			int portraitSlot = -1;
			float groupStart = 0.f;
			if ( hero.team == 2 )
			{
				groupStart = leftStart;
				portraitSlot = hero.teamSlot >= 0 ? hero.teamSlot :
					( hero.playerId >= 0 && hero.playerId < 5 ? hero.playerId : radiantFallbackSlot++ );
			}
			else if ( hero.team == 3 )
			{
				groupStart = rightStart;
				// The top bar displays Dire team slots left to right as well. Mirroring
				// them here puts otherwise correct vitals under the opposite portrait.
				portraitSlot = hero.teamSlot >= 0 ? hero.teamSlot :
					( hero.playerId >= 5 && hero.playerId < 10 ? hero.playerId - 5 : direFallbackSlot++ );
			}
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
	constexpr const char* kHeroJsonUrl = "https://raw.githubusercontent.com/odota/dotaconstants/master/build/heroes.json";

	if ( g_HeroDataLoader.EnsureCacheAndLoad( kHeroJsonUrl , heroJsonPath ) )
		DEV_LOG( "[heroes] loaded %zu heroes from %s\n" , g_HeroDataLoader.GetAll().size() , g_HeroDataLoader.GetSourcePath().c_str() );
	else
		DEV_LOG( "[heroes] skip hero data load (missing/invalid file: %s)\n" , heroJsonPath.c_str() );

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

	DrawHeroVitalsOverlay();

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

	// Skip hero/Lua work until we have a real usercmd (match fully loaded).
	if ( !pCUserCmd )
		return;

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
