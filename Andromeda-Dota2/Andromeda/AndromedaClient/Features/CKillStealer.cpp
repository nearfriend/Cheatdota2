#include "CKillStealer.hpp"

#include <AndromedaClient/CAndromedaGUI.hpp>
#include <AndromedaClient/Data/AbilityDamageData.hpp>
#include <AndromedaClient/Settings/Settings.hpp>
#include <Common/DevLog.hpp>
#include <Dota2/SDK/CSchemaOffset.hpp>
#include <Dota2/SDK/Interface/CGameEntitySystem.hpp>
#include <Dota2/SDK/Interface/IVEngineClient2.hpp>
#include <Dota2/SDK/Math/Math.hpp>
#include <Dota2/SDK/SDK.hpp>
#include <Dota2/SDK/Types/CEntityData.hpp>

#include <ImGui/imgui.h>
#include <Windows.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

// Design notes (rewritten from scratch after a session of profiling a slower,
// more elaborate version of this feature):
//  - No VirtualQuery in the hot path. Every field read here is a plain
//    dereference of an offset on an entity the game itself handed us -
//    CLastHitAssistant.cpp does the same and has never had a problem with it.
//    VirtualQuery is a kernel syscall; doing it per-field, per-entity, per-tick
//    was the dominant cost in the previous version.
//  - No reliance on CGameEntitySystem::GetLocalPlayerController(). In this
//    environment it never succeeds, and its internal fallback (a full scan of
//    every populated entity slot with a virtual call on each one) cost
//    100-190ms per attempt when measured. We still try it once in a while in
//    case it starts working, but the real, primary way we find "you" is the
//    same screen-center-nearest-hero heuristic CLastHitAssistant already uses
//    successfully, which needs nothing more than positions we already read.
//  - One flat entity-chunk scan per think-tick builds every hero snapshot at
//    once (allies, enemies, and the local-hero candidates), instead of a
//    separate handle-cache layer refreshed on its own schedule.
//  - The kill-plan search is greedy-by-largest-effective-damage, which is
//    provably optimal for "fewest tools to reach at least this much damage"
//    (a covering problem) - no combinatorial subset search, so there is no n
//    for which this can blow up.
//  - Logging is throttled and only fires when Debug Logs is enabled - nothing
//    unconditional runs on a hot path.

namespace
{
	constexpr uint32_t kThinkIntervalMs = 150;
	constexpr uint32_t kPlanExpiryMs = 2500;
	constexpr size_t kMaxPlanActions = 12;

	enum class ToolKind : uint8_t
	{
		Ability,
		Item,
		Attack
	};

	struct KillStealerOffsets
	{
		uint32_t health = 0;
		uint32_t maxHealth = 0;
		uint32_t team = 0;
		uint32_t mana = 0;
		uint32_t abilities = 0;
		uint32_t inventory = 0;
		uint32_t inventoryItems = 0;
		uint32_t abilityLevel = 0;
		uint32_t abilityCooldown = 0;
		uint32_t abilityManaCost = 0;
		uint32_t abilityCastRange = 0;
		uint32_t abilityActivated = 0;
		uint32_t sceneNode = 0;
		uint32_t absOrigin = 0;
		uint32_t armor = 0;
		uint32_t magicResistance = 0;
		uint32_t spellAmp = 0;
		uint32_t damageMin = 0;
		uint32_t damageBonus = 0;
		uint32_t attackRange = 0;
		uint32_t isIllusion = 0;
		uint32_t isClone = 0;
		uint32_t heroPlayerId = 0;
		uint32_t playerOwnerId = 0;
		uint32_t waitingToSpawn = 0;
		uint32_t controllerAssignedHero = 0;
		uint32_t isLocalController = 0;
		bool hasControllerAssignedHero = false;
		bool hasIsLocalController = false;
		bool hasInventory = false;
		bool hasAbilityActivated = false;
		bool hasArmor = false;
		bool hasMagicResistance = false;
		bool hasSpellAmp = false;
		bool hasDamageBonus = false;
		bool hasAttackRange = false;
		bool hasIsIllusion = false;
		bool hasIsClone = false;
		bool hasHeroPlayerId = false;
		bool hasPlayerOwnerId = false;
		bool hasWaitingToSpawn = false;
		bool resolved = false;
	};

	struct HeroSnapshot
	{
		C_BaseEntity* entity = nullptr;
		int entIndex = -1;
		std::string name;
		Vector3 origin{};
		uint8_t team = 0;
		int playerId = -1;
		// False when the scene node still holds the exact-zero placeholder,
		// meaning this hero has never had a real position replicated to us.
		bool originReplicated = false;
		int health = 0;
		int maxHealth = 0;
		float mana = 0.f;
		float armor = 0.f;
		float magicResistance = 0.25f;
		// Pre-normalization value, kept only so debug logging can show whether
		// the percentage-vs-fraction rescale in BuildSnapshot actually fired.
		float rawMagicResistance = 0.25f;
		float spellAmp = 0.f;
		float attackDamage = 0.f;
		float attackRange = 150.f;
	};

	struct KillTool
	{
		ToolKind kind = ToolKind::Ability;
		std::string name;
		float castRange = 0.f;
		float rawDamage = 0.f;
		AbilityDamageType damageType = AbilityDamageType::Magical;
		WORD key = 0;
		bool noTarget = false;
		bool unitTarget = false;
		bool pointTarget = false;
		bool isDamageAmplifier = false;
		uint32_t delayMs = 0;
		// Mana cost at the level CollectTools read it at. Needed so a plan
		// combining multiple actions can simulate them draining the SAME mana
		// pool in sequence - see the cumulative check in BuildKillPlan.
		int manaCost = 0;
	};

	struct KillPlanEvaluation
	{
		size_t actionCount = 0;
		float totalDamage = 0.f;
		float totalDelay = 0.f;
	};

	auto IsReadableRuntimeMemory(const void* ptr, size_t size) -> bool
	{
		if (!ptr || size == 0)
			return false;
		MEMORY_BASIC_INFORMATION mbi{};
		if (!VirtualQuery(ptr, &mbi, sizeof(mbi)))
			return false;
		if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
			return false;
		const DWORD readable = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
			PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
		if (!(mbi.Protect & readable))
			return false;
		const auto start = reinterpret_cast<uintptr_t>(ptr);
		const auto regionStart = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
		const auto regionEnd = regionStart + mbi.RegionSize;
		return start >= regionStart && start + size <= regionEnd && start + size >= start;
	}

	// Plain dereference, no VirtualQuery - `base` is always the game's own live
	// entity pointer (or an offset a few hundred bytes into it), never
	// attacker-controlled or independently computed.
	template <typename T>
	auto ReadField(const void* base, uint32_t offset, T fallback = T{}) -> T
	{
		if (!base || !offset)
			return fallback;
		T value{};
		std::memcpy(&value, reinterpret_cast<const uint8_t*>(base) + offset, sizeof(T));
		return value;
	}

	auto ReadOrigin(const void* entity, const KillStealerOffsets& offsets, Vector3& out) -> bool
	{
		if (!entity || !offsets.sceneNode || !offsets.absOrigin)
			return false;
		void* sceneNode = ReadField<void*>(entity, offsets.sceneNode, nullptr);
		if (!sceneNode)
			return false;
		out = ReadField<Vector3>(sceneNode, offsets.absOrigin);
		return std::isfinite(out.m_x) && std::isfinite(out.m_y) && std::isfinite(out.m_z);
	}

	auto ToLower(const std::string& in) -> std::string
	{
		std::string out = in;
		std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
		return out;
	}

	auto EntityName(C_BaseEntity* entity) -> std::string
	{
		if (!entity)
			return {};
		auto* identity = entity->pEntityIdentity();
		if (!identity)
			return {};
		if (const char* name = identity->DesingerName().String(); name && name[0])
			return name;
		if (const char* name = identity->Name().String(); name && name[0])
			return name;
		return {};
	}

