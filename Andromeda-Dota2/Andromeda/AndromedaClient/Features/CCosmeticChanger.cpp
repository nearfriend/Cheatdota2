#include "CCosmeticChanger.hpp"

#include <AndromedaClient/CAndromedaClient.hpp>
#include <AndromedaClient/Features/FeatureSupport.hpp>
#include <AndromedaClient/Settings/Settings.hpp>
#include <Common/DevLog.hpp>
#include <Dota2/SDK/CSchemaOffset.hpp>
#include <Dota2/SDK/FunctionListSDK.hpp>
#include <Dota2/SDK/Interface/CGameEntitySystem.hpp>
#include <Dota2/SDK/Interface/CLocalHeroResolver.hpp>
#include <Dota2/SDK/SDK.hpp>
#include <Dota2/SDK/Types/CEntityData.hpp>
#include <Dota2/SDK/Types/CHandle.hpp>
#include <Dota2/Hook/Hook_GetProtoCDOTAGameAccountPlus.hpp>
#include <Dota2/Hook/Hook_SetModel.hpp>
#include <DllLauncher.hpp>

#include <Windows.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace
{
	// Source 2 C_NetworkUtlVectorBase / CUtlVector: size@0x00, pad@0x04,
	// data ptr@0x08. Mirrors NetworkHandleVector in CAndromedaClient.cpp.
	struct NetworkHandleVector
	{
		int32_t size = 0;
		int32_t pad = 0;
		const CHandle* data = nullptr;
	};

	template <typename T>
	auto ReadAt( const void* base , uint32_t offset ) -> T
	{
		T value{};
		if ( base && offset )
			std::memcpy( &value , reinterpret_cast<const uint8_t*>( base ) + offset , sizeof( T ) );
		return value;
	}

	// Cheap plausibility test for a pointer read out of game memory: user-space,
	// past the null page, pointer-aligned. Reading m_pGameSceneNode off an ability
	// or an item (most of what the entity scan sees) yields garbage - small ints,
	// floats, packed ASCII - and nearly all of it fails one of these three. This
	// runs BEFORE the VirtualQuery-backed read so the garbage costs no kernel call
	// at all, which is what made the harvest affordable.
	auto PlausiblePointer( const void* pointer ) -> bool
	{
		const auto value = reinterpret_cast<uintptr_t>( pointer );
		return value >= 0x10000ull && value < 0x7FFFFFFFFFFFull && ( value & 7u ) == 0;
	}

	// Guarded bulk read.
	//
	// This MUST NOT fault. An earlier version used __try/__except on the theory
	// that SEH is free when the read succeeds - it is, but a fault here is
	// ruinous: the cheat's exception handler dumps a 256-line stack trace to the
	// log for every exception in the process, including ones __except swallows.
	// One run produced 773 dumps and a 7.3MB log, and the file I/O alone was worse
	// than anything it saved. That is why the codebase validates instead.
	//
	// PlausiblePointer is still worth running first: it is free and rejects most
	// garbage before the kernel call. What is left is one VirtualQuery per read,
	// paid only on a cache miss - the per-entity cache is what keeps that rare,
	// and the bulk copy is what keeps it to one call instead of 48.
	auto SafeCopy( void* destination , const void* source , size_t size ) -> bool
	{
		if ( !PlausiblePointer( source ) ||
			!FeatureSupport::IsReadableRuntimeMemory( source , size ) )
			return false;
		std::memcpy( destination , source , size );
		return true;
	}

	template <typename T>
	auto SafeRead( const void* address , T& out ) -> bool
	{
		return PlausiblePointer( address ) && SafeCopy( &out , address , sizeof( T ) );
	}

	// Entity name of a hero's cosmetic slot entities on this build.
	constexpr const char* kWearableName = "dota_item_wearable";

	auto LoadCosmeticModel( const std::string& path ) -> void*
	{
		// ResourcePath uses a 200-byte inline string plus two engine-computed hashes.
		// Restrict input to that capacity so no engine heap allocation needs cleanup.
		struct ResourcePath
		{
			uint32_t length = 0;
			uint32_t capacity = 0xC00000C8;
			char inlinePath[200] = {};
			uint64_t hash = 0;
			uint64_t type = 0;
		};
		static_assert( sizeof( ResourcePath ) == 0xE0 );
		if ( path.size() >= 200 || path.compare( 0 , 7 , "models/" ) != 0 ||
			path.size() < 5 || path.compare( path.size() - 5 , 5 , ".vmdl" ) != 0 )
			return nullptr;
		void* initialize = GetFunctionList()->ResourcePath_Init.GetFunction();
		HMODULE module = GetModuleHandleA( "resourcesystem.dll" );
		if ( !initialize || !module )
			return nullptr;
		using Factory = void* ( __cdecl* )( const char* , int* );
		auto factory = reinterpret_cast<Factory>( GetProcAddress( module , "CreateInterface" ) );
		void* system = factory ? factory( "ResourceSystem013" , nullptr ) : nullptr;
		void** table = nullptr;
		if ( !SafeRead( system , table ) ||
			!FeatureSupport::IsReadableRuntimeMemory( table , 0x280 ) )
			return nullptr;
		void* load = table[0x140 / sizeof( void* )];
		// Fail closed if the interface implementation changed. Verified against
		// resourcesystem.dll RVA 0x16720 and the client caller at 0x22E7AFC.
		constexpr uint8_t prologue[] = { 0x48, 0x89, 0x5C, 0x24, 0x10,
			0x48, 0x89, 0x6C, 0x24, 0x18, 0x48, 0x89, 0x74, 0x24, 0x20,
			0x57, 0x48, 0x83, 0xEC, 0x60 };
		if ( !FeatureSupport::IsReadableRuntimeMemory( load , sizeof( prologue ) ) ||
			std::memcmp( load , prologue , sizeof( prologue ) ) != 0 )
			return nullptr;
		ResourcePath resource;
		using Initialize = bool ( __fastcall* )( ResourcePath* , const char* );
		if ( !reinterpret_cast<Initialize>( initialize )( &resource , path.c_str() ) )
			return nullptr;
		using Load = void* ( __fastcall* )( void* , ResourcePath* , const char* );
		void* binding = reinterpret_cast<Load>( load )( system , &resource , "cosmetic_swap" );
		using State = int ( __fastcall* )( void* , void* );
		return binding && reinterpret_cast<State>( table[0x188 / sizeof( void* )] )( system , binding ) == 3
			? binding : nullptr;
	}

	// NOTE: an in-match test confirmed that rewriting m_iItemDefinitionIndex on an
	// already-spawned wearable does NOT swap the rendered model - Source 2 caches
	// the model handle at spawn. So ApplySelections writes the defindex only to keep
	// the item view consistent, and relies on SetModel for the visible swap.

	auto SplitTabLine( const std::string& line ) -> std::vector<std::string>
	{
		std::vector<std::string> fields;
		size_t start = 0;
		while ( start <= line.size() )
		{
			const size_t tab = line.find( '\t' , start );
			if ( tab == std::string::npos )
			{
				fields.push_back( line.substr( start ) );
				break;
			}

			fields.push_back( line.substr( start , tab - start ) );
			start = tab + 1;
		}

		return fields;
	}

	auto HeroFromModelPath( const char* model ) -> std::string
	{
		if ( !model || !model[0] )
			return {};

		const char* prefix = std::strstr( model , "models/items/" );
		size_t prefixLength = std::strlen( "models/items/" );
		if ( !prefix )
		{
			prefix = std::strstr( model , "models/heroes/" );
			prefixLength = std::strlen( "models/heroes/" );
		}
		if ( !prefix )
			return {};

		prefix += prefixLength;
		const char* end = std::strchr( prefix , '/' );
		if ( !end || end <= prefix )
			return {};

		return std::string( prefix , static_cast<size_t>( end - prefix ) );
	}

	auto LowerText( std::string text ) -> std::string
	{
		for ( auto& character : text )
			character = static_cast<char>( std::tolower( static_cast<unsigned char>( character ) ) );
		return text;
	}

	auto ContainsText( const std::string& text , const char* needle ) -> bool
	{
		return text.find( needle ) != std::string::npos;
	}

	auto CosmeticCategory( const std::string& slot , const std::string& name , const std::string& model ) -> std::string
	{
		const std::string raw = LowerText( slot + " " + name + " " + model );

		if ( ContainsText( raw , "offhand" ) || ContainsText( raw , "off-hand" ) || ContainsText( raw , "shield" ) )
			return "offhand";
		if ( ContainsText( raw , "weapon" ) || ContainsText( raw , "sword" ) || ContainsText( raw , "axe" ) || ContainsText( raw , "staff" ) || ContainsText( raw , "blade" ) )
			return "weapon";
		if ( ContainsText( raw , "belt" ) || ContainsText( raw , "waist" ) )
			return "belt";
		if ( ContainsText( raw , "head" ) || ContainsText( raw , "helm" ) || ContainsText( raw , "hair" ) || ContainsText( raw , "mask" ) || ContainsText( raw , "hat" ) )
			return "head";
		if ( ContainsText( raw , "arms" ) || ContainsText( raw , "bracer" ) || ContainsText( raw , "glove" ) || ContainsText( raw , "gauntlet" ) )
			return "arms";
		if ( ContainsText( raw , "wings" ) )
			return "wings";
		if ( ContainsText( raw , "back" ) || ContainsText( raw , "cape" ) || ContainsText( raw , "cloak" ) )
			return "back";
		if ( ContainsText( raw , "shoulder" ) || ContainsText( raw , "pauldron" ) )
			return "shoulder";
		if ( ContainsText( raw , "armor" ) || ContainsText( raw , "body" ) || ContainsText( raw , "chest" ) )
			return "armor";
		if ( ContainsText( raw , "mount" ) )
			return "mount";
		if ( ContainsText( raw , "tail" ) )
			return "tail";
		if ( ContainsText( raw , "ambient" ) || ContainsText( raw , "effect" ) || ContainsText( raw , "taunt" ) )
			return "effect";

		if ( !slot.empty() && slot != "unknown" )
			return slot;
		return "other";
	}

	auto CosmeticImageFromModel( const std::string& model ) -> std::string
	{
		constexpr const char* prefix = "models/items/";
		const size_t start = model.find( prefix );
		if ( start == std::string::npos )
			return {};

		std::string image = "econ/items/" + model.substr( start + std::strlen( prefix ) );
		constexpr const char* extension = ".vmdl";
		const size_t extensionPos = image.rfind( extension );
		if ( extensionPos != std::string::npos )
			image.erase( extensionPos );
		return image;
	}

	auto FindCatalogItem( const std::string& hero , const std::string& slot , uint32_t defIndex ) -> const CCosmeticChanger::CatalogItem*
	{
		const auto& catalog = CCosmeticChanger::GetCatalog();
		for ( const auto& item : catalog )
		{
			if ( item.hero == hero && ( item.slot == slot || item.category == slot ) && item.defIndex == defIndex )
				return &item;
		}
		return nullptr;
	}

	// Item-definition indexes are globally unique across Dota's econ, so a lookup
	// on the defindex alone identifies an equipped wearable outright - and yields
	// its hero, name and category. That matters because the model-path read fails
	// for a good number of live wearables (logged as "model=?"), and the hero was
	// otherwise inferred only from that path: this resolves name, slot and hero for
	// every equipped slot whose defindex is a real catalog cosmetic.
	auto FindCatalogItemByDefIndex( uint32_t defIndex ) -> const CCosmeticChanger::CatalogItem*
	{
		if ( defIndex == 0 )
			return nullptr;

		const auto& catalog = CCosmeticChanger::GetCatalog();
		for ( const auto& item : catalog )
		{
			if ( item.defIndex == defIndex )
				return &item;
		}
		return nullptr;
	}

	auto CosmeticCatalog() -> const std::vector<CCosmeticChanger::CatalogItem>&
	{
		static std::vector<CCosmeticChanger::CatalogItem> catalog;
		static bool loaded = false;
		static bool loggedMissing = false;

		if ( loaded )
			return catalog;

		loaded = true;

		const std::string path = CCosmeticChanger::GetCatalogPath();
		std::ifstream file( path );
		if ( !file.is_open() )
		{
			if ( !loggedMissing )
			{
				DEV_LOG( "[cosmetic-catalog] missing %s - generate/copy it next to the DLL to log all item names\n" , path.c_str() );
				loggedMissing = true;
			}
			return catalog;
		}

		std::string line;
		bool header = true;
		while ( std::getline( file , line ) )
		{
			if ( header )
			{
				header = false;
				continue;
			}

			auto fields = SplitTabLine( line );
			if ( fields.size() < 7 )
				continue;

			CCosmeticChanger::CatalogItem item;
			item.hero = fields[0];
			const std::string rawSlot = fields[1];
			item.defIndex = static_cast<uint32_t>( std::strtoul( fields[2].c_str() , nullptr , 10 ) );
			item.name = fields[4].empty() ? fields[3] : fields[4];
			item.rarity = fields[5];
			item.model = fields[6];
			item.image = fields.size() > 7 ? fields[7] : std::string();
			if ( item.image.empty() )
				item.image = CosmeticImageFromModel( item.model );

			// items_game's own item_slot is authoritative, so when the export carries
			// it we use it verbatim for both the slot key and the category. The
			// keyword heuristic below is only a fallback for the handful of rows with
			// no slot at all: it matches on name+model too, so an item's NAME can
			// hijack its classification (a belt called "...Blade..." scores as a
			// weapon because "blade" is tested before "belt").
			//
			// Selections are keyed by (hero, slot), so the slot must be a real body
			// part - leaving every row "unknown" collapses the whole loadout into one
			// override, where picking a head silently replaces the weapon picked a
			// moment earlier.
			if ( !rawSlot.empty() )
			{
				item.slot = LowerText( rawSlot );
				item.category = item.slot;
			}
			else
			{
				item.category = CosmeticCategory( std::string() , item.name , item.model );
				item.slot = item.category;
			}
			catalog.push_back( item );
		}

		DEV_LOG( "[cosmetic-catalog] loaded %zu items from %s\n" , catalog.size() , path.c_str() );
		return catalog;
	}

	auto LogCatalogForHero( const std::string& hero , bool force ) -> void
	{
		static std::vector<std::string> loggedHeroes;

		if ( hero.empty() )
			return;

		if ( !force && std::find( loggedHeroes.begin() , loggedHeroes.end() , hero ) != loggedHeroes.end() )
			return;

		const auto& catalog = CosmeticCatalog();
		if ( catalog.empty() )
			return;

		int count = 0;
		for ( const auto& item : catalog )
			if ( item.hero == hero )
				++count;

		if ( count <= 0 )
			return;

		if ( !force )
			loggedHeroes.push_back( hero );
		DEV_LOG( "[cosmetic-catalog] === all catalog cosmetics for hero=%s (%d items) ===\n" , hero.c_str() , count );
		for ( const auto& item : catalog )
		{
			if ( item.hero != hero )
				continue;

			DEV_LOG( "  [cosmetic-catalog] def=%u slot=%s category=%s rarity=%s name=%s model=%s image=%s\n" ,
				item.defIndex ,
				item.slot.c_str() ,
				item.category.c_str() ,
				item.rarity.empty() ? "?" : item.rarity.c_str() ,
				item.name.c_str() ,
				item.model.c_str() ,
				item.image.c_str() );
		}
	}
}

