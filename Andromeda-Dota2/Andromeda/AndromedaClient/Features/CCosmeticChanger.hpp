#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

class CGameEntitySystem;
class C_BaseEntity;

// Local, view-only cosmetic changer. Rewrites the wearable entities attached to
// the LOCAL hero so his skin shows differently on this client only - no other
// player sees it, since their clients render our real equipped items.
//
// The swap itself needs two writes per slot: the econ defindex, so anything that
// re-reads the item view agrees, and an explicit CBaseModelEntity::SetModel, which
// is what actually changes the mesh - an in-match test proved a bare defindex
// write does not, because Source 2 caches the model handle at spawn. SetModel has
// no signature configured yet (CFunctionList::SetModel), so the picker and the
// catalog are live while the apply path stays inert and says so in the menu.
//
// Everything here runs on the game tick (Hook_OnCreateMove), never the render
// thread, because the apply calls an engine function on live entities. The menu
// reads GetCurrentHero/GetStatus/IsModelWorn from the render thread, so those go
// through m_SnapshotMutex, and the selection list goes through
// Settings::CosmeticChanger::Mutex.
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

	// Runs on the game tick (Hook_OnCreateMove), not the render thread: the apply
	// path calls a real engine function on live entities, so it must not race the
	// frame the engine is drawing. The menu reads the snapshot from the render
	// thread instead, so everything it can touch is mutex-guarded.
	auto OnGameTick() -> void;
	// Copies instead of returning a reference: the snapshot is rebuilt on the tick
	// thread while the menu reads it.
	auto GetCurrentHero() const -> std::string;
	auto GetCurrentHeroIndex() const -> int;
	// True when the hero currently renders this catalog item, i.e. one of his live
	// wearables carries its defindex or its model path. Drives the menu Worn badge.
	auto IsModelWorn( const CatalogItem& item ) const -> bool;
	auto GetStatus() const -> std::string;
	auto RequestCatalogDump() -> void;

	static auto GetCatalog() -> const std::vector<CatalogItem>&;
	static auto GetCatalogPath() -> std::string;
	static auto LogCatalogForHeroName( const std::string& hero , bool force = false ) -> void;
	// Picked cosmetics survive a restart: the menu saves on every change and asks
	// to load once, so a loadout set in one session is still applied in the next.
	static auto GetSelectionsPath() -> std::string;
	static auto LoadSelections() -> void;
	static auto SaveSelections() -> void;

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

	// What a wearable slot looked like before we ever wrote to it. Captured on the
	// first apply pass that touches the slot, so clearing a pick in the menu can
	// put the hero's real item back instead of leaving the override on screen.
	struct OriginalSlot
	{
		int slot = 0;
		uint32_t defIndex = 0;
		std::string model;
	};

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
	// Writes one wearable's defindex + model. Both halves are best-effort and the
	// return value is whether the model actually took.
	auto WriteWearable( C_BaseEntity* wearable , void* defAddr , uint32_t defIndex , const std::string& model ) -> bool;
	// Remembers a slot's untouched defindex/model the first time it is overridden.
	auto RememberOriginal( int slot , uint32_t defIndex , const std::string& model ) -> void;
	auto FindOriginal( int slot ) const -> const OriginalSlot*;
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
	// Cleared whenever the local hero entity changes - the slot indexes only mean
	// something for one hero's wearable list.
	std::vector<OriginalSlot> m_Originals;
	int m_OriginalsHeroIndex = -1;
	// Guards m_CurrentHero / m_CurrentHeroIndex / m_Wearables, which the tick
	// thread rebuilds and the menu reads.
	mutable std::mutex m_SnapshotMutex;
};
