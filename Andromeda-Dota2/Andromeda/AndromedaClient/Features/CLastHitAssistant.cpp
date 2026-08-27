#include "CLastHitAssistant.hpp"

#include <AndromedaClient/CAndromedaGUI.hpp>
#include <AndromedaClient/Settings/Settings.hpp>
#include <Common/DevLog.hpp>
#include <Common/MemoryEngine.hpp>
#include <Dota2/SDK/CSchemaOffset.hpp>
#include <Dota2/SDK/Interface/CGameEntitySystem.hpp>
#include <Dota2/SDK/Interface/CLocalHeroResolver.hpp>
#include <Dota2/SDK/Math/Math.hpp>
#include <Dota2/SDK/SDK.hpp>

#include <ImGui/imgui.h>
#include <Windows.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

namespace
{
	constexpr ULONGLONG kScanIntervalMs = 100;
	constexpr int kMaxScannedEntityIndex = MAX_TOTAL_ENTITIES - 1;
	constexpr size_t kMaxLoggedEnemyHp = 24;
	// Attack orders only go out for creeps the hero can hit roughly where it
	// stands. Using the 700-unit detect radius instead (which is what the
	// scan/overlay uses) turned every star into a "walk into the lane" order.
	constexpr float kAttackReachLeeway = 150.f;
	constexpr ULONGLONG kAutoAttackIntervalMs = 500;
	// 'A' arms Dota's attack cursor; the click that commits it has to arrive
	// on a later tick (see PendingAttack below).
	constexpr ULONGLONG kAttackClickDelayMs = 45;
	constexpr ULONGLONG kCursorRestoreDelayMs = 30;
	// Two creeps standing on top of each other project to nearly the same
	// screen point; a click meant for the low one then lands on the healthy
	// one in front. Below kMinClickSeparationPx the click is pushed toward the
	// target's far side, and if that still is not enough the attack is skipped.
	constexpr float kMinClickSeparationPx = 26.f;
	constexpr float kMaxClickNudgePx = 12.f;
	// Once a creep has been ordered on, stay on it: mid-wave the "lowest hp"
	// answer flips constantly, and re-targeting every 500 ms cancels the
	// wind-up before any attack lands.
	constexpr ULONGLONG kTargetLockMs = 1200;

	struct LastHitOffsets
	{
		uint32_t health = 0;
		uint32_t maxHealth = 0;
		uint32_t team = 0;
		uint32_t sceneNode = 0;
		uint32_t absOrigin = 0;
		uint32_t damageMin = 0;
		uint32_t damageMax = 0;
		uint32_t damageBonus = 0;
		uint32_t attackRange = 0;
		uint32_t playerOwnerId = 0;
		uint32_t heroPlayerId = 0;
		uint32_t assignedHero = 0;
		uint32_t controllerPlayerId = 0;
		uint32_t isLocalController = 0;
		uint32_t isIllusion = 0;
		uint32_t isClone = 0;
		bool resolved = false;
		bool usable = false;
		bool combatUsable = false;
		bool hasDamageMax = false;
		bool hasPlayerOwnerId = false;
		bool hasHeroPlayerId = false;
		bool hasAssignedHero = false;
		bool hasControllerPlayerId = false;
		bool hasIsLocalController = false;
		bool hasIsIllusion = false;
		bool hasIsClone = false;
	};

	struct LocalHeroInfo
	{
		C_BaseEntity *entity = nullptr;
		Vector3 origin{};
		uint8_t team = 0;
		int attackDamage = 0;
		float attackRange = 0.f;
		float detectRange = 0.f;
		bool screenCenterFallback = false;
	};

	struct CreepCandidate
	{
		// Kept so the order can be re-validated against the live entity right
		// before it is issued - the candidate list itself is up to
		// kScanIntervalMs stale, which is long enough for a creep to die.
		int entIndex = -1;
		Vector3 origin{};
		int health = 0;
		int maxHealth = 0;
		float distance = 0.f;
		float lethalHealth = 0.f;
		bool deny = false;
		bool killable = false;
		bool inAttackRange = false;
	};

	// Every lane creep near the hero, healthy ones included. The candidate
	// list only holds one-shot creeps, but it is the HEALTHY neighbour that
	// swallows a click meant for the creep behind it, so the click test needs
	// to see them too.
	struct CreepMarker
	{
		int entIndex = -1;
		Vector3 origin{};
	};

	auto NearbyCreepCache() -> std::vector<CreepMarker> &
	{
		static std::vector<CreepMarker> creeps;
		return creeps;
	}

	struct EnemyCreepSnapshot
	{
		int health = 0;
		int maxHealth = 0;
		float distance = 0.f;
		uint8_t team = 0;
		bool inRange = false;
		bool killable = false;
	};

	struct LastHitScanStats
	{
		int scanned = 0;
		int laneLike = 0;
		int alliedLane = 0;
		int enemyLane = 0;
		int enemyWithOrigin = 0;
		int enemyInRange = 0;
		int enemyKillable = 0;
		int inRange = 0;
		int lethal = 0;
		int candidates = 0;
		float nearestDistance = 99999.f;
		int nearestHealth = 0;
		int nearestMaxHealth = 0;
		uint8_t nearestTeam = 0;
	};

	auto ResolveOffsets() -> LastHitOffsets &
	{
		static LastHitOffsets offsets{};
		if (offsets.resolved)
			return offsets;

		static ULONGLONG lastAttemptTick = 0;
		const ULONGLONG now = GetTickCount64();
		if (lastAttemptTick && now - lastAttemptTick < 500)
			return offsets;
		lastAttemptTick = now;

		auto *schema = GetSchemaOffset();
		if (!schema)
			return offsets;

		const bool hasHealth = schema->TryGetOffset("C_BaseEntity", "m_iHealth", offsets.health);
		const bool hasMaxHealth = schema->TryGetOffset("C_BaseEntity", "m_iMaxHealth", offsets.maxHealth);
		const bool hasTeam = schema->TryGetOffset("C_BaseEntity", "m_iTeamNum", offsets.team);
		const bool hasSceneNode = schema->TryGetOffset("C_BaseEntity", "m_pGameSceneNode", offsets.sceneNode);
		const bool hasOrigin = schema->TryGetOffset("CGameSceneNode", "m_vecAbsOrigin", offsets.absOrigin);
		const bool hasDamageMin = schema->TryGetOffset("C_DOTA_BaseNPC", "m_iDamageMin", offsets.damageMin);
		offsets.hasDamageMax = schema->TryGetOffset("C_DOTA_BaseNPC", "m_iDamageMax", offsets.damageMax);
		const bool hasDamageBonus = schema->TryGetOffset("C_DOTA_BaseNPC", "m_iDamageBonus", offsets.damageBonus);
		const bool hasAttackRange = schema->TryGetOffset("C_DOTA_BaseNPC", "m_iAttackRange", offsets.attackRange);
		offsets.hasHeroPlayerId = schema->TryGetOffset("C_DOTA_BaseNPC_Hero", "m_iPlayerID", offsets.heroPlayerId) ||
								  schema->TryGetOffset("C_DOTA_BaseNPC", "m_iPlayerID", offsets.heroPlayerId);
		offsets.hasPlayerOwnerId = schema->TryGetOffset("C_DOTA_BaseNPC", "m_nPlayerOwnerID", offsets.playerOwnerId);
		offsets.hasAssignedHero = schema->TryGetOffset("C_DOTAPlayerController", "m_hAssignedHero", offsets.assignedHero);
		offsets.hasControllerPlayerId = schema->TryGetOffset("C_DOTAPlayerController", "m_nPlayerID", offsets.controllerPlayerId) ||
										schema->TryGetOffset("C_DOTAPlayerController", "m_iPlayerID", offsets.controllerPlayerId);
		offsets.hasIsLocalController =
			schema->TryGetOffset("CBasePlayerController", "m_bIsLocalPlayerController", offsets.isLocalController) ||
			schema->TryGetOffset("C_BasePlayerController", "m_bIsLocalPlayerController", offsets.isLocalController) ||
			schema->TryGetOffset("C_DOTAPlayerController", "m_bIsLocalPlayerController", offsets.isLocalController);
		offsets.hasIsIllusion = schema->TryGetOffset("C_DOTA_BaseNPC", "m_bIsIllusion", offsets.isIllusion);
		offsets.hasIsClone = schema->TryGetOffset("C_DOTA_BaseNPC", "m_bIsClone", offsets.isClone);

		offsets.usable = hasHealth && hasMaxHealth && hasTeam && hasSceneNode && hasOrigin && hasAttackRange;
		offsets.combatUsable = offsets.usable && hasDamageMin;
		offsets.resolved = offsets.combatUsable;

		static ULONGLONG lastLogTick = 0;
		if (!lastLogTick || now - lastLogTick >= 5000 || offsets.resolved)
		{
			DEV_LOG("[last-hit] offsets visual=%d combat=%d health=0x%X max=0x%X team=0x%X scene=0x%X origin=0x%X damage=0x%X-0x%X+0x%X range=0x%X owner=%d/0x%X player=%d/0x%X assigned=%d/0x%X ctrlPlayer=%d/0x%X localCtrl=%d/0x%X illusion=%d/0x%X clone=%d/0x%X\n",
					offsets.usable ? 1 : 0, offsets.combatUsable ? 1 : 0, offsets.health, offsets.maxHealth,
					offsets.team, offsets.sceneNode, offsets.absOrigin, offsets.damageMin, offsets.damageMax,
					offsets.damageBonus, offsets.attackRange, offsets.hasPlayerOwnerId ? 1 : 0, offsets.playerOwnerId,
					offsets.hasHeroPlayerId ? 1 : 0, offsets.heroPlayerId,
					offsets.hasAssignedHero ? 1 : 0, offsets.assignedHero,
					offsets.hasControllerPlayerId ? 1 : 0, offsets.controllerPlayerId,
					offsets.hasIsLocalController ? 1 : 0, offsets.isLocalController,
					offsets.hasIsIllusion ? 1 : 0, offsets.isIllusion,
					offsets.hasIsClone ? 1 : 0, offsets.isClone);
			lastLogTick = now;
		}

		return offsets;
	}