auto CCosmeticChanger::GetCatalog() -> const std::vector<CatalogItem>&
{
	return CosmeticCatalog();
}

auto CCosmeticChanger::GetCatalogPath() -> std::string
{
	return GetDllDir() + "cosmetic_catalog.tsv";
}

auto CCosmeticChanger::LogCatalogForHeroName( const std::string& hero , bool force ) -> void
{
	LogCatalogForHero( hero , force );
}

auto CCosmeticChanger::GetCurrentHero() const -> std::string
{
	std::scoped_lock lock( m_SnapshotMutex );
	return m_CurrentHero;
}

auto CCosmeticChanger::GetCurrentHeroIndex() const -> int
{
	std::scoped_lock lock( m_SnapshotMutex );
	return m_CurrentHeroIndex;
}

auto CCosmeticChanger::IsModelWorn( const CatalogItem& item ) const -> bool
{
	std::scoped_lock lock( m_SnapshotMutex );
	for ( const auto& worn : m_Wearables )
	{
		// The defindex is what the item view says; the model path is what is
		// actually rendered. Either one matching means this item is on the hero -
		// after an apply the two agree, but a failed model write leaves only the
		// defindex, and an unreadable item view leaves only the model.
		if ( item.defIndex != 0 && worn.defIndex == item.defIndex )
			return true;
		if ( !item.model.empty() && worn.model == item.model )
			return true;
	}
	return false;
}

auto CCosmeticChanger::IsModelAvailable( const CatalogItem& item ) const -> bool
{
	if ( item.model.empty() )
		return false;

	std::scoped_lock lock( m_HookMutex );
	return m_Bindings.find( item.model ) != m_Bindings.end();
}

auto CCosmeticChanger::AvailableModelCount() const -> size_t
{
	std::scoped_lock lock( m_HookMutex );
	return m_Bindings.size();
}

auto CCosmeticChanger::CanApply() const -> bool
{
	// Either way of reaching CSkeletonInstance::SetModel counts: the resolved
	// address, or the hook trampoline. The trampoline matters because MinHook
	// overwrites the function's first bytes with a jump - if CFunctionList's
	// pattern search runs after the hook is enabled it will miss, and the
	// trampoline is then the only usable entry. The m_ModelName route was tried
	// and disproven - see the note in WriteWearable.
	return ( SDK_SetModelAvailable() || SetModel_o != nullptr ) &&
		BuildCombinedModel_o != nullptr && GetItemModel_o != nullptr;
}

auto CCosmeticChanger::GetSelectionsPath() -> std::string
{
	return GetDllDir() + "cosmetic_selections.tsv";
}

auto CCosmeticChanger::LoadSelections() -> void
{
	// The menu calls this every frame, so the read happens once and every later
	// call is a single bool test. Saving is what keeps the file current after that.
	static bool loaded = false;
	if ( loaded )
		return;
	loaded = true;

	const std::string path = GetSelectionsPath();
	std::ifstream file( path );
	if ( !file.is_open() )
		return;

	std::scoped_lock lock( Settings::CosmeticChanger::Mutex );
	std::string line;
	int restored = 0;
	while ( std::getline( file , line ) )
	{
		if ( line.empty() || line[0] == '#' )
			continue;

		const auto fields = SplitTabLine( line );
		if ( fields.size() < 3 )
			continue;

		// A non-numeric defindex yields 0, which SetSelection rejects - that is also
		// what skips the header row without a special case for it.
		const uint32_t defIndex = static_cast<uint32_t>( std::strtoul( fields[2].c_str() , nullptr , 10 ) );
		if ( defIndex == 0 )
			continue;

		Settings::CosmeticChanger::SetSelection( fields[0] , fields[1] , defIndex );
		++restored;
	}

	if ( restored > 0 )
		DEV_LOG( "[cosmetic-ui] restored %d saved selection(s) from %s\n" , restored , path.c_str() );
}

auto CCosmeticChanger::SaveSelections() -> void
{
	const std::string path = GetSelectionsPath();
	std::scoped_lock lock( Settings::CosmeticChanger::Mutex );
	std::ofstream file( path , std::ios::trunc );
	if ( !file.is_open() )
	{
		static bool warned = false;
		if ( !warned )
		{
			DEV_LOG( "[cosmetic-ui] cannot write %s - selections will not survive a restart\n" , path.c_str() );
			warned = true;
		}
		return;
	}

	file << "hero\tslot\tdefindex\n";
	for ( const auto& selection : Settings::CosmeticChanger::Selections )
		file << selection.hero << '\t' << selection.slot << '\t' << selection.defIndex << '\n';
}

auto CCosmeticChanger::GetStatus() const -> std::string
{
	std::scoped_lock lock( m_SnapshotMutex );
	if ( !Settings::CosmeticChanger::Enable )
		return "disabled";
	if ( !m_ResolveTried )
		return "waiting for first cosmetic pass";
	if ( !m_Offsets.resolved )
		return "schema unresolved";
	if ( m_CurrentHero.empty() )
		return "waiting for local hero wearables";

	// The catalog supersedes the GC/SO-cache route for listing cosmetics, so what
	// matters here is whether a swap can actually be applied - i.e. SetModel.
	const char* apply = !CanApply()
		? "preview only (no swap route)"
		: ( Settings::CosmeticChanger::ApplyOverrides
			? "loading and applying selected models"
			: "apply off" );

	char buffer[224] = {};
	snprintf( buffer , sizeof( buffer ) , "ready: %s (%zu wearable slots, %s, %zu models loaded)" ,
		m_CurrentHero.c_str() , m_Wearables.size() , apply , RecordedBindingCount() );
	return buffer;
}

auto CCosmeticChanger::RequestCatalogDump() -> void
{
	m_ForceCatalogLog = true;
}