	// Two-pronged check (matching CLastHitAssistant.cpp's proven-reliable
	// LooksLikeHeroEntity): name-string alone isn't always enough - if the
	// designer/entity name is empty or doesn't resolve for whatever reason,
	// the schema class name is a working fallback so a hero doesn't silently
	// get dropped from the scan entirely.
	auto TextContains(const char* text, const char* needle) -> bool
	{
		return text && needle && std::strstr(text, needle) != nullptr;
	}

	auto LooksLikeHeroEntity(C_BaseEntity* entity, const std::string& name) -> bool
	{
		if (!entity)
			return false;

		const std::string lower = ToLower(name);
		if (lower.find("npc_dota_hero_") != std::string::npos)
			return true;

		const char* className = entity->GetSchemaClassName();
		return TextContains(className, "DOTA_BaseNPC_Hero") ||
			TextContains(className, "C_DOTA_BaseNPC_Hero") ||
			TextContains(className, "DOTA_Unit_Hero");
	}

	auto IsPlayableTeam(uint8_t team) -> bool
	{
		return team == 2 || team == 3;
	}

	auto ResolveOffsets() -> KillStealerOffsets&
	{
		static KillStealerOffsets offsets{};
		if (offsets.resolved)
			return offsets;

		static ULONGLONG lastAttemptTick = 0;
		const ULONGLONG now = GetTickCount64();
		if (lastAttemptTick && now - lastAttemptTick < 500)
			return offsets;
		lastAttemptTick = now;

		auto* schema = GetSchemaOffset();
		if (!schema)
			return offsets;

		const bool hasHealth = schema->TryGetOffset("C_BaseEntity", "m_iHealth", offsets.health);
		const bool hasMaxHealth = schema->TryGetOffset("C_BaseEntity", "m_iMaxHealth", offsets.maxHealth);
		const bool hasTeam = schema->TryGetOffset("C_BaseEntity", "m_iTeamNum", offsets.team);
		const bool hasMana = schema->TryGetOffset("C_DOTA_BaseNPC", "m_flMana", offsets.mana);
		const bool hasAbilities = schema->TryGetOffset("C_DOTA_BaseNPC", "m_vecAbilities", offsets.abilities);
		const bool hasSceneNode = schema->TryGetOffset("C_BaseEntity", "m_pGameSceneNode", offsets.sceneNode);
		const bool hasAbsOrigin = schema->TryGetOffset("CGameSceneNode", "m_vecAbsOrigin", offsets.absOrigin);
		const bool hasAbilityLevel = schema->TryGetOffset("C_DOTABaseAbility", "m_iLevel", offsets.abilityLevel);
		const bool hasAbilityCooldown = schema->TryGetOffset("C_DOTABaseAbility", "m_flCooldown", offsets.abilityCooldown) ||
			schema->TryGetOffset("C_DOTABaseAbility", "m_fCooldown", offsets.abilityCooldown);
		const bool hasAbilityMana = schema->TryGetOffset("C_DOTABaseAbility", "m_iManaCost", offsets.abilityManaCost);
		schema->TryGetOffset("C_DOTABaseAbility", "m_flCastRange", offsets.abilityCastRange);
		offsets.hasAbilityActivated = schema->TryGetOffset("C_DOTABaseAbility", "m_bIsActivated", offsets.abilityActivated) ||
			schema->TryGetOffset("C_DOTABaseAbility", "m_bActivated", offsets.abilityActivated);

		const bool hasInventoryContainer = schema->TryGetOffset("C_DOTA_BaseNPC", "m_Inventory", offsets.inventory);
		const bool hasNestedItems = schema->TryGetOffset("C_DOTA_UnitInventory", "m_hItems", offsets.inventoryItems) ||
			schema->TryGetOffset("CDOTA_UnitInventory", "m_hItems", offsets.inventoryItems);
		if (hasNestedItems)
			offsets.hasInventory = hasInventoryContainer;
		else
			offsets.hasInventory = schema->TryGetOffset("C_DOTA_BaseNPC", "m_hItems", offsets.inventoryItems);

		offsets.hasArmor = schema->TryGetOffset("C_DOTA_BaseNPC", "m_flPhysicalArmorValue", offsets.armor);
		offsets.hasMagicResistance = schema->TryGetOffset("C_DOTA_BaseNPC", "m_flMagicalResistanceValue", offsets.magicResistance) ||
			schema->TryGetOffset("C_DOTA_BaseNPC", "m_flMagicalResistance", offsets.magicResistance);
		offsets.hasSpellAmp = schema->TryGetOffset("C_DOTA_BaseNPC", "m_flSpellAmplification", offsets.spellAmp) ||
			schema->TryGetOffset("C_DOTA_BaseNPC", "m_flSpellAmp", offsets.spellAmp) ||
			schema->TryGetOffset("C_DOTA_BaseNPC_Hero", "m_flSpellAmplification", offsets.spellAmp);
		schema->TryGetOffset("C_DOTA_BaseNPC", "m_iDamageMin", offsets.damageMin);
		offsets.hasDamageBonus = schema->TryGetOffset("C_DOTA_BaseNPC", "m_iDamageBonus", offsets.damageBonus);
		offsets.hasAttackRange = schema->TryGetOffset("C_DOTA_BaseNPC", "m_iAttackRange", offsets.attackRange);
		offsets.hasIsIllusion = schema->TryGetOffset("C_DOTA_BaseNPC", "m_bIsIllusion", offsets.isIllusion);
		offsets.hasIsClone = schema->TryGetOffset("C_DOTA_BaseNPC", "m_bIsClone", offsets.isClone);
		offsets.hasHeroPlayerId = schema->TryGetOffset("C_DOTA_BaseNPC_Hero", "m_iPlayerID", offsets.heroPlayerId) ||
			schema->TryGetOffset("C_DOTA_BaseNPC", "m_iPlayerID", offsets.heroPlayerId);
		offsets.hasPlayerOwnerId = schema->TryGetOffset("C_DOTA_BaseNPC", "m_nPlayerOwnerID", offsets.playerOwnerId);
		// NOTE: there is deliberately no per-team vision lookup here. A
		// m_iTaggedAsVisibleByTeam-style bitmask would be the right signal for
		// "can we actually see this enemy", but this build's schema has no such
		// field - dumping C_DOTA_BaseNPC and C_BaseEntity produced only
		// m_bSelectionRingVisible, m_flInvisibilityLevel and
		// m_nVisibilityNoInterpolationTick. Use CShcemaOffset::LogFieldsMatching
		// to re-check if a future build adds one.
		offsets.hasWaitingToSpawn = schema->TryGetOffset("C_DOTA_BaseNPC", "m_bIsWaitingToSpawn", offsets.waitingToSpawn);
		offsets.hasControllerAssignedHero = schema->TryGetOffset("C_DOTAPlayerController", "m_hAssignedHero", offsets.controllerAssignedHero);
		offsets.hasIsLocalController =
			schema->TryGetOffset("CBasePlayerController", "m_bIsLocalPlayerController", offsets.isLocalController) ||
			schema->TryGetOffset("C_BasePlayerController", "m_bIsLocalPlayerController", offsets.isLocalController) ||
			schema->TryGetOffset("C_DOTAPlayerController", "m_bIsLocalPlayerController", offsets.isLocalController);

		offsets.resolved = hasHealth && hasMaxHealth && hasTeam && hasMana && hasAbilities &&
			hasSceneNode && hasAbsOrigin && hasAbilityLevel && hasAbilityCooldown && hasAbilityMana;

		return offsets;
	}

	auto IsLikelyPlayerId(int value) -> bool
	{
		return value >= 0 && value < 24;
	}

	auto UnitPlayerId(const C_BaseEntity* entity, const KillStealerOffsets& offsets) -> int
	{
		if (offsets.hasHeroPlayerId)
		{
			const int heroPlayerId = ReadField<int>(entity, offsets.heroPlayerId, -1);
			if (IsLikelyPlayerId(heroPlayerId))
				return heroPlayerId;
		}
		if (offsets.hasPlayerOwnerId)
		{
			const int ownerPlayerId = ReadField<int>(entity, offsets.playerOwnerId, -1);
			if (IsLikelyPlayerId(ownerPlayerId))
				return ownerPlayerId;
		}
		return -1;
	}