	template <typename T>
	auto ReadField(const C_BaseEntity *entity, uint32_t offset, T fallback = T{}) -> T
	{
		if (!entity || !offset)
			return fallback;
		return *reinterpret_cast<const T *>(reinterpret_cast<uintptr_t>(entity) + offset);
	}

	auto ReadOrigin(const C_BaseEntity *entity, const LastHitOffsets &offsets, Vector3 &out) -> bool
	{
		if (!entity || !offsets.sceneNode || !offsets.absOrigin)
			return false;

		auto *sceneNode = *reinterpret_cast<void *const *>(reinterpret_cast<uintptr_t>(entity) + offsets.sceneNode);
		if (!sceneNode)
			return false;

		out = *reinterpret_cast<const Vector3 *>(reinterpret_cast<uintptr_t>(sceneNode) + offsets.absOrigin);
		return std::isfinite(out.m_x) && std::isfinite(out.m_y) && std::isfinite(out.m_z);
	}

	auto IsLikelyPlayerId(int value) -> bool
	{
		return value >= 0 && value < 24;
	}

	auto IsPlayableTeam(uint8_t team) -> bool
	{
		return team == 2 || team == 3;
	}

	auto TryLocalPlayerId(int &outPlayerId) -> bool
	{
		outPlayerId = -1;
		auto *engine = SDK::Interfaces::EngineToClient();
		if (!engine)
			return false;

		engine->GetLocalPlayer(outPlayerId, 0);
		return IsLikelyPlayerId(outPlayerId);
	}

	auto UnitPlayerId(const C_BaseEntity *entity, const LastHitOffsets &offsets) -> int
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

	auto TextContains(const char *text, const char *needle) -> bool
	{
		return text && needle && std::strstr(text, needle) != nullptr;
	}

	auto LooksLikeHeroEntity(C_BaseEntity *entity) -> bool
	{
		if (!entity)
			return false;

		if (auto *identity = entity->pEntityIdentity())
		{
			const char *name = identity->DesingerName().String();
			if (!name || !name[0])
				name = identity->Name().String();
			if (TextContains(name, "npc_dota_hero_"))
				return true;
		}

		const char *className = entity->GetSchemaClassName();
		return TextContains(className, "DOTA_BaseNPC_Hero") ||
			   TextContains(className, "C_DOTA_BaseNPC_Hero") ||
			   TextContains(className, "DOTA_Unit_Hero");
	}

	auto EntityDesignerOrName(C_BaseEntity *entity) -> const char *
	{
		if (!entity)
			return "";

		if (auto *identity = entity->pEntityIdentity())
		{
			const char *name = identity->DesingerName().String();
			if (!name || !name[0])
				name = identity->Name().String();
			if (name)
				return name;
		}
		return "";
	}

	auto LooksLikePlayerControllerEntity(C_BaseEntity *entity) -> bool
	{
		if (!entity)
			return false;
		const char *className = entity->GetSchemaClassName();
		return TextContains(className, "DOTAPlayerController");
	}

	auto ContainsNoCase(const char *text, const char *needle) -> bool
	{
		if (!text || !needle || !needle[0])
			return false;

		for (const char *scan = text; *scan; ++scan)
		{
			const char *left = scan;
			const char *right = needle;
			while (*left && *right &&
				   std::tolower(static_cast<unsigned char>(*left)) ==
					   std::tolower(static_cast<unsigned char>(*right)))
			{
				++left;
				++right;
			}
			if (!*right)
				return true;
		}
		return false;
	}

	// Identity is matched case-insensitively against the entity's designer /
	// entity name, with the schema class name only as a bonus: on this build
	// GetSchemaClassName() comes back empty for every entity (the feature's
	// own reject log recorded "class":"" for all 5407 samples), while the
	// designer name resolves fine ("npc_dota_roshan",
	// "npc_dota_unit_warlock_imp", ...). The old filter leaned on the class
	// name containing "BaseNPC", which therefore never matched anything, and
	// on case-sensitive needles that missed half their spellings.
	auto EntityNameContains(C_BaseEntity *entity, const char *needle) -> bool
	{
		if (!entity)
			return false;
		return ContainsNoCase(EntityDesignerOrName(entity), needle) ||
			   ContainsNoCase(entity->GetSchemaClassName(), needle);
	}

	// Everything the overlay must never mark: jungle camps and Roshan, every
	// building, wards, couriers, heroes, and hero-summoned units. Summons all
	// spawn under the "npc_dota_unit_" prefix (the reject log shows
	// "npc_dota_unit_warlock_imp"), so that one needle covers imps, wolves,
	// spiderlings and friends without listing every hero's unit by hand.
	auto LooksLikeNonCreepTarget(C_BaseEntity *entity) -> bool
	{
		// Needles are matched against the whole name, so each one must be a
		// string that cannot occur inside a lane creep's name
		// ("npc_dota_creep_goodguys_melee", "npc_dota_badguys_siege", ...).
		static constexpr const char *kDenyNeedles[] = {
			"neutral", "roshan", "ancient",
			"tower", "fort", "barracks", "rax", "building", "filler",
			"healer", "shrine", "outpost", "watch", "effigy",
			"ward", "courier", "hero", "thinker", "additive", "player",
			// Summoned units. Most carry the "npc_dota_unit_" prefix, but the
			// manaless ones that do not (spirit bear, wolves, treants) would
			// otherwise sail through the stats envelope in IsLaneCreep.
			"npc_dota_unit_", "bear", "wolf", "treant", "spiderling",
			"eidolon", "familiar", "zombie", "golem", "boar", "hawk",
			"necronomicon", "illusion", "clone", "spirit"};

		for (const char *needle : kDenyNeedles)
		{
			if (EntityNameContains(entity, needle))
				return true;
		}
		return false;
	}