auto CCosmeticChanger::ResolveOffsets() -> bool
{
	if ( m_Offsets.resolved )
		return true;
	m_ResolveTried = true;

	auto* schema = GetSchemaOffset();
	if ( !schema )
		return false;

	// The wearable handle vector hangs off the combat-character base; some
	// builds surface it on the DOTA npc binding instead.
	m_Offsets.hasWearables =
		schema->TryGetOffset( "C_BaseCombatCharacter" , "m_hMyWearables" , m_Offsets.myWearables ) ||
		schema->TryGetOffset( "C_DOTA_BaseNPC" , "m_hMyWearables" , m_Offsets.myWearables );

	// defindex = wearable + m_AttributeManager + m_Item + m_iItemDefinitionIndex.
	m_Offsets.hasAttr =
		schema->TryGetOffset( "C_EconEntity" , "m_AttributeManager" , m_Offsets.attributeManager );
	// m_Item lives inside the embedded C_AttributeContainer. The class/field
	// spelling has drifted between builds, so try the known variants.
	m_Offsets.hasItem =
		schema->TryGetOffset( "C_AttributeContainer" , "m_Item" , m_Offsets.item ) ||
		schema->TryGetOffset( "CAttributeContainer" , "m_Item" , m_Offsets.item ) ||
		schema->TryGetOffset( "C_EconItemAttributeContainer" , "m_Item" , m_Offsets.item );
	m_Offsets.hasDef =
		schema->TryGetOffset( "C_EconItemView" , "m_iItemDefinitionIndex" , m_Offsets.itemDefinitionIndex );

	// Owner handle, for the owner-scan fallback when m_hMyWearables is empty.
	m_Offsets.hasOwner =
		schema->TryGetOffset( "C_BaseEntity" , "m_hOwnerEntity" , m_Offsets.ownerEntity ) ||
		schema->TryGetOffset( "C_BaseEntity" , "m_hOwner" , m_Offsets.ownerEntity );

	// Scene-node -> skeleton -> model-resource chain, purely for a readable model
	// path in the log. Best-effort: not part of the resolved gate, so a build
	// that spells these differently just loses the path, never the feature.
	m_Offsets.hasModelPath =
		schema->TryGetOffset( "C_BaseEntity" , "m_pGameSceneNode" , m_Offsets.sceneNode ) &&
		schema->TryGetOffset( "CSkeletonInstance" , "m_modelState" , m_Offsets.modelState ) &&
		schema->TryGetOffset( "CModelState" , "m_hModel" , m_Offsets.modelHandle );

	// The name field and its dirty flag. Both optional - they only enable the
	// alternative swap route, and losing them costs nothing that already worked.
	m_Offsets.hasModelName = schema->TryGetOffset( "CModelState" , "m_ModelName" , m_Offsets.modelName );
	m_Offsets.hasModelChanged = schema->TryGetOffset( "CModelState" , "m_modelChanged" , m_Offsets.modelChanged );

	// Extra econ metadata on the item view (relative to the item-view base, i.e.
	// added to attributeManager + item). Each is optional; whichever resolves gets
	// logged. These names are stable on recent builds but guarded either way.
	const bool q = schema->TryGetOffset( "C_EconItemView" , "m_iEntityQuality" , m_Offsets.itemQuality );
	const bool l = schema->TryGetOffset( "C_EconItemView" , "m_iEntityLevel" , m_Offsets.itemLevel );
	const bool id = schema->TryGetOffset( "C_EconItemView" , "m_iItemID" , m_Offsets.itemId );
	const bool acc = schema->TryGetOffset( "C_EconItemView" , "m_iAccountID" , m_Offsets.itemAccountId );
	const bool inv = schema->TryGetOffset( "C_EconItemView" , "m_iInventoryPosition" , m_Offsets.itemInvPos );
	m_Offsets.hasItemMeta = q || l || id || acc || inv;

	// If m_Item did not resolve, fall back to the best-known constant so we can
	// still attempt a read this run - the value is flagged so a bad guess shows
	// up as nonsense defindexes in the log rather than a silent no-op.
	if ( !m_Offsets.hasItem )
	{
		m_Offsets.item = kFallbackItemOffset;
		m_Offsets.usedFallbackItem = true;
	}

	m_Offsets.hasDefIndex = m_Offsets.hasAttr && m_Offsets.hasDef && ( m_Offsets.hasItem || m_Offsets.usedFallbackItem );
	// m_hMyWearables reads empty here, so the owner-scan (needs hasOwner) is the
	// real enumeration path - either source of wearables is enough to proceed.
	m_Offsets.resolved = m_Offsets.hasDefIndex && ( m_Offsets.hasWearables || m_Offsets.hasOwner );

	if ( !m_LoggedSchema )
	{
		DEV_LOG( "[cosmetic] schema wearables=%d(0x%X) attrMgr=%d(0x%X) item=%d(0x%X)%s defIdx=%d(0x%X) owner=%d(0x%X)\n" ,
			m_Offsets.hasWearables , m_Offsets.myWearables ,
			m_Offsets.hasAttr , m_Offsets.attributeManager ,
			m_Offsets.hasItem , m_Offsets.item , m_Offsets.usedFallbackItem ? "[fallback]" : "" ,
			m_Offsets.hasDef , m_Offsets.itemDefinitionIndex ,
			m_Offsets.hasOwner , m_Offsets.ownerEntity );
		DEV_LOG( "[cosmetic] model-path chain: gsn=%d(0x%X) modelState=%d(0x%X) hModel=%d(0x%X) ok=%d\n" ,
			m_Offsets.sceneNode != 0 , m_Offsets.sceneNode ,
			m_Offsets.modelState != 0 , m_Offsets.modelState ,
			m_Offsets.modelHandle != 0 , m_Offsets.modelHandle ,
			m_Offsets.hasModelPath );
		DEV_LOG( "[cosmetic] model-name route: m_ModelName=%d(0x%X) m_modelChanged=%d(0x%X)\n" ,
			m_Offsets.hasModelName , m_Offsets.modelName ,
			m_Offsets.hasModelChanged , m_Offsets.modelChanged );
		// THE MODEL COMBINER. Dota does not render a hero and its wearables as
		// separate meshes - it merges them into one combined model, and the
		// renderer draws that, not the individual wearable model handles we have
		// been setting. client.dll carries "skip_model_combine",
		// "model_combiner_dumpstats", "m_combinerMaterialOverrideList" and the two
		// index fields below, so the system is definitely present on this build.
		//
		// The index fields have NO code xrefs, meaning they are schema fields and
		// therefore reachable through exactly this system - but their owning class
		// is unknown, so sweep every class for them. What comes back tells us where
		// the combined-model state lives, which is the level a visible swap has to
		// operate at.
		schema->LogFieldsAnywhere( "CombinedModel" );
		schema->LogFieldsAnywhere( "combiner" );
		schema->LogFieldsAnywhere( "Wearable" );

		// m_modelChanged did not resolve under that spelling. Dump the whole class
		// once (empty needle matches every field) so the real name of the
		// model-dirty flag - the trigger the name write needs - is on the record.
		if ( !m_Offsets.hasModelChanged )
			schema->LogFieldsMatching( "CModelState" , "" );
		if ( !m_Offsets.hasOwner )
			schema->LogFieldsMatching( "C_BaseEntity" , "Owner" );
		if ( !m_Offsets.hasModelPath )
		{
			// Pin the true spelling of whichever link missed, next run.
			schema->LogFieldsMatching( "C_BaseEntity" , "SceneNode" );
			schema->LogFieldsMatching( "CSkeletonInstance" , "model" );
			schema->LogFieldsMatching( "CModelState" , "" );
		}
		DEV_LOG( "[cosmetic] item-meta: quality=0x%X level=0x%X id=0x%X acct=0x%X invPos=0x%X (any=%d)\n" ,
			m_Offsets.itemQuality , m_Offsets.itemLevel , m_Offsets.itemId ,
			m_Offsets.itemAccountId , m_Offsets.itemInvPos , m_Offsets.hasItemMeta );
		m_LoggedSchema = true;
	}

	// One-time full dump of the econ item view / attribute classes so we can read
	// the exact field names and later enumerate each item's dynamic attributes.
	DumpItemViewSchema();

	// One-time field dump so a single run reveals the true m_Item spelling/offset
	// (and neighbouring fields) whenever the schema lookup missed it.
	if ( !m_Offsets.hasItem )
		DumpItemSchema();

	return m_Offsets.resolved;
}

auto CCosmeticChanger::DumpItemSchema() -> void
{
	if ( m_LoggedFields )
		return;
	m_LoggedFields = true;

	auto* schema = GetSchemaOffset();
	if ( !schema )
		return;

	// Dump every field of the container/view classes plus the econ-entity's
	// attribute field so we can pin the real name of m_Item next run. An empty
	// needle matches all fields; LogFieldsMatching reports missing classes too.
	DEV_LOG( "[cosmetic] m_Item unresolved - dumping candidate classes:\n" );
	schema->LogFieldsMatching( "C_AttributeContainer" , "" );
	schema->LogFieldsMatching( "CAttributeContainer" , "" );
	schema->LogFieldsMatching( "C_EconItemView" , "" );
	schema->LogFieldsMatching( "C_EconEntity" , "Attribute" );
	schema->LogFieldsMatching( "C_EconEntity" , "Item" );
}

auto CCosmeticChanger::WearableModelPath( C_BaseEntity* wearable ) const -> const char*
{
	// Every hop is validated - the wearable is the game's own pointer, but the
	// scene node, the resource binding and the model object are all read out of
	// game memory, so a stale or mis-offset pointer must fail closed, not crash.
	if ( !m_Offsets.hasModelPath || !wearable )
		return "";

	void* binding = WearableModelBinding( wearable );
	return binding ? ModelPathFromBinding( binding ) : "";
}

auto CCosmeticChanger::ModelPathFromBinding( const void* binding ) const -> const char*
{
	// ResourceBindingBase_t::data (+0) is the CModel*. Validated: the binding may
	// have come from the hook on another thread, or from an entity that has since
	// despawned, so a dead pointer must fail closed here rather than fault.
	void* model = nullptr;
	if ( !SafeRead( binding , model ) || !model )
		return "";

	// The model's own vpk path (".../foo.vmdl") is stored as a string pointer in
	// the model header. Rather than hard-code a build-specific offset, scan the
	// first handful of pointer slots for one that points at a readable string
	// looking like a model path. Guarded throughout, so a miss just returns "".
	//
	// COST: this used to validate every slot read AND every candidate separately -
	// up to 48 VirtualQuery calls per model, which measured at ~3ms and made the
	// harvest unaffordable. Now the header is validated once and copied in bulk,
	// and only pointers that survive the free plausibility test cost a kernel
	// call. Same result, typically one or two VirtualQuery instead of 48.
	static thread_local char buffer[128];
	constexpr int kSlotsToScan = 24; // 24 * 8 = first 0xC0 bytes of the header
	const void* header[kSlotsToScan] = {};
	if ( !PlausiblePointer( model ) || !SafeCopy( header , model , sizeof( header ) ) )
		return "";

	for ( int slot = 0; slot < kSlotsToScan; ++slot )
	{
		const char* candidate = reinterpret_cast<const char*>( header[slot] );
		// Strings are byte-aligned; requiring pointer alignment discards valid paths.
		const auto address = reinterpret_cast<uintptr_t>( candidate );
		if ( address < 0x10000ull || address >= 0x7FFFFFFFFFFFull )
			continue;

		// Copy a bounded run in one guarded go, then inspect the copy. Reading the
		// string byte-by-byte straight out of game memory would re-risk a fault on
		// every character; this faults at most once, on the copy.
		char raw[128] = {};
		if ( !FeatureSupport::IsReadableRuntimeMemory( candidate , sizeof( raw ) ) )
			continue;
		std::memcpy( raw , candidate , sizeof( raw ) );

		int n = 0;
		for ( ; n < static_cast<int>( sizeof( buffer ) ) - 1 && n < static_cast<int>( sizeof( raw ) ); ++n )
		{
			char c = raw[n];
			if ( c == '\0' )
				break;
			if ( c < 0x20 || c > 0x7E )    // non-printable -> not a path string
			{
				n = -1;
				break;
			}
			buffer[n] = c;
		}
		if ( n <= 0 )
			continue;
		buffer[n] = '\0';

		if ( std::strstr( buffer , "models/" ) || std::strstr( buffer , ".vmdl" ) )
			return buffer;
	}

	return "";
}

auto CCosmeticChanger::DumpItemViewSchema() -> void
{
	if ( m_LoggedItemView )
		return;
	m_LoggedItemView = true;

	auto* schema = GetSchemaOffset();
	if ( !schema )
		return;

	// Full field lists for the econ item view and its attribute container/list,
	// so a single run reveals the exact offsets of every per-item field (quality,
	// level, id, style, the attribute vector, etc.) on this build.
	DEV_LOG( "[cosmetic] dumping C_EconItemView / attribute schema:\n" );
	schema->LogFieldsMatching( "C_EconItemView" , "" );
	schema->LogFieldsMatching( "C_AttributeList" , "" );
	schema->LogFieldsMatching( "CAttributeList" , "" );
	schema->LogFieldsMatching( "C_EconItemAttribute" , "" );
	schema->LogFieldsMatching( "CEconItemAttribute" , "" );
}

auto CCosmeticChanger::LogWearableItemInfo( C_BaseEntity* wearable , int slot ) const -> void
{
	if ( !wearable || !m_Offsets.hasItemMeta )
		return;

	// The item view sits at wearable + attributeManager + item (embedded structs,
	// no pointer hops). Every field read is validated - these live ~0xB40 in, past
	// the size of a non-econ entity, so an unvalidated read there is unsafe.
	auto* base = reinterpret_cast<const uint8_t*>( wearable ) + m_Offsets.attributeManager + m_Offsets.item;

	uint8_t quality = 0;
	uint32_t level = 0;
	uint64_t itemId = 0;
	uint32_t accountId = 0;
	uint32_t invPos = 0;
	const bool hasQ = m_Offsets.itemQuality && FeatureSupport::TryRead( base + m_Offsets.itemQuality , quality );
	const bool hasL = m_Offsets.itemLevel && FeatureSupport::TryRead( base + m_Offsets.itemLevel , level );
	const bool hasId = m_Offsets.itemId && FeatureSupport::TryRead( base + m_Offsets.itemId , itemId );
	const bool hasAcc = m_Offsets.itemAccountId && FeatureSupport::TryRead( base + m_Offsets.itemAccountId , accountId );
	const bool hasInv = m_Offsets.itemInvPos && FeatureSupport::TryRead( base + m_Offsets.itemInvPos , invPos );

	DEV_LOG( "      sub-info slot %d: quality=%s%u level=%s%u itemid=%s%llu acct=%s%u invPos=%s%u\n" ,
		slot ,
		hasQ ? "" : "(n/a)" , static_cast<unsigned>( quality ) ,
		hasL ? "" : "(n/a)" , static_cast<unsigned>( level ) ,
		hasId ? "" : "(n/a)" , static_cast<unsigned long long>( itemId ) ,
		hasAcc ? "" : "(n/a)" , static_cast<unsigned>( accountId ) ,
		hasInv ? "" : "(n/a)" , static_cast<unsigned>( invPos ) );
}