	auto TryLocalPlayerId(int& outPlayerId) -> bool
	{
		outPlayerId = -1;
		auto* engine = SDK::Interfaces::EngineToClient();
		if (!engine)
			return false;
		engine->GetLocalPlayer(outPlayerId, 0);
		return IsLikelyPlayerId(outPlayerId);
	}

	// outReject, when supplied, receives a static string naming the gate that
	auto BuildSnapshot(C_BaseEntity* entity, int entIndex, const KillStealerOffsets& offsets, HeroSnapshot& out) -> bool
	{
		const int health = ReadField<int>(entity, offsets.health, 0);
		const int maxHealth = ReadField<int>(entity, offsets.maxHealth, 0);
		if (health <= 0)
			return false;
		if (maxHealth <= 0 || maxHealth > 50000)
			return false;

		const uint8_t team = ReadField<uint8_t>(entity, offsets.team, 0);
		if (!IsPlayableTeam(team))
			return false;

		if (offsets.hasIsIllusion && ReadField<bool>(entity, offsets.isIllusion, false))
			return false;
		if (offsets.hasIsClone && ReadField<bool>(entity, offsets.isClone, false))
			return false;
		// Pre-spawn hero entities exist during strategy time and while dead
		// heroes wait to respawn; they carry health and a team but are not on
		// the map, so they can never be a target.
		if (offsets.hasWaitingToSpawn && ReadField<bool>(entity, offsets.waitingToSpawn, false))
			return false;

		Vector3 origin{};
		if (!ReadOrigin(entity, offsets, origin))
			return false;

		out = {};
		out.entity = entity;
		out.entIndex = entIndex;
		out.name = EntityName(entity);
		out.origin = origin;
		out.team = team;
		out.playerId = UnitPlayerId(entity, offsets);
		// An exact zero on all three axes is the never-replicated placeholder,
		// not a real standing position. It matters because (0,0,0) sits near the
		// middle of the Dota map, so an unseen hero parked there otherwise reads
		// as the closest enemy to anyone fighting around mid.
		out.originReplicated = !(origin.m_x == 0.f && origin.m_y == 0.f && origin.m_z == 0.f);
		out.health = health;
		out.maxHealth = maxHealth;
		out.mana = ReadField<float>(entity, offsets.mana, 0.f);
		if (!std::isfinite(out.mana) || out.mana < 0.f || out.mana > 100000.f)
			out.mana = 0.f;
		out.armor = offsets.hasArmor ? ReadField<float>(entity, offsets.armor, 0.f) : 0.f;
		if (offsets.hasMagicResistance)
		{
			float rawResistance = ReadField<float>(entity, offsets.magicResistance, 0.25f);
			// m_flMagicalResistanceValue/m_flMagicalResistance has been observed
			// coming back as a whole-number percentage (e.g. 25.0 for 25%) in
			// this build rather than the 0-1 fraction the -0.95..0.95 clamp
			// below assumes. Left unnormalized, any hero with resistance >=1
			// (i.e. essentially every hero, since base is already ~25) clamped
			// straight to the 0.95 ceiling - silently applying 95% magic
			// resistance to every enemy, every time, which is why Finger of
			// Death (and every other magic spell) kept reading as non-lethal
			// even against low-HP targets. A fraction-scale value is always
			// comfortably under 1 in practice, so treat anything |x|>=1 as a
			// percentage and rescale it before the clamp.
			out.rawMagicResistance = rawResistance;
			if (std::isfinite(rawResistance) && std::fabs(rawResistance) >= 1.f)
				rawResistance *= 0.01f;
			out.magicResistance = std::clamp(rawResistance, -0.95f, 0.95f);
		}
		else
		{
			out.magicResistance = 0.25f;
		}
		out.spellAmp = offsets.hasSpellAmp ? std::clamp(ReadField<float>(entity, offsets.spellAmp, 0.f), -0.75f, 3.0f) : 0.f;
		out.attackDamage = static_cast<float>((std::max)(1,
			ReadField<int>(entity, offsets.damageMin, 0) + (offsets.hasDamageBonus ? ReadField<int>(entity, offsets.damageBonus, 0) : 0)));
		out.attackRange = offsets.hasAttackRange
			? static_cast<float>((std::max)(150, ReadField<int>(entity, offsets.attackRange, 150)))
			: 150.f;
		return true;
	}

	// Whether this hero's position can be trusted for range checks.
	//
	// NOTE: this is deliberately NOT a fog-of-war test. A per-team vision
	// bitmask would be the right signal, but this build's schema exposes no
	// such field - a dump of C_DOTA_BaseNPC and C_BaseEntity turned up only
	// m_bSelectionRingVisible, m_flInvisibilityLevel and
	// m_nVisibilityNoInterpolationTick, none of which describe team vision. So
	// the only staleness we can actually detect is the never-replicated
	// (0,0,0) placeholder, which matters because (0,0,0) sits near the middle
	// of the map and would otherwise read as the closest enemy to anyone
	// fighting around mid.
	//
	// In practice enemy positions do keep updating (see debug.log: Warlock
	// moved through several distinct positions while unseen), so a hero that
	// walked into fog is not currently distinguishable here.
	auto HasTrustworthyPosition(const HeroSnapshot& hero) -> bool
	{
		return hero.originReplicated;
	}

	// One flat pass over every populated entity slot, throttled to the same
	// cadence as the rest of the feature. Builds every playable-team hero
	// snapshot in one go - no separate handle-cache layer to keep in sync.
	// Walks the allocated identity chunks directly instead of indexing
	// GetHighestEntityIndex()..1. That index has been the actual cause of
	// "local hero not resolved": in a real capture, our own hero (team=2)
	// never appeared in an index-bounded scan for an entire ~7800-line
	// session, while CAndromedaClient.cpp's CollectInPlayHeroVitals - which
	// already walks chunks for exactly this reason - found it every tick
	// (see its own comment: "GetHighestEntityIndex is build-dependent in
	// Dota"). Chunk 0 covers entity 0..511, chunk 1 covers 512..1023, and so
	// on for all MAX_ENTITY_LISTS chunks - together they cover every index
	// GetHighestEntityIndex() could ever report and then some, so this is
	// strictly a superset of the old scan, not a different one.
	auto ScanHeroes(CGameEntitySystem* entitySystem, const KillStealerOffsets& offsets) -> std::vector<HeroSnapshot>
	{
		std::vector<HeroSnapshot> heroes;
		if (!entitySystem)
			return heroes;

		heroes.reserve(12);
		for (int chunkIndex = 0; chunkIndex < MAX_ENTITY_LISTS; ++chunkIndex)
		{
			auto* chunk = entitySystem->m_pIdentityChunks[chunkIndex];
			if (!chunk)
				continue;

			for (int entryIndex = 0; entryIndex < MAX_ENTITIES_IN_LIST; ++entryIndex)
			{
				auto* entity = chunk->m_pIdentities[entryIndex].pBaseEntity();
				if (!entity)
					continue;
				const int index = chunkIndex * MAX_ENTITIES_IN_LIST + entryIndex;

				HeroSnapshot snapshot{};
				if (!BuildSnapshot(entity, index, offsets, snapshot))
					continue;
				if (!LooksLikeHeroEntity(entity, snapshot.name))
					continue;

				heroes.push_back(std::move(snapshot));
			}
		}

		return heroes;
	}