	// Lane creeps only - melee, ranged and siege, on either playing team.
	// Dota names them "npc_dota_creep_lane" / "npc_dota_creep_siege" with unit
	// names like "npc_dota_creep_goodguys_melee" and "npc_dota_badguys_siege",
	// so a creep-prefixed or siege-flavoured name that survived the deny list
	// above is a lane creep.
	auto LooksLikeLaneCreepByName(C_BaseEntity *entity) -> bool
	{
		if (LooksLikeNonCreepTarget(entity))
			return false;

		return EntityNameContains(entity, "npc_dota_creep") ||
			   EntityNameContains(entity, "creep_lane") ||
			   EntityNameContains(entity, "creep_siege") ||
			   EntityNameContains(entity, "creep_melee") ||
			   EntityNameContains(entity, "creep_ranged") ||
			   EntityNameContains(entity, "basenpc_creep") ||
			   EntityNameContains(entity, "flagbearer") ||
			   EntityNameContains(entity, "siege") ||
			   EntityNameContains(entity, "goodguys_melee") ||
			   EntityNameContains(entity, "goodguys_ranged") ||
			   EntityNameContains(entity, "badguys_melee") ||
			   EntityNameContains(entity, "badguys_ranged");
	}

	auto DetectRangeForHero(float) -> float
	{
		return 1200.f;
	}

	auto LastHitAssistantActive() -> bool
	{
		return Settings::LastHitAssistant::Enable;
	}

	auto TryBuildLocalHero(C_BaseEntity *hero, const LastHitOffsets &offsets,
						   LocalHeroInfo &out, bool requireHeroIdentity = false) -> bool
	{
		out = {};
		if (!hero)
			return false;
		if (requireHeroIdentity && !LooksLikeHeroEntity(hero))
			return false;

		const int health = ReadField<int>(hero, offsets.health);
		const int maxHealth = ReadField<int>(hero, offsets.maxHealth);
		if (health <= 0 || maxHealth < 300 || maxHealth > 50000)
			return false;
		if (offsets.hasIsIllusion && ReadField<bool>(hero, offsets.isIllusion))
			return false;
		if (offsets.hasIsClone && ReadField<bool>(hero, offsets.isClone))
			return false;

		Vector3 origin{};
		if (!ReadOrigin(hero, offsets, origin))
			return false;

		const uint8_t team = ReadField<uint8_t>(hero, offsets.team);
		if (!IsPlayableTeam(team))
			return false;

		const int rawAttackRange = ReadField<int>(hero, offsets.attackRange, 0);
		if (rawAttackRange < 100 || rawAttackRange > 2500)
			return false;

		const int rawDamageMin = offsets.combatUsable ? ReadField<int>(hero, offsets.damageMin) : 0;
		const int rawDamageMax = offsets.hasDamageMax ? ReadField<int>(hero, offsets.damageMax, rawDamageMin) : rawDamageMin;
		const int rawDamageBonus = ReadField<int>(hero, offsets.damageBonus);
		const int attackDamage = (std::max)(1, (std::max)(rawDamageMin, rawDamageMax) + rawDamageBonus);
		if (attackDamage < 1 || attackDamage > 1000)
			return false;

		out.entity = hero;
		out.origin = origin;
		out.team = team;
		out.attackDamage = attackDamage;
		out.attackRange = static_cast<float>(rawAttackRange);
		out.detectRange = DetectRangeForHero(out.attackRange);
		return true;
	}

	auto TryResolveHeroFromIndex(CGameEntitySystem *entitySystem, const LastHitOffsets &offsets,
								 int index, LocalHeroInfo &out, bool requireHeroIdentity = false) -> bool
	{
		if (!entitySystem || index < 0 || index > kMaxScannedEntityIndex)
			return false;

		return TryBuildLocalHero(entitySystem->GetBaseEntity<C_BaseEntity>(index), offsets,
								 out, requireHeroIdentity);
	}

	auto TryResolveLocalHeroFromControllerScan(CGameEntitySystem *entitySystem, const LastHitOffsets &offsets,
											   int localPlayerId, LocalHeroInfo &out, int &cachedHeroIndex) -> bool
	{
		out = {};
		if (!entitySystem || !offsets.hasAssignedHero)
			return false;

		const bool canReadLocalFlag = offsets.hasIsLocalController &&
									  offsets.isLocalController > 0 && offsets.isLocalController < 0x1000;
		const bool canReadControllerPlayer = offsets.hasControllerPlayerId && offsets.controllerPlayerId > 0;

		int controllerCount = 0;
		int assignedCount = 0;
		int localFlagCount = 0;
		int playerMatchCount = 0;
		int bestScore = 0;
		int bestIndex = -1;
		LocalHeroInfo bestHero{};

		for (int chunkIndex = 0; chunkIndex < MAX_ENTITY_LISTS; ++chunkIndex)
		{
			auto *chunk = entitySystem->m_pIdentityChunks[chunkIndex];
			if (!chunk)
				continue;

			for (int entryIndex = 0; entryIndex < MAX_ENTITIES_IN_LIST; ++entryIndex)
			{
				auto *entity = chunk->m_pIdentities[entryIndex].pBaseEntity();
				if (!LooksLikePlayerControllerEntity(entity))
					continue;
				++controllerCount;

				CHandle invalidHero{INVALID_EHANDLE_INDEX};
				const CHandle heroHandle = ReadField<CHandle>(entity, offsets.assignedHero, invalidHero);
				if (!heroHandle.IsValid())
					continue;
				++assignedCount;

				LocalHeroInfo hero{};
				if (!TryBuildLocalHero(entitySystem->GetBaseEntityFromHandle(heroHandle), offsets, hero, true))
					continue;

				int score = 0;
				if (canReadLocalFlag && ReadField<bool>(entity, offsets.isLocalController, false))
				{
					score += 1000;
					++localFlagCount;
				}

				const int controllerPlayerId = canReadControllerPlayer ? ReadField<int>(entity, offsets.controllerPlayerId, -1) : -1;
				// Engine GetLocalPlayer() is often 0 even on Dire, so only use
				// controller player-id matching when it is not the ambiguous zero slot.
				if (localPlayerId > 0 && controllerPlayerId == localPlayerId)
				{
					score += 650;
					++playerMatchCount;
				}

				if (score > bestScore)
				{
					bestScore = score;
					bestIndex = static_cast<int>(static_cast<uint16_t>(heroHandle.GetEntryIndex()) & ENT_ENTRY_MASK);
					bestHero = hero;
				}
			}
		}

		static ULONGLONG lastLogTick = 0;
		const ULONGLONG now = GetTickCount64();
		if (!lastLogTick || now - lastLogTick >= 3000)
		{
			DEV_LOG("[last-hit] controller-scan controllers=%d assigned=%d localFlags=%d playerMatches=%d localPlayer=%d bestScore=%d bestIndex=%d bestTeam=%u\n",
					controllerCount, assignedCount, localFlagCount, playerMatchCount,
					localPlayerId, bestScore, bestIndex, static_cast<unsigned int>(bestHero.team));
			lastLogTick = now;
		}

		if (bestScore <= 0 || !bestHero.entity)
			return false;

		cachedHeroIndex = bestIndex;
		out = bestHero;
		return true;
	}

	auto TryScreenCenterDistanceSq(const Vector3 &origin, float &outDistanceSq) -> bool
	{
		Vector3 lifted = origin;
		lifted.m_z += 64.f;
		ImVec2 screen{};
		if (!Math::WorldToScreen(lifted, screen) && !Math::WorldToScreen(origin, screen))
			return false;

		ImVec2 display{};
		if (ImGui::GetCurrentContext())
			display = ImGui::GetIO().DisplaySize;
		if (display.x <= 0.f || display.y <= 0.f)
		{
			auto *gui = GetAndromedaGUI();
			const HWND window = gui ? gui->m_hCS2Window : nullptr;
			RECT client{};
			if (window && GetClientRect(window, &client))
				display = ImVec2(static_cast<float>(client.right), static_cast<float>(client.bottom));
		}
		if (display.x <= 0.f || display.y <= 0.f)
			return false;

		const float dx = screen.x - display.x * 0.5f;
		const float dy = screen.y - display.y * 0.5f;
		outDistanceSq = dx * dx + dy * dy;
		return std::isfinite(outDistanceSq);
	}