auto CCosmeticChanger::LogModelNameField( C_BaseEntity* wearable , int slot ) const -> void
{
	if ( !m_Offsets.hasModelName )
		return;

	void* sceneNode = WearableSkeleton( wearable );
	if ( !sceneNode )
		return;

	const uint8_t* field = reinterpret_cast<const uint8_t*>( sceneNode ) +
		m_Offsets.modelState + m_Offsets.modelName;

	// Read the raw qword first. Whatever the field turns out to be - a char*, an
	// interned symbol pointer, or the first 8 bytes of an inline buffer - this
	// value plus the decode attempts below identify it.
	uint64_t raw = 0;
	if ( !FeatureSupport::TryRead( field , raw ) )
	{
		DEV_LOG( "      [model-name] slot %d: field at +0x%X unreadable\n" ,
			slot , m_Offsets.modelState + m_Offsets.modelName );
		return;
	}

	auto readableText = []( const void* address ) -> const char*
	{
		static thread_local char text[160];
		if ( !address || !FeatureSupport::IsReadableRuntimeMemory( address , 8 ) )
			return nullptr;
		const char* source = reinterpret_cast<const char*>( address );
		int n = 0;
		for ( ; n < static_cast<int>( sizeof( text ) ) - 1; ++n )
		{
			const char c = source[n];
			if ( c == '\0' )
				break;
			if ( c < 0x20 || c > 0x7E )
				return nullptr;
			text[n] = c;
		}
		if ( n <= 0 )
			return nullptr;
		text[n] = '\0';
		return text;
	};

	// Case 1: the field IS the string (inline buffer). Case 2: the field points at
	// the string (char* or interned symbol).
	const char* inlineText = readableText( field );
	const char* pointedText = readableText( reinterpret_cast<const void*>( raw ) );

	DEV_LOG( "      [model-name] slot %d: +0x%X raw=0x%llX inline=%s pointed=%s\n" ,
		slot ,
		m_Offsets.modelState + m_Offsets.modelName ,
		static_cast<unsigned long long>( raw ) ,
		inlineText ? inlineText : "-" ,
		pointedText ? pointedText : "-" );
}

auto CCosmeticChanger::WearableSkeleton( C_BaseEntity* wearable ) const -> void*
{
	if ( !m_Offsets.hasModelPath || !wearable )
		return nullptr;

	void* sceneNode = nullptr;
	if ( !FeatureSupport::TryReadField( wearable , m_Offsets.sceneNode , sceneNode ) )
		return nullptr;
	return sceneNode;
}

auto CCosmeticChanger::WearableModelBinding( C_BaseEntity* wearable ) const -> void*
{
	void* sceneNode = WearableSkeleton( wearable );
	if ( !PlausiblePointer( sceneNode ) )
		return nullptr;

	void* binding = nullptr;
	if ( !SafeRead(
			reinterpret_cast<const uint8_t*>( sceneNode ) + m_Offsets.modelState + m_Offsets.modelHandle ,
			binding ) )
		return nullptr;
	return binding;
}

auto CCosmeticChanger::OnSkeletonSetModel( void* skeleton , void* binding ) -> void*
{
	if ( !binding )
		return binding;

	// Hook thread. One lock, two hash operations, no reads of game memory - the
	// game may call SetModel many times a frame and this must not be felt.
	std::scoped_lock lock( m_HookMutex );

	// Record once; the path is resolved later, on the tick thread.
	if ( m_KnownBindings.insert( binding ).second )
		m_PendingBindings.push_back( binding );

	// Substitute when the game is (re)installing a model on one of the local
	// hero's overridden wearables - which is what happens on respawn. The active
	// apply on the tick thread covers everything else; this is what makes a swap
	// survive the game putting the original back.
	if ( Settings::CosmeticChanger::ApplyOverrides && skeleton )
	{
		const auto found = m_SkeletonOverrides.find( skeleton );
		if ( found != m_SkeletonOverrides.end() && found->second )
			return found->second;
	}
	return binding;
}

auto CCosmeticChanger::RecordBinding( void* binding ) -> void
{
	if ( !binding )
		return;

	std::scoped_lock lock( m_HookMutex );
	if ( m_KnownBindings.insert( binding ).second )
		m_PendingBindings.push_back( binding );
}

auto CCosmeticChanger::DrainRecordedBindings() -> void
{
	// Tick thread. Take whatever the hook has queued, resolve paths for a bounded
	// number (each resolution is a few validated reads), and requeue the rest for
	// the next pass so a burst of loads cannot stall one tick.
	std::vector<void*> pending;
	{
		std::scoped_lock lock( m_HookMutex );
		pending.swap( m_PendingBindings );
	}

	// Models a current selection wants, for ANY hero. A binding for one of these is
	// retained the moment it is recorded, not when it is applied - that is what
	// lets a model loaded outside the match (browsing it in Dota's armory) still
	// be resident when the match starts, instead of being unloaded on the level
	// change before the swap ever gets to use it.
	std::unordered_set<std::string> wanted;
	{
		std::scoped_lock settingsLock( Settings::CosmeticChanger::Mutex );
		for ( const auto& selection : Settings::CosmeticChanger::Selections )
		{
			if ( const auto* item = FindCatalogItemByDefIndex( selection.defIndex ) )
				if ( !item->model.empty() )
					wanted.insert( item->model );
		}
	}

	constexpr size_t kResolvePerPass = 32;
	size_t index = 0;
	for ( ; index < pending.size() && index < kResolvePerPass; ++index )
	{
		const char* path = ModelPathFromBinding( pending[index] );
		if ( !path || !path[0] )
		{
			// SetModel may first expose a resource before its data is ready.
			// Allow a subsequent hook/harvest observation to queue it again.
			std::scoped_lock lock( m_HookMutex );
			m_KnownBindings.erase( pending[index] );
			continue;
		}

		// A reloaded resource may have a new binding for the same path.
		{
			std::scoped_lock lock( m_HookMutex );
			m_Bindings.insert_or_assign( path , pending[index] );
		}

		// Freshly resolved from this exact binding, so it is known to name `path`
		// right now - safe to retain without a second check.
		if ( wanted.count( path ) )
			RetainBinding( pending[index] );
	}

	if ( index < pending.size() )
	{
		std::scoped_lock lock( m_HookMutex );
		m_PendingBindings.insert( m_PendingBindings.end() , pending.begin() + index , pending.end() );
	}

	// The reverse case: a model recorded BEFORE it was selected. Sweep the
	// recorded set for wanted paths not yet retained. Copied out under the lock,
	// verified outside it, because verification reads game memory and the hook
	// thread must not block on that.
	std::vector<std::pair<std::string , void*>> candidates;
	{
		std::scoped_lock lock( m_HookMutex );
		for ( const auto& entry : m_Bindings )
			if ( wanted.count( entry.first ) && !m_RetainedBindings.count( entry.second ) )
				candidates.emplace_back( entry.first , entry.second );
	}
	for ( const auto& [path , binding] : candidates )
	{
		const char* current = ModelPathFromBinding( binding );
		if ( current && path == current )
			RetainBinding( binding );
	}
}

auto CCosmeticChanger::RecordedBindingCount() const -> size_t
{
	std::scoped_lock lock( m_HookMutex );
	return m_Bindings.size();
}

auto CCosmeticChanger::RetainBinding( void* binding ) -> bool
{
	// The game bumps a binding's refcount at +0x20 before installing it - the
	// caller at client.dll+0x18A9346 does `lock inc dword [rax+0x20]` - and the
	// resource system frees a binding whose count reaches zero. Do the same for
	// anything we install, so a model that only we are using cannot be freed under
	// the skeleton. Once per binding and never released: one leaked reference per
	// model keeps it alive for the session, which is exactly what we want.
	if ( !PlausiblePointer( binding ) ||
		!FeatureSupport::IsReadableRuntimeMemory( binding , 0x24 ) )
		return false;

	if ( m_RetainedBindings.insert( binding ).second )
		InterlockedIncrement( reinterpret_cast<volatile LONG*>( reinterpret_cast<uint8_t*>( binding ) + 0x20 ) );
	return true;
}

auto CCosmeticChanger::InstallBinding( void* skeleton , void* binding , const std::string& model ) -> bool
{
	if ( !skeleton || !binding )
		return false;

	// A recorded binding can be stale: the model it named may have been unloaded
	// since and the allocation reused for something else. Readability alone does
	// not catch that, so confirm it still names the model we mean BEFORE taking a
	// reference on it or handing it to the engine.
	const char* current = ModelPathFromBinding( binding );
	if ( !current || model != current )
		return false;

	if ( !RetainBinding( binding ) )
		return false;

	// Prefer the trampoline: it bypasses our own hook, so the active apply installs
	// exactly what it was given instead of going back through the substitution
	// lookup. The resolved address is the fallback when the hook is not installed.
	if ( SetModel_o )
	{
		SetModel_o( skeleton , binding );
		return true;
	}
	return SDK_SkeletonSetModel( skeleton , binding );
}

auto CCosmeticChanger::PublishSkeletonOverrides( C_BaseEntity* const* wearables , const std::vector<WearableSlot>& live , int count , const std::string& hero ) -> void
{
	// Tick thread, settings lock already held by ApplySelections. Map each
	// overridden local-hero wearable's skeleton to the binding the hook should
	// substitute if the game reinstalls a model on it.
	std::unordered_map<const void* , void*> overrides;
	for ( int i = 0; i < count && i < static_cast<int>( live.size() ); ++i )
	{
		const WearableSlot& slot = live[i];
		if ( slot.category.empty() || !wearables[i] )
			continue;

		const auto* selection = Settings::CosmeticChanger::FindSelection( hero , slot.category );
		if ( !selection || selection->defIndex == 0 )
			continue;

		const auto* target = FindCatalogItemByDefIndex( selection->defIndex );
		if ( !target || target->model.empty() )
			continue;

		void* binding = FindBinding( target->model );
		void* skeleton = WearableSkeleton( wearables[i] );
		if ( !binding || !skeleton )
			continue;

		// Same staleness guard as InstallBinding: never publish - let alone retain
		// - a binding that no longer names the model it was recorded under.
		const char* current = ModelPathFromBinding( binding );
		if ( !current || target->model != current || !RetainBinding( binding ) )
			continue;

		overrides.emplace( skeleton , binding );
	}

	std::scoped_lock lock( m_HookMutex );
	m_SkeletonOverrides.swap( overrides );
}

auto CCosmeticChanger::FindBinding( const std::string& model ) const -> void*
{
	if ( model.empty() )
		return nullptr;

	std::scoped_lock lock( m_HookMutex );
	const auto found = m_Bindings.find( model );
	return found == m_Bindings.end() ? nullptr : found->second;
}

auto CCosmeticChanger::DefIndexOffset() const -> uint32_t
{
	// The attribute manager and item are embedded structs, so their offsets add
	// on top of the econ-entity pointer with no pointer hops.
	return m_Offsets.attributeManager + m_Offsets.item + m_Offsets.itemDefinitionIndex;
}

auto CCosmeticChanger::ShouldLogWearableSet( const uint16_t* defIndexes , int count , int heroIndex ) -> bool
{
	uint64_t signature = 14695981039346656037ull;
	auto mix = [&signature]( uint64_t value )
	{
		signature ^= value;
		signature *= 1099511628211ull;
	};

	mix( static_cast<uint64_t>( static_cast<uint32_t>( heroIndex ) ) );
	mix( static_cast<uint64_t>( static_cast<uint32_t>( count ) ) );

	for ( int i = 0; i < count; ++i )
	{
		mix( static_cast<uint64_t>( static_cast<uint32_t>( i ) ) );
		mix( static_cast<uint64_t>( defIndexes[i] ) );
	}

	if ( m_HasWearableSnapshot && signature == m_LastWearableSignature )
		return false;

	m_LastWearableSignature = signature;
	m_HasWearableSnapshot = true;
	return true;
}

