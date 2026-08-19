#include "CLastHitAssistant.hpp"

#include <AndromedaClient/CAndromedaGUI.hpp>
#include <AndromedaClient/Settings/Settings.hpp>
#include <Common/DevLog.hpp>
#include <Common/MemoryEngine.hpp>
#include <Dota2/SDK/CSchemaOffset.hpp>
#include <Dota2/SDK/Interface/CGameEntitySystem.hpp>
#include <Dota2/SDK/Math/Math.hpp>
#include <Dota2/SDK/SDK.hpp>

#include <ImGui/imgui.h>
#include <Windows.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

namespace
{
	constexpr ULONGLONG kScanIntervalMs = 100;
	constexpr int kMaxScannedEntityIndex = MAX_TOTAL_ENTITIES - 1;
	constexpr float kMinimumVisualLeadHp = 120.f;
	constexpr size_t kMaxLoggedEnemyHp = 24;

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
		Vector3 origin{};
		int health = 0;
		int maxHealth = 0;
		float lethalHealth = 0.f;
		bool deny = false;
		bool killable = false;
	};

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

	// #region agent log
	auto AgentDbgLog(const char *runId, const char *hypothesisId, const char *location, const char *message, const char *dataJson) -> void
	{
		FILE *file = nullptr;
		if (fopen_s(&file, "c:\\CheatDota2-nearfriend\\Cheatdota2\\debug-e0a193.log", "a") != 0 || !file)
			return;
		std::fprintf(file,
					 "{\"sessionId\":\"e0a193\",\"runId\":\"%s\",\"hypothesisId\":\"%s\",\"location\":\"%s\",\"message\":\"%s\",\"data\":%s,\"timestamp\":%llu}\n",
					 runId ? runId : "", hypothesisId ? hypothesisId : "", location ? location : "",
					 message ? message : "", dataJson ? dataJson : "{}",
					 static_cast<unsigned long long>(GetTickCount64()));
		std::fclose(file);
	}
	// #endregion

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

	auto IsPlayerControlledUnit(const C_BaseEntity *entity, const LastHitOffsets &offsets) -> bool
	{
		return IsLikelyPlayerId(UnitPlayerId(entity, offsets));
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

	auto EntityTextContains(C_BaseEntity *entity, const char *needle) -> bool
	{
		if (!entity || !needle)
			return false;
		const char *name = EntityDesignerOrName(entity);
		const char *className = entity->GetSchemaClassName();
		return TextContains(name, needle) || TextContains(className, needle);
	}

	auto LooksLikePlayerControllerEntity(C_BaseEntity *entity) -> bool
	{
		if (!entity)
			return false;
		const char *className = entity->GetSchemaClassName();
		return TextContains(className, "DOTAPlayerController");
	}

	auto LooksLikeExcludedLaneTarget(C_BaseEntity *entity) -> bool
	{
		return EntityTextContains(entity, "neutral") || EntityTextContains(entity, "Neutral") ||
			   EntityTextContains(entity, "tower") || EntityTextContains(entity, "Tower") ||
			   EntityTextContains(entity, "fort") || EntityTextContains(entity, "Fort") ||
			   EntityTextContains(entity, "barracks") || EntityTextContains(entity, "Barracks") ||
			   EntityTextContains(entity, "rax") || EntityTextContains(entity, "ward") ||
			   EntityTextContains(entity, "Ward") || EntityTextContains(entity, "courier") ||
			   EntityTextContains(entity, "Courier") || EntityTextContains(entity, "hero") ||
			   EntityTextContains(entity, "Hero");
	}

	auto LooksLikeLaneCreepByName(C_BaseEntity *entity) -> bool
	{
		if (LooksLikeExcludedLaneTarget(entity))
			return false;

		return EntityTextContains(entity, "npc_dota_creep") ||
			   EntityTextContains(entity, "npc_dota_goodguys_siege") ||
			   EntityTextContains(entity, "npc_dota_badguys_siege") ||
			   EntityTextContains(entity, "siege") ||
			   EntityTextContains(entity, "Siege") ||
			   EntityTextContains(entity, "Creep_Lane") ||
			   EntityTextContains(entity, "creep_lane") ||
			   EntityTextContains(entity, "BaseNPC_Creep") ||
			   EntityTextContains(entity, "BaseNPC_Creep_Lane") ||
			   EntityTextContains(entity, "BaseNPC");
	}

	auto DetectRangeForHero(float attackRange) -> float
	{
		const float configured = std::isfinite(Settings::LastHitAssistant::DetectRange) ? Settings::LastHitAssistant::DetectRange : 950.f;
		return std::clamp((std::max)(attackRange, configured), 150.f, 1600.f);
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

	auto IsCreepPlayerControlled(const C_BaseEntity *entity, const LastHitOffsets &offsets) -> bool
	{
		if (offsets.hasPlayerOwnerId)
		{
			const int ownerPlayerId = ReadField<int>(entity, offsets.playerOwnerId, -1);
			if (IsLikelyPlayerId(ownerPlayerId))
				return true;
		}
		return false;
	}

	auto LooksLikeLaneCreepByStats(C_BaseEntity *entity, const LastHitOffsets &offsets,
								   int health, int maxHealth, uint8_t team) -> bool
	{
		if (!entity || health <= 0)
			return false;
		if (team != 2 && team != 3)
			return false;
		if (LooksLikeExcludedLaneTarget(entity))
			return false;
		if (LooksLikeLaneCreepByName(entity))
			return true;
		if (maxHealth < 80 || maxHealth > 2500)
			return false;
		return !IsCreepPlayerControlled(entity, offsets);
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
		DEV_LOG("[last-hit] %s local=%d fallback=%d team=%u damage=%d threshold=%.0f attack=%.0f detect=%.0f origin=(%.0f,%.0f,%.0f) scanned=%d lane=%d enemy=%d enemy_origin=%d enemy_in_range=%d enemy_killable=%d in_range=%d lethal=%d candidates=%d nearest=%.0f hp=%d/%d team=%u\n",
				reason ? reason : "debug", hero.entity ? 1 : 0, hero.screenCenterFallback ? 1 : 0,
				static_cast<unsigned int>(hero.team), hero.attackDamage, starThreshold, hero.attackRange, hero.detectRange,
				hero.origin.m_x, hero.origin.m_y, hero.origin.m_z, stats.scanned, stats.laneLike,
				stats.enemyLane, stats.enemyWithOrigin, stats.enemyInRange, stats.enemyKillable,
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
		const float lethalHealth = static_cast<float>(hero.attackDamage);
		for (int chunkIndex = 0; chunkIndex < MAX_ENTITY_LISTS; ++chunkIndex)
		{
			auto *chunk = entitySystem->m_pIdentityChunks[chunkIndex];
			if (!chunk)
				continue;

			for (int entryIndex = 0; entryIndex < MAX_ENTITIES_IN_LIST; ++entryIndex)
			{
				auto *identity = &chunk->m_pIdentities[entryIndex];
				auto *entity = identity->pBaseEntity();
				if (!entity || entity == hero.entity)
					continue;
				++stats.scanned;

				const int health = ReadField<int>(entity, offsets.health);
				const int maxHealth = ReadField<int>(entity, offsets.maxHealth);
				const uint8_t team = ReadField<uint8_t>(entity, offsets.team);
				if (!LooksLikeLaneCreepByStats(entity, offsets, health, maxHealth, team))
					continue;
				++stats.laneLike;

				if (team == hero.team)
				{
					++stats.alliedLane;
					continue;
				}
				++stats.enemyLane;

				Vector3 origin{};
				if (!ReadOrigin(entity, offsets, origin))
					continue;
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

				EnemyCreepSnapshot snapshot{};
				snapshot.health = health;
				snapshot.maxHealth = maxHealth;
				snapshot.distance = distance;
				snapshot.team = team;
				snapshot.inRange = distanceSq <= scanRangeSq;
				snapshot.killable = static_cast<float>(health) <= lethalHealth;
				enemyCreeps.push_back(snapshot);

				if (!snapshot.inRange)
					continue;
				++stats.enemyInRange;
				++stats.inRange;

				if (!snapshot.killable)
					continue;
				++stats.enemyKillable;
				++stats.lethal;

				candidates.push_back({origin, health, maxHealth, lethalHealth, false, true});
				++stats.candidates;
			}
		}

		std::sort(candidates.begin(), candidates.end(), [](const CreepCandidate &left, const CreepCandidate &right)
				  {
			if ( left.killable != right.killable )
				return left.killable;
			const float leftUrgency = static_cast<float>( left.health ) / (std::max)( left.lethalHealth , 1.f );
			const float rightUrgency = static_cast<float>( right.health ) / (std::max)( right.lethalHealth , 1.f );
			return leftUrgency < rightUrgency; });

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
	{
		// #region agent log
		{
			static ULONGLONG lastSkipLogTick = 0;
			const ULONGLONG now = GetTickCount64();
			if (!lastSkipLogTick || now - lastSkipLogTick >= 1000)
			{
				char data[96]{};
				std::snprintf(data, sizeof(data), "{\"imgui\":%d,\"enabled\":%d}",
							  ImGui::GetCurrentContext() ? 1 : 0, LastHitAssistantActive() ? 1 : 0);
				AgentDbgLog("pre-fix", "D", "CLastHitAssistant.cpp:OnRender", "render-skipped", data);
				lastSkipLogTick = now;
			}
		}
		// #endregion
		return;
	}

	const auto &offsets = ResolveOffsets();
	LocalHeroInfo hero{};
	const auto &candidates = CollectCandidates(hero);
	auto *drawList = ImGui::GetForegroundDrawList();

	if (!hero.entity)
	{
		drawList->AddText(ImVec2(24.f, 118.f), IM_COL32(255, 205, 55, 235),
						  "Last Hit Helper: waiting for local hero");
		// #region agent log
		{
			static ULONGLONG lastHeroLogTick = 0;
			const ULONGLONG now = GetTickCount64();
			if (!lastHeroLogTick || now - lastHeroLogTick >= 1000)
			{
				AgentDbgLog("pre-fix", "D", "CLastHitAssistant.cpp:OnRender", "missing-local-hero", "{\"hero\":0}");
				lastHeroLogTick = now;
			}
		}
		// #endregion
		return;
	}

	// ---------------------------------------------------------------
	// Universal lane-creep overlay (no range circle).
	// Draws star+HP for every low-HP lane creep on-screen, including allies.
	//
	// Low-HP condition matches the last-hit assistant lethal threshold:
	// health <= (localHero.attackDamage + timingBuffer)
	// ---------------------------------------------------------------
	{
		const float lethalHealth = static_cast<float>(hero.attackDamage);
		const float scanRange = hero.detectRange + 35.f;
		const float scanRangeSq = scanRange * scanRange;
		int totalAlive = 0;
		int passedStats = 0;
		int passedExclude = 0;
		int aliveOnTeam = 0;
		int aboveLethal = 0;
		int skippedByDedupe = 0;
		int skippedByOrigin = 0;
		int skippedByProjection = 0;
		int drawnUniversal = 0;

		auto *entitySystem = SDK::Interfaces::GameEntitySystem();
		if (entitySystem)
		{
			const int highest = entitySystem->GetHighestEntityIndex();
			for (int i = 1; i <= highest; ++i)
			{
				auto *ent = entitySystem->GetBaseEntity<C_BaseEntity>(i);
				if (!ent || ent == hero.entity)
					continue;

				const int health = ReadField<int>(ent, offsets.health);
				const int maxHealth = ReadField<int>(ent, offsets.maxHealth);
				const uint8_t team = ReadField<uint8_t>(ent, offsets.team);

			if (health <= 5 || maxHealth <= 0)
				continue;
			if (team != 2 && team != 3)
				continue;
			++totalAlive;

			if (LooksLikeExcludedLaneTarget(ent))
				continue;
			++passedExclude;

			if (LooksLikeHeroEntity(ent))
				continue;

			// Diagnostic: log rejected entities to understand filter failures
			const bool passedLaneCreep = LooksLikeLaneCreepByStats(ent, offsets, health, maxHealth, team);
			if (!passedLaneCreep)
			{
				static ULONGLONG lastRejectLogTick = 0;
				static int rejectLogCount = 0;
				const ULONGLONG nowReject = GetTickCount64();
				if (!lastRejectLogTick || nowReject - lastRejectLogTick >= 5000)
				{
					lastRejectLogTick = nowReject;
					rejectLogCount = 0;
				}
				if (rejectLogCount < 12)
				{
					const char *designerName = EntityDesignerOrName(ent);
					const char *className = ent->GetSchemaClassName();
					const bool nameMatch = LooksLikeLaneCreepByName(ent);
					const bool excluded = LooksLikeExcludedLaneTarget(ent);
					const bool playerOwned = IsPlayerControlledUnit(ent, offsets);
					char rejectData[512]{};
					std::snprintf(rejectData, sizeof(rejectData),
								  "{\"designer\":\"%s\",\"class\":\"%s\",\"hp\":%d,\"maxHp\":%d,\"team\":%u,\"nameMatch\":%d,\"excluded\":%d,\"playerOwned\":%d}",
								  designerName ? designerName : "", className ? className : "",
								  health, maxHealth, static_cast<unsigned int>(team),
								  nameMatch ? 1 : 0, excluded ? 1 : 0, playerOwned ? 1 : 0);
					AgentDbgLog("diag", "filter", "CLastHitAssistant.cpp:universal", "rejected-entity", rejectData);
					++rejectLogCount;
				}
				continue;
			}
			++passedStats;

				if (static_cast<float>(health) > lethalHealth)
				{
					++aboveLethal;
					continue;
				}

				Vector3 origin{};
				if (!ReadOrigin(ent, offsets, origin))
				{
					++skippedByOrigin;
					continue;
				}

				ImVec2 screen{};
				Vector3 markerOrigin = origin;
				markerOrigin.m_z += 90.f;
				if (!Math::WorldToScreen(markerOrigin, screen) && !Math::WorldToScreen(origin, screen))
				{
					++skippedByProjection;
					continue;
				}

				const ImU32 fillColor = IM_COL32(255, 221, 32, 245);
				const ImU32 outlineColor = IM_COL32(255, 246, 154, 255);
				DrawStarMarker(drawList, screen, 9.f, fillColor, outlineColor);

				char hpStr[32];
				snprintf(hpStr, sizeof(hpStr), "%d/%d", health, maxHealth);
				drawList->AddText(screen + ImVec2(0, -12), IM_COL32(255, 255, 255, 255), hpStr);
				++drawnUniversal;
			}
		}

		// #region agent log
		{
			static ULONGLONG lastUniversalLogTick = 0;
			const ULONGLONG now = GetTickCount64();
			if (!lastUniversalLogTick || now - lastUniversalLogTick >= 1000)
			{
				char data[512]{};
				std::snprintf(data, sizeof(data),
							  "{\"totalAlive\":%d,\"passedExclude\":%d,\"passedStats\":%d,\"aboveLethal\":%d,\"skippedByDedupe\":%d,\"skippedByOrigin\":%d,\"skippedByProjection\":%d,\"drawnUniversal\":%d,\"candidateCount\":%d,\"heroTeam\":%u,\"lethalHealth\":%.0f,\"attackDamage\":%d}",
							  totalAlive, passedExclude, passedStats, aboveLethal, skippedByDedupe,
							  skippedByOrigin, skippedByProjection, drawnUniversal,
							  static_cast<int>(candidates.size()), static_cast<unsigned int>(hero.team),
							  lethalHealth, hero.attackDamage);
				AgentDbgLog("pre-fix2", "A,B,C,D,E", "CLastHitAssistant.cpp:OnRender", "universal-creep-pass", data);
				lastUniversalLogTick = now;
			}
		}
		// #endregion
	}

	int candidateProjected = 0;
	int candidateOffscreen = 0;
	for (const auto &candidate : candidates)
	{
		ImVec2 screen{};
		Vector3 markerOrigin = candidate.origin;
		markerOrigin.m_z += 90.f;
		if (!Math::WorldToScreen(markerOrigin, screen) && !Math::WorldToScreen(candidate.origin, screen))
		{
			++candidateOffscreen;
			continue;
		}
		++candidateProjected;

		DrawStarMarker(drawList, screen, 9.f, IM_COL32(255, 221, 32, 245),
					   IM_COL32(255, 246, 154, 255));

		// Display the bot's current HP next to the marker
		char hpStr[32];
		snprintf(hpStr, sizeof(hpStr), "%d/%d", candidate.health, candidate.maxHealth);
		drawList->AddText(screen + ImVec2(0, -12), IM_COL32(255, 255, 255, 255), hpStr);
	}

	// #region agent log
	{
		static ULONGLONG lastCandidateLogTick = 0;
		const ULONGLONG now = GetTickCount64();
		if (!lastCandidateLogTick || now - lastCandidateLogTick >= 1000)
		{
			char data[192]{};
			std::snprintf(data, sizeof(data),
						  "{\"candidateCount\":%d,\"candidateProjected\":%d,\"candidateOffscreen\":%d}",
						  static_cast<int>(candidates.size()), candidateProjected, candidateOffscreen);
			AgentDbgLog("pre-fix", "B,C", "CLastHitAssistant.cpp:OnRender", "candidate-pass", data);
			lastCandidateLogTick = now;
		}
	}
	// #endregion
}