	auto ResolveHeroNearScreenCenter(CGameEntitySystem *entitySystem, const LastHitOffsets &offsets,
									 LocalHeroInfo &out, int &cachedHeroIndex, int localPlayerId = -1, uint8_t preferredTeam = 0) -> bool
	{
		out = {};
		if (!entitySystem)
			return false;

		float bestDistanceSq = 3.4e38f;
		int bestIndex = -1;
		LocalHeroInfo bestHero{};
		int heroLikeCount = 0;
		int builtHeroCount = 0;
		int projectedHeroCount = 0;
		int allocatedChunks = 0;
		for (int chunkIndex = 0; chunkIndex < MAX_ENTITY_LISTS; ++chunkIndex)
		{
			auto *chunk = entitySystem->m_pIdentityChunks[chunkIndex];
			if (!chunk)
				continue;
			++allocatedChunks;

			for (int entryIndex = 0; entryIndex < MAX_ENTITIES_IN_LIST; ++entryIndex)
			{
				auto *identity = &chunk->m_pIdentities[entryIndex];
				auto *entity = identity->pBaseEntity();
				if (!entity || !LooksLikeHeroEntity(entity))
					continue;
				++heroLikeCount;

				LocalHeroInfo hero{};
				if (!TryBuildLocalHero(entity, offsets, hero, false))
					continue;
				if (IsPlayableTeam(preferredTeam) && hero.team != preferredTeam)
					continue;
				++builtHeroCount;

				float distanceSq = 0.f;
				if (!TryScreenCenterDistanceSq(hero.origin, distanceSq))
					continue;
				++projectedHeroCount;

				if (localPlayerId >= 0 && UnitPlayerId(entity, offsets) == localPlayerId)
					distanceSq *= 0.15f;
				if (distanceSq < bestDistanceSq)
				{
					bestDistanceSq = distanceSq;
					bestIndex = chunkIndex * MAX_ENTITIES_IN_LIST + entryIndex;
					bestHero = hero;
				}
			}
		}

		static ULONGLONG lastLogTick = 0;
		const ULONGLONG now = GetTickCount64();
		if (!lastLogTick || now - lastLogTick >= 3000)
		{
			DEV_LOG("[last-hit] center-hero-fallback chunks=%d heroes=%d built=%d projected=%d best=%d dist=%.0f local=%d preferredTeam=%u team=%u damage=%d origin=(%.0f,%.0f,%.0f)\n",
					allocatedChunks, heroLikeCount, builtHeroCount, projectedHeroCount,
					bestIndex, bestDistanceSq, bestHero.entity ? 1 : 0,
					static_cast<unsigned int>(preferredTeam),
					static_cast<unsigned int>(bestHero.team), bestHero.attackDamage,
					bestHero.origin.m_x, bestHero.origin.m_y, bestHero.origin.m_z);
			lastLogTick = now;
		}

		if (!bestHero.entity)
			return false;
		cachedHeroIndex = bestIndex;
		bestHero.screenCenterFallback = true;
		out = bestHero;
		return true;
	}

	auto ResolveLocalHero(CGameEntitySystem *entitySystem, const LastHitOffsets &offsets,
						  LocalHeroInfo &out) -> bool
	{
		out = {};
		if (!entitySystem || !offsets.usable)
			return false;

		static int cachedHeroIndex = -1;
		static bool cachedHeroFromScreenCenter = false;
		static uint8_t cachedLocalTeam = 0;
		int localPlayerId = -1;
		const bool hasLocalPlayerId = TryLocalPlayerId(localPlayerId);
		const uint8_t preferredTeam = 0; // no team filter until a hero is actually found

		// Shared resolver first (Dota2/SDK/Interface/CLocalHeroResolver.hpp).
		// It carries the player-slot match that CKillStealer and CAutoCombo
		// already proved on this build, so the screen-center guess further
		// down - which can lock onto an ENEMY hero standing near the middle of
		// the screen, and with it flip every ally/enemy creep colour and the
		// attack decision - is now genuinely a last resort.
		{
			C_BaseEntity *resolved = nullptr;
			int resolvedIndex = -1;
			if (CLocalHeroResolver::Resolve(entitySystem, resolved, resolvedIndex) &&
				TryBuildLocalHero(resolved, offsets, out))
			{
				cachedHeroIndex = resolvedIndex;
				cachedHeroFromScreenCenter = false;
				cachedLocalTeam = out.team;
				out.screenCenterFallback = false;
				return true;
			}
		}

		// Try GetLocalPlayerController() first - this is the most reliable Valve engine path
		auto *controller = CGameEntitySystem::GetLocalPlayerController();
		if (controller)
		{
			const CHandle heroHandle = controller->m_hAssignedHero();
			if (heroHandle.IsValid())
			{
				if (TryBuildLocalHero(entitySystem->GetBaseEntityFromHandle(heroHandle), offsets, out))
				{
					cachedHeroIndex = static_cast<int>(static_cast<uint16_t>(heroHandle.GetEntryIndex()) & ENT_ENTRY_MASK);
					cachedHeroFromScreenCenter = false;
					cachedLocalTeam = out.team;
					out.screenCenterFallback = false;
					return true;
				}
			}
		}

		// If GetLocalPlayerController() failed, try resolving from cached index
		if (TryResolveHeroFromIndex(entitySystem, offsets, cachedHeroIndex, out, true))
		{
			if (IsPlayableTeam(preferredTeam) && out.team != preferredTeam)
			{
				cachedHeroIndex = -1;
				cachedHeroFromScreenCenter = false;
			}
			else
			{
				if (!cachedHeroFromScreenCenter && IsPlayableTeam(out.team))
					cachedLocalTeam = out.team;
				out.screenCenterFallback = cachedHeroFromScreenCenter;
				return true;
			}
		}
		else
		{
			cachedHeroIndex = -1;
			cachedHeroFromScreenCenter = false;
		}

		// Try controller scan as fallback
		if (TryResolveLocalHeroFromControllerScan(entitySystem, offsets,
												  hasLocalPlayerId ? localPlayerId : -1, out, cachedHeroIndex))
		{
			cachedHeroFromScreenCenter = false;
			cachedLocalTeam = out.team;
			return true;
		}

		// Try local player ID matching
		if (hasLocalPlayerId && localPlayerId > 0)
		{
			for (int chunkIndex = 0; chunkIndex < MAX_ENTITY_LISTS; ++chunkIndex)
			{
				auto *chunk = entitySystem->m_pIdentityChunks[chunkIndex];
				if (!chunk)
					continue;

				for (int entryIndex = 0; entryIndex < MAX_ENTITIES_IN_LIST; ++entryIndex)
				{
					auto *entity = chunk->m_pIdentities[entryIndex].pBaseEntity();
					if (!entity || UnitPlayerId(entity, offsets) != localPlayerId)
						continue;

					if (TryBuildLocalHero(entity, offsets, out, true))
					{
						if (!IsPlayableTeam(preferredTeam) || out.team == preferredTeam)
						{
							cachedHeroIndex = chunkIndex * MAX_ENTITIES_IN_LIST + entryIndex;
							cachedHeroFromScreenCenter = false;
							cachedLocalTeam = out.team;
							return true;
						}
					}
				}
			}
		}

		// Try screen center fallback
		if (ResolveHeroNearScreenCenter(entitySystem, offsets, out, cachedHeroIndex,
										hasLocalPlayerId ? localPlayerId : -1, preferredTeam))
		{
			// Accept the found hero; update cached team for future filtering
			cachedHeroFromScreenCenter = true;
			cachedLocalTeam = out.team;
			return true;
		}

		// If screen-center fallback didn't produce a hero, try a direct player-owner search
		if (!out.entity)
		{
			if (hasLocalPlayerId && localPlayerId >= 0)
			{
				auto *entitySystem = SDK::Interfaces::GameEntitySystem();
				if (entitySystem)
				{
					for (int i = 1; i <= entitySystem->GetHighestEntityIndex(); ++i)
					{
						auto *ent = entitySystem->GetBaseEntity<C_BaseEntity>(i);
						if (!ent)
							continue;
						if (UnitPlayerId(ent, offsets) == localPlayerId)
						{
							if (TryBuildLocalHero(ent, offsets, out, true))
							{
								cachedHeroIndex = i;
								cachedHeroFromScreenCenter = false;
								cachedLocalTeam = out.team;
								return true;
							}
						}
					}
				}
			}
		}

		static ULONGLONG lastResolveFailLogTick = 0;
		const ULONGLONG now = GetTickCount64();
		if (!lastResolveFailLogTick || now - lastResolveFailLogTick >= 3000)
		{
			DEV_LOG("[last-hit] local-hero-resolve-fail controller=%d localPlayer=%d preferredTeam=%u cachedTeam=%u highest=%d\n",
					controller ? 1 : 0, hasLocalPlayerId ? localPlayerId : -1,
					static_cast<unsigned int>(preferredTeam), static_cast<unsigned int>(cachedLocalTeam),
					entitySystem ? entitySystem->GetHighestEntityIndex() : -1);
			lastResolveFailLogTick = now;
		}
		return false;
	}