auto CCosmeticChanger::UpdateSnapshot( const uint16_t* defIndexes , C_BaseEntity* const* wearables , int count , int heroIndex ) -> void
{
	// A different hero means the recorded slot indexes no longer refer to anything,
	// so the originals go with them.
	if ( heroIndex != m_OriginalsHeroIndex )
	{
		m_HasAppliedSignature = false;
		m_HasCombinedOverrides = false;
		m_CombinedBuildAccepted = false;
		m_LastCombinedSignature = 0;
		m_ObserveCombinedBuild = false;
		m_Originals.clear();
		m_OriginalsHeroIndex = heroIndex;
		// A new hero entity means new wearable skeletons. The substitute map is
		// keyed by the old ones and must not answer for whatever now lives at those
		// addresses; it is rebuilt on the next apply pass.
		{
			std::scoped_lock lock( m_HookMutex );
			m_SkeletonOverrides.clear();
		}
	}

	if ( !defIndexes || !wearables || count <= 0 )
	{
		std::scoped_lock lock( m_SnapshotMutex );
		m_CurrentHeroIndex = heroIndex;
		m_CurrentHero.clear();
		m_Wearables.clear();
		return;
	}

	std::string hero;
	std::vector<WearableSlot> slots;
	slots.reserve( static_cast<size_t>( count ) );
	for ( int i = 0; i < count; ++i )
	{
		WearableSlot slot{};
		slot.slot = i;
		slot.defIndex = defIndexes[i];

		if ( wearables[i] )
		{
			const char* model = WearableModelPath( wearables[i] );
			if ( model && model[0] )
			{
				slot.model = model;
				slot.hero = HeroFromModelPath( model );
			}
		}

		// The catalog identifies the wearable from its defindex alone, which also
		// covers the slots whose live model path could not be read. Only fill in
		// what the live read did not already give us.
		if ( const auto* item = FindCatalogItemByDefIndex( slot.defIndex ) )
		{
			slot.name = item->name;
			slot.category = item->category;
			// Model folders use aliases (drow, windrunner, ...), while selections
			// use the catalog hero ID (drow_ranger, windranger, ...).
			// Always use the same authoritative key as the picker.
			slot.hero = item->hero;
			if ( slot.model.empty() )
				slot.model = item->model;
		}

		// The first slot that names a hero names the whole loadout: every wearable on
		// one hero belongs to that hero.
		if ( hero.empty() && !slot.hero.empty() )
			hero = slot.hero;

		slots.push_back( slot );
	}

	std::scoped_lock lock( m_SnapshotMutex );
	m_CurrentHeroIndex = heroIndex;
	m_CurrentHero = std::move( hero );
	m_Wearables = std::move( slots );
}

auto CCosmeticChanger::LogConfiguredSelections( const std::string& hero ) -> void
{
	if ( !Settings::CosmeticChanger::LogUiSelections || hero.empty() )
		return;

	std::scoped_lock settingsLock( Settings::CosmeticChanger::Mutex );

	uint64_t signature = 14695981039346656037ull;
	auto mix = [&signature]( uint64_t value )
	{
		signature ^= value;
		signature *= 1099511628211ull;
	};
	auto mixString = [&]( const std::string& text )
	{
		for ( unsigned char c : text )
			mix( c );
		mix( 0xFFu );
	};

	int count = 0;
	for ( const auto& selection : Settings::CosmeticChanger::Selections )
	{
		if ( selection.hero != hero )
			continue;

		mixString( selection.hero );
		mixString( selection.slot );
		mix( selection.defIndex );
		++count;
	}

	if ( count <= 0 )
	{
		m_HasSelectionSnapshot = false;
		return;
	}

	if ( m_HasSelectionSnapshot && signature == m_LastSelectionSignature )
		return;

	m_LastSelectionSignature = signature;
	m_HasSelectionSnapshot = true;

	DEV_LOG( "[cosmetic-ui] configured selections for hero=%s (%d slot overrides; apply requires loaded models):\n" ,
		hero.c_str() , count );
	for ( const auto& selection : Settings::CosmeticChanger::Selections )
	{
		if ( selection.hero != hero )
			continue;

		const auto* item = FindCatalogItem( selection.hero , selection.slot , selection.defIndex );
		DEV_LOG( "  [cosmetic-ui] slot=%s def=%u name=%s model=%s\n" ,
			selection.slot.c_str() ,
			selection.defIndex ,
			item ? item->name.c_str() : "?" ,
			item ? item->model.c_str() : "?" );
	}
}

auto CCosmeticChanger::RememberOriginal( int slot , uint32_t defIndex , const std::string& model ) -> void
{
	if ( FindOriginal( slot ) )
		return;

	OriginalSlot original{};
	original.slot = slot;
	original.defIndex = defIndex;
	original.model = model;
	m_Originals.push_back( std::move( original ) );
}

auto CCosmeticChanger::FindOriginal( int slot ) const -> const OriginalSlot*
{
	for ( const auto& original : m_Originals )
	{
		if ( original.slot == slot )
			return &original;
	}
	return nullptr;
}

auto CCosmeticChanger::GetCombinedModelOverride( void* hero , void* itemView ) -> const char*
{
	std::scoped_lock lock( m_HookMutex );
	// Counted before every early-out, so the pipeline log can tell a dead hook
	// apart from a live one that simply never matches.
	++m_ItemModelCallsTotal;
	if ( m_CombinedOverrides.empty() || !itemView )
		return nullptr;

	uint32_t defIndex = 0;
	if ( !FeatureSupport::TryReadField( itemView , m_Offsets.itemDefinitionIndex , defIndex ) )
		return nullptr;

	// The defindex must be one we were asked to override. This is the real gate:
	// m_CombinedOverrides only ever holds defindexes the LOCAL hero is currently
	// wearing and that the user picked a replacement for, so an item we were never
	// asked about can never be touched no matter who is asking.
	const auto found = m_CombinedOverrides.find( defIndex );
	if ( found == m_CombinedOverrides.end() ) return nullptr;

	// Identity is the defindex, not the pointer. Matching on the item-view address
	// missed 194 of 194 asks: the combiner passes its own CEconItemView, not the
	// one computed from the wearable entity, so the two never agreed. The pointer
	// set is kept as a positive signal rather than a requirement.
	const bool knownView = ( hero != nullptr && hero == m_CombinedHero ) ||
		m_LocalItemViews.count( itemView ) != 0;
	if ( !knownView )
		++m_SubstitutionsByDefIndex;
	++m_CombinedLookupHits;
	++m_CombinedSubstitutionsTotal;
	// Points into the immutable catalog, not temporary hook or settings storage.
	return found->second;
}

auto CCosmeticChanger::LogSkinPipeline( C_BaseEntity* hero , size_t itemViewCount , size_t overrideCount ) -> void
{
	uint64_t calls = 0;
	uint64_t subs = 0;
	uint64_t byDefIndex = 0;
	size_t publishedOverrides = 0;
	size_t publishedViews = 0;
	std::string meshAtPublish;
	{
		std::scoped_lock lock( m_HookMutex );
		calls = m_ItemModelCallsTotal;
		subs = m_CombinedSubstitutionsTotal;
		byDefIndex = m_SubstitutionsByDefIndex;
		publishedOverrides = m_CombinedOverrides.size();
		publishedViews = m_LocalItemViews.size();
		meshAtPublish = m_MeshAtPublish;
	}

	size_t selections = 0;
	bool enabled = false;
	bool applying = false;
	{
		std::scoped_lock settingsLock( Settings::CosmeticChanger::Mutex );
		enabled = Settings::CosmeticChanger::Enable;
		applying = Settings::CosmeticChanger::ApplyOverrides;
		for ( const auto& selection : Settings::CosmeticChanger::Selections )
			if ( selection.hero == m_CurrentHero )
				++selections;
	}

	const std::string mesh = WearableModelPath( hero );
	const bool meshChanged = !meshAtPublish.empty() && mesh != meshAtPublish;

	// A stage is only meaningful once everything above it passed; anything past the
	// first failure prints "--" rather than a misleading FAIL.
	struct Stage { const char* name; bool ok; std::string detail; };
	const Stage stages[] =
	{
		// FIRST, because with applying off no overrides are collected at all - which
		// used to surface as "slotMatch FAIL 0 of 1 matched" and read as a broken
		// pick rather than a switch being off.
		{ "switchedOn" , enabled && applying ,
			std::string( "Enable=" ) + ( enabled ? "on" : "OFF" ) +
			" ApplyToMyHero=" + ( applying ? "on" : "OFF" ) } ,
		{ "selection" , selections > 0 ,
			std::to_string( selections ) + " pick(s) for " + ( m_CurrentHero.empty() ? "?" : m_CurrentHero ) } ,
		{ "hero" , hero != nullptr ,
			"idx=" + std::to_string( m_CurrentHeroIndex ) } ,
		{ "wearables" , itemViewCount > 0 ,
			std::to_string( itemViewCount ) + " item view(s)" } ,
		{ "slotMatch" , overrideCount > 0 ,
			std::to_string( overrideCount ) + " of " + std::to_string( selections ) + " pick(s) matched a live slot" } ,
		{ "published" , publishedOverrides > 0 && publishedViews > 0 ,
			"map=" + std::to_string( publishedOverrides ) + " views=" + std::to_string( publishedViews ) } ,
		{ "combinerAsks" , calls > 0 ,
			std::to_string( calls ) + " GetItemModel call(s) seen" } ,
		{ "substituted" , subs > 0 ,
			std::to_string( subs ) + " substitution(s) returned, " +
			std::to_string( byDefIndex ) + " via defindex match" } ,
		{ "meshRebuilt" , meshChanged ,
			meshAtPublish.empty() ? std::string( "no baseline yet" ) : ( meshAtPublish + " -> " + mesh ) } ,
	};

	DEV_LOG( "[skin-pipeline] ---- hero=%s mesh=%s ----\n" ,
		m_CurrentHero.empty() ? "?" : m_CurrentHero.c_str() , mesh.c_str() );

	bool blocked = false;
	int index = 0;
	for ( const auto& stage : stages )
	{
		++index;
		const char* status = blocked ? "--  " : ( stage.ok ? "OK  " : "FAIL" );
		DEV_LOG( "[skin-pipeline] %d %-13s %s  %s\n" ,
			index , stage.name , status , blocked ? "(blocked above)" : stage.detail.c_str() );
		if ( !stage.ok && !blocked )
			blocked = true;
	}

	if ( !blocked )
		DEV_LOG( "[skin-pipeline] all stages passed - the swap is on screen\n" );
}

auto CCosmeticChanger::RequestEngineRebuild( C_BaseEntity* hero ) -> void
{
	if ( !hero )
		return;

	// hero+0x1D80 holds the combined-model bitfield byte (right after the model
	// indices at 0x1D08/0x1D0C). Bit 0x08 is the "needs rebuild" request the
	// builder consumes - client.dll+0x1ACD35B does `and byte[hero+0x1D80],0xF7`,
	// clearing exactly this bit after doing rebuild setup. Bit 0x10 is the
	// re-entry guard ("currently building"). Setting 0x08 is what the game does
	// internally when a loadout changes: it asks the engine to rebuild the combined
	// mesh on its next pass, and that rebuild is the one our GetItemModel hook can
	// substitute into. We only ever SET the request bit and never touch 0x10, so
	// we cannot fake "currently building" and stall the pipeline.
	constexpr uint32_t kCombinedFlagsByte = 0x1D80;
	constexpr uint8_t kForceBuildBit = 0x08;

	auto* flags = reinterpret_cast<uint8_t*>( hero ) + kCombinedFlagsByte;
	if ( !FeatureSupport::IsReadableRuntimeMemory( flags , 1 ) )
	{
		DEV_LOG( "[cosmetic-rebuild] flags byte at +0x%X unreadable\n" , kCombinedFlagsByte );
		return;
	}

	const uint8_t before = *flags;
	if ( before & kForceBuildBit )
		return; // a rebuild is already pending; do not thrash the flag

	DWORD oldProtect = 0;
	if ( VirtualProtect( flags , 1 , PAGE_EXECUTE_READWRITE , &oldProtect ) )
	{
		*flags = static_cast<uint8_t>( before | kForceBuildBit );
		VirtualProtect( flags , 1 , oldProtect , &oldProtect );
		DEV_LOG( "[cosmetic-rebuild] requested engine rebuild: flags 0x%02X -> 0x%02X\n" ,
			before , *flags );
	}
}

