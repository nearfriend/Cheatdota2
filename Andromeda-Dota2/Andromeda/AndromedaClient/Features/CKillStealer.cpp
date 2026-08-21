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
#include <cmath>
#include <cctype>
#include <cstring>
#include <iterator>
#include <string>
#include <vector>

namespace
{
	enum class KillToolKind : uint8_t
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
		uint32_t maxMana = 0;
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
		uint32_t playerOwnerId = 0;
		uint32_t heroPlayerId = 0;
		uint32_t assignedHero = 0;
		uint32_t isIllusion = 0;
		uint32_t isClone = 0;
		bool hasInventory = false;
		bool hasAbilityActivated = false;
		bool hasArmor = false;
		bool hasMagicResistance = false;
		bool hasSpellAmp = false;
		bool hasDamageBonus = false;
		bool hasAttackRange = false;
		bool hasPlayerOwnerId = false;
		bool hasHeroPlayerId = false;
		bool hasAssignedHero = false;
		bool hasIsIllusion = false;
		bool hasIsClone = false;
		bool resolved = false;
	};

	struct NetworkHandleVector
	{
		int32_t size = 0;
		int32_t pad = 0;
		const CHandle *data = nullptr;
		int32_t allocationCount = 0;
		int32_t growSize = 0;
	};

	struct HeroCandidate
	{
		C_BaseEntity *entity = nullptr;
		CHandle handle{INVALID_EHANDLE_INDEX};
		std::string name;
		Vector3 origin{};
		uint8_t team = 0;
		int health = 0;
		int maxHealth = 0;
		float mana = 0.f;
		float armor = 0.f;
		float magicResistance = 0.25f;
		float spellAmp = 0.f;
		float attackDamage = 0.f;
		float attackRange = 150.f;
		int playerId = -1;
		int ownerId = -1;
	};

	struct KillTool
	{
		KillToolKind kind = KillToolKind::Ability;
		std::string name;
		float rawDamage = 0.f;
		float castRange = 0.f;
		int manaCost = 0;
		WORD key = 0;
		AbilityDamageType damageType = AbilityDamageType::Magical;
		bool noTarget = false;
		bool unitTarget = false;
		bool pointTarget = false;
		bool isDamageAmplifier = false;
		uint32_t delayMs = 0;
	};

	struct HeroScanStats
	{
		int chunks = 0;
		int entities = 0;
		int controllers = 0;
		int directHeroes = 0;
	};

	auto IsReadableRuntimeMemory(const void *ptr, size_t size = 1) -> bool
	{
		if (!ptr || size == 0)
			return false;

		MEMORY_BASIC_INFORMATION mbi{};
		if (!VirtualQuery(ptr, &mbi, sizeof(mbi)))
			return false;

		if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
			return false;

		const DWORD readable =
			PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
			PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
		if (!(mbi.Protect & readable))
			return false;

		const auto start = reinterpret_cast<uintptr_t>(ptr);
		const auto regionStart = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
		const auto regionEnd = regionStart + mbi.RegionSize;
		return start >= regionStart && start + size <= regionEnd && start + size >= start;
	}

	template <typename T>
	auto TryRead(const void *address, T &out) -> bool
	{
		if (!IsReadableRuntimeMemory(address, sizeof(T)))
			return false;
		std::memcpy(&out, address, sizeof(T));
		return true;
	}

	template <typename T>
	auto TryReadField(const void *base, uint32_t offset, T &out) -> bool
	{
		if (!base || !offset)
			return false;
		return TryRead(reinterpret_cast<const void *>(reinterpret_cast<uintptr_t>(base) + offset), out);
	}

	template <typename T>
	auto ReadField(const void *base, uint32_t offset, T fallback = T{}) -> T
	{
		T value{};
		return TryReadField(base, offset, value) ? value : fallback;
	}

	auto CopyReadableString(const char *text, std::string &out, size_t maxLength = 128) -> bool
	{
		out.clear();
		if (!text)
			return false;

		MEMORY_BASIC_INFORMATION mbi{};
		if (!VirtualQuery(text, &mbi, sizeof(mbi)))
			return false;
		if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
			return false;
		const DWORD readable =
			PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
			PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
		if (!(mbi.Protect & readable))
			return false;

		const auto start = reinterpret_cast<uintptr_t>(text);
		const auto regionEnd = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
		const size_t readableLength = start < regionEnd ? static_cast<size_t>(regionEnd - start) : 0;
		const size_t limit = (std::min)(maxLength, readableLength);
		for (size_t index = 0; index < limit; ++index)
		{
			const char character = text[index];
			if (character == '\0')
				return !out.empty();

			const unsigned char value = static_cast<unsigned char>(character);
			if (value < 32 || value > 126)
				return false;

			out.push_back(character);
		}

		return !out.empty();
	}

	auto ToLower(std::string value) -> std::string
	{
		for (auto &character : value)
			character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
		return value;
	}

	auto EntityName(CEntityInstance *entity, CEntityIdentity *identity = nullptr) -> std::string
	{
		std::string result;

		if (identity && IsReadableRuntimeMemory(identity, sizeof(CEntityIdentity)))
		{
			const char *designer = identity->DesingerName().String();
			if (CopyReadableString(designer, result))
				return result;

			const char *name = identity->Name().String();
			if (CopyReadableString(name, result))
				return result;
		}

		return {};
	}

	auto LooksLikeHeroName(const std::string &name) -> bool
	{
		const std::string lower = ToLower(name);
		return lower.find("npc_dota_hero_") != std::string::npos ||
			   lower.find("dota_unit_hero") != std::string::npos ||
			   lower.find("base_npc_hero") != std::string::npos ||
			   lower.find("basenpc_hero") != std::string::npos;
	}

	auto DisplayHeroName(const std::string &name) -> std::string
	{
		const std::string lower = ToLower(name);
		constexpr const char *prefixes[] =
			{
				"npc_dota_hero_",
				"c_dota_unit_hero_",
				"cdota_unit_hero_",
				"dota_unit_hero_"};

		for (const char *prefix : prefixes)
		{
			const size_t length = std::strlen(prefix);
			if (lower.rfind(prefix, 0) == 0)
				return lower.substr(length);
		}

		return lower.empty() ? "hero" : lower;
	}

	auto IsLikelyPlayerId(int value) -> bool
	{
		return value >= 0 && value < 24;
	}

	auto AddUniqueHandle(std::vector<CHandle> &handles, CHandle handle, size_t limit) -> bool
	{
		if (!handle.IsValid())
			return false;
		for (const auto &existing : handles)
		{
			if (existing.m_Index == handle.m_Index)
				return false;
		}
		if (handles.size() >= limit)
			return false;
		handles.push_back(handle);
		return true;
	}

	auto TryEntityAtIndex(CGameEntitySystem *entitySystem, int index, CEntityIdentity *&identityOut, C_BaseEntity *&entityOut) -> bool
	{
		identityOut = nullptr;
		entityOut = nullptr;
		if (!entitySystem || index < 0 || index >= MAX_TOTAL_ENTITIES)
			return false;

		auto *chunk = entitySystem->m_pIdentityChunks[index / MAX_ENTITIES_IN_LIST];
		if (!chunk)
			return false;

		auto *identity = &chunk->m_pIdentities[index % MAX_ENTITIES_IN_LIST];
		if (!IsReadableRuntimeMemory(identity, sizeof(CEntityIdentity)))
			return false;

		auto *entity = identity->pBaseEntity();
		if (!entity || !IsReadableRuntimeMemory(entity, sizeof(void *)))
			return false;

		identityOut = identity;
		entityOut = entity;
		return true;
	}

	auto EntityFromHandle(CGameEntitySystem *entitySystem, CHandle handle, CEntityIdentity **identityOut = nullptr) -> C_BaseEntity *
	{
		if (identityOut)
			*identityOut = nullptr;
		if (!handle.IsValid())
			return nullptr;

		CEntityIdentity *identity = nullptr;
		C_BaseEntity *entity = nullptr;
		if (!TryEntityAtIndex(entitySystem, handle.GetEntryIndex(), identity, entity))
			return nullptr;

		if (identityOut)
			*identityOut = identity;
		return entity;
	}

	auto TryReadOrigin(C_BaseEntity *entity, const KillStealerOffsets &offsets, Vector3 &out) -> bool
	{
		void *sceneNode = nullptr;
		if (!TryReadField(entity, offsets.sceneNode, sceneNode) || !sceneNode)
			return false;

		if (!TryReadField(sceneNode, offsets.absOrigin, out))
			return false;

		return std::isfinite(out.m_x) && std::isfinite(out.m_y) && std::isfinite(out.m_z);
	}

	auto NormalizeResistance(float value, float fallback) -> float
	{
		if (!std::isfinite(value))
			return fallback;
		if (std::abs(value) > 1.5f)
			value *= 0.01f;
		return std::clamp(value, -0.95f, 0.95f);
	}

	auto NormalizeSpellAmp(float value) -> float
	{
		if (!std::isfinite(value))
			return 0.f;
		if (std::abs(value) > 1.5f)
			value *= 0.01f;
		return std::clamp(value, -0.75f, 3.0f);
	}

	auto PhysicalDamageAfterArmor(float damage, float armor) -> float
	{
		if (!std::isfinite(armor))
			armor = 0.f;
		const float reduction = 0.052f * armor / (0.9f + 0.048f * std::abs(armor));
		return damage * (1.f - std::clamp(reduction, -0.95f, 0.95f));
	}

	// Ethereal Blade makes its target take double magic damage for its duration
	// (magicAmplified=true when the plan also includes item_ethereal_blade).
	auto EffectiveDamage(const KillTool &tool, const HeroCandidate &localHero, const HeroCandidate &target, bool magicAmplified = false) -> float
	{
		float damage = tool.rawDamage;
		if (tool.kind != KillToolKind::Attack)
			damage *= 1.f + localHero.spellAmp;

		switch (tool.damageType)
		{
		case AbilityDamageType::Pure:
			return damage;
		case AbilityDamageType::Physical:
			return PhysicalDamageAfterArmor(damage, target.armor);
		case AbilityDamageType::Magical:
		default:
			return damage * ( magicAmplified ? 2.f : 1.f ) * (1.f - target.magicResistance);
		}
	}

	auto ResolveOffsets() -> KillStealerOffsets &
	{
		static KillStealerOffsets offsets{};
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
		const bool hasMana = schema->TryGetOffset("C_DOTA_BaseNPC", "m_flMana", offsets.mana);
		const bool hasMaxMana = schema->TryGetOffset("C_DOTA_BaseNPC", "m_flMaxMana", offsets.maxMana);
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
		offsets.hasPlayerOwnerId = schema->TryGetOffset("C_DOTA_BaseNPC", "m_nPlayerOwnerID", offsets.playerOwnerId);
		offsets.hasHeroPlayerId = schema->TryGetOffset("C_DOTA_BaseNPC_Hero", "m_iPlayerID", offsets.heroPlayerId) ||
								  schema->TryGetOffset("C_DOTA_BaseNPC", "m_iPlayerID", offsets.heroPlayerId);
		offsets.hasAssignedHero = schema->TryGetOffset("C_DOTAPlayerController", "m_hAssignedHero", offsets.assignedHero);
		offsets.hasIsIllusion = schema->TryGetOffset("C_DOTA_BaseNPC", "m_bIsIllusion", offsets.isIllusion);
		offsets.hasIsClone = schema->TryGetOffset("C_DOTA_BaseNPC", "m_bIsClone", offsets.isClone);

		offsets.resolved = hasHealth && hasMaxHealth && hasTeam && hasMana && hasMaxMana &&
						   hasAbilities && hasSceneNode && hasAbsOrigin && hasAbilityLevel &&
						   hasAbilityCooldown && hasAbilityMana;

		static ULONGLONG lastLogTick = 0;
		if (offsets.resolved || !lastLogTick || now - lastLogTick >= 300000)
		{
			DEV_LOG("[kill-stealer] offsets ready=%d hp=0x%X max=0x%X team=0x%X mana=0x%X abilities=0x%X inv=%d(0x%X+0x%X) ability(level=0x%X cd=0x%X mana=0x%X range=0x%X active=%d/0x%X) origin=0x%X+0x%X armor=%d/0x%X resist=%d/0x%X amp=%d/0x%X damage=0x%X+0x%X range=%d/0x%X player=%d/0x%X owner=%d/0x%X\n",
					offsets.resolved ? 1 : 0, offsets.health, offsets.maxHealth, offsets.team,
					offsets.mana, offsets.abilities, offsets.hasInventory ? 1 : 0, offsets.inventory,
					offsets.inventoryItems, offsets.abilityLevel, offsets.abilityCooldown,
					offsets.abilityManaCost, offsets.abilityCastRange,
					offsets.hasAbilityActivated ? 1 : 0, offsets.abilityActivated,
					offsets.sceneNode, offsets.absOrigin,
					offsets.hasArmor ? 1 : 0, offsets.armor,
					offsets.hasMagicResistance ? 1 : 0, offsets.magicResistance,
					offsets.hasSpellAmp ? 1 : 0, offsets.spellAmp,
					offsets.damageMin, offsets.damageBonus,
					offsets.hasAttackRange ? 1 : 0, offsets.attackRange,
					offsets.hasHeroPlayerId ? 1 : 0, offsets.heroPlayerId,
					offsets.hasPlayerOwnerId ? 1 : 0, offsets.playerOwnerId);
			lastLogTick = now;
		}

		return offsets;
	}

	auto TryLocalPlayerId(int &outPlayerId) -> bool
	{
		outPlayerId = -1;
		auto *engine = SDK::Interfaces::EngineToClient();
		if (!engine)
			return false;
		engine->GetLocalPlayer(outPlayerId, 0);
		return outPlayerId >= 0 && outPlayerId < 64;
	}

	auto BuildHeroCandidate(CGameEntitySystem *entitySystem, C_BaseEntity *entity, CEntityIdentity *identity,
							CHandle handle, const std::string &rawName, const KillStealerOffsets &offsets,
							bool requireHeroName, HeroCandidate &outHero) -> bool
	{
		if (!entitySystem || !entity || !offsets.resolved)
			return false;

		std::string heroName = rawName;
		bool hasHeroName = LooksLikeHeroName(heroName);
		if (!hasHeroName)
		{
			if (heroName.empty())
				heroName = EntityName(entity, identity);
			hasHeroName = LooksLikeHeroName(heroName);
		}
		if (requireHeroName && !hasHeroName)
			return false;
		if (heroName.empty())
			heroName = handle.IsValid() ? "hero_" + std::to_string(handle.GetEntryIndex()) : "hero";

		HeroCandidate hero{};
		hero.entity = entity;
		hero.handle = handle;
		hero.name = DisplayHeroName(heroName);
		hero.health = ReadField<int>(entity, offsets.health);
		hero.maxHealth = ReadField<int>(entity, offsets.maxHealth);
		hero.team = ReadField<uint8_t>(entity, offsets.team);
		if (hero.health <= 0 || hero.maxHealth <= 0 || hero.maxHealth > 50000)
			return false;
		if (hero.team != 2 && hero.team != 3)
			return false;

		if (offsets.hasIsIllusion && ReadField<bool>(entity, offsets.isIllusion))
			return false;
		if (offsets.hasIsClone && ReadField<bool>(entity, offsets.isClone))
			return false;
		if (!TryReadOrigin(entity, offsets, hero.origin))
			return false;

		hero.mana = ReadField<float>(entity, offsets.mana, 0.f);
		if (!std::isfinite(hero.mana) || hero.mana < 0.f || hero.mana > 100000.f)
			hero.mana = 0.f;
		hero.armor = offsets.hasArmor ? ReadField<float>(entity, offsets.armor, 0.f) : 0.f;
		hero.magicResistance = offsets.hasMagicResistance ? NormalizeResistance(ReadField<float>(entity, offsets.magicResistance, 0.25f), 0.25f) : 0.25f;
		hero.spellAmp = offsets.hasSpellAmp ? NormalizeSpellAmp(ReadField<float>(entity, offsets.spellAmp, 0.f)) : 0.f;
		hero.attackDamage = static_cast<float>((std::max)(1, ReadField<int>(entity, offsets.damageMin, 0) +
																 (offsets.hasDamageBonus ? ReadField<int>(entity, offsets.damageBonus, 0) : 0)));
		hero.attackRange = offsets.hasAttackRange ? static_cast<float>((std::max)(150, ReadField<int>(entity, offsets.attackRange, 150))) : 150.f;
		hero.playerId = offsets.hasHeroPlayerId ? ReadField<int>(entity, offsets.heroPlayerId, -1) : -1;
		hero.ownerId = offsets.hasPlayerOwnerId ? ReadField<int>(entity, offsets.playerOwnerId, -1) : -1;
		outHero = hero;
		return true;
	}

	auto RefreshHeroHandles(CGameEntitySystem *entitySystem, const KillStealerOffsets &offsets,
							std::vector<CHandle> &handles, HeroScanStats &stats, int &nextIndex) -> void
	{
		stats = {};
		if (!entitySystem || !offsets.resolved)
			return;

		constexpr size_t kMaxCachedHeroes = 24;
		constexpr int kEntityBudgetPerSlice = 160;
		constexpr int kMaxDiscoveryIndex = 4095;
		int highest = entitySystem->GetHighestEntityIndex();
		if (highest <= 0 || highest > kMaxDiscoveryIndex)
			highest = kMaxDiscoveryIndex;
		if (nextIndex < 0 || nextIndex > highest)
			nextIndex = 0;

		int lastChunk = -1;
		for (int scanned = 0; scanned < kEntityBudgetPerSlice; ++scanned)
		{
			const int index = nextIndex++;
			if (nextIndex > highest)
				nextIndex = 0;

			const int chunkIndex = index / MAX_ENTITIES_IN_LIST;
			if (chunkIndex != lastChunk)
			{
				lastChunk = chunkIndex;
				++stats.chunks;
			}
			++stats.entities;

			CEntityIdentity *identity = nullptr;
			C_BaseEntity *entity = nullptr;
			if (!TryEntityAtIndex(entitySystem, index, identity, entity))
				continue;

			int health = 0;
			int maxHealth = 0;
			uint8_t team = 0;
			if (!TryReadField(entity, offsets.health, health) ||
				!TryReadField(entity, offsets.maxHealth, maxHealth) ||
				!TryReadField(entity, offsets.team, team))
				continue;
			if (health <= 0 || maxHealth < 300 || maxHealth > 50000)
				continue;
			if (team != 2 && team != 3)
				continue;

			const int playerId = offsets.hasHeroPlayerId ? ReadField<int>(entity, offsets.heroPlayerId, -1) : -1;
			const int ownerId = offsets.hasPlayerOwnerId ? ReadField<int>(entity, offsets.playerOwnerId, -1) : -1;
			if (!IsLikelyPlayerId(playerId) && !IsLikelyPlayerId(ownerId))
				continue;

			if (offsets.hasIsIllusion && ReadField<bool>(entity, offsets.isIllusion))
				continue;
			if (offsets.hasIsClone && ReadField<bool>(entity, offsets.isClone))
				continue;

			if (AddUniqueHandle(handles, identity->Handle(), kMaxCachedHeroes))
				++stats.directHeroes;
		}
	}

	auto CollectHeroesFromHandles(CGameEntitySystem *entitySystem, const KillStealerOffsets &offsets,
								  const std::vector<CHandle> &handles) -> std::vector<HeroCandidate>
	{
		std::vector<HeroCandidate> heroes;
		if (!entitySystem || !offsets.resolved)
			return heroes;

		for (const auto &handle : handles)
		{
			CEntityIdentity *identity = nullptr;
			auto *entity = EntityFromHandle(entitySystem, handle, &identity);
			if (!entity)
				continue;

			HeroCandidate hero{};
			if (!BuildHeroCandidate(entitySystem, entity, identity, handle, {}, offsets, false, hero))
				continue;

			bool duplicate = false;
			for (const auto &existing : heroes)
			{
				if (existing.handle.m_Index == hero.handle.m_Index)
				{
					duplicate = true;
					break;
				}
			}
			if (!duplicate)
				heroes.push_back(hero);
		}

		return heroes;
	}

	auto ResolveLocalHero(CGameEntitySystem *entitySystem, const KillStealerOffsets &offsets, const std::vector<HeroCandidate> &heroes, HeroCandidate &outLocal) -> bool
	{
		// Phase 1: Try the most reliable Valve engine path first - get local player controller
		auto *controller = CGameEntitySystem::GetLocalPlayerController();
		if (controller)
		{
			const CHandle heroHandle = controller->m_hAssignedHero();
			if (heroHandle.IsValid())
			{
				auto *heroEntity = entitySystem->GetBaseEntityFromHandle(heroHandle);
				if (heroEntity)
				{
					HeroCandidate hero{};
					if (BuildHeroCandidate(entitySystem, heroEntity, nullptr, heroHandle, "", offsets, false, hero))
					{
						outLocal = hero;
						DEV_LOG("[kill-stealer] local-hero-resolved controller=1 assignedHero=%s\n", hero.name.c_str());
						return true;
					}
				}
				else
				{
					DEV_LOG("[kill-stealer] local-hero controller found but assignedHero entity null\n");
				}
			}
			else
			{
				DEV_LOG("[kill-stealer] local-hero controller found but assignedHero invalid\n");
			}
		}
		else
		{
			DEV_LOG("[kill-stealer] local-hero controller=0\n");
		}

		// Phase 2: Try to get local player ID from engine
		int localPlayerId = -1;
		const bool hasLocalPlayerId = TryLocalPlayerId(localPlayerId);
		if (hasLocalPlayerId)
		{
			DEV_LOG("[kill-stealer] local-hero playerId=%d\n", localPlayerId);
			for (const auto &hero : heroes)
			{
				if (hero.playerId == localPlayerId || hero.ownerId == localPlayerId)
				{
					outLocal = hero;
					DEV_LOG("[kill-stealer] local-hero resolved by playerId=%d hero=%s\n", localPlayerId, hero.name.c_str());
					return true;
				}
			}
			DEV_LOG("[kill-stealer] local-hero playerId=%d not found in heroes\n", localPlayerId);
		}
		else
		{
			DEV_LOG("[kill-stealer] local-hero TryLocalPlayerId failed\n");
		}

		// Phase 3: If we have exactly one hero, use it as local hero
		if (heroes.size() == 1)
		{
			outLocal = heroes.front();
			DEV_LOG("[kill-stealer] local-hero resolved as single hero: %s\n", heroes.front().name.c_str());
			return true;
		}

		// Phase 4: Last resort - try to find a hero that looks like our team
		if (!heroes.empty())
		{
			// Try to find a hero with the most common team number (usually our team)
			int teamCount[6] = {0, 0, 0, 0, 0, 0};
			for (const auto &hero : heroes)
			{
				if (hero.team >= 2 && hero.team <= 5)
					teamCount[hero.team]++;
			}
			// Pick the team with most heroes (assuming we're on that team)
			int bestTeam = 2;
			int bestCount = 0;
			for (int t = 2; t <= 5; t++)
			{
				if (teamCount[t] > bestCount)
				{
					bestCount = teamCount[t];
					bestTeam = t;
				}
			}
			// Find a hero on our suspected team
			for (const auto &hero : heroes)
			{
				if (hero.team == bestTeam)
				{
					outLocal = hero;
					DEV_LOG("[kill-stealer] local-hero resolved by team=%d hero=%s\n", bestTeam, hero.name.c_str());
					return true;
				}
			}
		}

		DEV_LOG("[kill-stealer] local-hero-resolved=0 no-local-hero\n");
		return false;
	}

	auto ReadHandleVector(const void *field, int maxCount, std::vector<CHandle> &out) -> bool
	{
		out.clear();
		NetworkHandleVector vector{};
		if (!TryRead(field, vector))
			return false;

		if (vector.size <= 0 || vector.size > maxCount || !vector.data ||
			!IsReadableRuntimeMemory(vector.data, sizeof(CHandle) * static_cast<size_t>(vector.size)))
			return false;

		out.resize(vector.size);
		std::memcpy(out.data(), vector.data, sizeof(CHandle) * static_cast<size_t>(vector.size));
		return true;
	}

	auto ReadInventoryHandles(C_BaseEntity *hero, const KillStealerOffsets &offsets, std::vector<CHandle> &out) -> bool
	{
		out.clear();
		if (!hero || !offsets.hasInventory)
			return false;

		const uintptr_t itemsBase = reinterpret_cast<uintptr_t>(hero) + offsets.inventory + offsets.inventoryItems;
		int32_t reportedSize = 0;
		if (!TryRead(reinterpret_cast<const void *>(itemsBase), reportedSize))
			return false;

		const CHandle *handles = nullptr;
		int count = 0;
		constexpr int kMaxInventorySlots = 27;

		if (reportedSize > 0 && reportedSize <= kMaxInventorySlots)
		{
			handles = reinterpret_cast<const CHandle *>(itemsBase + sizeof(int32_t));
			count = reportedSize;
		}
		else
		{
			handles = reinterpret_cast<const CHandle *>(itemsBase);
			count = 6;
		}

		if (!IsReadableRuntimeMemory(handles, sizeof(CHandle) * static_cast<size_t>(count)))
			return false;

		out.assign(handles, handles + count);
		return true;
	}

	auto ReadCastRange(C_BaseEntity *ability, const KillStealerOffsets &offsets, float fallback) -> float
	{
		float castRange = fallback;
		if (offsets.abilityCastRange)
		{
			int rangeInt = 0;
			if (TryReadField(ability, offsets.abilityCastRange, rangeInt) && rangeInt >= 50 && rangeInt <= 25000)
				castRange = static_cast<float>(rangeInt);
			else
			{
				float rangeFloat = 0.f;
				if (TryReadField(ability, offsets.abilityCastRange, rangeFloat) &&
					std::isfinite(rangeFloat) && rangeFloat >= 50.f && rangeFloat <= 25000.f)
					castRange = rangeFloat;
			}
		}

		if (castRange <= 0.f)
			castRange = 800.f;
		return castRange;
	}

	auto FindDamageEntry(const std::string &name) -> const AbilityDamageEntry *
	{
		auto *data = GetAbilityDamageData();
		if (!data)
			return nullptr;

		if (const auto *entry = data->Find(name))
			return entry;

		const std::string lowered = ToLower(name);
		if (lowered != name)
			return data->Find(lowered);

		return nullptr;
	}

	auto PreferredSlotForAbility(const std::string &name) -> int
	{
		auto *data = GetAbilityDamageData();
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

	auto Distance2D(const Vector3 &left, const Vector3 &right) -> float
	{
		const float dx = left.m_x - right.m_x;
		const float dy = left.m_y - right.m_y;
		return std::sqrt(dx * dx + dy * dy);
	}

	auto ProjectTargetScreen(const Vector3 &origin, ImVec2 &out) -> bool
	{
		Vector3 targetPoint = origin;
		targetPoint.m_z += 80.f;
		if (Math::WorldToScreen(targetPoint, out))
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

	auto MoveCursorToClientPoint(HWND window, const ImVec2 &screen, POINT &previousOut) -> bool
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

	auto ToPlanAction(const KillTool &tool) -> CKillStealer::KillPlanAction
	{
		CKillStealer::KillPlanAction action{};
		switch (tool.kind)
		{
		case KillToolKind::Item:
			action.kind = CKillStealer::KillPlanAction::Kind::Item;
			break;
		case KillToolKind::Attack:
			action.kind = CKillStealer::KillPlanAction::Kind::Attack;
			break;
		case KillToolKind::Ability:
		default:
			action.kind = CKillStealer::KillPlanAction::Kind::Ability;
			break;
		}

		action.name = tool.name;
		action.key = static_cast<uint16_t>(tool.key);
		action.delayMs = tool.delayMs;
		action.noTarget = tool.noTarget;
		action.unitTarget = tool.unitTarget;
		action.pointTarget = tool.pointTarget;
		return action;
	}

	auto CollectTools(CGameEntitySystem *entitySystem, const HeroCandidate &localHero,
					  const KillStealerOffsets &offsets) -> std::vector<KillTool>
	{
		std::vector<KillTool> tools;
		static constexpr std::array<WORD, 6> kAbilityKeys = {'Q', 'W', 'E', 'D', 'F', 'R'};
		static constexpr std::array<WORD, 6> kItemKeys = {'Z', 'X', 'C', 'V', 'B', 'N'};

		if (Settings::KillStealer::UseAbilities)
		{
			std::vector<CHandle> abilityHandles;
			const auto *field = reinterpret_cast<const void *>(reinterpret_cast<uintptr_t>(localHero.entity) + offsets.abilities);
			if (ReadHandleVector(field, 48, abilityHandles))
			{
				int fallbackSlot = 0;
				for (const auto &handle : abilityHandles)
				{
					CEntityIdentity *identity = nullptr;
					auto *ability = EntityFromHandle(entitySystem, handle, &identity);
					if (!ability)
						continue;

					const std::string abilityName = EntityName(ability, identity);
					const auto *data = FindDamageEntry(abilityName);
					if (!data || !data->IsUsableDamage() ||
						(!data->unitTarget && !data->noTarget && !data->pointTarget))
						continue;

					const int level = ReadField<int>(ability, offsets.abilityLevel, 0);
					if (level <= 0)
						continue;

					if (offsets.hasAbilityActivated && !ReadField<bool>(ability, offsets.abilityActivated, true))
						continue;

					const float cooldown = ReadField<float>(ability, offsets.abilityCooldown, 0.f);
					if (std::isfinite(cooldown) && cooldown > 0.15f)
						continue;

					int manaCost = ReadField<int>(ability, offsets.abilityManaCost, data->ManaForLevel(level));
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
					tool.kind = KillToolKind::Ability;
					tool.name = abilityName;
					tool.rawDamage = rawDamage;
					tool.castRange = data->noTarget ? 25000.f : ReadCastRange(ability, offsets, data->castRange);
					tool.manaCost = manaCost;
					tool.key = kAbilityKeys[preferredSlot];
					tool.damageType = data->damageType;
					tool.noTarget = data->noTarget;
					tool.unitTarget = data->unitTarget;
					tool.pointTarget = data->pointTarget;
					tool.delayMs = data->noTarget ? 90u : (Settings::KillStealer::QuickCast ? 120u : 180u);
					tools.push_back(tool);
				}
			}
		}

		if (Settings::KillStealer::UseItems)
		{
			std::vector<CHandle> itemHandles;
			if (ReadInventoryHandles(localHero.entity, offsets, itemHandles))
			{
				const int slotLimit = (std::min)(static_cast<int>(itemHandles.size()), static_cast<int>(kItemKeys.size()));
				for (int slot = 0; slot < slotLimit; ++slot)
				{
					CEntityIdentity *identity = nullptr;
					auto *item = EntityFromHandle(entitySystem, itemHandles[slot], &identity);
					if (!item)
						continue;

					const std::string itemName = EntityName(item, identity);
					const auto *data = FindDamageEntry(itemName);
					if (!data || (!data->unitTarget && !data->noTarget && !data->pointTarget))
						continue;

					// Ethereal Blade deals no direct damage itself but doubles the
					// magic damage the target takes for its duration, so it's kept
					// even though it fails the usual IsUsableDamage() gate below -
					// BuildKillPlan leads with it and amplifies the rest of the plan.
					const bool isEtherealBlade = Settings::KillStealer::PrioritizeEtherealBlade && itemName == "item_ethereal_blade";
					if (!isEtherealBlade && !data->IsUsableDamage())
						continue;

					const int level = (std::max)(1, ReadField<int>(item, offsets.abilityLevel, 1));
					const float cooldown = ReadField<float>(item, offsets.abilityCooldown, 0.f);
					if (std::isfinite(cooldown) && cooldown > 0.15f)
						continue;

					int manaCost = ReadField<int>(item, offsets.abilityManaCost, data->ManaForLevel(level));
					if (manaCost <= 0)
						manaCost = data->ManaForLevel(level);
					if (manaCost > static_cast<int>(localHero.mana + 0.5f))
						continue;

					const float rawDamage = data->DamageForLevel(level);
					if (!isEtherealBlade && rawDamage <= 0.f)
						continue;

					KillTool tool{};
					tool.kind = KillToolKind::Item;
					tool.name = itemName;
					tool.rawDamage = rawDamage;
					tool.castRange = data->noTarget ? 25000.f : ReadCastRange(item, offsets, data->castRange);
					tool.manaCost = manaCost;
					tool.key = kItemKeys[slot];
					tool.damageType = data->damageType;
					tool.noTarget = data->noTarget;
					tool.unitTarget = data->unitTarget;
					tool.pointTarget = data->pointTarget;
					tool.isDamageAmplifier = isEtherealBlade;
					tool.delayMs = data->noTarget ? 90u : (Settings::KillStealer::QuickCast ? 120u : 180u);
					tools.push_back(tool);
				}
			}
		}

		if (Settings::KillStealer::UseAutoAttack && localHero.attackDamage > 0.f && Settings::KillStealer::AttackKey > 0)
		{
			KillTool tool{};
			tool.kind = KillToolKind::Attack;
			tool.name = "auto_attack";
			tool.rawDamage = localHero.attackDamage;
			tool.castRange = localHero.attackRange + 75.f;
			tool.key = static_cast<WORD>(Settings::KillStealer::AttackKey);
			tool.damageType = AbilityDamageType::Physical;
			tool.unitTarget = true;
			tool.delayMs = 220u;
			tools.push_back(tool);
		}

		return tools;
	}

	struct KillPlanEvaluation
	{
		size_t actionCount = 0;
		float totalDamage = 0.f;
		float totalDelay = 0.f;
	};

	auto BuildKillPlan(const HeroCandidate &localHero, const HeroCandidate &target, const std::vector<KillTool> &tools,
					   uint32_t now, CKillStealer::KillPlanState &outPlan, KillPlanEvaluation &outEvaluation) -> bool
	{
		outPlan = {};
		outEvaluation = {};

		const float threshold = static_cast<float>(target.health) + Settings::KillStealer::HealthBuffer;
		const float distance = Distance2D(localHero.origin, target.origin);

		std::vector<size_t> usableIndices;
		usableIndices.reserve(tools.size());
		for (size_t index = 0; index < tools.size(); ++index)
		{
			const auto &tool = tools[index];
			if (!tool.noTarget && distance > tool.castRange + 75.f)
				continue;
			usableIndices.push_back(index);
		}

		if (usableIndices.empty())
			return false;
		if (usableIndices.size() >= 31)
			return false;

		struct BestSubset
		{
			uint32_t mask = 0;
			size_t actionCount = 0;
			float totalDamage = 0.f;
			float totalDelay = 0.f;
		};

		BestSubset best{};
		bool found = false;
		const uint32_t subsetLimit = 1u << static_cast<uint32_t>(usableIndices.size());
		for (uint32_t mask = 1; mask < subsetLimit; ++mask)
		{
			bool hasAmplifier = false;
			for (size_t bit = 0; bit < usableIndices.size(); ++bit)
			{
				if ((mask & (1u << static_cast<uint32_t>(bit))) && tools[usableIndices[bit]].isDamageAmplifier)
				{
					hasAmplifier = true;
					break;
				}
			}

			float totalDamage = 0.f;
			float totalDelay = 0.f;
			size_t actionCount = 0;

			for (size_t bit = 0; bit < usableIndices.size(); ++bit)
			{
				if (!(mask & (1u << static_cast<uint32_t>(bit))))
					continue;

				const auto &tool = tools[usableIndices[bit]];
				const bool amplifyThis = hasAmplifier && !tool.isDamageAmplifier && tool.damageType == AbilityDamageType::Magical;
				totalDamage += EffectiveDamage(tool, localHero, target, amplifyThis);
				totalDelay += static_cast<float>(tool.delayMs);
				++actionCount;
			}

			if (totalDamage < threshold)
				continue;

			const float currentOverkill = totalDamage - threshold;
			const float bestOverkill = best.totalDamage - threshold;
			const bool better =
				!found ||
				actionCount < best.actionCount ||
				(actionCount == best.actionCount && currentOverkill < bestOverkill - 0.01f) ||
				(actionCount == best.actionCount && std::abs(currentOverkill - bestOverkill) <= 0.01f &&
				 totalDelay < best.totalDelay - 0.01f);

			if (better)
			{
				best.mask = mask;
				best.actionCount = actionCount;
				best.totalDamage = totalDamage;
				best.totalDelay = totalDelay;
				found = true;
			}
		}

		if (!found)
			return false;

		std::vector<size_t> selectedIndices;
		for (size_t bit = 0; bit < usableIndices.size(); ++bit)
		{
			if (best.mask & (1u << static_cast<uint32_t>(bit)))
				selectedIndices.push_back(usableIndices[bit]);
		}

		const bool planHasAmplifier = std::any_of(selectedIndices.begin(), selectedIndices.end(),
			[&](size_t index) { return tools[index].isDamageAmplifier; });

		std::sort(selectedIndices.begin(), selectedIndices.end(), [&](size_t leftIndex, size_t rightIndex)
		{
			// Ethereal Blade (or any future damage amplifier) always leads the plan
			// so its magic-damage-taken buff is up before the nukes that benefit from it.
			if (tools[leftIndex].isDamageAmplifier != tools[rightIndex].isDamageAmplifier)
				return tools[leftIndex].isDamageAmplifier;

			const bool leftAmplified = planHasAmplifier && !tools[leftIndex].isDamageAmplifier && tools[leftIndex].damageType == AbilityDamageType::Magical;
			const bool rightAmplified = planHasAmplifier && !tools[rightIndex].isDamageAmplifier && tools[rightIndex].damageType == AbilityDamageType::Magical;
			const float leftDamage = EffectiveDamage(tools[leftIndex], localHero, target, leftAmplified);
			const float rightDamage = EffectiveDamage(tools[rightIndex], localHero, target, rightAmplified);
			if (std::abs(leftDamage - rightDamage) > 0.01f)
				return leftDamage > rightDamage;
			if (tools[leftIndex].noTarget != tools[rightIndex].noTarget)
				return tools[leftIndex].noTarget;
			if (tools[leftIndex].delayMs != tools[rightIndex].delayMs)
				return tools[leftIndex].delayMs < tools[rightIndex].delayMs;
			return leftIndex < rightIndex;
		});

		outPlan.targetHandle = target.handle.m_Index;
		outPlan.nextActionTick = now;
		outPlan.expiresAt = now + 2500u;
		outPlan.actionIndex = 0;
		outPlan.actions.clear();
		outPlan.actions.reserve(selectedIndices.size());
		for (const size_t index : selectedIndices)
			outPlan.actions.push_back(ToPlanAction(tools[index]));

		outEvaluation.actionCount = best.actionCount;
		outEvaluation.totalDamage = best.totalDamage;
		outEvaluation.totalDelay = best.totalDelay;
		return !outPlan.actions.empty();
	}

	auto FindHeroByHandle(const std::vector<HeroCandidate> &heroes, uint32_t handle) -> const HeroCandidate *
	{
		for (const auto &hero : heroes)
		{
			if (hero.handle.m_Index == handle)
				return &hero;
		}
		return nullptr;
	}

	auto CastPlanAction(const CKillStealer::KillPlanAction &action, const HeroCandidate &target) -> bool
	{
		const HWND window = WindowReadyForInput();
		if (!window || action.key == 0)
			return false;

		if (action.noTarget)
			return SendKeyPress(static_cast<WORD>(action.key));

		ImVec2 screen{};
		if (!ProjectTargetScreen(target.origin, screen))
			return false;

		POINT previous{};
		if (!MoveCursorToClientPoint(window, screen, previous))
			return false;

		const bool needsClick = action.kind == CKillStealer::KillPlanAction::Kind::Attack || !Settings::KillStealer::QuickCast;
		const bool sent = SendKeyPress(static_cast<WORD>(action.key)) && (!needsClick || SendLeftClick());
		SetCursorPos(previous.x, previous.y);
		return sent;
	}

	auto DrawMarkers(const HeroCandidate &localHero, const std::vector<HeroCandidate> &enemies,
					 const std::vector<KillTool> &tools, uint32_t now) -> void
	{
		if (!Settings::KillStealer::DrawKillableMarkers || !ImGui::GetCurrentContext())
			return;

		auto *drawList = ImGui::GetForegroundDrawList();
		for (const auto &enemy : enemies)
		{
			CKillStealer::KillPlanState plan{};
			[[maybe_unused]] KillPlanEvaluation evaluation{};
			if (!BuildKillPlan(localHero, enemy, tools, now, plan, evaluation))
				continue;

			Vector3 marker = enemy.origin;
			marker.m_z += 115.f;
			ImVec2 screen{};
			if (!Math::WorldToScreen(marker, screen))
				continue;

			drawList->AddCircleFilled(screen, 13.f, IM_COL32(218, 51, 62, 215), 24);
			drawList->AddCircle(screen, 16.f, IM_COL32(255, 235, 210, 235), 28, 2.f);
			const char *label = plan.actions.size() == 1 && plan.actions.front().kind == CKillStealer::KillPlanAction::Kind::Attack ? "AA" : "KS";
			const ImVec2 size = ImGui::CalcTextSize(label);
			drawList->AddText(ImVec2(screen.x - size.x * 0.5f, screen.y - size.y * 0.5f),
							  IM_COL32(255, 255, 255, 255), label);
		}
	}

	auto LogDebugSnapshot(const HeroCandidate *localHero, const std::vector<HeroCandidate> &enemies,
						  const std::vector<KillTool> &tools, uint32_t now, size_t cachedHandles, int scannedEntities,
						  int scannedChunks, int scannedControllers, int scannedDirectHeroes) -> void
	{
		if (!Settings::KillStealer::DrawDebugInfo)
			return;

		if (!localHero)
		{
			DEV_LOG("[kill-stealer] debug target=<none> reason=no-local-hero enemies=%zu tools=%zu cached=%zu scanned=%d chunks=%d controllers=%d direct_heroes=%d\n",
					enemies.size(), tools.size(), cachedHandles, scannedEntities, scannedChunks,
					scannedControllers, scannedDirectHeroes);
			return;
		}

		const HeroCandidate *target = nullptr;
		float nearest = 999999.f;
		for (const auto &enemy : enemies)
		{
			const float distance = Distance2D(localHero->origin, enemy.origin);
			if (distance < nearest)
			{
				nearest = distance;
				target = &enemy;
			}
		}

		if (!target)
		{
			DEV_LOG("[kill-stealer] debug target=<none> local=%s team=%u mana=%.0f enemies=0 tools=%zu cached=%zu scanned=%d chunks=%d controllers=%d direct_heroes=%d\n",
					localHero->name.c_str(), localHero->team, localHero->mana, tools.size(),
					cachedHandles, scannedEntities, scannedChunks, scannedControllers, scannedDirectHeroes);
			return;
		}

		float bestSingleDamage = 0.f;
		for (const auto &tool : tools)
		{
			if (!tool.noTarget && nearest > tool.castRange + 75.f)
				continue;
			bestSingleDamage = (std::max)(bestSingleDamage, EffectiveDamage(tool, *localHero, *target));
		}

		CKillStealer::KillPlanState plan{};
		KillPlanEvaluation evaluation{};
		const bool lethal = BuildKillPlan(*localHero, *target, tools, now, plan, evaluation);

		DEV_LOG("[kill-stealer] debug target=%s hp=%d/%d team=%u dist=%.0f armor=%.1f magic_resist=%.2f local=%s mana=%.0f spell_amp=%.2f tools=%zu best_single=%.1f plan_actions=%zu plan_damage=%.1f plan_delay=%.0f lethal=%d cached=%zu scanned=%d chunks=%d controllers=%d direct_heroes=%d\n",
				target->name.c_str(), target->health, target->maxHealth, target->team, nearest,
				target->armor, target->magicResistance, localHero->name.c_str(), localHero->mana,
				localHero->spellAmp, tools.size(), bestSingleDamage, evaluation.actionCount,
				evaluation.totalDamage, evaluation.totalDelay, lethal ? 1 : 0,
				cachedHandles, scannedEntities, scannedChunks, scannedControllers, scannedDirectHeroes);

		if (lethal)
		{
			for (const auto &action : plan.actions)
			{
				DEV_LOG("[kill-stealer] debug plan-step name=%s kind=%d key=%c click=%d target=%d\n",
						action.name.c_str(), static_cast<int>(action.kind),
						action.key ? static_cast<char>(action.key) : '?', action.noTarget ? 0 : 1,
						action.pointTarget || action.unitTarget ? 1 : 0);
			}
		}
	}
}

auto CKillStealer::CancelPlan() -> void
{
	m_Plan = {};
}

auto CKillStealer::OnRender() -> void
{
	const uint32_t now = GetTickCount();

	if (!Settings::KillStealer::Enable)
	{
		CancelPlan();
		m_HeroHandles.clear();
		m_NextHeroScanTick = 0;
		m_NextHeroScanIndex = 0;
		m_NextThinkTick = now;
		if (now - m_LastStatusLogTick >= 30000)
		{
			DEV_LOG("[kill-stealer] disabled enable=0 debug=%d abilities=%d items=%d attack=%d\n",
					Settings::KillStealer::DrawDebugInfo ? 1 : 0,
					Settings::KillStealer::UseAbilities ? 1 : 0,
					Settings::KillStealer::UseItems ? 1 : 0,
					Settings::KillStealer::UseAutoAttack ? 1 : 0);
			m_LastStatusLogTick = now;
		}
		return;
	}

	constexpr uint32_t kThinkIntervalMs = 100;
	if (now < m_NextThinkTick)
		return;
	m_NextThinkTick = now + kThinkIntervalMs;

	auto &offsets = ResolveOffsets();
	auto *entitySystem = SDK::Interfaces::GameEntitySystem();
	if (!entitySystem || !offsets.resolved)
	{
		CancelPlan();
		return;
	}

	if (now >= m_NextHeroScanTick || m_HeroHandles.empty())
	{
		HeroScanStats stats{};
		RefreshHeroHandles(entitySystem, offsets, m_HeroHandles, stats, m_NextHeroScanIndex);
		m_LastScanEntities = stats.entities;
		m_LastScanChunks = stats.chunks;
		m_LastScanControllers = stats.controllers;
		m_LastScanDirectHeroes = stats.directHeroes;
		m_NextHeroScanTick = now + (m_HeroHandles.empty() ? 100u : 250u);
	}

	if (m_HeroHandles.empty())
	{
		CancelPlan();
		if (now - m_LastDebugLogTick >= 3000)
		{
			LogDebugSnapshot(nullptr, {}, {}, now, m_HeroHandles.size(), m_LastScanEntities,
							 m_LastScanChunks, m_LastScanControllers, m_LastScanDirectHeroes);
			m_LastDebugLogTick = now;
		}
		return;
	}

	const auto heroes = CollectHeroesFromHandles(entitySystem, offsets, m_HeroHandles);
	if (heroes.empty() && !m_HeroHandles.empty())
	{
		m_HeroHandles.clear();
		m_NextHeroScanIndex = 0;
	}

	HeroCandidate localHero{};
	if (!ResolveLocalHero(entitySystem, offsets, heroes, localHero))
	{
		CancelPlan();
		if (now - m_LastDebugLogTick >= 3000)
		{
			LogDebugSnapshot(nullptr, heroes, {}, now, m_HeroHandles.size(), m_LastScanEntities,
							 m_LastScanChunks, m_LastScanControllers, m_LastScanDirectHeroes);
			m_LastDebugLogTick = now;
		}
		return;
	}

	std::vector<HeroCandidate> enemies;
	for (const auto &hero : heroes)
	{
		if (hero.handle.m_Index == localHero.handle.m_Index)
			continue;
		if (hero.team != localHero.team && (hero.team == 2 || hero.team == 3))
			enemies.push_back(hero);
	}

	if (enemies.empty())
	{
		CancelPlan();
		if (now - m_LastDebugLogTick >= 3000)
		{
			LogDebugSnapshot(&localHero, enemies, {}, now, m_HeroHandles.size(), m_LastScanEntities,
							 m_LastScanChunks, m_LastScanControllers, m_LastScanDirectHeroes);
			m_LastDebugLogTick = now;
		}
		return;
	}

	const auto tools = CollectTools(entitySystem, localHero, offsets);
	DrawMarkers(localHero, enemies, tools, now);

	if (now - m_LastDebugLogTick >= 3000)
	{
		LogDebugSnapshot(&localHero, enemies, tools, now, m_HeroHandles.size(), m_LastScanEntities,
						 m_LastScanChunks, m_LastScanControllers, m_LastScanDirectHeroes);
		m_LastDebugLogTick = now;
	}

	auto advancePlan = [&](const std::vector<HeroCandidate> &enemyList) -> bool
	{
		if (!m_Plan.active)
			return false;

		if (now >= m_Plan.expiresAt)
		{
			CancelPlan();
			return false;
		}

		const auto *target = FindHeroByHandle(enemyList, m_Plan.targetHandle);
		if (!target || target->health <= 0)
		{
			CancelPlan();
			return false;
		}

		if (m_Plan.actionIndex >= m_Plan.actions.size())
		{
			CancelPlan();
			return false;
		}

		if (now < m_Plan.nextActionTick)
			return true;

		const auto &action = m_Plan.actions[m_Plan.actionIndex];
		if (!CastPlanAction(action, *target))
		{
			CancelPlan();
			return false;
		}

		++m_Plan.actionIndex;
		if (m_Plan.actionIndex >= m_Plan.actions.size())
		{
			CancelPlan();
			return false;
		}

		m_Plan.nextActionTick = now + action.delayMs;
		return true;
	};

	if (advancePlan(enemies))
		return;

	CKillStealer::KillPlanState bestPlan{};
	KillPlanEvaluation bestEvaluation{};
	const HeroCandidate *bestTarget = nullptr;
	float bestDistance = 999999.f;
	float bestOverkill = 999999.f;

	for (const auto &enemy : enemies)
	{
		CKillStealer::KillPlanState candidatePlan{};
		KillPlanEvaluation candidateEvaluation{};
		if (!BuildKillPlan(localHero, enemy, tools, now, candidatePlan, candidateEvaluation))
			continue;

		const float distance = Distance2D(localHero.origin, enemy.origin);
		const float overkill = candidateEvaluation.totalDamage - (static_cast<float>(enemy.health) + Settings::KillStealer::HealthBuffer);
		const bool better =
			!bestTarget ||
			candidateEvaluation.actionCount < bestEvaluation.actionCount ||
			(candidateEvaluation.actionCount == bestEvaluation.actionCount && overkill < bestOverkill - 0.01f) ||
			(candidateEvaluation.actionCount == bestEvaluation.actionCount && std::abs(overkill - bestOverkill) <= 0.01f &&
			 candidateEvaluation.totalDelay < bestEvaluation.totalDelay - 0.01f) ||
			(candidateEvaluation.actionCount == bestEvaluation.actionCount && std::abs(overkill - bestOverkill) <= 0.01f &&
			 std::abs(candidateEvaluation.totalDelay - bestEvaluation.totalDelay) <= 0.01f && distance < bestDistance - 0.01f);

		if (better)
		{
			bestTarget = &enemy;
			bestPlan = candidatePlan;
			bestEvaluation = candidateEvaluation;
			bestDistance = distance;
			bestOverkill = overkill;
		}
	}

	if (!bestTarget)
	{
		CancelPlan();
		return;
	}

	m_Plan = bestPlan;
	m_Plan.active = true;
	m_Plan.nextActionTick = now;
	if (!advancePlan(enemies))
		CancelPlan();
}