	// The one place that decides whether an entity may be starred/attacked.
	//
	// Ownership deliberately plays no part here: m_nPlayerOwnerID reads back
	// 0 - a perfectly valid player slot - for units nobody owns, so the old
	// "is this player-controlled?" test answered yes for Roshan and neutrals
	// alike (reject log: every sample, including team-4 Roshan, came out
	// playerOwned=1). Summons are excluded by name instead.
	auto IsLaneCreep(C_BaseEntity *entity,
					 int health, int maxHealth, uint8_t team) -> bool
	{
		if (!entity || health <= 0 || maxHealth <= 0)
			return false;
		// Jungle camps and Roshan sit on the neutral team; a lane creep always
		// belongs to one of the two playing teams. This alone keeps the whole
		// jungle out of the overlay even if a camp were ever named oddly.
		if (!IsPlayableTeam(team))
			return false;
		if (LooksLikeHeroEntity(entity) || LooksLikeNonCreepTarget(entity))
			return false;

		// A creep must NAME itself as one. There was briefly a fallback that
		// accepted any unit fitting a creep-shaped stats envelope (health
		// band, has an attack, no mana pool) on the theory that the name
		// allow-list could not be complete - that is what put stars on trees
		// and towers.
		//
		// Two reasons it can never come back. The health/damage/mana reads it
		// leaned on are C_DOTA_BaseNPC fields: on a tree, a prop, or any of
		// the thousands of non-NPC entities the chunk walk now reaches, those
		// offsets land outside the object entirely, so the "stats" being
		// tested were unrelated memory that passed the envelope by accident.
		// And the creeps it was meant to rescue were never missing for want of
		// a name - they were missing because the scan was bounded by
		// GetHighestEntityIndex() (see CollectCandidatesFresh). With that
		// fixed, the name is the only identity signal this build actually
		// gives us, so it decides.
		//
		// If a creep ever does go unstarred, the "unstarred" log line below
		// prints its name: the fix is one more needle here, never a return to
		// guessing from stats.
		return LooksLikeLaneCreepByName(entity);
	}

	// Names every creep-plausible unit the classifier turned down, once per
	// distinct name per 5s window, so a creep that never gets a star can be
	// identified by name instead of guessed at.
	//
	// Deliberately reads nothing but m_iHealth / m_iMaxHealth / m_iTeamNum -
	// the three fields every entity actually has. Printing damage or mana here
	// would mean reaching into C_DOTA_BaseNPC offsets on entities that are not
	// NPCs at all, which is the same out-of-bounds read that used to star
	// trees.
	auto LogUnstarredUnit(C_BaseEntity *entity, int health, int maxHealth, uint8_t team) -> void
	{
		static std::array<std::array<char, 64>, 16> seenNames{};
		static size_t seenCount = 0;
		static ULONGLONG windowStart = 0;

		const ULONGLONG now = GetTickCount64();
		if (!windowStart || now - windowStart >= 5000)
		{
			windowStart = now;
			seenCount = 0;
		}
		if (seenCount >= seenNames.size())
			return;

		const char *name = EntityDesignerOrName(entity);
		if (!name)
			name = "";
		for (size_t index = 0; index < seenCount; ++index)
		{
			if (std::strcmp(seenNames[index].data(), name) == 0)
				return;
		}
		std::snprintf(seenNames[seenCount].data(), seenNames[seenCount].size(), "%s", name);
		++seenCount;

		DEV_LOG("[last-hit] unstarred name=\"%s\" hp=%d/%d team=%u\n",
				name, health, maxHealth, static_cast<unsigned int>(team));
	}

	auto LogLastHitScan(const char *reason, const LocalHeroInfo &hero,
						const LastHitScanStats &stats, const std::vector<EnemyCreepSnapshot> &enemyCreeps) -> void
	{
		static ULONGLONG lastLogTick = 0;
		const ULONGLONG now = GetTickCount64();
		if (lastLogTick && now - lastLogTick < 3000)
			return;
		lastLogTick = now;

		const float starThreshold = hero.entity ? static_cast<float>(hero.attackDamage) : 0.f;
		DEV_LOG("[last-hit] %s local=%d fallback=%d team=%u damage=%d threshold=%.0f attack=%.0f detect=%.0f origin=(%.0f,%.0f,%.0f) scanned=%d lane=%d allied=%d enemy=%d enemy_origin=%d enemy_in_range=%d enemy_killable=%d in_range=%d lethal=%d candidates=%d nearest=%.0f hp=%d/%d team=%u\n",
				reason ? reason : "debug", hero.entity ? 1 : 0, hero.screenCenterFallback ? 1 : 0,
				static_cast<unsigned int>(hero.team), hero.attackDamage, starThreshold, hero.attackRange, hero.detectRange,
				hero.origin.m_x, hero.origin.m_y, hero.origin.m_z, stats.scanned, stats.laneLike,
				stats.alliedLane, stats.enemyLane, stats.enemyWithOrigin, stats.enemyInRange, stats.enemyKillable,
				stats.inRange, stats.lethal, stats.candidates, stats.nearestDistance,
				stats.nearestHealth, stats.nearestMaxHealth, static_cast<unsigned int>(stats.nearestTeam));

		std::array<char, 2048> hpList{};
		size_t used = 0;
		const size_t listedCount = (std::min)(enemyCreeps.size(), kMaxLoggedEnemyHp);
		for (size_t index = 0; index < listedCount && used + 1 < hpList.size(); ++index)
		{
			const auto &creep = enemyCreeps[index];
			const int written = std::snprintf(hpList.data() + used, hpList.size() - used,
											  "#%zu hp=%d/%d dist=%.0f in=%d kill=%d team=%u%s",
											  index + 1, creep.health, creep.maxHealth, creep.distance,
											  creep.inRange ? 1 : 0, creep.killable ? 1 : 0,
											  static_cast<unsigned int>(creep.team), index + 1 < listedCount ? "; " : "");
			if (written <= 0)
				break;
			used += (std::min)(static_cast<size_t>(written), hpList.size() - used - 1);
		}
		DEV_LOG("[last-hit] enemy-hp listed=%zu/%zu [%s]\n",
				listedCount, enemyCreeps.size(), hpList.data());
	}