auto CCosmeticChanger::TryCombinedModelSwap( C_BaseEntity* hero ) -> bool
{
	if ( !hero || m_CurrentHero.empty() )
		return false;

	void* skeleton = WearableSkeleton( hero );
	if ( !skeleton )
		return false;

	const std::string current = WearableModelPath( hero );

	// Dota's generated combined meshes live at models/heroes/<hero>/<hero>_c_<n>.vmdl.
	// Collect every one the hook has recorded for THIS hero, so we never install
	// another hero's mesh onto ours.
	const std::string prefix = "models/heroes/" + m_CurrentHero + "/" + m_CurrentHero + "_c_";
	std::vector<std::pair<std::string , void*>> candidates;
	{
		std::scoped_lock lock( m_HookMutex );
		for ( const auto& entry : m_Bindings )
			if ( entry.first.rfind( prefix , 0 ) == 0 )
				candidates.emplace_back( entry.first , entry.second );
	}

	DEV_LOG( "[cosmetic-mesh] hero=%s current=%s candidates=%zu\n" ,
		m_CurrentHero.c_str() , current.c_str() , candidates.size() );
	for ( const auto& [path , binding] : candidates )
		DEV_LOG( "    %s%s\n" , path.c_str() , path == current ? "  <- current" : "" );

	if ( !Settings::CosmeticChanger::SwapCombinedMesh )
		return false;

	// Anything other than what is on the hero right now is, by definition, a
	// different appearance - which is exactly what we are trying to demonstrate.
	for ( const auto& [path , binding] : candidates )
	{
		if ( path == current )
			continue;
		const bool installed = InstallBinding( skeleton , binding , path );
		DEV_LOG( "[cosmetic-mesh] install %s -> %s installed=%d\n" ,
			current.c_str() , path.c_str() , installed );
		if ( installed )
			return true;
	}

	return false;
}

auto CCosmeticChanger::RebuildCombinedModel( C_BaseEntity* const* wearables , int count , bool apply ) -> void
{
	if ( !BuildCombinedModel_o || !GetItemModel_o || !wearables || count <= 0 ) return;
	std::scoped_lock settingsLock( Settings::CosmeticChanger::Mutex );
	auto* system = SDK::Interfaces::GameEntitySystem();
	auto* hero = system ? system->GetBaseEntity( m_CurrentHeroIndex ) : nullptr;
	if ( !hero ) return;
	std::vector<void*> itemViews;
	std::unordered_map<uint32_t , const char*> overrides;
	// Nothing to wait for any more: the combiner resolves paths itself, so an
	// override no longer depends on its model already being resident.
	constexpr bool waiting = false;
	uint64_t signature = 14695981039346656037ull;
	auto mix = [&]( uint64_t value ) { signature = ( signature ^ value ) * 1099511628211ull; };
	mix( reinterpret_cast<uintptr_t>( hero ) );
	for ( int i = 0; i < count; ++i )
	{
		if ( !wearables[i] ) continue;
		void* item = reinterpret_cast<uint8_t*>( wearables[i] ) + m_Offsets.attributeManager + m_Offsets.item;
		uint32_t defIndex = 0;
		if ( !FeatureSupport::TryReadField( item , m_Offsets.itemDefinitionIndex , defIndex ) ) continue;
		itemViews.push_back( item );
		mix( reinterpret_cast<uintptr_t>( item ) );
		mix( defIndex );
		if ( !apply ) continue;
		const auto* original = FindCatalogItemByDefIndex( defIndex );
		if ( !original || original->hero != m_CurrentHero ) continue;
		const auto* selection = Settings::CosmeticChanger::FindSelection( m_CurrentHero , original->category );
		const auto* target = selection ? FindCatalogItemByDefIndex( selection->defIndex ) : nullptr;
		if ( !target || target->hero != m_CurrentHero || target->model.empty() ) continue;
		// NO residency requirement here. This path hands the combiner a PATH and it
		// resolves the model itself, exactly as it does for the items you really
		// own - so unlike the SetModel route, the target does not have to be loaded
		// already. Demanding a binding first was gating every override on a model
		// being present in the match, which is the limitation that made arbitrary
		// cosmetics unreachable.
		overrides[defIndex] = target->model.c_str();
		mix( target->defIndex );
	}
	// Reported before any early-out below, so the log always shows where the swap
	// stands - including the cases that return without attempting anything.
	const uint64_t nowTick = GetTickCount64();
	if ( nowTick - m_LastPipelineLog >= 5000 )
	{
		m_LastPipelineLog = nowTick;
		LogSkinPipeline( hero , itemViews.size() , overrides.size() );
	}

	if ( waiting || itemViews.empty() ) return;
	if ( overrides.empty() && !m_HasCombinedOverrides ) return;
	const uint64_t now = GetTickCount64();
	if ( signature == m_LastCombinedSignature &&
		( m_CombinedBuildAccepted || now - m_LastCombinedAttempt < 10000 ) ) return;
	const size_t overrideCount = overrides.size();
	{
		std::scoped_lock lock( m_HookMutex );
		m_CombinedHero = hero;
		m_CombinedOverrides = std::move( overrides );
		m_CombinedLookupHits = 0;
		// Published so the substitution also applies to engine-initiated rebuilds,
		// which are the ones that actually complete.
		m_LocalItemViews.clear();
		m_LocalItemViews.insert( itemViews.begin() , itemViews.end() );
		// Baseline the mesh ONLY when the override set actually changed. This block
		// also re-runs every 10s as a retry, and re-baselining on each retry meant a
		// rebuild that had already landed was captured as the "before" - so stage 9
		// reported no change for a swap that was plainly on screen (Void, c_28).
		if ( signature != m_LastCombinedSignature )
			m_MeshAtPublish = WearableModelPath( hero );
	}

	// NO request-bit write here. Setting bit 0x08 at hero+0x1D80 was tested and did
	// nothing: the log showed `flags 0x00 -> 0x08` firing while the hero's mesh
	// stayed pinned at the same index, and the bit was still set on later passes
	// because the builder never got far enough to consume it. The bit that matters
	// is 0x10, and it is held only across the builder call itself - see the bypass
	// at the Hook_BuildCombinedModel site below.
	// The verified builder reads count at +0 and CEconItemView** at +8.
	// It consumes the list synchronously and owns its generated model request.
	struct ItemViewList { int32_t count; int32_t pad; void* const* data; };
	const ItemViewList items{ static_cast<int32_t>( itemViews.size() ) , 0 , itemViews.data() };
	const std::string before = WearableModelPath( hero );
	m_PreviousCombinedBinding = WearableModelBinding( hero );
	m_LastCombinedSignature = signature;
	m_LastCombinedAttempt = now;
	// The builder refuses outright when the hero already owns a combined model.
	// Disassembled at client.dll+0x1ACD250: it reads a request handle at
	// hero+0x1A7C and, unless that is -1 or -2, looks it up in the table at
	// +0x5DF5660; a live entry means "already built" and it returns false in ~0us,
	// which is exactly the accepted=0 elapsed=0ms we were logging. -1 is the
	// engine's own "none" sentinel - the two values it tests for - so clearing the
	// handle is how the builder is told there is nothing there yet.
	constexpr uint32_t kCombinedRequestOffset = 0x1A7C;
	constexpr uint32_t kNoRequest = 0xFFFFFFFFu;
	auto* requestSlot = reinterpret_cast<uint32_t*>(
		reinterpret_cast<uint8_t*>( hero ) + kCombinedRequestOffset );
	uint32_t previousRequest = kNoRequest;
	const bool clearedRequest =
		FeatureSupport::IsReadableRuntimeMemory( requestSlot , sizeof( uint32_t ) ) &&
		*requestSlot != kNoRequest;
	if ( clearedRequest )
	{
		previousRequest = *requestSlot;
		*requestSlot = kNoRequest;
	}

	// With the request handle already -1 and a non-empty item list, the only
	// remaining early return-false in the builder is its first instruction pair:
	//   mov rax,[client.dll+0x61C86A8] ; cmp byte [rax+0x58],0 ; je <return false>
	// a subsystem gate. Read it, and if it is closed hold it open across the call
	// only - restoring immediately, because this is a global the whole game shares.
	constexpr uint32_t kCombinerGateGlobal = 0x61C86A8;
	constexpr uint32_t kCombinerGateByte = 0x58;
	uint8_t* gate = nullptr;
	uint8_t gateBefore = 0;
	if ( auto* clientBase = reinterpret_cast<uint8_t*>( GetModuleHandleA( CLIENT_DLL ) ) )
	{
		void* holder = nullptr;
		if ( FeatureSupport::TryRead( clientBase + kCombinerGateGlobal , holder ) && holder )
		{
			auto* candidate = reinterpret_cast<uint8_t*>( holder ) + kCombinerGateByte;
			if ( FeatureSupport::IsReadableRuntimeMemory( candidate , 1 ) )
			{
				gate = candidate;
				gateBefore = *gate;
				if ( gateBefore == 0 )
					*gate = 1;
			}
		}
	}

	// Before asking the builder for anything, report what complete meshes we
	// already hold and - when enabled - just install one. This needs no builder
	// call at all, which matters because the builder has refused every request.
	TryCombinedModelSwap( hero );

	// Bypass the builder's stale-list revalidation for the duration of our call.
	//
	// client.dll+0x1ACD329: `test byte[hero+0x1D80],0x10 / jne 0x1ACD363`. With the
	// bit SET the builder skips recomputing the expected item list and jumps
	// straight to the container check at 0x1ACD363 - and the [cosmetic-gate] log
	// proves that check passes for us (list non-null, count 19-24, data non-null).
	// With the bit CLEAR it recomputes, finds our cached list differs, and returns
	// false at 0x1ACD355 - which is where all 107 of our calls died.
	//
	// Held for the call ONLY and restored immediately: the bit doubles as the
	// engine's "currently building" re-entry guard, so leaving it set would lie to
	// every other reader.
	constexpr uint32_t kCombinedFlagsOffset = 0x1D80;
	constexpr uint8_t kSkipRevalidateBit = 0x10;
	auto* combinedFlags = reinterpret_cast<uint8_t*>( hero ) + kCombinedFlagsOffset;
	uint8_t flagsBefore = 0;
	const bool bypass = Settings::CosmeticChanger::ForceMeshRebuild &&
		FeatureSupport::IsReadableRuntimeMemory( combinedFlags , 1 );
	if ( bypass )
	{
		flagsBefore = *combinedFlags;
		DWORD flagProtect = 0;
		if ( VirtualProtect( combinedFlags , 1 , PAGE_EXECUTE_READWRITE , &flagProtect ) )
		{
			*combinedFlags = static_cast<uint8_t>( flagsBefore | kSkipRevalidateBit );
			VirtualProtect( combinedFlags , 1 , flagProtect , &flagProtect );
		}
	}

	const bool accepted = Hook_BuildCombinedModel( hero , &items );

	if ( bypass )
	{
		DWORD flagProtect = 0;
		if ( VirtualProtect( combinedFlags , 1 , PAGE_EXECUTE_READWRITE , &flagProtect ) )
		{
			// Restore exactly what was there, except let the builder's own clear of
			// the request bit stand if it consumed one.
			*combinedFlags = static_cast<uint8_t>(
				( *combinedFlags & ~kSkipRevalidateBit ) | ( flagsBefore & kSkipRevalidateBit ) );
			VirtualProtect( combinedFlags , 1 , flagProtect , &flagProtect );
		}
	}

	if ( gate && gateBefore == 0 )
		*gate = gateBefore;

	// Only restore on refusal. On acceptance the builder has written its own new
	// request handle, and putting the old one back would strand the model it built.
	if ( !accepted && clearedRequest )
		*requestSlot = previousRequest;
	uint32_t hits = 0;
	{
		std::scoped_lock lock( m_HookMutex );
		hits = m_CombinedLookupHits;
	}
	m_CombinedBuildAccepted = accepted && ( overrideCount == 0 || hits > 0 );
	m_ObserveCombinedBuild = m_CombinedBuildAccepted;
	// Keep restoration pending after a rejected request; the next pass retries.
	if ( overrideCount > 0 ) m_HasCombinedOverrides = true;
	else if ( accepted ) m_HasCombinedOverrides = false;
	DEV_LOG( "[cosmetic-combine] hero=%s items=%zu overrides=%zu substitutions=%u accepted=%d elapsed=%llu ms request=%s(0x%X) before=%s after=%s\n" ,
		m_CurrentHero.c_str() , itemViews.size() , overrideCount , hits , accepted , GetTickCount64() - now ,
		clearedRequest ? "cleared" : "none" , previousRequest ,
		before.c_str() , WearableModelPath( hero ) );
	uint64_t totalSubs = 0;
	{
		std::scoped_lock lock( m_HookMutex );
		totalSubs = m_CombinedSubstitutionsTotal;
	}
	// Which of the builder's gates is actually stopping us. Disassembled at
	// client.dll+0x1ACD363: the last check before the per-item loop (which is what
	// calls GetItemModel) is 0x61FE40( hero->[0x15E8] ), and that returns true only
	// when the container is non-null, its count at +0x20 is > 0, and its data
	// pointer at +0x0 is non-null. Read the same three values here so the log says
	// which one fails instead of leaving it to be guessed at. Read-only.
	{
		constexpr uint32_t kListOffset = 0x15E8;
		constexpr uint32_t kFlagsOffset = 0x1D80;
		void* list = nullptr;
		int32_t listCount = -1;
		void* listData = nullptr;
		uint8_t flagsByte = 0;

		FeatureSupport::TryRead( reinterpret_cast<uint8_t*>( hero ) + kListOffset , list );
		FeatureSupport::TryRead( reinterpret_cast<uint8_t*>( hero ) + kFlagsOffset , flagsByte );
		if ( list )
		{
			FeatureSupport::TryRead( reinterpret_cast<uint8_t*>( list ) + 0x20 , listCount );
			FeatureSupport::TryRead( list , listData );
		}

		const bool gatePasses = list && listCount > 0 && listData;
		DEV_LOG( "  [cosmetic-gate] list=%p count=%d data=%p flags=0x%02X -> itemLoop=%s\n" ,
			list , listCount , listData , flagsByte ,
			gatePasses ? "REACHABLE" : "blocked" );
	}

	if ( bypass )
		DEV_LOG( "  [cosmetic-bypass] flags 0x%02X -> 0x%02X during build, restored to 0x%02X\n" ,
			flagsBefore , static_cast<uint8_t>( flagsBefore | kSkipRevalidateBit ) , *combinedFlags );

	DEV_LOG( "  [cosmetic-combine] gate=%s value=%d substitutionsTotal=%llu (engine rebuilds included)\n" ,
		gate ? ( gateBefore == 0 ? "forced-open" : "already-open" ) : "unreadable" ,
		static_cast<int>( gateBefore ) ,
		static_cast<unsigned long long>( totalSubs ) );
}