	// This is the authoritative identity path, so the backoff here only exists
	// to bound cost, never to give up on it. Failures are normal and expected
	// while loading / in hero select, when there genuinely is no controller yet
	// - a long backoff would then still be in force at the moment the match
	// starts, handing the whole game to the screen-center guess.
	//
	// The cost concern is real but already bounded upstream: GetLocalPlayerController()
	// throttles its own full-entity scan to once per 250ms and caches the handle
	// on success, so a resolved hero costs a handle deref, not a scan. Cap the
	// backoff at 2s so it recovers as soon as the controller actually exists.
	auto TryResolveViaController(CGameEntitySystem* entitySystem, const KillStealerOffsets& offsets, HeroSnapshot& out) -> bool
	{
		static int failStreak = 0;
		static ULONGLONG nextAttemptTick = 0;

		const ULONGLONG now = GetTickCount64();
		if (now < nextAttemptTick)
			return false;

		const auto Fail = [&]() -> bool
		{
			++failStreak;
			nextAttemptTick = now + (failStreak >= 3 ? 2000ull : 500ull);
			return false;
		};

		auto* controller = CGameEntitySystem::GetLocalPlayerController();
		if (!controller)
			return Fail();

		const CHandle heroHandle = controller->m_hAssignedHero();
		if (!heroHandle.IsValid())
			return Fail();

		auto* heroEntity = entitySystem->GetBaseEntityFromHandle(heroHandle);
		if (!heroEntity || !BuildSnapshot(heroEntity, heroHandle.GetEntryIndex(), offsets, out))
			return Fail();

		failStreak = 0;
		nextAttemptTick = 0;
		return true;
	}

	// Match the engine's local player slot against each hero's own m_iPlayerID.
	// Unlike m_bIsLocalPlayerController (see ResolveLocalHero), m_iPlayerID is
	// replicated for every hero, so comparing it to our own slot is an exact
	// identity test rather than a byte that only happens to be meaningful on
	// our own controller. Operates on the hero list we already scanned this
	// tick, so it costs nothing extra.
	auto ResolveLocalHeroByPlayerId(const std::vector<HeroSnapshot>& heroes, const HeroSnapshot*& out) -> bool
	{
		out = nullptr;

		int localPlayerId = -1;
		const bool haveLocalPlayerId = TryLocalPlayerId(localPlayerId);

		const HeroSnapshot* match = nullptr;
		bool ambiguous = false;
		if (haveLocalPlayerId)
		{
			for (const auto& hero : heroes)
			{
				if (hero.playerId != localPlayerId)
					continue;
				// Ambiguous - two heroes claiming our slot (illusions/clones that
				// kept the owner's id). Refuse rather than pick one at random.
				if (match)
				{
					ambiguous = true;
					break;
				}
				match = &hero;
			}
		}

		if (match && !ambiguous)
		{
			out = match;
			return true;
		}

		return false;
	}

	auto ResolveLocalHero(CGameEntitySystem* entitySystem, const KillStealerOffsets& offsets,
		const std::vector<HeroSnapshot>& heroes, HeroSnapshot& out) -> bool
	{
		static int s_CachedEntIndex = -1;
		// True only when the cached hero came from an identity source (the
		// assigned-hero handle, or an exact player-id match) rather than a
		// positional guess. An untrusted cache is still reused between attempts
		// so the readout doesn't flicker, but the resolvers above it keep
		// running so a wrong guess can always be replaced.
		static bool s_CacheTrusted = false;

		// PRIMARY: our engine player slot matched against each hero's replicated
		// m_iPlayerID. This is the only method observed to be correct in this
		// environment - see debug.log, where the controller path never resolved
		// at all and the screen-center path produced a wrong hero.
		//
		// Deliberately runs BEFORE the cache, every tick, rather than being
		// short-circuited by it: it is a plain loop over the hero list we
		// already scanned, so it costs nothing, and re-deriving it each tick
		// means a stale or wrong cached entry can never outlive one tick. Every
		// path below is a fallback for when the engine reports no usable slot.
		const HeroSnapshot* playerIdHero = nullptr;
		if (ResolveLocalHeroByPlayerId(heroes, playerIdHero))
		{
			out = *playerIdHero;
			s_CachedEntIndex = out.entIndex;
			s_CacheTrusted = true;
			return true;
		}

		// Reuse the last trusted hero while the engine slot is briefly
		// unavailable, so the readout doesn't drop out mid-game.
		if (s_CachedEntIndex >= 0 && s_CacheTrusted)
		{
			for (const auto& hero : heroes)
			{
				if (hero.entIndex == s_CachedEntIndex)
				{
					out = hero;
					return true;
				}
			}
			// Trusted hero vanished (died/reconnect) - drop and re-resolve.
			s_CachedEntIndex = -1;
			s_CacheTrusted = false;
		}

		// FALLBACK: the engine's local-player-controller, whose m_hAssignedHero
		// is by definition our hero. Correct in principle, but its resolver has
		// never succeeded on this build.
		//
		// Do NOT reintroduce a bare m_bIsLocalPlayerController scan ahead of
		// this. That byte is only meaningful on our own controller; on other
		// players' controllers it is not replicated, so it reads as whatever
		// happens to be in memory. A stale non-zero byte there makes the first
		// matching controller win and hands back somebody else's hero - which
		// is exactly the bug where a Lion player was shown as Sniper.
		HeroSnapshot controllerHero{};
		if (TryResolveViaController(entitySystem, offsets, controllerHero))
		{
			out = controllerHero;
			s_CachedEntIndex = controllerHero.entIndex;
			s_CacheTrusted = true;
			return true;
		}

		// No screen-center guess below this point, deliberately. It used to be
		// the last resort, but it has no way to know our team before a hero is
		// ever trusted-resolved, so at the very start of a match (before our
		// own hero entity exists client-side - see debug.log, where it took
		// several minutes for npc_dota_hero_lion to appear in the scan at all
		// while enemy bots were already present) it would happily report the
		// nearest hero to the screen center as "you" even when that hero was on
		// the ENEMY team. Reporting nothing until playerid/controller actually
		// identifies us is strictly better than reporting a guess that can be
		// an opponent: every consumer of ResolveLocalHero (targeting, the
		// overlay, the range circle) already treats "not resolved" as "do
		// nothing yet", which is the correct behavior while our hero doesn't
		// exist to act through anyway.
		s_CachedEntIndex = -1;
		return false;
	}

	auto ReadHandleArray(const void* field, int maxCount, std::vector<CHandle>& out) -> bool
	{
		out.clear();
		struct NetworkHandleVector
		{
			int32_t size = 0;
			int32_t pad = 0;
			const CHandle* data = nullptr;
			int32_t allocationCount = 0;
			int32_t growSize = 0;
		};

		if (!field)
			return false;
		NetworkHandleVector direct{};
		std::memcpy(&direct, field, sizeof(direct));

		if (direct.size <= 0 || direct.size > maxCount || !direct.data ||
			!IsReadableRuntimeMemory(direct.data, sizeof(CHandle) * static_cast<size_t>(direct.size)))
			return false;

		out.resize(direct.size);
		std::memcpy(out.data(), direct.data, sizeof(CHandle) * static_cast<size_t>(direct.size));
		return true;
	}

	auto ReadInventoryHandles(C_BaseEntity* hero, const KillStealerOffsets& offsets, std::vector<CHandle>& out) -> bool
	{
		out.clear();
		if (!hero || !offsets.hasInventory)
			return false;

		const uint8_t* itemsBase = reinterpret_cast<const uint8_t*>(hero) + offsets.inventory + offsets.inventoryItems;
		int32_t reportedSize = 0;
		std::memcpy(&reportedSize, itemsBase, sizeof(reportedSize));

		const CHandle* handles = nullptr;
		int count = 0;
		constexpr int kMaxInventorySlots = 27;

		if (reportedSize > 0 && reportedSize <= kMaxInventorySlots)
		{
			handles = reinterpret_cast<const CHandle*>(itemsBase + sizeof(int32_t));
			count = reportedSize;
		}
		else
		{
			handles = reinterpret_cast<const CHandle*>(itemsBase);
			count = 6;
		}

		if (!IsReadableRuntimeMemory(handles, sizeof(CHandle) * static_cast<size_t>(count)))
			return false;

		out.assign(handles, handles + count);
		return true;
	}

	auto FindDamageEntry(const std::string& name) -> const AbilityDamageEntry*
	{
		auto* data = GetAbilityDamageData();
		if (!data)
			return nullptr;
		if (const auto* entry = data->Find(name))
			return entry;
		const std::string lowered = ToLower(name);
		if (lowered != name)
			return data->Find(lowered);
		return nullptr;
	}