	auto CollectCandidatesFresh(LocalHeroInfo &heroOut) -> std::vector<CreepCandidate>
	{
		heroOut = {};
		NearbyCreepCache().clear();
		std::vector<CreepCandidate> candidates;
		std::vector<EnemyCreepSnapshot> enemyCreeps;
		LastHitScanStats stats{};

		auto &offsets = ResolveOffsets();
		auto *entitySystem = SDK::Interfaces::GameEntitySystem();
		if (!offsets.usable || !entitySystem)
		{
			LogLastHitScan("not-ready", heroOut, stats, enemyCreeps);
			return candidates;
		}

		LocalHeroInfo hero{};
		if (!ResolveLocalHero(entitySystem, offsets, hero))
		{
			LogLastHitScan("no-local-hero", hero, stats, enemyCreeps);
			return candidates;
		}

		heroOut = hero;
		const float scanRange = hero.detectRange + 35.f;
		const float scanRangeSq = scanRange * scanRange;
		const float attackReach = hero.attackRange + kAttackReachLeeway;
		const float attackReachSq = attackReach * attackReach;
		const float lethalHealth = static_cast<float>(hero.attackDamage);

		// One pass produces the whole feature: the stars that get drawn and
		// the orders that get issued. It used to be two - a chunk walk here
		// and a second, differently-filtered index walk inside OnRender - so
		// the overlay and the attack could disagree about what a creep even
		// was, and every creep in both lists got its star drawn twice.
		//
		// The allocated identity chunks are walked directly rather than
		// indexing 1..GetHighestEntityIndex(). That index is build-dependent
		// and under-reports on this build - CKillStealer.cpp switched away
		// from it after our own hero failed to appear in an index-bounded
		// scan for an entire session, and CAndromedaClient.cpp's hero-vitals
		// scan says the same. Indexing it here quietly truncated the creep
		// list to whatever happened to sit below the reported bound (the
		// "8 creeps nearby but only 2 listed" case). Chunk N covers indices
		// N*512..N*512+511, so this is a strict superset. Entities reached
		// this way have no schema class name, which is why identity is
		// matched on the designer name (see EntityNameContains).
		for (int chunkIndex = 0; chunkIndex < MAX_ENTITY_LISTS; ++chunkIndex)
		{
			auto *chunk = entitySystem->m_pIdentityChunks[chunkIndex];
			if (!chunk)
				continue;

			for (int entryIndex = 0; entryIndex < MAX_ENTITIES_IN_LIST; ++entryIndex)
			{
				auto *entity = chunk->m_pIdentities[entryIndex].pBaseEntity();
				if (!entity || entity == hero.entity)
					continue;
				const int index = chunkIndex * MAX_ENTITIES_IN_LIST + entryIndex;
				++stats.scanned;

				const int health = ReadField<int>(entity, offsets.health);
				const int maxHealth = ReadField<int>(entity, offsets.maxHealth);
				const uint8_t team = ReadField<uint8_t>(entity, offsets.team);
				if (!IsLaneCreep(entity, health, maxHealth, team))
				{
					// Only creep-plausible rejects are worth naming: alive, on a
					// playing team, and small enough to be a creep rather than a
					// building.
					if (health > 0 && maxHealth > 0 && maxHealth <= 2000 && IsPlayableTeam(team) &&
						!LooksLikeHeroEntity(entity))
						LogUnstarredUnit(entity, health, maxHealth, team);
					continue;
				}
				++stats.laneLike;

				const bool allied = team == hero.team;
				if (allied)
					++stats.alliedLane;
				else
					++stats.enemyLane;

				Vector3 origin{};
				if (!ReadOrigin(entity, offsets, origin))
					continue;
				if (!allied)
					++stats.enemyWithOrigin;

				const float dx = origin.m_x - hero.origin.m_x;
				const float dy = origin.m_y - hero.origin.m_y;
				const float distanceSq = dx * dx + dy * dy;
				const float distance = std::sqrt(distanceSq);
				if (distanceSq < stats.nearestDistance * stats.nearestDistance)
				{
					stats.nearestDistance = distance;
					stats.nearestHealth = health;
					stats.nearestMaxHealth = maxHealth;
					stats.nearestTeam = team;
				}

				const bool inScanRange = distanceSq <= scanRangeSq;
				const bool killable = static_cast<float>(health) <= lethalHealth;

				EnemyCreepSnapshot snapshot{};
				snapshot.health = health;
				snapshot.maxHealth = maxHealth;
				snapshot.distance = distance;
				snapshot.team = team;
				snapshot.inRange = inScanRange;
				snapshot.killable = killable;
				if (!allied)
					enemyCreeps.push_back(snapshot);

				if (inScanRange)
				{
					if (!allied)
						++stats.enemyInRange;
					++stats.inRange;
					NearbyCreepCache().push_back({index, origin});
				}

				// Distance decides whether an ATTACK is issued (candidate.
				// inAttackRange below), never whether a star is drawn. Gating the
				// star on the 700-unit scan radius silently hid every one-shot
				// creep past it - creeps in the next wave over, or in a lane the
				// camera is looking at from a distance.
				if (!killable)
					continue;
				if (!allied)
					++stats.enemyKillable;
				++stats.lethal;

				CreepCandidate candidate{};
				candidate.entIndex = index;
				candidate.origin = origin;
				candidate.health = health;
				candidate.maxHealth = maxHealth;
				candidate.distance = distance;
				candidate.lethalHealth = lethalHealth;
				candidate.deny = allied;
				candidate.killable = true;
				candidate.inAttackRange = distanceSq <= attackReachSq;
				candidates.push_back(candidate);
				++stats.candidates;
			}
		}

		// Enemy last hits pay gold, so they outrank a deny; within a side take
		// the creep closest to dying, then the nearer one.
		std::sort(candidates.begin(), candidates.end(), [](const CreepCandidate &left, const CreepCandidate &right)
				  {
			if (left.deny != right.deny)
				return !left.deny;
			if (left.health != right.health)
				return left.health < right.health;
			return left.distance < right.distance; });

		std::sort(enemyCreeps.begin(), enemyCreeps.end(), [](const EnemyCreepSnapshot &left, const EnemyCreepSnapshot &right)
				  { return left.distance < right.distance; });

		LogLastHitScan(candidates.empty() ? "no-candidate" : "ready", hero, stats, enemyCreeps);
		return candidates;
	}

	auto CollectCandidates(LocalHeroInfo &heroOut) -> const std::vector<CreepCandidate> &
	{
		static ULONGLONG cacheTick = 0;
		static LocalHeroInfo cacheHero{};
		static std::vector<CreepCandidate> cacheCandidates;

		const ULONGLONG now = GetTickCount64();
		if (cacheTick && now - cacheTick < kScanIntervalMs)
		{
			heroOut = cacheHero;
			return cacheCandidates;
		}

		cacheCandidates = CollectCandidatesFresh(cacheHero);
		cacheTick = now;
		heroOut = cacheHero;
		return cacheCandidates;
	}

	auto DrawStarMarker(ImDrawList *drawList, const ImVec2 &center, float radius,
						ImU32 fillColor, ImU32 outlineColor) -> void
	{
		if (!drawList)
			return;

		ImVec2 points[10]{};
		const float innerRadius = radius * 0.45f;
		const float startAngle = -1.57079632679f;
		for (int index = 0; index < 10; ++index)
		{
			const float currentRadius = (index % 2 == 0) ? radius : innerRadius;
			const float angle = startAngle + static_cast<float>(index) * 0.62831853071f;
			points[index] = ImVec2(center.x + std::cos(angle) * currentRadius,
								   center.y + std::sin(angle) * currentRadius);
		}

		ImVec2 shadowPoints[10]{};
		for (int index = 0; index < 10; ++index)
			shadowPoints[index] = ImVec2(points[index].x + 1.5f, points[index].y + 1.5f);

		drawList->AddCircleFilled(center, radius * 1.35f, IM_COL32(255, 224, 38, 36), 24);
		drawList->AddPolyline(shadowPoints, 10, IM_COL32(20, 18, 8, 180),
							  ImDrawFlags_Closed, 3.4f);
		for (int index = 0; index < 10; ++index)
			drawList->AddTriangleFilled(center, points[index], points[(index + 1) % 10], fillColor);
		drawList->AddPolyline(points, 10, IM_COL32(88, 62, 8, 230), ImDrawFlags_Closed, 2.4f);
		drawList->AddPolyline(points, 10, outlineColor, ImDrawFlags_Closed, 1.1f);
	}

	auto DrawStyledScreenCircle(ImDrawList *drawList, const ImVec2 &center, float radius,
								int segments = 128) -> void
	{
		if (!drawList || radius <= 1.f)
			return;

		drawList->AddCircleFilled(center, radius, IM_COL32(70, 170, 255, 9), segments);
		drawList->AddCircle(center, radius + 1.7f, IM_COL32(5, 9, 12, 125), segments, 4.0f);
		drawList->AddCircle(center, radius, IM_COL32(65, 190, 255, 82), segments, 5.0f);
		drawList->AddCircle(center, radius, IM_COL32(255, 232, 132, 235), segments, 2.0f);
		drawList->AddCircle(center, (std::max)(1.f, radius - 2.4f),
							IM_COL32(155, 245, 255, 135), segments, 1.1f);
	}