auto CCosmeticChanger::LogRenderState( C_BaseEntity* wearable , const char* target , bool installed ) -> void
{
	// A wearable handle can change while Dota still draws a combined hero mesh.
	// Record the renderer inputs separately from the resource installation result.
	auto* system = SDK::Interfaces::GameEntitySystem();
	auto* hero = system ? system->GetBaseEntity( m_CurrentHeroIndex ) : nullptr;
	if ( !hero )
		return;
	uint32_t currentOffset = 0, pendingOffset = 0;
	auto* schema = GetSchemaOffset();
	const bool hasCurrent = schema && schema->TryGetOffset( "C_DOTA_BaseNPC_Hero" , "m_nCurrentCombinedModelIndex" , currentOffset );
	const bool hasPending = schema && schema->TryGetOffset( "C_DOTA_BaseNPC_Hero" , "m_nPendingCombinedModelIndex" , pendingOffset );
	int current = -1, pending = -1;
	if ( hasCurrent ) FeatureSupport::TryReadField( hero , currentOffset , current );
	if ( hasPending ) FeatureSupport::TryReadField( hero , pendingOffset , pending );
	const std::string heroModel = WearableModelPath( hero );
	const std::string wearableModel = WearableModelPath( wearable );
	DEV_LOG( "[cosmetic-render] heroIndex=%d hero=%p wearable=%p handleInstalled=%d target=%s\n" ,
		m_CurrentHeroIndex , hero , wearable , installed , target );
	DEV_LOG( "[cosmetic-render] heroModel=%s wearableModel=%s combinedCurrent=%d pending=%d schema=%d/%d\n" ,
		heroModel.c_str() , wearableModel.c_str() , current , pending , hasCurrent , hasPending );
}

auto CCosmeticChanger::WriteWearable( C_BaseEntity* wearable , void* defAddr , uint32_t defIndex , const std::string& model ) -> bool
{
	if ( !wearable || model.empty() )
		return false;

	// NO defindex write. It never changed the mesh, and it is the one write common
	// to both builds that crashed on entity teardown at client.dll+0x1833AAF -
	// present with the m_ModelName write and still present after that was removed,
	// and only ever executed in the Apply-on sessions that crashed. Teardown most
	// likely resolves the econ item by defindex and dereferences the miss. The
	// item view keeps its real defindex; only the rendered model changes.
	( void ) defAddr;
	( void ) defIndex;

	void* skeleton = WearableSkeleton( wearable );
	if ( !skeleton )
		return false;

	// Use ResourceSystem's blocking loader, which owns its loading manifest.
	// The script PrecacheResource helper requires an unrelated live context.

	bool changed = false;

	// NO m_ModelName write. Tested in-match and it does nothing: the field took our
	// path (the probe read it back), m_hModel never moved, and the rendered mesh
	// never changed - it is networked bookkeeping, not an input anything re-reads.
	// Leave the networked name under the engine's ownership.

	void* binding = FindBinding( model );
	if ( !binding && !m_LoadRequestedThisPass )
	{
		const uint64_t now = GetTickCount64();
		auto& lastAttempt = m_ModelLoadAttempts[model];
		if ( !lastAttempt || now - lastAttempt >= 10000 )
		{
			lastAttempt = now;
			m_LoadRequestedThisPass = true;
			binding = LoadCosmeticModel( model );
			DEV_LOG( "[cosmetic-load] path=%s binding=%p elapsed=%llu ms\n" ,
				model.c_str() , binding , GetTickCount64() - now );
			if ( binding && model == ModelPathFromBinding( binding ) && RetainBinding( binding ) )
			{
				std::scoped_lock lock( m_HookMutex );
				m_Bindings.insert_or_assign( model , binding );
			}
			else
				binding = nullptr;
		}
	}
	if ( binding )
	{
		// The binding was cached from an entity seen on an earlier pass, and that
		// entity may since have despawned. One validated read here - at apply time,
		// not per entity per pass - is cheap, and handing the engine a freed
		// pointer is exactly the class of mistake that has crashed this build
		// twice already.
		if ( InstallBinding( skeleton , binding , model ) )
			changed = WearableModelBinding( wearable ) == binding;
		LogRenderState( wearable , model.c_str() , changed );
	}

	return changed;
}

auto CCosmeticChanger::ApplySelections( C_BaseEntity* const* wearables , void* const* defAddrs , int count ) -> void
{
	if ( !Settings::CosmeticChanger::Enable || !Settings::CosmeticChanger::ApplyOverrides )
	{
		m_HasAppliedSignature = false;
		{
			std::scoped_lock lock( m_HookMutex );
			m_SkeletonOverrides.clear();
			m_CombinedHero = nullptr;
			m_CombinedOverrides.clear();
		}
		RebuildCombinedModel( wearables , count , false );
		return;
	}
	if ( !wearables || !defAddrs || count <= 0 )
		return;

	// A resident binding still requires the engine setter to change the mesh.
	if ( !CanApply() )
	{
		if ( !m_WarnedNoSetModel )
		{
			DEV_LOG( "[cosmetic-apply] cannot apply: required setter/combiner/item-model hook unavailable.\n" );
			m_WarnedNoSetModel = true;
		}
		return;
	}

	// The menu mutates the selection list from the render thread while this runs on
	// the game tick, so the whole decision is made under the settings lock. The live
	// wearable state below is this thread's own, and needs no lock.
	std::scoped_lock settingsLock( Settings::CosmeticChanger::Mutex );
	m_LoadRequestedThisPass = false;

	std::string hero;
	std::vector<WearableSlot> live;
	{
		std::scoped_lock lock( m_SnapshotMutex );
		hero = m_CurrentHero;
		live = m_Wearables;
	}
	if ( hero.empty() )
		return;

	// Only act when the (hero, wearable-set, selection-set) combination changes, so
	// a steady state is not re-applied every pass - SetModel is a real engine call
	// and re-running it each tick would thrash model loading.
	uint64_t signature = 14695981039346656037ull;
	auto mix = [&signature]( uint64_t value )
	{
		signature ^= value;
		signature *= 1099511628211ull;
	};
	for ( unsigned char c : hero )
		mix( c );
	for ( int i = 0; i < count; ++i )
	{
		mix( reinterpret_cast<uintptr_t>( wearables[i] ) );
		mix( reinterpret_cast<uintptr_t>( WearableModelBinding( wearables[i] ) ) );
		mix( i < static_cast<int>( live.size() ) ? live[i].defIndex : 0u );
	}
	for ( const auto& selection : Settings::CosmeticChanger::Selections )
	{
		if ( selection.hero != hero )
			continue;
		for ( unsigned char c : selection.slot )
			mix( c );
		mix( selection.defIndex );
		// A failed attempt must be reconsidered when its model becomes resident.
		if ( const auto* target = FindCatalogItemByDefIndex( selection.defIndex ) )
			mix( reinterpret_cast<uintptr_t>( FindBinding( target->model ) ) );
	}

	// Revisit pending loads at the existing one-second tick rate. Engine loading
	// itself is limited to one request per pass and ten seconds per failed path.
	mix( GetTickCount64() / 10000 );
	const bool logAttempt = !m_HasAppliedSignature || m_LastAppliedSignature != signature;
	m_LastAppliedSignature = signature;
	m_HasAppliedSignature = true;

	// Tell the hook which of our skeletons to substitute on, so a swap survives
	// the game reinstalling the original model (respawn, illusion refresh).
	PublishSkeletonOverrides( wearables , live , count , hero );

	int applied = 0;
	int reverted = 0;
	for ( int i = 0; i < count && i < static_cast<int>( live.size() ); ++i )
	{
		const WearableSlot& slot = live[i];
		if ( slot.category.empty() || !wearables[i] )
			continue;

		// Match the live wearable to a picked cosmetic by body-part slot.
		const auto* selection = Settings::CosmeticChanger::FindSelection( hero , slot.category );
		if ( !selection || selection->defIndex == 0 )
		{
			// No pick for this slot. If we overrode it earlier in this match, put the
			// hero's real item back - otherwise clearing a pick in the menu would
			// leave the override on screen until the next respawn.
			const auto* original = FindOriginal( i );
			if ( !original || original->model.empty() || original->model == slot.model )
				continue;

			const bool restored = WriteWearable( wearables[i] , defAddrs[i] , original->defIndex , original->model );
			if ( restored || logAttempt )
			DEV_LOG( "  [cosmetic-apply] slot %d (%s): restored %u (%s) setModel=%d\n" ,
				i , slot.category.c_str() , original->defIndex , original->model.c_str() , restored );
			if ( restored )
				++reverted;
			continue;
		}

		const auto* target = FindCatalogItemByDefIndex( selection->defIndex );
		if ( !target || target->model.empty() )
			continue;

		// Snapshot the untouched slot before the first write to it, so the revert
		// above has something real to go back to.
		RememberOriginal( i , slot.defIndex , slot.model );
		if ( target->model == slot.model )
			continue;

		const bool ok = WriteWearable( wearables[i] , defAddrs[i] , selection->defIndex , target->model );
		if ( !ok && logAttempt )
			DEV_LOG( "[cosmetic-apply] pending %s: %s\n" , target->name.c_str() ,
				FindBinding( target->model ) ? "binding installation not confirmed" : "resource load pending or failed; see cosmetic-load" );
		if ( ok || logAttempt )
		DEV_LOG( "  [cosmetic-apply] slot %d (%s): %u -> %u (%s) setModel=%d model=%s\n" ,
			i , slot.category.c_str() ,
			slot.defIndex , selection->defIndex , target->name.c_str() ,
			ok , target->model.c_str() );
		if ( ok )
			++applied;
	}

	if ( applied > 0 || reverted > 0 )
		DEV_LOG( "[cosmetic-apply] hero=%s applied %d override(s), restored %d slot(s)\n" ,
			hero.c_str() , applied , reverted );
	// Include bindings loaded during this pass in the hook's substitution map.
	PublishSkeletonOverrides( wearables , live , count , hero );
	RebuildCombinedModel( wearables , count , true );
}