	auto PreferredSlotForAbility(const std::string& name) -> int
	{
		auto* data = GetAbilityDamageData();
		if (!data)
			return -1;
		const int direct = data->PreferredSlot(name);
		if (direct >= 0)
			return direct;
		const std::string lowered = ToLower(name);
		if (lowered != name)
			return data->PreferredSlot(lowered);
		return -1;
	}

	auto ReadCastRange(C_BaseEntity* ability, const KillStealerOffsets& offsets, float fallback) -> float
	{
		float castRange = fallback;
		if (offsets.abilityCastRange)
		{
			const int rangeInt = ReadField<int>(ability, offsets.abilityCastRange, 0);
			if (rangeInt >= 50 && rangeInt <= 25000)
				castRange = static_cast<float>(rangeInt);
			else
			{
				const float rangeFloat = ReadField<float>(ability, offsets.abilityCastRange, 0.f);
				if (std::isfinite(rangeFloat) && rangeFloat >= 50.f && rangeFloat <= 25000.f)
					castRange = rangeFloat;
			}
		}
		return castRange > 0.f ? castRange : 800.f;
	}

	auto CollectTools(CGameEntitySystem* entitySystem, const HeroSnapshot& localHero, const KillStealerOffsets& offsets) -> std::vector<KillTool>
	{
		std::vector<KillTool> tools;
		static constexpr std::array<WORD, 6> kAbilityKeys = {'Q', 'W', 'E', 'D', 'F', 'R'};
		static constexpr std::array<WORD, 6> kItemKeys = {'Z', 'X', 'C', 'V', 'B', 'N'};

		if (Settings::KillStealer::UseAbilities && localHero.entity)
		{
			std::vector<CHandle> abilityHandles;
			const auto* field = reinterpret_cast<const uint8_t*>(localHero.entity) + offsets.abilities;
			if (ReadHandleArray(field, 48, abilityHandles))
			{
				int fallbackSlot = 0;
				for (size_t slotIndex = 0; slotIndex < abilityHandles.size(); ++slotIndex)
				{
					const CHandle& handle = abilityHandles[slotIndex];
					if (!handle.IsValid())
						continue;
					auto* ability = entitySystem->GetBaseEntityFromHandle(handle);
					if (!ability)
						continue;

					const std::string abilityName = EntityName(ability);
					const auto* data = FindDamageEntry(abilityName);
					if (!data)
						continue;
					if (!data->IsUsableDamage() || (!data->unitTarget && !data->noTarget && !data->pointTarget))
						continue;

					const int level = ReadField<int>(ability, offsets.abilityLevel, 0);
					if (level <= 0)
						continue;
					if (offsets.hasAbilityActivated && !ReadField<bool>(ability, offsets.abilityActivated, true))
						continue;

					const float cooldown = ReadField<float>(ability, offsets.abilityCooldown, 0.f);
					if (std::isfinite(cooldown) && cooldown > 0.15f)
						continue;

					int manaCost = ReadField<int>(ability, offsets.abilityManaCost, 0);
					if (manaCost <= 0)
						manaCost = data->ManaForLevel(level);
					if (manaCost > static_cast<int>(localHero.mana + 0.5f))
						continue;

					const float rawDamage = data->DamageForLevel(level);
					if (rawDamage <= 0.f)
						continue;

					int preferredSlot = PreferredSlotForAbility(abilityName);
					if (preferredSlot < 0 || preferredSlot >= static_cast<int>(kAbilityKeys.size()))
						preferredSlot = fallbackSlot;
					++fallbackSlot;
					if (preferredSlot < 0 || preferredSlot >= static_cast<int>(kAbilityKeys.size()))
						continue;

					KillTool tool{};
					tool.kind = ToolKind::Ability;
					tool.name = abilityName;
					tool.castRange = data->noTarget ? 25000.f : ReadCastRange(ability, offsets, data->castRange);
					tool.rawDamage = rawDamage;
					tool.damageType = data->damageType;
					tool.key = kAbilityKeys[preferredSlot];
					tool.noTarget = data->noTarget;
					tool.unitTarget = data->unitTarget;
					tool.pointTarget = data->pointTarget;
					tool.delayMs = data->noTarget ? 90u : (Settings::KillStealer::QuickCast ? 120u : 180u);
					tool.manaCost = manaCost;
					tools.push_back(tool);
				}
			}
		}

		if (Settings::KillStealer::UseItems && localHero.entity)
		{
			std::vector<CHandle> itemHandles;
			if (ReadInventoryHandles(localHero.entity, offsets, itemHandles))
			{
				const int slotLimit = (std::min)(static_cast<int>(itemHandles.size()), static_cast<int>(kItemKeys.size()));
				for (int slot = 0; slot < slotLimit; ++slot)
				{
					if (!itemHandles[slot].IsValid())
						continue;
					auto* item = entitySystem->GetBaseEntityFromHandle(itemHandles[slot]);
					if (!item)
						continue;

					const std::string itemName = EntityName(item);
					const auto* data = FindDamageEntry(itemName);
					if (!data || (!data->unitTarget && !data->noTarget && !data->pointTarget))
						continue;

					const bool isEtherealBlade = Settings::KillStealer::PrioritizeEtherealBlade && itemName == "item_ethereal_blade";
					if (!isEtherealBlade && !data->IsUsableDamage())
						continue;

					const int level = (std::max)(1, ReadField<int>(item, offsets.abilityLevel, 1));
					const float cooldown = ReadField<float>(item, offsets.abilityCooldown, 0.f);
					if (std::isfinite(cooldown) && cooldown > 0.15f)
						continue;

					int manaCost = ReadField<int>(item, offsets.abilityManaCost, 0);
					if (manaCost <= 0)
						manaCost = data->ManaForLevel(level);
					if (manaCost > static_cast<int>(localHero.mana + 0.5f))
						continue;

					const float rawDamage = isEtherealBlade ? 0.f : data->DamageForLevel(level);
					if (!isEtherealBlade && rawDamage <= 0.f)
						continue;

					KillTool tool{};
					tool.kind = ToolKind::Item;
					tool.name = itemName;
					tool.castRange = data->noTarget ? 25000.f : ReadCastRange(item, offsets, data->castRange);
					tool.rawDamage = rawDamage;
					tool.damageType = data->damageType;
					tool.key = kItemKeys[slot];
					tool.noTarget = data->noTarget;
					tool.unitTarget = data->unitTarget;
					tool.pointTarget = data->pointTarget;
					tool.isDamageAmplifier = isEtherealBlade;
					tool.delayMs = data->noTarget ? 90u : (Settings::KillStealer::QuickCast ? 120u : 180u);
					tool.manaCost = manaCost;
					tools.push_back(tool);
				}
			}
		}

		if (Settings::KillStealer::UseAutoAttack && localHero.attackDamage > 0.f && Settings::KillStealer::AttackKey > 0)
		{
			KillTool tool{};
			tool.kind = ToolKind::Attack;
			tool.name = "auto_attack";
			tool.castRange = localHero.attackRange + 75.f;
			tool.rawDamage = localHero.attackDamage;
			tool.damageType = AbilityDamageType::Physical;
			tool.key = static_cast<WORD>(Settings::KillStealer::AttackKey);
			tool.unitTarget = true;
			tool.delayMs = 220u;
			tools.push_back(tool);
		}

		return tools;
	}

	auto PhysicalDamageAfterArmor(float damage, float armor) -> float
	{
		const float reduction = 0.052f * armor / (0.9f + 0.048f * std::abs(armor));
		return damage * (1.f - std::clamp(reduction, -0.95f, 0.95f));
	}

	// Ethereal Blade doubles the magic damage its target takes for its
	// duration; `magicAmplified` is true when the chosen plan also includes it.
	auto EffectiveDamage(const KillTool& tool, const HeroSnapshot& localHero, const HeroSnapshot& target, bool magicAmplified) -> float
	{
		float damage = tool.rawDamage;
		if (tool.kind != ToolKind::Attack)
			damage *= 1.f + localHero.spellAmp;

		switch (tool.damageType)
		{
		case AbilityDamageType::Pure:
			return damage;
		case AbilityDamageType::Physical:
			return PhysicalDamageAfterArmor(damage, target.armor);
		case AbilityDamageType::Magical:
		default:
			return damage * (magicAmplified ? 2.f : 1.f) * (1.f - target.magicResistance);
		}
	}

