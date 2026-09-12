#include "CCosmeticChanger.hpp"

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
#include <DllLauncher.hpp>

#include <Windows.h>

#include <algorithm>
#include <cctype>
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

	// Entity name of a hero's cosmetic slot entities on this build.
	constexpr const char* kWearableName = "dota_item_wearable";

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
	const char* apply = !SDK_SetModelAvailable()
		? "preview only (no SetModel sig)"
		: ( Settings::CosmeticChanger::ApplyOverrides ? "applying" : "apply off" );

	char buffer[192] = {};
	snprintf( buffer , sizeof( buffer ) , "ready: %s (%zu wearable slots, %s)" ,
		m_CurrentHero.c_str() , m_Wearables.size() , apply );
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

	// entity -> m_pGameSceneNode (a CSkeletonInstance for model entities).
	void* sceneNode = nullptr;
	if ( !FeatureSupport::TryReadField( wearable , m_Offsets.sceneNode , sceneNode ) || !sceneNode )
		return "";

	// sceneNode -> m_modelState.m_hModel. CStrongHandle is a single pointer to
	// the resource binding; ResourceBindingBase_t::data (+0) is the CModel*.
	void* binding = nullptr;
	if ( !FeatureSupport::TryRead(
			reinterpret_cast<const uint8_t*>( sceneNode ) + m_Offsets.modelState + m_Offsets.modelHandle ,
			binding ) || !binding )
		return "";

	void* model = nullptr;
	if ( !FeatureSupport::TryRead( binding , model ) || !model )
		return "";

	// The model's own vpk path (".../foo.vmdl") is stored as a string pointer in
	// the model header. Rather than hard-code a build-specific offset, scan the
	// first handful of pointer slots for one that points at a readable string
	// looking like a model path. Guarded throughout, so a miss just returns "".
	static thread_local char buffer[128];
	constexpr int kSlotsToScan = 24; // 24 * 8 = first 0xC0 bytes of the header
	for ( int slot = 0; slot < kSlotsToScan; ++slot )
	{
		const char* candidate = nullptr;
		if ( !FeatureSupport::TryRead(
				reinterpret_cast<const uint8_t*>( model ) + slot * sizeof( void* ) , candidate ) )
			continue;
		if ( !candidate || !FeatureSupport::IsReadableRuntimeMemory( candidate , 8 ) )
			continue;

		// Copy a bounded, printable run and require it to look like a model path.
		int n = 0;
		for ( ; n < static_cast<int>( sizeof( buffer ) ) - 1; ++n )
		{
			char c = candidate[n];
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
		m_Originals.clear();
		m_OriginalsHeroIndex = heroIndex;
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
			if ( slot.hero.empty() )
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

	DEV_LOG( "[cosmetic-ui] configured selections for hero=%s (%d slot overrides, preview-only until model re-resolve is implemented):\n" ,
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

auto CCosmeticChanger::WriteWearable( C_BaseEntity* wearable , void* defAddr , uint32_t defIndex , const std::string& model ) -> bool
{
	if ( !wearable || model.empty() )
		return false;

	// Keep the econ defindex consistent with the model we install, so anything that
	// re-reads the item view agrees with what is on screen. This write is not what
	// changes the mesh - SetModel below is.
	if ( defAddr && defIndex != 0 )
	{
		const uint16_t value = static_cast<uint16_t>( defIndex );
		DWORD oldProtect = 0;
		if ( FeatureSupport::IsReadableRuntimeMemory( defAddr , sizeof( uint16_t ) ) &&
			VirtualProtect( defAddr , sizeof( uint16_t ) , PAGE_EXECUTE_READWRITE , &oldProtect ) )
		{
			std::memcpy( defAddr , &value , sizeof( uint16_t ) );
			VirtualProtect( defAddr , sizeof( uint16_t ) , oldProtect , &oldProtect );
		}
	}

	return SDK_SetEntityModel( wearable , model.c_str() );
}

auto CCosmeticChanger::ApplySelections( C_BaseEntity* const* wearables , void* const* defAddrs , int count ) -> void
{
	if ( !Settings::CosmeticChanger::ApplyOverrides )
		return;
	if ( !wearables || !defAddrs || count <= 0 )
		return;

	// Without the model setter there is nothing useful to do: the defindex write on
	// its own was proven not to change the rendered mesh, so "applying" would just
	// churn memory and silently look broken. Say so once and stay in preview mode.
	if ( !SDK_SetModelAvailable() )
	{
		if ( !m_WarnedNoSetModel )
		{
			DEV_LOG( "[cosmetic-apply] cannot apply: no SetModel signature configured (see CFunctionList::SetModel). "
				"Selections stay preview-only - a defindex write alone does not swap the model.\n" );
			m_WarnedNoSetModel = true;
		}
		return;
	}

	// The menu mutates the selection list from the render thread while this runs on
	// the game tick, so the whole decision is made under the settings lock. The live
	// wearable state below is this thread's own, and needs no lock.
	std::scoped_lock settingsLock( Settings::CosmeticChanger::Mutex );

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
		mix( i < static_cast<int>( live.size() ) ? live[i].defIndex : 0u );
	for ( const auto& selection : Settings::CosmeticChanger::Selections )
	{
		if ( selection.hero != hero )
			continue;
		for ( unsigned char c : selection.slot )
			mix( c );
		mix( selection.defIndex );
	}

	if ( m_HasAppliedSignature && signature == m_LastAppliedSignature )
		return;
	m_LastAppliedSignature = signature;
	m_HasAppliedSignature = true;

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
		if ( selection->defIndex == slot.defIndex && target->model == slot.model )
			continue;

		const bool ok = WriteWearable( wearables[i] , defAddrs[i] , selection->defIndex , target->model );
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
	constexpr int kMaxWearables = 16;
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
			if ( !owner.IsValid() || owner.GetEntryIndex() != heroIndex )
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
	}

}

auto CCosmeticChanger::OnGameTick() -> void
{
	if ( !Settings::CosmeticChanger::Enable )
		return;

	const uint32_t now = static_cast<uint32_t>( GetTickCount64() );
	// Throttle the whole pass: it walks the entity list and can call SetModel, and
	// the tick fires far more often than either needs.
	if ( now - m_LastLogTick < 1000 )
		return;
	m_LastLogTick = now;

	if ( !ResolveOffsets() )
		return;

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
		return;

	const uintptr_t vecBase = reinterpret_cast<uintptr_t>( hero ) + m_Offsets.myWearables;
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
