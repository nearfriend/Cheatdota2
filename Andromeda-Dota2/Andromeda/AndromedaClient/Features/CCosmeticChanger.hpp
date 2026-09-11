#pragma once

#include <cstdint>
#include <string>
#include <vector>

class CGameEntitySystem;
class C_BaseEntity;

// Local, view-only cosmetic changer. Rewrites the wearable entities attached to
// the LOCAL hero so his skin shows differently on this client only - no other
// player sees it, since their clients render our real equipped items.
//
// Step 1 (this file, for now): resolve the wearable schema and enumerate the
// hero's current wearables, logging each slot's item-definition index. That
// confirms the fields resolve and gives the defindexes we later rewrite.
//
// On this build C_BaseCombatCharacter::m_hMyWearables reads empty for the local
// hero (confirmed in-match: size=0 every tick), so cosmetics are not listed
// there. The fallback path is an owner-scan: walk the entity list and pick the
// entities whose owner handle points back at our hero, which is how the hero's
// attached wearable entities are actually reachable here.
class CCosmeticChanger final
{
public:
	struct CatalogItem
	{
		std::string hero;
		std::string slot;
		std::string category;
		uint32_t defIndex = 0;
		std::string name;
		std::string rarity;
		std::string model;
		std::string image;
	};

	struct WearableSlot
	{
		int slot = 0;
		uint32_t defIndex = 0;
		std::string hero;
		std::string model;
		// Resolved from the catalog by defindex (unique), so these are filled even
		// when the live model-path read fails. `category` is the slot key used to
		// match a UI selection against this wearable.
		std::string name;
		std::string category;
	};

	auto OnRender() -> void;
	auto GetCurrentHero() const -> const std::string&;
	auto GetCurrentHeroIndex() const -> int;
	auto GetWearableSlots() const -> const std::vector<WearableSlot>&;
	auto GetStatus() const -> std::string;
	auto RequestCatalogDump() -> void;

	static auto GetCatalog() -> const std::vector<CatalogItem>&;
	static auto GetCatalogPath() -> std::string;
	static auto LogCatalogForHeroName( const std::string& hero , bool force = false ) -> void;

private:
	struct Offsets
	{
		// C_BaseCombatCharacter::m_hMyWearables - the hero's wearable handles.
		uint32_t myWearables = 0;
		// C_EconEntity::m_AttributeManager -> C_AttributeContainer::m_Item ->
		// C_EconItemView::m_iItemDefinitionIndex. Nested, added together.
		uint32_t attributeManager = 0;
		uint32_t item = 0;
		uint32_t itemDefinitionIndex = 0;
		// C_BaseEntity::m_hOwnerEntity - used by the owner-scan fallback to tie a
		// wearable entity back to the hero that wears it.
		uint32_t ownerEntity = 0;
		// Scene-node -> skeleton -> model-resource chain, used only to surface a
		// human-readable model path per wearable in the log. All best-effort: if
		// any link is unresolved the slot line just omits the path.
		uint32_t sceneNode = 0;         // C_BaseEntity::m_pGameSceneNode
		uint32_t modelState = 0;        // CSkeletonInstance::m_modelState
		uint32_t modelHandle = 0;       // CModelState::m_hModel
		// Extra per-item econ metadata inside C_EconItemView (the "sub info" of a
		// wearable), all relative to the econ item view base. Best-effort.
		uint32_t itemQuality = 0;       // C_EconItemView::m_iEntityQuality
		uint32_t itemLevel = 0;         // C_EconItemView::m_iEntityLevel
		uint32_t itemId = 0;            // C_EconItemView::m_iItemID (uint64)
		uint32_t itemAccountId = 0;     // C_EconItemView::m_iAccountID
		uint32_t itemInvPos = 0;        // C_EconItemView::m_iInventoryPosition
		bool hasWearables = false;
		bool hasAttr = false;
		bool hasItem = false;
		bool hasDef = false;
		bool hasDefIndex = false;
		bool hasOwner = false;
		bool hasModelPath = false;      // whole scene-node->model chain resolved
		bool hasItemMeta = false;       // at least one econ metadata field resolved
		// True when m_Item could not be resolved from the schema and we fell back
		// to kFallbackItemOffset so we can still attempt a read. Reads made this
		// way are best-effort guesses, not schema-confirmed.
		bool usedFallbackItem = false;
		bool resolved = false;
	};

	// Best-known m_Item offset within C_AttributeContainer on recent Source 2
	// Dota builds, used only when the schema lookup fails. Clearly labelled in
	// the log so a wrong guess is obvious in the read-back defindexes.
	static constexpr uint32_t kFallbackItemOffset = 0x50;

	auto ResolveOffsets() -> bool;
	auto DumpItemSchema() -> void;
	// One-time dump of the econ item view / attribute classes, so a single run
	// reveals the exact field spellings and lets us log richer per-item info.
	auto DumpItemViewSchema() -> void;
	// Log the extra econ metadata (quality/level/id/attributes) of one wearable.
	auto LogWearableItemInfo( C_BaseEntity* wearable , int slot ) const -> void;
	// Best-effort model path of a wearable entity, for a readable name in the
	// log. Returns "" when the scene-node/model chain is unresolved or the read
	// fails; never dereferences unguarded, so a bad offset cannot crash.
	auto WearableModelPath( C_BaseEntity* wearable ) const -> const char*;
	// Combined nested offset from an econ entity to its item-definition index.
	auto DefIndexOffset() const -> uint32_t;
	// True when the equipped wearable defindex set changed since the last log.
	auto ShouldLogWearableSet( const uint16_t* defIndexes , int count , int heroIndex ) -> bool;
	// Updates the read-only state surfaced by the Skin Changer menu page.
	auto UpdateSnapshot( const uint16_t* defIndexes , C_BaseEntity* const* wearables , int count , int heroIndex ) -> void;
	// Logs the selected UI overrides once per change while the safe apply path is still pending.
	auto LogConfiguredSelections( const std::string& hero ) -> void;
	// Applies the picked cosmetics to the live wearables: writes each target
	// defindex and asks the game to load+install the target model. Requires a
	// resolved SetModel signature; no-ops (and says so once) without one.
	auto ApplySelections( C_BaseEntity* const* wearables , void* const* defAddrs , int count ) -> void;
	// Fallback enumeration: log every entity owned by the hero and its defindex.
	auto ScanOwnedByHero( CGameEntitySystem* entitySystem , int heroIndex ) -> void;

	Offsets m_Offsets{};
	bool m_ResolveTried = false;
	bool m_LoggedSchema = false;
	bool m_LoggedFields = false;
	bool m_LoggedItemView = false;
	bool m_LoggedGC = false;
	bool m_LoggedGCWaiting = false;
	bool m_HasWearableSnapshot = false;
	bool m_HasSelectionSnapshot = false;
	bool m_ForceCatalogLog = false;
	bool m_WarnedNoSetModel = false;
	uint64_t m_LastAppliedSignature = 0;
	bool m_HasAppliedSignature = false;
	uint64_t m_LastWearableSignature = 0;
	uint64_t m_LastSelectionSignature = 0;
	uint32_t m_LastLogTick = 0;
	int m_CurrentHeroIndex = -1;
	std::string m_CurrentHero;
	std::vector<WearableSlot> m_Wearables;
};