	auto Distance2D(const Vector3& left, const Vector3& right) -> float
	{
		const float dx = left.m_x - right.m_x;
		const float dy = left.m_y - right.m_y;
		return std::sqrt(dx * dx + dy * dy);
	}

	// Greedy-by-largest-effective-damage: provably optimal for "fewest tools to
	// reach at least `threshold`" (a covering problem, not exact subset-sum) -
	// if any k-tool combo reaches the threshold, the top-k highest-damage combo
	// reaches it too. O(n log n), no combinatorial search, no n for which this
	// can blow up.

	// Single source of truth for "how much damage counts as lethal". A flat
	// HealthBuffer alone can't absorb the damage estimate's own margin of
	// error (magic resistance, spell amp), so a plan that only just clears
	// health+HealthBuffer can still fail to finish the target in game even
	// though it "should" have killed - requiring a percentage of headroom on
	// top is what makes a plan that reads as lethal actually be lethal.
	auto LethalThreshold(int targetHealth) -> float
	{
		const float health = static_cast<float>(targetHealth);
		return health * (1.f + Settings::KillStealer::SafetyMarginPercent * 0.01f) + Settings::KillStealer::HealthBuffer;
	}

	auto BuildKillPlan(const HeroSnapshot& localHero, const HeroSnapshot& target, const std::vector<KillTool>& tools,
		uint32_t now, CKillStealer::PlanState& outPlan, KillPlanEvaluation& outEvaluation) -> bool
	{
		outPlan = {};
		outEvaluation = {};

		const float threshold = LethalThreshold(target.health);
		const float distance = Distance2D(localHero.origin, target.origin);

		std::vector<size_t> usable;
		usable.reserve(tools.size());
		for (size_t index = 0; index < tools.size(); ++index)
		{
			const auto& tool = tools[index];
			if (!tool.noTarget && distance > tool.castRange + 75.f)
				continue;
			usable.push_back(index);
		}

		if (usable.empty())
			return false;

		size_t amplifierPos = static_cast<size_t>(-1);
		for (size_t pos = 0; pos < usable.size(); ++pos)
		{
			if (tools[usable[pos]].isDamageAmplifier)
			{
				amplifierPos = pos;
				break;
			}
		}

		auto buildGreedy = [&](bool includeAmplifier) -> std::vector<size_t>
		{
			std::vector<size_t> order;
			order.reserve(usable.size());
			for (size_t pos = 0; pos < usable.size(); ++pos)
			{
				if (includeAmplifier && pos == amplifierPos)
					continue;
				order.push_back(pos);
			}

			std::sort(order.begin(), order.end(), [&](size_t a, size_t b)
			{
				const bool ampA = includeAmplifier && tools[usable[a]].damageType == AbilityDamageType::Magical;
				const bool ampB = includeAmplifier && tools[usable[b]].damageType == AbilityDamageType::Magical;
				return EffectiveDamage(tools[usable[a]], localHero, target, ampA) >
					   EffectiveDamage(tools[usable[b]], localHero, target, ampB);
			});

			std::vector<size_t> chosen;
			float total = 0.f;
			// Tracks mana as it would actually deplete if these actions fired
			// in sequence. CollectTools already confirmed each tool is
			// individually affordable against the hero's current mana, but
			// that check has no idea what else this same plan is about to
			// spend first - two spells that are each affordable alone can
			// easily add up to more than the hero actually has. Without this,
			// a plan gets built assuming mana it won't have by the second
			// action, the second action's own live re-check (see
			// IsRemainingPlanStillLethal) then correctly finds it unaffordable
			// and cancels, and the target is left alive at whatever HP the
			// first action alone brought them to.
			float manaRemaining = localHero.mana;
			if (includeAmplifier)
			{
				chosen.push_back(usable[amplifierPos]);
				manaRemaining -= static_cast<float>(tools[usable[amplifierPos]].manaCost);
			}

			for (size_t pos : order)
			{
				if (total >= threshold)
					break;
				const auto& tool = tools[usable[pos]];
				if (static_cast<float>(tool.manaCost) > manaRemaining + 0.5f)
					continue;
				manaRemaining -= static_cast<float>(tool.manaCost);
				const bool amp = includeAmplifier && tool.damageType == AbilityDamageType::Magical;
				total += EffectiveDamage(tool, localHero, target, amp);
				chosen.push_back(usable[pos]);
			}
			return total >= threshold ? chosen : std::vector<size_t>{};
		};

		auto evaluate = [&](const std::vector<size_t>& chosen, bool amplifierActive) -> KillPlanEvaluation
		{
			KillPlanEvaluation eval{};
			eval.actionCount = chosen.size();
			for (size_t index : chosen)
			{
				const bool amp = amplifierActive && !tools[index].isDamageAmplifier && tools[index].damageType == AbilityDamageType::Magical;
				eval.totalDamage += EffectiveDamage(tools[index], localHero, target, amp);
				eval.totalDelay += static_cast<float>(tools[index].delayMs);
			}
			return eval;
		};

		std::vector<size_t> withoutAmp = buildGreedy(false);
		std::vector<size_t> withAmp = amplifierPos != static_cast<size_t>(-1) ? buildGreedy(true) : std::vector<size_t>{};
		if (withoutAmp.empty() && withAmp.empty())
			return false;

		std::vector<size_t> selected;
		bool selectedHasAmp = false;
		KillPlanEvaluation selectedEval{};

		if (!withoutAmp.empty() && !withAmp.empty())
		{
			const auto evalWithout = evaluate(withoutAmp, false);
			const auto evalWith = evaluate(withAmp, true);
			const bool preferWith = evalWith.actionCount < evalWithout.actionCount ||
				(evalWith.actionCount == evalWithout.actionCount && evalWith.totalDamage < evalWithout.totalDamage - 0.01f);
			selected = preferWith ? withAmp : withoutAmp;
			selectedEval = preferWith ? evalWith : evalWithout;
			selectedHasAmp = preferWith;
		}
		else if (!withoutAmp.empty())
		{
			selected = withoutAmp;
			selectedEval = evaluate(withoutAmp, false);
		}
		else
		{
			selected = withAmp;
			selectedEval = evaluate(withAmp, true);
			selectedHasAmp = true;
		}

		if (selected.size() > kMaxPlanActions)
			selected.resize(kMaxPlanActions);

		std::sort(selected.begin(), selected.end(), [&](size_t leftIndex, size_t rightIndex)
		{
			if (tools[leftIndex].isDamageAmplifier != tools[rightIndex].isDamageAmplifier)
				return tools[leftIndex].isDamageAmplifier;

			const bool leftAmp = selectedHasAmp && !tools[leftIndex].isDamageAmplifier && tools[leftIndex].damageType == AbilityDamageType::Magical;
			const bool rightAmp = selectedHasAmp && !tools[rightIndex].isDamageAmplifier && tools[rightIndex].damageType == AbilityDamageType::Magical;
			const float leftDamage = EffectiveDamage(tools[leftIndex], localHero, target, leftAmp);
			const float rightDamage = EffectiveDamage(tools[rightIndex], localHero, target, rightAmp);
			if (std::abs(leftDamage - rightDamage) > 0.01f)
				return leftDamage > rightDamage;
			if (tools[leftIndex].delayMs != tools[rightIndex].delayMs)
				return tools[leftIndex].delayMs < tools[rightIndex].delayMs;
			return leftIndex < rightIndex;
		});

		outPlan.targetEntIndex = target.entIndex;
		outPlan.nextActionTick = now;
		outPlan.expiresAt = now + kPlanExpiryMs;
		outPlan.actionIndex = 0;
		outPlan.actions.reserve(selected.size());
		for (const size_t index : selected)
		{
			CKillStealer::PlanAction action{};
			action.kind = tools[index].kind == ToolKind::Item ? CKillStealer::PlanAction::Kind::Item :
				tools[index].kind == ToolKind::Attack ? CKillStealer::PlanAction::Kind::Attack : CKillStealer::PlanAction::Kind::Ability;
			action.name = tools[index].name;
			action.key = static_cast<uint16_t>(tools[index].key);
			action.delayMs = tools[index].delayMs;
			action.noTarget = tools[index].noTarget;
			action.unitTarget = tools[index].unitTarget;
			action.pointTarget = tools[index].pointTarget;
			action.isDamageAmplifier = tools[index].isDamageAmplifier;
			outPlan.actions.push_back(action);
		}

		outEvaluation = selectedEval;
		return !outPlan.actions.empty();
	}