	auto DrawStyledWorldCircle(ImDrawList *drawList, const Vector3 &origin, float radius) -> bool
	{
		if (!drawList || radius <= 1.f)
			return false;

		constexpr int segments = 128;
		constexpr float kTwoPi = 6.28318530718f;
		std::array<ImVec2, segments> points{};
		for (int index = 0; index < segments; ++index)
		{
			const float angle = static_cast<float>(index) * kTwoPi / static_cast<float>(segments);
			const Vector3 world(origin.m_x + std::cos(angle) * radius,
								origin.m_y + std::sin(angle) * radius, origin.m_z + 4.f);
			if (!Math::WorldToScreen(world, points[index]))
				return false;
		}

		drawList->AddConvexPolyFilled(points.data(), segments, IM_COL32(70, 170, 255, 8));
		drawList->AddPolyline(points.data(), segments, IM_COL32(5, 9, 12, 125),
							  ImDrawFlags_Closed, 4.0f);
		drawList->AddPolyline(points.data(), segments, IM_COL32(65, 190, 255, 82),
							  ImDrawFlags_Closed, 5.0f);
		drawList->AddPolyline(points.data(), segments, IM_COL32(255, 232, 132, 235),
							  ImDrawFlags_Closed, 2.0f);
		drawList->AddPolyline(points.data(), segments, IM_COL32(155, 245, 255, 135),
							  ImDrawFlags_Closed, 1.1f);
		return true;
	}

	auto ProjectHeroScreen(const Vector3 &origin, ImVec2 &out) -> bool
	{
		Vector3 lifted = origin;
		lifted.m_z += 64.f;
		if (Math::WorldToScreen(lifted, out))
			return true;
		return Math::WorldToScreen(origin, out);
	}

	auto ProjectStarTargetScreen(const Vector3 &origin, ImVec2 &out) -> bool
	{
		Vector3 markerOrigin = origin;
		markerOrigin.m_z += 140.f;
		if (Math::WorldToScreen(markerOrigin, out))
			return true;
		return Math::WorldToScreen(origin, out);
	}

	// Aimed at the creep's torso rather than its head: at +80 the point sits
	// at or above the top of a melee creep's model, so a slightly high camera
	// angle put the click on the ground behind it - and a click on ground is
	// what turns an attack order into an attack-MOVE that the engine then
	// re-targets on its own.
	auto ProjectClickTargetScreen(const Vector3 &origin, ImVec2 &out) -> bool
	{
		Vector3 clickOrigin = origin;
		clickOrigin.m_z += 50.f;
		if (Math::WorldToScreen(clickOrigin, out))
			return true;
		return Math::WorldToScreen(origin, out);
	}

	auto WindowReadyForInput() -> HWND
	{
		auto *gui = GetAndromedaGUI();
		const HWND window = gui ? gui->m_hCS2Window : nullptr;
		if (!window || GetForegroundWindow() != window || gui->IsVisible())
			return nullptr;
		return window;
	}

	// SetCursorPos does not go through the raw-input path Source 2 reads, so a
	// SetCursorPos-only move relocates the Windows arrow while the game keeps
	// aiming wherever the player's cursor already was - the attack then lands
	// on whatever was under the OLD position. CKillStealer.cpp documents the
	// same trap at length; routing the move through SendInput puts it in the
	// same synthetic-input stream as the key press and click that follow.
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

