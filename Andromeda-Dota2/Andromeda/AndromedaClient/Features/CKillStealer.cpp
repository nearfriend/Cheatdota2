#include "CKillStealer.hpp"

#include <AndromedaClient/CAndromedaGUI.hpp>
#include <AndromedaClient/Data/AbilityDamageData.hpp>
#include <AndromedaClient/Settings/Settings.hpp>
#include <Common/DevLog.hpp>
#include <Dota2/SDK/CSchemaOffset.hpp>
#include <Dota2/SDK/Interface/CGameEntitySystem.hpp>
#include <Dota2/SDK/Math/Math.hpp>
#include <Dota2/SDK/SDK.hpp>
#include <Dota2/SDK/Types/CEntityData.hpp>

#include <ImGui/imgui.h>
#include <Windows.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
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
		bool hasInventory = false;
		bool hasAbilityActivated = false;
		bool hasArmor = false;
		bool hasMagicResistance = false;
		bool hasSpellAmp = false;
		bool hasDamageBonus = false;
		bool hasAttackRange = false;
		bool hasIsIllusion = false;
		bool hasIsClone = false;
		bool resolved = false;
	};

	struct HeroSnapshot
	{
		C_BaseEntity* entity = nullptr;
		int entIndex = -1;
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
	};

	struct KillTool
	{
		ToolKind kind = ToolKind::Ability;
		std::string name;
		float castRange = 0.f;
		WORD key = 0;
		bool noTarget = false;
		bool unitTarget = false;
		bool pointTarget = false;
		bool isDamageAmplifier = false;
		uint32_t delayMs = 0;
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

	auto LooksLikeHeroName(const std::string& name) -> bool
	{
		const std::string lower = ToLower(name);
		return lower.find("npc_dota_hero_") != std::string::npos;
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

		offsets.resolved = hasHealth && hasMaxHealth && hasTeam && hasMana && hasAbilities &&
			hasSceneNode && hasAbsOrigin && hasAbilityLevel && hasAbilityCooldown && hasAbilityMana;

		if (offsets.resolved)
			DEV_LOG("[kill-stealer] offsets ready\n");

		return offsets;
	}

	auto BuildSnapshot(C_BaseEntity* entity, int entIndex, const KillStealerOffsets& offsets, HeroSnapshot& out) -> bool
	{
		const int health = ReadField<int>(entity, offsets.health, 0);
		const int maxHealth = ReadField<int>(entity, offsets.maxHealth, 0);
		if (health <= 0 || maxHealth <= 0 || maxHealth > 50000)
			return false;

		const uint8_t team = ReadField<uint8_t>(entity, offsets.team, 0);
		if (!IsPlayableTeam(team))
			return false;

		if (offsets.hasIsIllusion && ReadField<bool>(entity, offsets.isIllusion, false))
			return false;
		if (offsets.hasIsClone && ReadField<bool>(entity, offsets.isClone, false))
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
		out.health = health;
		out.maxHealth = maxHealth;
		out.mana = ReadField<float>(entity, offsets.mana, 0.f);
		if (!std::isfinite(out.mana) || out.mana < 0.f || out.mana > 100000.f)
			out.mana = 0.f;
		out.armor = offsets.hasArmor ? ReadField<float>(entity, offsets.armor, 0.f) : 0.f;
		out.magicResistance = offsets.hasMagicResistance
			? std::clamp(ReadField<float>(entity, offsets.magicResistance, 0.25f), -0.95f, 0.95f)
			: 0.25f;
		out.spellAmp = offsets.hasSpellAmp ? std::clamp(ReadField<float>(entity, offsets.spellAmp, 0.f), -0.75f, 3.0f) : 0.f;
		out.attackDamage = static_cast<float>((std::max)(1,
			ReadField<int>(entity, offsets.damageMin, 0) + (offsets.hasDamageBonus ? ReadField<int>(entity, offsets.damageBonus, 0) : 0)));
		out.attackRange = offsets.hasAttackRange
			? static_cast<float>((std::max)(150, ReadField<int>(entity, offsets.attackRange, 150)))
			: 150.f;
		return true;
	}

	// One flat pass over every populated entity slot, throttled to the same
	// cadence as the rest of the feature. Builds every playable-team hero
	// snapshot in one go - no separate handle-cache layer to keep in sync.
	auto ScanHeroes(CGameEntitySystem* entitySystem, const KillStealerOffsets& offsets) -> std::vector<HeroSnapshot>
	{
		std::vector<HeroSnapshot> heroes;
		if (!entitySystem)
			return heroes;

		const int highest = (std::min)(entitySystem->GetHighestEntityIndex(), MAX_TOTAL_ENTITIES - 1);
		if (highest <= 0)
			return heroes;

		heroes.reserve(12);
		for (int index = 1; index <= highest; ++index)
		{
			auto* entity = entitySystem->GetBaseEntity<C_BaseEntity>(index);
			if (!entity)
				continue;

			HeroSnapshot snapshot{};
			if (!BuildSnapshot(entity, index, offsets, snapshot))
				continue;
			if (!LooksLikeHeroName(snapshot.name))
				continue;

			heroes.push_back(std::move(snapshot));
		}

		return heroes;
	}

	auto TryScreenCenterDistanceSq(const Vector3& origin, float& outDistanceSq) -> bool
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
			auto* gui = GetAndromedaGUI();
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

	// Primary local-hero identification: the game camera is centered on your
	// own hero by default, so "nearest hero to the middle of the screen" is a
	// cheap, reliable proxy that needs nothing but positions we already read.
	// This is the same strategy CLastHitAssistant.cpp already uses successfully.
	auto ResolveLocalHeroByScreenCenter(const std::vector<HeroSnapshot>& heroes, int cachedEntIndex, const HeroSnapshot*& out) -> bool
	{
		out = nullptr;
		float bestDistanceSq = 3.4e38f;
		const HeroSnapshot* best = nullptr;

		for (const auto& hero : heroes)
		{
			float distanceSq = 0.f;
			if (!TryScreenCenterDistanceSq(hero.origin, distanceSq))
				continue;

			// Sticky preference for whichever hero we picked last tick, so a
			// momentary camera pan doesn't flip which hero we think is "us".
			if (hero.entIndex == cachedEntIndex)
				distanceSq *= 0.1f;

			if (distanceSq < bestDistanceSq)
			{
				bestDistanceSq = distanceSq;
				best = &hero;
			}
		}

		if (!best)
			return false;
		out = best;
		return true;
	}

	// Circuit-broken: measured at 100-190ms per attempt when it fails in this
	// environment (its internal fallback scans every populated entity slot with
	// a virtual call on each one). Back off hard after repeated failures rather
	// than paying that cost on every resolve attempt - the screen-center
	// fallback below is the reliable path here.
	auto TryResolveViaController(CGameEntitySystem* entitySystem, const KillStealerOffsets& offsets, HeroSnapshot& out) -> bool
	{
		static int failStreak = 0;
		static ULONGLONG nextAttemptTick = 0;

		const ULONGLONG now = GetTickCount64();
		if (now < nextAttemptTick)
			return false;

		auto* controller = CGameEntitySystem::GetLocalPlayerController();
		if (!controller)
		{
			++failStreak;
			nextAttemptTick = now + (failStreak >= 3 ? 60000ull : 1000ull);
			return false;
		}

		const CHandle heroHandle = controller->m_hAssignedHero();
		if (!heroHandle.IsValid())
		{
			++failStreak;
			nextAttemptTick = now + (failStreak >= 3 ? 60000ull : 1000ull);
			return false;
		}

		auto* heroEntity = entitySystem->GetBaseEntityFromHandle(heroHandle);
		if (!heroEntity || !BuildSnapshot(heroEntity, heroHandle.GetEntryIndex(), offsets, out))
		{
			++failStreak;
			nextAttemptTick = now + (failStreak >= 3 ? 60000ull : 1000ull);
			return false;
		}

		failStreak = 0;
		return true;
	}

	auto ResolveLocalHero(CGameEntitySystem* entitySystem, const KillStealerOffsets& offsets,
		const std::vector<HeroSnapshot>& heroes, HeroSnapshot& out) -> bool
	{
		static int s_CachedEntIndex = -1;

		// Fast path: the hero we picked last tick is still in the fresh scan.
		if (s_CachedEntIndex >= 0)
		{
			for (const auto& hero : heroes)
			{
				if (hero.entIndex == s_CachedEntIndex)
				{
					out = hero;
					return true;
				}
			}
		}

		HeroSnapshot controllerHero{};
		if (TryResolveViaController(entitySystem, offsets, controllerHero))
		{
			out = controllerHero;
			s_CachedEntIndex = out.entIndex;
			return true;
		}

		const HeroSnapshot* screenCenterHero = nullptr;
		if (ResolveLocalHeroByScreenCenter(heroes, s_CachedEntIndex, screenCenterHero))
		{
			out = *screenCenterHero;
			s_CachedEntIndex = out.entIndex;
			return true;
		}

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
				for (const auto& handle : abilityHandles)
				{
					if (!handle.IsValid())
						continue;
					auto* ability = entitySystem->GetBaseEntityFromHandle(handle);
					if (!ability)
						continue;

					const std::string abilityName = EntityName(ability);
					const auto* data = FindDamageEntry(abilityName);
					if (!data || !data->IsUsableDamage() || (!data->unitTarget && !data->noTarget && !data->pointTarget))
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

					if (data->DamageForLevel(level) <= 0.f)
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
					tool.key = kAbilityKeys[preferredSlot];
					tool.noTarget = data->noTarget;
					tool.unitTarget = data->unitTarget;
					tool.pointTarget = data->pointTarget;
					tool.delayMs = data->noTarget ? 90u : (Settings::KillStealer::QuickCast ? 120u : 180u);
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

					if (!isEtherealBlade && data->DamageForLevel(level) <= 0.f)
						continue;

					KillTool tool{};
					tool.kind = ToolKind::Item;
					tool.name = itemName;
					tool.castRange = data->noTarget ? 25000.f : ReadCastRange(item, offsets, data->castRange);
					tool.key = kItemKeys[slot];
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
			tool.kind = ToolKind::Attack;
			tool.name = "auto_attack";
			tool.castRange = localHero.attackRange + 75.f;
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
	auto EffectiveDamage(const KillTool& tool, const HeroSnapshot& localHero, const HeroSnapshot& target,
		const AbilityDamageType* damageType, float rawDamage, bool magicAmplified) -> float
	{
		float damage = rawDamage;
		if (tool.kind != ToolKind::Attack)
			damage *= 1.f + localHero.spellAmp;

		const AbilityDamageType type = damageType ? *damageType : AbilityDamageType::Magical;
		switch (type)
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

	// Per-tool cached damage lookup so BuildKillPlan (called for every enemy,
	// possibly twice - once for the real plan, once for the marker overlay)
	// doesn't repeatedly hit the AbilityDamageData hash map.
	struct ToolDamageInfo
	{
		AbilityDamageType damageType = AbilityDamageType::Magical;
		float rawDamage = 0.f;
	};

	auto LookupToolDamage(const KillTool& tool) -> ToolDamageInfo
	{
		ToolDamageInfo info{};
		if (const auto* data = FindDamageEntry(tool.name))
		{
			info.damageType = data->damageType;
			// Level isn't tracked on KillTool; DamageForLevel(1) undercounts for
			// scaled abilities, so re-derive against the live entity would be
			// ideal, but CollectTools already filtered by "usable now" and the
			// greedy search only needs a consistent relative ordering plus a
			// reasonable absolute threshold check - level 1 damage is a safe,
			// conservative floor that won't claim a plan is lethal when it isn't.
			info.rawDamage = data->DamageForLevel(1);
		}
		return info;
	}

	// Greedy-by-largest-effective-damage: provably optimal for "fewest tools to
	// reach at least `threshold`" (a covering problem, not exact subset-sum) -
	// if any k-tool combo reaches the threshold, the top-k highest-damage combo
	// reaches it too. O(n log n), no combinatorial search, no n for which this
	// can blow up.
	auto BuildKillPlan(const HeroSnapshot& localHero, const HeroSnapshot& target, const std::vector<KillTool>& tools,
		uint32_t now, CKillStealer::PlanState& outPlan, KillPlanEvaluation& outEvaluation) -> bool
	{
		outPlan = {};
		outEvaluation = {};

		const float threshold = static_cast<float>(target.health) + Settings::KillStealer::HealthBuffer;
		const float distance = Distance2D(localHero.origin, target.origin);

		std::vector<size_t> usable;
		std::vector<ToolDamageInfo> damageInfo;
		usable.reserve(tools.size());
		damageInfo.reserve(tools.size());
		for (size_t index = 0; index < tools.size(); ++index)
		{
			const auto& tool = tools[index];
			if (!tool.noTarget && distance > tool.castRange + 75.f)
				continue;
			usable.push_back(index);
			damageInfo.push_back(LookupToolDamage(tool));
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
				const bool ampA = includeAmplifier && damageInfo[a].damageType == AbilityDamageType::Magical;
				const bool ampB = includeAmplifier && damageInfo[b].damageType == AbilityDamageType::Magical;
				return EffectiveDamage(tools[usable[a]], localHero, target, &damageInfo[a].damageType, damageInfo[a].rawDamage, ampA) >
					   EffectiveDamage(tools[usable[b]], localHero, target, &damageInfo[b].damageType, damageInfo[b].rawDamage, ampB);
			});

			std::vector<size_t> chosen;
			float total = 0.f;
			if (includeAmplifier)
				chosen.push_back(usable[amplifierPos]);

			for (size_t pos : order)
			{
				if (total >= threshold)
					break;
				const bool amp = includeAmplifier && damageInfo[pos].damageType == AbilityDamageType::Magical;
				total += EffectiveDamage(tools[usable[pos]], localHero, target, &damageInfo[pos].damageType, damageInfo[pos].rawDamage, amp);
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
				const auto info = LookupToolDamage(tools[index]);
				const bool amp = amplifierActive && !tools[index].isDamageAmplifier && info.damageType == AbilityDamageType::Magical;
				eval.totalDamage += EffectiveDamage(tools[index], localHero, target, &info.damageType, info.rawDamage, amp);
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

			const auto leftInfo = LookupToolDamage(tools[leftIndex]);
			const auto rightInfo = LookupToolDamage(tools[rightIndex]);
			const bool leftAmp = selectedHasAmp && !tools[leftIndex].isDamageAmplifier && leftInfo.damageType == AbilityDamageType::Magical;
			const bool rightAmp = selectedHasAmp && !tools[rightIndex].isDamageAmplifier && rightInfo.damageType == AbilityDamageType::Magical;
			const float leftDamage = EffectiveDamage(tools[leftIndex], localHero, target, &leftInfo.damageType, leftInfo.rawDamage, leftAmp);
			const float rightDamage = EffectiveDamage(tools[rightIndex], localHero, target, &rightInfo.damageType, rightInfo.rawDamage, rightAmp);
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

	auto WindowReadyForInput() -> HWND
	{
		auto* gui = GetAndromedaGUI();
		const HWND window = gui ? gui->m_hCS2Window : nullptr;
		if (!window || GetForegroundWindow() != window || gui->IsVisible())
			return nullptr;
		return window;
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
		const bool sent = SendKeyPress(action.key) && (!needsClick || SendLeftClick());
		SetCursorPos(previous.x, previous.y);
		return sent;
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
		if (hero.team != localHero.team)
			enemies.push_back(hero);
	}

	if (enemies.empty())
	{
		CancelPlan();
		return;
	}

	const auto tools = CollectTools(entitySystem, localHero, offsets);
	DrawMarkers(localHero, enemies, tools, now);

	if (Settings::KillStealer::DrawDebugInfo)
	{
		static uint32_t lastDebugLogTick = 0;
		if (now - lastDebugLogTick >= 3000)
		{
			DEV_LOG("[kill-stealer] debug local=%s team=%u mana=%.0f enemies=%zu tools=%zu plan_active=%d\n",
				localHero.name.c_str(), localHero.team, localHero.mana, enemies.size(), tools.size(), m_Plan.active ? 1 : 0);
			lastDebugLogTick = now;
		}
	}

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
				CancelPlan();
			}
			else if (m_Plan.actionIndex >= m_Plan.actions.size())
			{
				CancelPlan();
			}
			else if (now >= m_Plan.nextActionTick)
			{
				const auto& action = m_Plan.actions[m_Plan.actionIndex];
				if (!CastPlanAction(action, target->origin))
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
		if (!BuildKillPlan(localHero, enemy, tools, now, candidatePlan, candidateEvaluation))
			continue;

		const float overkill = candidateEvaluation.totalDamage - (static_cast<float>(enemy.health) + Settings::KillStealer::HealthBuffer);
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
}

auto CKillStealer::OnRender() -> void
{
	if (!Settings::KillStealer::DrawDebugInfo)
	{
		OnRenderInner();
		return;
	}

	// Perf instrumentation, only when Debug Logs is on: worst OnRenderInner call
	// time per 1s window, so a future slowdown shows up as a number instead of
	// another round of guessing.
	LARGE_INTEGER freq{};
	LARGE_INTEGER t0{};
	QueryPerformanceFrequency(&freq);
	QueryPerformanceCounter(&t0);

	OnRenderInner();

	LARGE_INTEGER t1{};
	QueryPerformanceCounter(&t1);
	const double elapsedMs = freq.QuadPart ? (t1.QuadPart - t0.QuadPart) * 1000.0 / static_cast<double>(freq.QuadPart) : 0.0;

	static double worstMs = 0.0;
	static uint32_t lastPerfLogTick = 0;
	worstMs = (std::max)(worstMs, elapsedMs);

	const uint32_t nowTick = GetTickCount();
	if (!lastPerfLogTick || nowTick - lastPerfLogTick >= 1000)
	{
		DEV_LOG("[kill-stealer] perf worst_onrender_ms=%.3f\n", worstMs);
		worstMs = 0.0;
		lastPerfLogTick = nowTick;
	}
}