	// Re-derives whether the actions from the plan's current position onward
	// still add up to lethal against the target's CURRENT health. Matches
	// each remaining PlanAction back to its live KillTool by name (tools is
	// recollected fresh every tick, so cooldown/mana/etc. are current) rather
	// than trusting the damage total computed once when the plan was first
	// built - see the call site in OnRenderInner for why that one-time check
	// isn't enough on its own.
	auto IsRemainingPlanStillLethal(const CKillStealer::PlanState& plan, const std::vector<KillTool>& tools,
		const HeroSnapshot& localHero, const HeroSnapshot& target) -> bool
	{
		const bool planUsesAmplifier = std::any_of(plan.actions.begin(), plan.actions.end(),
			[](const CKillStealer::PlanAction& action) { return action.isDamageAmplifier; });

		float remainingDamage = 0.f;
		for (size_t index = plan.actionIndex; index < plan.actions.size(); ++index)
		{
			const auto& planAction = plan.actions[index];
			const auto tool = std::find_if(tools.begin(), tools.end(),
				[&](const KillTool& candidate) { return candidate.name == planAction.name; });
			if (tool == tools.end())
				continue;
			const bool amp = planUsesAmplifier && !tool->isDamageAmplifier && tool->damageType == AbilityDamageType::Magical;
			remainingDamage += EffectiveDamage(*tool, localHero, target, amp);
		}

		return remainingDamage >= LethalThreshold(target.health);
	}

	auto WindowReadyForInput() -> HWND
	{
		auto* gui = GetAndromedaGUI();
		const HWND window = gui ? gui->m_hCS2Window : nullptr;
		if (!window || GetForegroundWindow() != window || gui->IsVisible())
			return nullptr;
		return window;
	}

	// SetCursorPos teleports the OS cursor directly - it does NOT go through
	// the same input pipeline as a real mouse move, so it never generates a
	// WM_INPUT / raw-input mouse event. Source 2 (like most modern engines)
	// reads raw input for precise cursor tracking rather than trusting
	// WM_MOUSEMOVE alone, so a SetCursorPos-only move can visibly relocate
	// the Windows arrow while the game's own idea of "where the player is
	// pointing" - the thing that actually decides where a targeted ability
	// lands - never updates. Routing the move through SendInput instead
	// (MOUSEEVENTF_MOVE|ABSOLUTE) puts it in the exact same synthetic-input
	// stream as the key press and click that follow it, so anything already
	// listening to that stream for the key/click sees the move too.
	auto SendMouseMoveAbsolute(int screenX, int screenY) -> bool
	{
		const int virtualX = GetSystemMetrics(SM_XVIRTUALSCREEN);
		const int virtualY = GetSystemMetrics(SM_YVIRTUALSCREEN);
		const int virtualW = GetSystemMetrics(SM_CXVIRTUALSCREEN);
		const int virtualH = GetSystemMetrics(SM_CYVIRTUALSCREEN);
		if (virtualW <= 1 || virtualH <= 1)
			return false;

		INPUT input{};
		input.type = INPUT_MOUSE;
		input.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
		input.mi.dx = static_cast<LONG>((static_cast<long long>(screenX - virtualX) * 65535LL) / (virtualW - 1));
		input.mi.dy = static_cast<LONG>((static_cast<long long>(screenY - virtualY) * 65535LL) / (virtualH - 1));
		return SendInput(1, &input, sizeof(INPUT)) == 1;
	}

	auto MoveCursorToClientPoint(HWND window, const ImVec2& screen, POINT& previousOut) -> bool
	{
		RECT client{};
		if (!GetClientRect(window, &client) || client.right <= 0 || client.bottom <= 0)
			return false;
		const int x = std::clamp(static_cast<int>(std::lround(screen.x)), 0, static_cast<int>(client.right) - 1);
		const int y = std::clamp(static_cast<int>(std::lround(screen.y)), 0, static_cast<int>(client.bottom) - 1);
		POINT target{x, y};
		if (!ClientToScreen(window, &target))
			return false;
		GetCursorPos(&previousOut);
		// Primary move: SendInput, so the game's input stream actually sees a
		// mouse-move event (see the comment on SendMouseMoveAbsolute). Fall
		// back to SetCursorPos only if that call itself couldn't be queued at
		// all (e.g. no valid virtual-screen metrics) - a visibly-wrong
		// position beats silently not moving at all.
		bool moved = SendMouseMoveAbsolute(target.x, target.y);
		if (!moved)
			moved = SetCursorPos(target.x, target.y) != FALSE;
		return moved;
	}

	auto SendKeyPress(WORD key) -> bool
	{
		INPUT inputs[2]{};
		inputs[0].type = INPUT_KEYBOARD;
		inputs[0].ki.wVk = key;
		inputs[1] = inputs[0];
		inputs[1].ki.dwFlags = KEYEVENTF_KEYUP;
		return SendInput(static_cast<UINT>(std::size(inputs)), inputs, sizeof(INPUT)) == std::size(inputs);
	}

	auto SendLeftClick() -> bool
	{
		INPUT inputs[2]{};
		inputs[0].type = INPUT_MOUSE;
		inputs[0].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
		inputs[1].type = INPUT_MOUSE;
		inputs[1].mi.dwFlags = MOUSEEVENTF_LEFTUP;
		return SendInput(static_cast<UINT>(std::size(inputs)), inputs, sizeof(INPUT)) == std::size(inputs);
	}

	// Without Dota's own Quickcast option enabled, casting a targeted ability
	// is a two-step interaction: the hotkey arms a targeting reticle, and only
	// a SEPARATE, later click confirms the target under it. Sending the key
	// and the click back-to-back in the same call - as this used to do -
	// raced that transition: the click could arrive before Source 2 had
	// finished switching into targeting mode, so it was consumed as a stray
	// click instead of a target confirmation, silently dropping the cast. A
	// human always has some reaction-time gap between the two; deferring the
	// click to a later, non-blocking tick reproduces that gap instead of
	// requiring the player to have Quickcast on (which skips this step
	// entirely) just to make automation reliable.
	bool g_HasPendingClick = false;
	ULONGLONG g_ClickAtTick = 0;

	auto ArmClick(ULONGLONG delayMs) -> void
	{
		g_HasPendingClick = true;
		g_ClickAtTick = GetTickCount64() + delayMs;
	}

	auto TickPendingClick() -> void
	{
		if (!g_HasPendingClick)
			return;
		if (GetTickCount64() < g_ClickAtTick)
			return;
		g_HasPendingClick = false;
		SendLeftClick();
	}

	auto ProjectTargetScreen(const Vector3& origin, ImVec2& out) -> bool
	{
		Vector3 lifted = origin;
		lifted.m_z += 80.f;
		if (Math::WorldToScreen(lifted, out))
			return true;
		return Math::WorldToScreen(origin, out);
	}

	auto CastPlanAction(const CKillStealer::PlanAction& action, const Vector3& targetOrigin) -> bool
	{
		const HWND window = WindowReadyForInput();
		if (!window || action.key == 0)
			return false;

		if (action.noTarget)
			return SendKeyPress(action.key);

		ImVec2 screen{};
		if (!ProjectTargetScreen(targetOrigin, screen))
			return false;

		POINT previous{};
		if (!MoveCursorToClientPoint(window, screen, previous))
			return false;

		const bool needsClick = action.kind == CKillStealer::PlanAction::Kind::Attack || !Settings::KillStealer::QuickCast;
		const bool keyOk = SendKeyPress(action.key);
		// Give Source 2 a beat to switch into targeting mode before the click
		// arrives - see the comment on ArmClick/TickPendingClick.
		if (needsClick)
			ArmClick(40);
		// Cursor is deliberately left on the enemy after the cast, not
		// restored to wherever it was before.
		return keyOk;
	}