auto CCosmeticChanger::ScanOwnedByHero( CGameEntitySystem* entitySystem , int heroIndex ) -> void
{
	// m_hMyWearables is empty on this build, so find the hero's wearable
	// entities by ownership instead: any entity whose m_hOwnerEntity points back
	// at our hero. Abilities/items are owned too, so we log the defindex of each
	// and let the readable econ ones (plausible defindex) stand out - the goal
	// is to learn which entities carry the cosmetic defindexes we later rewrite.
	if ( !m_Offsets.hasOwner )
	{
		DEV_LOG( "[cosmetic] owner-scan skipped: m_hOwnerEntity unresolved\n" );
		return;
	}

	const uint32_t defOffset = DefIndexOffset();
	int owned = 0;


	// Writable addresses of this hero's cosmetic-slot (dota_item_wearable)
	// defindexes, collected during the scan for the swap write-test below.
	constexpr int kMaxWearables = 64;
	void* wearableDefAddr[kMaxWearables] = {};
	uint16_t wearableDef[kMaxWearables] = {};
	C_BaseEntity* wearableEntity[kMaxWearables] = {};
	int wearableCount = 0;

	for ( int chunkIndex = 0; chunkIndex < MAX_ENTITY_LISTS; ++chunkIndex )
	{
		auto* chunk = entitySystem->m_pIdentityChunks[chunkIndex];
		if ( !chunk )
			continue;

		for ( int entryIndex = 0; entryIndex < MAX_ENTITIES_IN_LIST; ++entryIndex )
		{
			auto* entity = chunk->m_pIdentities[entryIndex].pBaseEntity();
			if ( !entity )
				continue;

			// Owner offset is small and always inside the entity, so the fast
			// unvalidated read (used everywhere else per-entity) is safe here.
			CHandle owner{ INVALID_EHANDLE_INDEX };
			if ( !FeatureSupport::TryReadField( entity , m_Offsets.ownerEntity , owner ) )
				continue;
			if ( !owner.IsValid() )
				continue;

			// Harvest from anything with an owner - that is every hero's wearables,
			// which is exactly the set of cosmetics already loaded and therefore
			// installable. Harvesting the WHOLE entity list here is what froze the
			// game before: this filter cuts it from ~32k entities to a few hundred,
			// and HarvestBinding caches so the expensive path walk runs once per
			// binding rather than once per entity per pass.
			if ( owner.GetEntryIndex() != heroIndex )
				continue;

			++owned;

			const int entIndex = chunkIndex * MAX_ENTITIES_IN_LIST + entryIndex;
			CEntityIdentity* identity = nullptr;
			C_BaseEntity* named = nullptr;
			const char* name = "?";
			if ( FeatureSupport::TryEntityAtIndex( entitySystem , entIndex , identity , named ) )
				if ( const char* raw = FeatureSupport::EntityNameRaw( named , identity ) )
					name = raw;

			// The defindex lives deep in the entity (~0xB40), so this read must be
			// validated - a non-econ owned entity may be smaller than that.
			uint16_t defIndex = 0;
			void* defAddr = reinterpret_cast<uint8_t*>( entity ) + defOffset;
			const bool readable = FeatureSupport::TryRead( defAddr , defIndex );

			// Only dota_item_wearable entities are cosmetic slots; collect their
			// (readable) defindex addresses for the swap test.
			if ( readable && std::strcmp( name , kWearableName ) == 0 &&
				wearableCount < kMaxWearables )
			{
				wearableDef[wearableCount] = defIndex;
				wearableDefAddr[wearableCount] = defAddr;
				wearableEntity[wearableCount] = entity;
				++wearableCount;
			}

		}
	}

	// Bindings the hook may have missed: the local hero's own wearables, if we
	// were injected after they spawned. A handful of validated reads, once a pass.
	for ( int i = 0; i < wearableCount; ++i )
		RecordBinding( WearableModelBinding( wearableEntity[i] ) );
	// Newly harvested bindings are resolved on the next tick.

	UpdateSnapshot( wearableDef , wearableEntity , wearableCount , heroIndex );

	if ( ( Settings::CosmeticChanger::LogCatalog || m_ForceCatalogLog ) && !m_CurrentHero.empty() )
	{
		LogCatalogForHero( m_CurrentHero , m_ForceCatalogLog );
		m_ForceCatalogLog = false;
	}
	LogConfiguredSelections( m_CurrentHero );
	// Turn the picked selections into real local model swaps. No-ops (once-warned)
	// until a verified SetModel signature is configured.
	ApplySelections( wearableEntity , wearableDefAddr , wearableCount );

	if ( !ShouldLogWearableSet( wearableDef , wearableCount , heroIndex ) || !Settings::CosmeticChanger::LogEquipped )
		return;

	// Clean, human-readable list of just the hero's cosmetic slots - the skins
	// currently equipped on the local hero, one line per wearable with its
	// item-definition index. This is the "available skin list" for the picker.
	DEV_LOG( "[cosmetic] === equipped cosmetics for hero idx=%d (%d wearable slots, %d entities owned) ===\n" ,
		heroIndex , wearableCount , owned );
	for ( int i = 0; i < wearableCount; ++i )
	{
		const WearableSlot* known = i < static_cast<int>( m_Wearables.size() ) ? &m_Wearables[i] : nullptr;
		DEV_LOG( "  [cosmetic] slot %d: defindex=%u name=%s category=%s model=%s\n" ,
			i , static_cast<unsigned>( wearableDef[i] ) ,
			( known && !known->name.empty() ) ? known->name.c_str() : "?" ,
			( known && !known->category.empty() ) ? known->category.c_str() : "?" ,
			( known && !known->model.empty() ) ? known->model.c_str() : "?" );
		LogWearableItemInfo( wearableEntity[i] , i );
		LogModelNameField( wearableEntity[i] , i );
	}

	// What is installable right now: every model the game has loaded this session
	// whose path resolved. Only these are swappable - the setter refuses anything
	// nonresident - so when a pick appears to do nothing, this list says why.
	{
		std::scoped_lock lock( m_HookMutex );
		DEV_LOG( "[cosmetic] %zu model(s) recorded from SetModel (%zu pending resolve, hook=%s):\n" ,
			m_Bindings.size() , m_PendingBindings.size() , SetModel_o ? "installed" : "ABSENT" );
		int listed = 0;
		for ( const auto& entry : m_Bindings )
		{
			if ( ++listed > 40 )
			{
				DEV_LOG( "    ... %zu more\n" , m_Bindings.size() - 40 );
				break;
			}
			DEV_LOG( "    %s\n" , entry.first.c_str() );
		}
	}

}

auto CCosmeticChanger::OnGameTick() -> void
{
	if ( !Settings::CosmeticChanger::Enable && !m_HasCombinedOverrides )
		return;

	const uint32_t now = static_cast<uint32_t>( GetTickCount64() );
	// Throttle the whole pass: it walks the entity list and can call SetModel, and
	// the tick fires far more often than either needs.
	if ( now - m_LastLogTick < 1000 )
		return;
	m_LastLogTick = now;

	// Applied every pass so the toggle takes effect without a reinject, and so the
	// original value is restored the moment it is turned back off.
	if ( auto* client = GetAndromedaClient() )
		client->ApplySkipModelCombine( Settings::CosmeticChanger::SkipModelCombine );

	if ( !ResolveOffsets() )
		return;
	// Resolve captured models even while waiting for a local hero.
	DrainRecordedBindings();

	// Stage 1 of the owned-cosmetics (armory) path: confirm we have a live GC
	// client system, the gateway to the SharedObject inventory cache. Logged once
	// so we can see whether/when the hook has captured it before building the
	// SO-cache traversal on top.
	if ( !m_LoggedGC )
	{
		auto* gc = GetCachedGCClientSystem();
		if ( gc )
		{
			DEV_LOG( "[cosmetic] GC client system ready in cosmetic pass: %p\n" , gc );
			m_LoggedGC = true;
		}
		else if ( !m_LoggedGCWaiting )
		{
			DEV_LOG( "[cosmetic] GC client system not captured yet (dedicated accessor signature is not configured; waiting for fallback getter)\n" );
			m_LoggedGCWaiting = true;
		}
	}

	auto* entitySystem = SDK::Interfaces::GameEntitySystem();
	if ( !entitySystem )
		return;

	C_BaseEntity* hero = nullptr;
	int heroIndex = -1;
	if ( !CLocalHeroResolver::Resolve( entitySystem , hero , heroIndex ) || !hero )
	{
		std::scoped_lock lock( m_HookMutex );
		m_CombinedHero = nullptr;
		m_CombinedOverrides.clear();
		m_CombinedBuildAccepted = false;
		return;
	}

	const uintptr_t vecBase = reinterpret_cast<uintptr_t>( hero ) + m_Offsets.myWearables;
	if ( m_ObserveCombinedBuild && heroIndex == m_CurrentHeroIndex )
	{
		void* binding = WearableModelBinding( hero );
		if ( binding && binding != m_PreviousCombinedBinding )
		{
			DEV_LOG( "[cosmetic-combine] hero rendered binding changed: hero=%d model=%s\n" , heroIndex , WearableModelPath( hero ) );
			m_ObserveCombinedBuild = false;
		}
		else if ( GetTickCount64() - m_LastCombinedAttempt > 15000 )
		{
			DEV_LOG( "[cosmetic-combine] rebuild accepted but no hero binding change observed after 15 seconds: hero=%d model=%s\n" , heroIndex , WearableModelPath( hero ) );
			m_ObserveCombinedBuild = false;
		}
	}
	const auto* vec = m_Offsets.hasWearables
		? reinterpret_cast<const NetworkHandleVector*>( vecBase )
		: nullptr;
	if ( !vec || vec->size <= 0 || vec->size > 32 || !vec->data )
	{
		// Expected on this build - m_hMyWearables is empty for the local hero.
		// Fall back to finding wearables by ownership.
		ScanOwnedByHero( entitySystem , heroIndex );
		return;
	}

	const uint32_t defOffset = DefIndexOffset();
	uint16_t wearableDef[32] = {};
	C_BaseEntity* wearableEntity[32] = {};
	void* wearableDefAddr[32] = {};
	int wearableCount = 0;

	for ( int i = 0; i < vec->size; ++i )
	{
		if ( !vec->data[i].IsValid() )
			continue;
		auto* wearable = entitySystem->GetBaseEntityFromHandle( vec->data[i] );
		if ( !wearable )
			continue;

		wearableDef[wearableCount] = ReadAt<uint16_t>( wearable , defOffset );
		wearableEntity[wearableCount] = wearable;
		wearableDefAddr[wearableCount] = reinterpret_cast<uint8_t*>( wearable ) + defOffset;
		++wearableCount;
	}

	UpdateSnapshot( wearableDef , wearableEntity , wearableCount , heroIndex );

	if ( ( Settings::CosmeticChanger::LogCatalog || m_ForceCatalogLog ) && !m_CurrentHero.empty() )
	{
		LogCatalogForHero( m_CurrentHero , m_ForceCatalogLog );
		m_ForceCatalogLog = false;
	}
	LogConfiguredSelections( m_CurrentHero );
	ApplySelections( wearableEntity , wearableDefAddr , wearableCount );

	if ( !ShouldLogWearableSet( wearableDef , wearableCount , heroIndex ) || !Settings::CosmeticChanger::LogEquipped )
		return;

	DEV_LOG( "[cosmetic] hero wearables=%d (itemOff=0x%X%s):\n" ,
		wearableCount , m_Offsets.item , m_Offsets.usedFallbackItem ? " GUESS" : "" );
	for ( int i = 0; i < wearableCount; ++i )
	{
		DEV_LOG( "  [cosmetic] slot=%d defindex=%u\n" , i , static_cast<unsigned>( wearableDef[i] ) );

		if ( i < static_cast<int>( m_Wearables.size() ) && !m_Wearables[i].model.empty() )
			DEV_LOG( "      model=%s\n" , m_Wearables[i].model.c_str() );
	}
}