	auto MoveCursorToClientPoint(HWND window, const ImVec2 &screen, POINT &previousOut) -> bool
	{
		RECT client{};
		if (!GetClientRect(window, &client) || client.right <= 0 || client.bottom <= 0)
			return false;

		// A point outside the client area means the creep is not actually on
		// screen: Math::WorldToScreen only rejects negative coordinates, never
		// ones past the right/bottom edge. Clamping such a point - which this
		// used to do - parks the cursor on the screen border and makes the
		// attack order land on whatever happens to be sitting there.
		const int x = static_cast<int>(std::lround(screen.x));
		const int y = static_cast<int>(std::lround(screen.y));
		if (x < 0 || y < 0 || x >= static_cast<int>(client.right) || y >= static_cast<int>(client.bottom))
			return false;

		POINT target{x, y};
		if (!ClientToScreen(window, &target))
			return false;

		GetCursorPos(&previousOut);
		if (SendMouseMoveAbsolute(target.x, target.y))
			return true;
		return SetCursorPos(target.x, target.y) != FALSE;
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

	auto SendRightClick() -> bool
	{
		INPUT inputs[2]{};
		inputs[0].type = INPUT_MOUSE;
		inputs[0].mi.dwFlags = MOUSEEVENTF_RIGHTDOWN;
		inputs[1].type = INPUT_MOUSE;
		inputs[1].mi.dwFlags = MOUSEEVENTF_RIGHTUP;
		return SendInput(static_cast<UINT>(std::size(inputs)), inputs, sizeof(INPUT)) == std::size(inputs);
	}

	// Issuing an attack is a three-beat sequence, not one burst:
	//   1. move the cursor onto the creep and press 'A' (arms Dota's
	//      attack cursor),
	//   2. a tick or two later, click - sending the click in the same breath
	//      as the key raced the mode switch and the click got eaten as a stray
	//      click, which is the same bug CKillStealer.cpp hit with targeted
	//      casts,
	//   3. only then put the cursor back. The old code restored it
	//      immediately after the click, so the click and the restore could
	//      reach the game in the wrong order and the order landed at the
	//      player's original cursor position instead of on the creep.
	struct PendingAttack
	{
		int stage = 0; // 0 = idle, 1 = click due, 2 = cursor restore due
		bool rightButton = false;
		ULONGLONG dueTick = 0;
		POINT previousCursor{};
	};

	auto PendingAttackState() -> PendingAttack &
	{
		static PendingAttack state{};
		return state;
	}

	auto RestoreCursor(const POINT &previous) -> void
	{
		if (!SendMouseMoveAbsolute(previous.x, previous.y))
			SetCursorPos(previous.x, previous.y);
	}

	auto TickPendingAttack() -> void
	{
		auto &pending = PendingAttackState();
		if (pending.stage == 0 || GetTickCount64() < pending.dueTick)
			return;

		if (pending.stage == 1)
		{
			if (pending.rightButton)
				SendRightClick();
			else
				SendLeftClick();
			pending.stage = 2;
			pending.dueTick = GetTickCount64() + kCursorRestoreDelayMs;
			return;
		}

		RestoreCursor(pending.previousCursor);
		pending.stage = 0;
	}

	struct TargetLock
	{
		int entIndex = -1;
		ULONGLONG tick = 0;
	};

	auto TargetLockState() -> TargetLock &
	{
		static TargetLock lock{};
		return lock;
	}

	auto LockedTargetIndex() -> int
	{
		auto &lock = TargetLockState();
		if (lock.entIndex < 0)
			return -1;
		if (GetTickCount64() - lock.tick > kTargetLockMs)
		{
			lock.entIndex = -1;
			return -1;
		}
		return lock.entIndex;
	}

	auto BetterAttackTarget(const CreepCandidate &left, const CreepCandidate &right) -> bool
	{
		if (left.deny != right.deny)
			return !left.deny;
		if (left.health != right.health)
			return left.health < right.health;
		return left.distance < right.distance;
	}

	// Re-reads every in-reach candidate off its live entity before anything is
	// clicked. The candidate list is up to kScanIntervalMs old - long enough
	// for the creep to have died, been healed back over the one-shot
	// threshold, moved, or for its entity slot to have been recycled into a
	// different unit entirely - and the old code clicked the stale origin of
	// candidates[0] regardless.
	auto TrySelectAttackTarget(const LocalHeroInfo &hero, const std::vector<CreepCandidate> &candidates,
							   CreepCandidate &out) -> bool
	{
		auto *entitySystem = SDK::Interfaces::GameEntitySystem();
		auto &offsets = ResolveOffsets();
		if (!entitySystem || !offsets.usable)
			return false;

		const float attackReach = hero.attackRange + kAttackReachLeeway;
		const float attackReachSq = attackReach * attackReach;
		bool found = false;

		for (const auto &candidate : candidates)
		{
			if (!candidate.inAttackRange || candidate.entIndex < 0)
				continue;

			auto *entity = entitySystem->GetBaseEntity<C_BaseEntity>(candidate.entIndex);
			if (!entity || entity == hero.entity)
				continue;

			const int health = ReadField<int>(entity, offsets.health);
			const int maxHealth = ReadField<int>(entity, offsets.maxHealth);
			const uint8_t team = ReadField<uint8_t>(entity, offsets.team);
			if (!IsLaneCreep(entity, health, maxHealth, team))
				continue;
			if (static_cast<float>(health) > candidate.lethalHealth)
				continue;

			Vector3 origin{};
			if (!ReadOrigin(entity, offsets, origin))
				continue;

			const float dx = origin.m_x - hero.origin.m_x;
			const float dy = origin.m_y - hero.origin.m_y;
			const float distanceSq = dx * dx + dy * dy;
			if (distanceSq > attackReachSq)
				continue;

			CreepCandidate fresh = candidate;
			fresh.origin = origin;
			fresh.health = health;
			fresh.maxHealth = maxHealth;
			fresh.distance = std::sqrt(distanceSq);
			fresh.deny = team == hero.team;

			// Stay on the creep already ordered on while it is still a valid
			// one-shot in reach. Without this the pick flips as soon as any
			// other creep in the wave drops a point of health lower, and each
			// flip cancels the attack that was mid-wind-up - the hero swings
			// at nothing while both creeps stay alive.
			if (fresh.entIndex == LockedTargetIndex())
			{
				out = fresh;
				return true;
			}

			if (!found || BetterAttackTarget(fresh, out))
			{
				out = fresh;
				found = true;
			}
		}

		return found;
	}

	// The click has to land on the target's own model. Dota reads whatever is
	// under the cursor, so a click that clips the healthy creep standing in
	// front becomes an order on THAT creep, and a click that slips onto ground
	// becomes an attack-move the engine re-targets by its own rules - usually
	// onto the nearest healthy creep. Both produce exactly the reported
	// symptom: the star sits on the low creep, the hero hits the full one.
	auto TryResolveClickPoint(const CreepCandidate &target, ImVec2 &out) -> bool
	{
		if (!ProjectClickTargetScreen(target.origin, out))
			return false;

		ImVec2 nearestNeighbour{};
		float nearestDistance = 3.4e38f;
		for (const auto &creep : NearbyCreepCache())
		{
			if (creep.entIndex == target.entIndex)
				continue;

			ImVec2 screen{};
			if (!ProjectClickTargetScreen(creep.origin, screen))
				continue;

			const float dx = screen.x - out.x;
			const float dy = screen.y - out.y;
			const float distance = std::sqrt(dx * dx + dy * dy);
			if (distance < nearestDistance)
			{
				nearestDistance = distance;
				nearestNeighbour = screen;
			}
		}

		if (nearestDistance >= kMinClickSeparationPx)
			return true;

		// Push the click toward the target's far side, away from the neighbour
		// that could swallow it. The nudge stays small enough to remain on the
		// model - overshooting it would put the click on ground instead.
		const float dx = out.x - nearestNeighbour.x;
		const float dy = out.y - nearestNeighbour.y;
		const float length = std::sqrt(dx * dx + dy * dy);
		if (length < 1.f)
			return false;

		out.x += dx * (kMaxClickNudgePx / length);
		out.y += dy * (kMaxClickNudgePx / length);

		// Still overlapping even after the nudge: skip. A missed last hit
		// costs one creep; hitting the wrong one pulls the wave.
		return nearestDistance + kMaxClickNudgePx >= kMinClickSeparationPx;
	}

	// Enemy creeps are ordered with a plain RIGHT click, denies with 'A' + a
	// left click.
	//
	// Both used to go through 'A' + left click, on the reasoning that it can
	// never turn into a move order. That is true, but the failure it does have
	// is worse: 'A' on anything other than the unit itself is an attack-MOVE,
	// and the engine then chooses the target - reliably the healthy creep
	// nearest the hero rather than the starred one. A right click that misses
	// only walks the hero a step, and one that lands attacks exactly the unit
	// under it. Denying an ally still requires the force-attack, so that path
	// keeps 'A' and leans on the click-separation test above instead.
	auto TickAutoAttack(const LocalHeroInfo &hero, const std::vector<CreepCandidate> &candidates) -> void
	{
		auto &pending = PendingAttackState();
		if (pending.stage != 0)
			return;

		static ULONGLONG lastAttackTick = 0;
		const ULONGLONG now = GetTickCount64();
		if (lastAttackTick && now - lastAttackTick < kAutoAttackIntervalMs)
			return;

		const HWND window = WindowReadyForInput();
		if (!window)
			return;

		CreepCandidate target{};
		if (!TrySelectAttackTarget(hero, candidates, target))
			return;

		ImVec2 screen{};
		if (!TryResolveClickPoint(target, screen))
		{
			// Overlapping creeps - do not guess. Retry on the next tick, by
			// which point the wave has moved.
			lastAttackTick = now;
			return;
		}

		POINT previous{};
		if (!MoveCursorToClientPoint(window, screen, previous))
			return;

		lastAttackTick = now;
		if (target.deny && !SendKeyPress('A'))
		{
			RestoreCursor(previous);
			return;
		}

		TargetLockState() = {target.entIndex, now};
		pending.stage = 1;
		pending.rightButton = !target.deny;
		pending.dueTick = now + kAttackClickDelayMs;
		pending.previousCursor = previous;
	}

	auto DrawScreenRangeFallback(ImDrawList *drawList, const LocalHeroInfo &hero) -> void
	{
		ImVec2 center{};
		if (!ProjectHeroScreen(hero.origin, center))
		{
			drawList->AddText(ImVec2(24.f, 118.f), IM_COL32(255, 205, 55, 235),
							  "Last Hit Helper: local hero is off-screen");
			return;
		}

		Vector3 sideWorld(hero.origin.m_x + hero.detectRange, hero.origin.m_y, hero.origin.m_z + 64.f);
		ImVec2 side{};
		float screenRadius = 150.f;
		if (Math::WorldToScreen(sideWorld, side))
			screenRadius = std::hypot(side.x - center.x, side.y - center.y);
		screenRadius = std::clamp(screenRadius, 45.f, 420.f);

		DrawStyledScreenCircle(drawList, center, screenRadius);
		drawList->AddCircleFilled(center, 3.5f, IM_COL32(248, 250, 238, 210), 16);
	}

	auto DrawDetectRange(ImDrawList *drawList, const LocalHeroInfo &hero) -> void
	{
		if (DrawStyledWorldCircle(drawList, hero.origin, hero.detectRange))
			return;

		DrawScreenRangeFallback(drawList, hero);
	}
}

auto CLastHitAssistant::OnRender() -> void
{
	if (!ImGui::GetCurrentContext() || !LastHitAssistantActive())
		return;

	LocalHeroInfo hero{};
	const auto &candidates = CollectCandidates(hero);
	if (!hero.entity)
		return;

	// Every entry in `candidates` is already a one-attack lane creep (see
	// IsLaneCreep / CollectCandidatesFresh), so the overlay is just a draw
	// pass over it. Gold marks an enemy creep to last hit, red an allied
	// creep to deny.
	auto *drawList = ImGui::GetForegroundDrawList();
	int drawn = 0;
	int offscreen = 0;
	for (const auto &candidate : candidates)
	{
		ImVec2 screen{};
		if (!ProjectStarTargetScreen(candidate.origin, screen))
		{
			++offscreen;
			continue;
		}

		const ImU32 fillColor = candidate.deny ? IM_COL32(255, 112, 112, 245) : IM_COL32(255, 221, 32, 245);
		const ImU32 outlineColor = candidate.deny ? IM_COL32(255, 198, 198, 255) : IM_COL32(255, 246, 154, 255);
		DrawStarMarker(drawList, screen, 9.f, fillColor, outlineColor);
		++drawn;
	}

	// Runs unconditionally: a sequence already in flight has to finish (click,
	// then cursor restore) even if the setting is switched off mid-attack,
	// otherwise the player's cursor stays parked on the creep.
	TickPendingAttack();

	if (Settings::LastHitAssistant::EnableAutoAttack)
		TickAutoAttack(hero, candidates);

	static ULONGLONG lastOverlayLogTick = 0;
	const ULONGLONG now = GetTickCount64();
	if (!lastOverlayLogTick || now - lastOverlayLogTick >= 3000)
	{
		DEV_LOG("[last-hit] overlay candidates=%zu drawn=%d offscreen=%d autoAttack=%d\n",
				candidates.size(), drawn, offscreen,
				Settings::LastHitAssistant::EnableAutoAttack ? 1 : 0);
		lastOverlayLogTick = now;
	}
}