	auto DrawMarkers(const HeroSnapshot& localHero, const std::vector<HeroSnapshot>& enemies,
		const std::vector<KillTool>& tools, uint32_t now) -> void
	{
		if (!Settings::KillStealer::DrawKillableMarkers || !ImGui::GetCurrentContext())
			return;

		auto* drawList = ImGui::GetForegroundDrawList();
		for (const auto& enemy : enemies)
		{
			CKillStealer::PlanState plan{};
			KillPlanEvaluation evaluation{};
			if (!BuildKillPlan(localHero, enemy, tools, now, plan, evaluation))
				continue;

			Vector3 marker = enemy.origin;
			marker.m_z += 115.f;
			ImVec2 screen{};
			if (!Math::WorldToScreen(marker, screen))
				continue;

			drawList->AddCircleFilled(screen, 13.f, IM_COL32(218, 51, 62, 215), 24);
			drawList->AddCircle(screen, 16.f, IM_COL32(255, 235, 210, 235), 28, 2.f);
			const char* label = plan.actions.size() == 1 && plan.actions.front().kind == CKillStealer::PlanAction::Kind::Attack ? "AA" : "KS";
			const ImVec2 size = ImGui::CalcTextSize(label);
			drawList->AddText(ImVec2(screen.x - size.x * 0.5f, screen.y - size.y * 0.5f), IM_COL32(255, 255, 255, 255), label);
		}
	}

}

auto CKillStealer::CancelPlan() -> void
{
	m_Plan = {};
}

auto CKillStealer::OnRenderInner() -> void
{
	const uint32_t now = GetTickCount();

	if (!Settings::KillStealer::Enable)
	{
		CancelPlan();
		m_NextThinkTick = now;
		return;
	}

	if (now < m_NextThinkTick)
		return;
	m_NextThinkTick = now + kThinkIntervalMs;

	auto& offsets = ResolveOffsets();
	auto* entitySystem = SDK::Interfaces::GameEntitySystem();
	if (!entitySystem || !offsets.resolved)
	{
		CancelPlan();
		return;
	}

	const auto heroes = ScanHeroes(entitySystem, offsets);

	HeroSnapshot localHero{};
	if (!ResolveLocalHero(entitySystem, offsets, heroes, localHero))
	{
		CancelPlan();
		return;
	}

	std::vector<HeroSnapshot> enemies;
	for (const auto& hero : heroes)
	{
		if (hero.entIndex == localHero.entIndex)
			continue;
		if (hero.team == localHero.team)
			continue;

		// Reject only the never-replicated (0,0,0) placeholder - see
		// HasTrustworthyPosition for why a real fog test is not available.
		if (!HasTrustworthyPosition(hero))
			continue;

		const float distance = Distance2D(localHero.origin, hero.origin);
		if (distance > Settings::KillStealer::DetectRange)
			continue;
		enemies.push_back(hero);
	}

	if (enemies.empty())
	{
		CancelPlan();
		return;
	}

	const auto tools = CollectTools(entitySystem, localHero, offsets);
	DrawMarkers(localHero, enemies, tools, now);

	// Advance an in-flight plan.
	if (m_Plan.active)
	{
		if (now >= m_Plan.expiresAt)
		{
			CancelPlan();
		}
		else
		{
			const HeroSnapshot* target = nullptr;
			for (const auto& enemy : enemies)
			{
				if (enemy.entIndex == m_Plan.targetEntIndex)
				{
					target = &enemy;
					break;
				}
			}

			if (!target || target->health <= 0)
			{
				DEV_LOG("[kill-stealer] CANCEL: %s (step %zu/%zu)\n",
					!target ? "target left enemy list" : "target already dead",
					m_Plan.actionIndex, m_Plan.actions.size());
				CancelPlan();
			}
			else if (m_Plan.actionIndex >= m_Plan.actions.size())
			{
				CancelPlan();
			}
			else if (now >= m_Plan.nextActionTick && !IsRemainingPlanStillLethal(m_Plan, tools, localHero, *target))
			{
				// The plan's total damage was only ever checked once, at the
				// tick it was built - a target that regenerated, got healed,
				// or (in one captured session) respawned back to full HP
				// while reusing the same entity slot, would otherwise absorb
				// the rest of the combo, ultimate included, for nothing: the
				// code only checked "does the target still exist", never "is
				// what's left of the plan still lethal".
				DEV_LOG("[kill-stealer] CANCEL: target no longer killable with what's left - hp=%d/%d mana=%.0f step=%zu/%zu next_action=%s\n",
					target->health, target->maxHealth, localHero.mana,
					m_Plan.actionIndex, m_Plan.actions.size(), m_Plan.actions[m_Plan.actionIndex].name.c_str());
				CancelPlan();
			}
			else if (now >= m_Plan.nextActionTick)
			{
				const auto& action = m_Plan.actions[m_Plan.actionIndex];
				const bool castOk = CastPlanAction(action, target->origin);
				DEV_LOG("[kill-stealer] CAST %s: %s key=%c step=%zu/%zu target_hp=%d\n",
					castOk ? "ok" : "FAILED", action.name.c_str(), static_cast<char>(action.key),
					m_Plan.actionIndex, m_Plan.actions.size(), target->health);
				if (!castOk)
				{
					CancelPlan();
				}
				else
				{
					++m_Plan.actionIndex;
					if (m_Plan.actionIndex >= m_Plan.actions.size())
						CancelPlan();
					else
						m_Plan.nextActionTick = now + action.delayMs;
				}
			}
		}
	}

	if (m_Plan.active)
		return;

	// Pick the best lethal target among enemies (fewest actions, then least overkill).
	PlanState bestPlan{};
	KillPlanEvaluation bestEvaluation{};
	const HeroSnapshot* bestTarget = nullptr;
	float bestOverkill = 3.4e38f;

	for (const auto& enemy : enemies)
	{
		PlanState candidatePlan{};
		KillPlanEvaluation candidateEvaluation{};
		const bool lethal = BuildKillPlan(localHero, enemy, tools, now, candidatePlan, candidateEvaluation);
		if (!lethal)
			continue;

		const float overkill = candidateEvaluation.totalDamage - LethalThreshold(enemy.health);
		const bool better = !bestTarget ||
			candidateEvaluation.actionCount < bestEvaluation.actionCount ||
			(candidateEvaluation.actionCount == bestEvaluation.actionCount && overkill < bestOverkill - 0.01f);

		if (better)
		{
			bestTarget = &enemy;
			bestPlan = candidatePlan;
			bestEvaluation = candidateEvaluation;
			bestOverkill = overkill;
		}
	}

	if (!bestTarget)
		return;

	m_Plan = bestPlan;
	m_Plan.active = true;
	m_Plan.nextActionTick = now;

	DEV_LOG("[kill-stealer] PLAN target=%s hp=%d/%d threshold=%.0f total_dmg=%.0f actions=%zu\n",
		bestTarget->name.c_str(), bestTarget->health, bestTarget->maxHealth,
		LethalThreshold(bestTarget->health), bestEvaluation.totalDamage, m_Plan.actions.size());
	for (const auto& action : m_Plan.actions)
	{
		const auto tool = std::find_if(tools.begin(), tools.end(),
			[&](const KillTool& candidate) { return candidate.name == action.name; });
		DEV_LOG("[kill-stealer]   step: %s key=%c mana_cost=%d\n",
			action.name.c_str(), static_cast<char>(action.key), tool != tools.end() ? tool->manaCost : -1);
	}
	DEV_LOG("[kill-stealer]   caster mana=%.0f\n", localHero.mana);
}

auto CKillStealer::OnRender() -> void
{
	// Must run every frame, or a cast's deferred confirm-click (see
	// ArmClick/TickPendingClick) could sit pending far longer than intended.
	TickPendingClick();

	OnRenderInner();
}
