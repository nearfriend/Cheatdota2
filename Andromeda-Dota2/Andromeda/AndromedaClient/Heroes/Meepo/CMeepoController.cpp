#include "CMeepoController.hpp"

#include <Dota2/SDK/Interface/CGameEntitySystem.hpp>
#include <Dota2/SDK/Types/CEntityData.hpp>
#include <Dota2/SDK/Types/CHandle.hpp>
#include <Dota2/SDK/SDK.hpp>
#include <Dota2/SDK/Update/CDOTAInput.hpp>
#include <Dota2/SDK/Update/CUserCmd.hpp>
#include <Dota2/SDK/CSchemaOffset.hpp>
#include <Dota2/SDK/Update/AbilityOffsets.hpp>

#include <Windows.h>
#include <algorithm>
#include <cstring>
#include <cstdio>
#include <unordered_map>

// Helper function to check if memory is readable (used by multiple functions)
inline bool IsReadable(const void* ptr, size_t size = 1)
{
	if (!ptr)
		return false;
	MEMORY_BASIC_INFORMATION mbi{};
	if (!VirtualQuery(ptr, &mbi, sizeof(mbi)))
		return false;
	const DWORD protect = mbi.Protect & ~(PAGE_GUARD | PAGE_NOACCESS);
	const bool committed = mbi.State == MEM_COMMIT;
	const bool canRead = protect == PAGE_READONLY || protect == PAGE_READWRITE ||
		protect == PAGE_WRITECOPY || protect == PAGE_EXECUTE_READ ||
		protect == PAGE_EXECUTE_READWRITE || protect == PAGE_EXECUTE_WRITECOPY;
	const auto regionEnd = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
	const auto ptrEnd = reinterpret_cast<uintptr_t>(ptr) + size;
	return committed && canRead && ptrEnd <= regionEnd;
}

void CMeepoController::OnCreateMove(CDOTAInput* /*pCDOTAInput*/, CUserCmd* /*pCUserCmd*/)
{
	static int s_nCallCount = 0;
	s_nCallCount++;

	// Log first few calls to verify OnCreateMove is being called
	if (s_nCallCount <= 5)
	{
		DEV_LOG("[meepo] OnCreateMove called (call #%d)\n", s_nCallCount);
	}

	if (!ResolveLocalHero())
	{
		// Log why hero resolution failed (first few times)
		if (s_nCallCount <= 5)
		{
			DEV_LOG("[meepo] OnCreateMove: Hero not resolved (call #%d)\n", s_nCallCount);
		}
		return;
	}

	RefreshAbilityList();
}

void CMeepoController::OnEntityAdded(CEntityInstance* pInst)
{
	if (!pInst)
		return;

	// Cast to hero
	auto* pHero = static_cast<C_DOTA_BaseNPC_Hero*>(pInst);
	if (!pHero)
		return;

	// In Demo mode, the first hero found is usually the player's hero
	// Store it (we can verify later if needed)
	if (!m_pHero)
	{
		m_pHero = pHero;
		DEV_LOG("[meepo] Hero set via OnEntityAdded: %p\n", m_pHero);

		// Try to verify it's Meepo by checking entity name (may work later when name is initialized)
		// For now, just store it and we'll verify in RefreshAbilityList
	}
}

void CMeepoController::OnAbilityAdded(CEntityInstance* pInst)
{
	if (!pInst || !m_pHero)
		return;

	// Cast to ability
	auto* ability = static_cast<C_DOTABaseAbility*>(pInst);
	if (!ability)
		return;

	// CRITICAL: Don't try to read names here - entity identity may not be initialized yet
	// Just store the ability pointer. Names will be read in RefreshAbilityList when they're available.
	// This matches InvokerController's approach - it only reads names in RefreshAbilityList, not OnAbilityAdded

	// Check if this ability is already in our list (by pointer, not name)
	bool found = false;
	for (const auto& existing : m_Abilities)
	{
		if (existing.pAbility == ability)
		{
			found = true;
			break;
		}
	}

	if (!found)
	{
		MeepoAbilityInfo info;
		info.name = "";  // Will be filled in RefreshAbilityList
		info.pAbility = ability;

		// Try to get ability properties using schema offsets
		uint32_t offset = 0;

		if (GetSchemaOffset()->TryGetOffset("C_DOTABaseAbility", "m_iLevel", offset) && offset > 0)
		{
			info.level = *reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(ability) + offset);
		}

		if (GetSchemaOffset()->TryGetOffset("C_DOTABaseAbility", "m_flCooldown", offset) && offset > 0)
		{
			info.cooldown = *reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(ability) + offset);
		}

		if (GetSchemaOffset()->TryGetOffset("C_DOTABaseAbility", "m_flCooldownLength", offset) && offset > 0)
		{
			info.cooldownLength = *reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(ability) + offset);
		}

		if (GetSchemaOffset()->TryGetOffset("C_DOTABaseAbility", "m_iManaCost", offset) && offset > 0)
		{
			info.manaCost = *reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(ability) + offset);
		}

		if (GetSchemaOffset()->TryGetOffset("C_DOTABaseAbility", "m_flCastRange", offset) && offset > 0)
		{
			info.castRange = *reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(ability) + offset);
		}

		if (GetSchemaOffset()->TryGetOffset("C_DOTABaseAbility", "m_bIsActivated", offset) && offset > 0)
		{
			info.isActivated = *reinterpret_cast<bool*>(reinterpret_cast<uint8_t*>(ability) + offset);
		}

		m_Abilities.push_back(info);
		DEV_LOG("[meepo] Ability added to list: %p (name will be read in RefreshAbilityList, total: %zu)\n", ability, m_Abilities.size());
	}
}

bool CMeepoController::ResolveLocalHero()
{
	// If hero was already captured via OnEntityAdded, use it
	if (m_pHero)
	{
		static bool s_bLoggedCached = false;
		if (!s_bLoggedCached)
		{
			DEV_LOG("[CHEAT_ENGINE] Hero address (Meepo, cached): %p\n", m_pHero);
			s_bLoggedCached = true;
		}
		return true;
	}

	// EXACT SAME APPROACH AS INVOKER: Use controller
	auto* pController = CGameEntitySystem::GetLocalPlayerController();
	if (!pController)
	{
		static int s_nControllerFailCount = 0;
		if (s_nControllerFailCount++ < 3)
		{
			DEV_LOG("[meepo] ResolveLocalHero: Controller is null (Demo mode?) - trying entity search fallback\n");
		}

		// FALLBACK FOR DEMO MODE: Search for hero by iterating entities
		return ResolveHeroByEntitySearch();
	}

	const auto heroHandle = pController->m_hAssignedHero();
	if (!heroHandle.IsValid())
	{
		static int s_nHandleFailCount = 0;
		if (s_nHandleFailCount++ < 3)
		{
			DEV_LOG("[meepo] ResolveLocalHero: Hero handle is invalid\n");
		}
		m_pHero = nullptr;
		return false;
	}

	m_pHero = static_cast<C_DOTA_BaseNPC_Hero*>(
		SDK::Interfaces::GameEntitySystem()->GetBaseEntityFromHandle(heroHandle)
		);

	// Log hero address for Cheat Engine - this is where Meepo's hero object is in memory
	if (m_pHero)
	{
		DEV_LOG("[CHEAT_ENGINE] Hero address (Meepo): %p\n", m_pHero);
		DEV_LOG("[CHEAT_ENGINE] Hero handle: 0x%08X\n", heroHandle.Get());
	}
	else
	{
		static int s_nHeroFailCount = 0;
		if (s_nHeroFailCount++ < 3)
		{
			DEV_LOG("[meepo] ResolveLocalHero: Failed to get hero from handle 0x%08X\n", heroHandle.Get());
		}
	}

	return m_pHero != nullptr;
}

bool CMeepoController::ResolveHeroByEntitySearch()
{
	// Fallback method for Demo mode: search through entities to find hero
	auto* pGES = SDK::Interfaces::GameEntitySystem();
	if (!pGES)
		return false;

	static bool s_bSearched = false;
	if (s_bSearched && m_pHero)
		return true; // Already found, use cached

	constexpr int MAX_ENTITIES_TO_SEARCH = 2048;
	int highestIndex = pGES->GetHighestEntityIndex();
	if (highestIndex < 0 || highestIndex > MAX_ENTITIES_TO_SEARCH)
		highestIndex = MAX_ENTITIES_TO_SEARCH;

	static int s_nSearchAttempts = 0;
	s_nSearchAttempts++;

	if (s_nSearchAttempts <= 3)
	{
		DEV_LOG("[meepo] Starting entity search for hero (attempt #%d, max entities: %d)\n", s_nSearchAttempts, highestIndex);
	}

	int heroCount = 0;
	int consecutiveNulls = 0;

	for (int idx = 0; idx < highestIndex; ++idx)
	{
		try
		{
			auto* pEntity = pGES->GetBaseEntity<C_BaseEntity>(idx);
			if (!pEntity || !IsReadable(pEntity))
			{
				consecutiveNulls++;
				if (consecutiveNulls > 1000)
					break;
				continue;
			}

			consecutiveNulls = 0;

			// Try casting to hero
			auto* pHero = static_cast<C_DOTA_BaseNPC_Hero*>(pEntity);
			if (!pHero || !IsReadable(pHero))
				continue;

			// Verify it has identity
			CEntityIdentity* id = nullptr;
			try
			{
				id = pHero->pEntityIdentity();
			}
			catch (...)
			{
				continue;
			}

			if (!id || !IsReadable(id))
				continue;

			heroCount++;

			// In Demo mode, accept the first hero found
			if (heroCount == 1)
			{
				m_pHero = pHero;
				s_bSearched = true;

				DEV_LOG("[CHEAT_ENGINE] Hero address (Meepo, via entity search): %p\n", m_pHero);
				DEV_LOG("[meepo] Found hero at entity index %d (searched %d entities, found %d heroes)\n", idx, idx + 1, heroCount);
				return true;
			}
		}
		catch (...)
		{
			continue;
		}
	}

	if (s_nSearchAttempts <= 3)
	{
		DEV_LOG("[meepo] Entity search complete: found %d heroes, searched %d entities\n", heroCount, highestIndex);
	}

	return false;
}

bool CMeepoController::RefreshAbilityList()
{
	if (!m_pHero)
		return false;

	// Try schema offset first, fallback to static client offset
	uint32_t offset = 0;
	bool useSchemaOffset = GetSchemaOffset()->TryGetOffset( "C_DOTA_BaseNPC" , "m_vecAbilities" , offset ) && offset > 0;

	if ( !useSchemaOffset )
	{
		offset = BaseNPCOffsets::m_vecAbilities;
		static bool warned = false;
		if ( !warned )
		{
			DEV_LOG( "[meepo] Using fallback m_vecAbilities offset: 0x%X\n" , offset );
			warned = true;
		}
	}
	else
	{
		static bool warned = false;
		if ( !warned )
		{
			DEV_LOG( "[meepo] Using schema m_vecAbilities offset: 0x%X\n" , offset );
			warned = true;
		}
	}

	// CRITICAL: Try both approaches - CUtlVector and raw array
	// The offset might point to the CUtlVector structure OR directly to the data array
	try
	{
		// Approach 1: Try as CUtlVector<CHandle> (as defined in schema)
		auto* pAbilitiesVector = reinterpret_cast<CUtlVector<CHandle>*>(
			reinterpret_cast<uint8_t*>(m_pHero) + offset
			);

		if (!pAbilitiesVector || !IsReadable(pAbilitiesVector))
		{
			DEV_LOG("[meepo] Failed to get abilities vector pointer (not readable)\n");
			return false;
		}

		// Try to read count - if it's reasonable, use CUtlVector approach
		int count = 0;
		try
		{
			count = pAbilitiesVector->Count();
		}
		catch (...)
		{
			count = -1;
		}

		// If count is invalid, try Approach 2: raw array (like InvokerController)
		if (count < 0 || count > 24)
		{
			DEV_LOG("[meepo] CUtlVector approach failed (count=%d), trying raw array approach\n", count);

			// Approach 2: Treat as raw CHandle array (like InvokerController does)
			auto* abilityHandles = reinterpret_cast<CHandle*>(
				reinterpret_cast<uint8_t*>(m_pHero) + offset
				);

			if (!abilityHandles || !IsReadable(abilityHandles))
			{
				DEV_LOG("[meepo] Raw array approach also failed (not readable)\n");
				return false;
			}

			std::vector<MeepoAbilityInfo> fresh;
			fresh.reserve(kMaxAbilitySlots);

			static bool loggedOnce = false;
			if (!loggedOnce)
			{
				DEV_LOG("[meepo] Using raw array approach (like InvokerController), offset=0x%X\n", offset);
				loggedOnce = true;
			}

			for (int slot = 0; slot < kMaxAbilitySlots; ++slot)
			{
				const auto handle = abilityHandles[slot];
				if (!handle.IsValid())
					continue;

				auto* ability = static_cast<C_DOTABaseAbility*>(
					SDK::Interfaces::GameEntitySystem()->GetBaseEntityFromHandle(handle)
					);
				if (!ability)
				{
					if (slot < 4) // Log first 4 failures for debugging
						DEV_LOG("[meepo] Slot %d: handle valid but GetBaseEntityFromHandle returned null\n", slot);
					continue;
				}

				MeepoAbilityInfo info;
				info.pAbility = ability;

				// CRITICAL: Match InvokerController's EXACT approach - no IsReadable checks, just direct access
				// By the time RefreshAbilityList runs (every frame), entity identity should be initialized
				std::string name;
				if (auto* id = ability->pEntityIdentity())
				{
					// ENHANCED LOGGING FOR CHEAT ENGINE: Log addresses for manual offset finding
					static bool s_bLoggedAddresses = false;
					if (!s_bLoggedAddresses && slot < 4)
					{
						DEV_LOG("[CHEAT_ENGINE] ========================================\n");
						DEV_LOG("[CHEAT_ENGINE] Ability Address: %p\n", ability);
						DEV_LOG("[CHEAT_ENGINE] EntityIdentity Address: %p\n", id);

						// Log CUtlSymbolLarge structure addresses
						auto nameSymbol = id->Name();
						auto designerSymbol = id->DesingerName();

						// Get the address of the m_name field (offset 0x18 from EntityIdentity, check schema)
						uintptr_t nameSymbolAddr = reinterpret_cast<uintptr_t>(&nameSymbol);
						uintptr_t designerSymbolAddr = reinterpret_cast<uintptr_t>(&designerSymbol);

						DEV_LOG("[CHEAT_ENGINE] Name Symbol Address: %p\n", reinterpret_cast<void*>(nameSymbolAddr));
						DEV_LOG("[CHEAT_ENGINE] Designer Symbol Address: %p\n", reinterpret_cast<void*>(designerSymbolAddr));
						DEV_LOG("[CHEAT_ENGINE] Name().String() returns: '%s' (ptr=%p)\n", nameSymbol.String(), nameSymbol.String());
						DEV_LOG("[CHEAT_ENGINE] DesingerName().String() returns: '%s' (ptr=%p)\n", designerSymbol.String(), designerSymbol.String());
						DEV_LOG("[CHEAT_ENGINE] ========================================\n");
						DEV_LOG("[CHEAT_ENGINE] INSTRUCTIONS:\n");
						DEV_LOG("[CHEAT_ENGINE] 1. Copy Ability Address above\n");
						DEV_LOG("[CHEAT_ENGINE] 2. Add it to Cheat Engine\n");
						DEV_LOG("[CHEAT_ENGINE] 3. Browse memory region\n");
						DEV_LOG("[CHEAT_ENGINE] 4. Search for 'meepo_earthbind' or other ability names\n");
						DEV_LOG("[CHEAT_ENGINE] 5. Calculate offset: StringAddress - AbilityAddress\n");
						DEV_LOG("[CHEAT_ENGINE] ========================================\n");
						s_bLoggedAddresses = true;
					}

					name = id->Name().String();

					// If name is empty, try DesingerName() as fallback
					if (name.empty())
					{
						name = id->DesingerName().String();
					}

					if (slot < 4 && !name.empty())
						DEV_LOG("[meepo] Slot %d: Found ability '%s' (len=%zu) at %p\n", slot, name.c_str(), name.length(), ability);
				}

				// Final fallback (match InvokerController exactly)
				if (name.empty())
				{
					name = "unknown_" + std::to_string(slot);
					if (slot < 4)
						DEV_LOG("[meepo] Slot %d: Using fallback name '%s' (ability=%p, entityIdentity=%p)\n", slot, name.c_str(), ability, ability->pEntityIdentity());
				}

				info.name = name;

				// Get ability properties using schema offsets
				// NOTE: Schema offsets for ability properties are NOT FOUND in current build
				// These will need to be found manually using Cheat Engine (like m_hAbilities offset)
				// For now, properties will remain at default values (0, 0.0f, false)
				uint32_t propOffset = 0;
				static bool s_bWarnedAboutOffsets = false;

				if (!s_bWarnedAboutOffsets && slot == 0)
				{
					DEV_LOG("[meepo] WARNING: Ability property offsets not found - properties will show as Unknown\n");
					DEV_LOG("[meepo] Need to find offsets manually: m_iLevel, m_flCooldown, m_flCooldownLength, m_iManaCost, m_flCastRange, m_bIsActivated\n");
					s_bWarnedAboutOffsets = true;
				}

				if (GetSchemaOffset()->TryGetOffset("C_DOTABaseAbility", "m_iLevel", propOffset) && propOffset > 0)
				{
					try { info.level = *reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(ability) + propOffset); }
					catch (...) {}
				}

				if (GetSchemaOffset()->TryGetOffset("C_DOTABaseAbility", "m_flCooldown", propOffset) && propOffset > 0)
				{
					try { info.cooldown = *reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(ability) + propOffset); }
					catch (...) {}
				}

				if (GetSchemaOffset()->TryGetOffset("C_DOTABaseAbility", "m_flCooldownLength", propOffset) && propOffset > 0)
				{
					try { info.cooldownLength = *reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(ability) + propOffset); }
					catch (...) {}
				}

				if (GetSchemaOffset()->TryGetOffset("C_DOTABaseAbility", "m_iManaCost", propOffset) && propOffset > 0)
				{
					try { info.manaCost = *reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(ability) + propOffset); }
					catch (...) {}
				}

				if (GetSchemaOffset()->TryGetOffset("C_DOTABaseAbility", "m_flCastRange", propOffset) && propOffset > 0)
				{
					try { info.castRange = *reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(ability) + propOffset); }
					catch (...) {}
				}

				if (GetSchemaOffset()->TryGetOffset("C_DOTABaseAbility", "m_bIsActivated", propOffset) && propOffset > 0)
				{
					try { info.isActivated = *reinterpret_cast<bool*>(reinterpret_cast<uint8_t*>(ability) + propOffset); }
					catch (...) {}
				}

				fresh.push_back(info);
			}

			m_Abilities.swap(fresh);
			if (!m_Abilities.empty())
			{
				static bool loggedSuccess = false;
				if (!loggedSuccess)
				{
					DEV_LOG("[meepo] Successfully loaded %zu abilities using raw array approach\n", m_Abilities.size());
					loggedSuccess = true;
				}
			}
			return !m_Abilities.empty();
		}

		// Approach 1 worked - use CUtlVector
		DEV_LOG("[meepo] Found %d abilities using CUtlVector approach at offset 0x%X\n", count, offset);

		std::vector<MeepoAbilityInfo> fresh;
		fresh.reserve(count);

		for (int i = 0; i < count; ++i)
		{
			const auto handle = (*pAbilitiesVector)[i];
			if (!handle.IsValid())
			{
				if (i < 4) // Log first 4 for debugging
					DEV_LOG("[meepo] CUtlVector[%d]: handle invalid\n", i);
				continue;
			}

			auto* ability = static_cast<C_DOTABaseAbility*>(
				SDK::Interfaces::GameEntitySystem()->GetBaseEntityFromHandle(handle)
				);
			if (!ability)
			{
				if (i < 4) // Log first 4 for debugging
					DEV_LOG("[meepo] CUtlVector[%d]: handle valid but GetBaseEntityFromHandle returned null\n", i);
				continue;
			}

			MeepoAbilityInfo info;
			info.pAbility = ability;

			// CRITICAL: Match InvokerController's EXACT approach - no IsReadable checks, just direct access
			// By the time RefreshAbilityList runs (every frame), entity identity should be initialized
			std::string name;
			if (auto* id = ability->pEntityIdentity())
			{
				// ENHANCED LOGGING FOR CHEAT ENGINE: Log addresses for manual offset finding
				static bool s_bLoggedAddresses = false;
				if (!s_bLoggedAddresses && i < 4)
				{
					DEV_LOG("[CHEAT_ENGINE] ========================================\n");
					DEV_LOG("[CHEAT_ENGINE] Ability Address: %p\n", ability);
					DEV_LOG("[CHEAT_ENGINE] EntityIdentity Address: %p\n", id);

					// Log CUtlSymbolLarge structure addresses
					auto nameSymbol = id->Name();
					auto designerSymbol = id->DesingerName();

					// Get the address of the m_name field (offset 0x18 from EntityIdentity, check schema)
					uintptr_t nameSymbolAddr = reinterpret_cast<uintptr_t>(&nameSymbol);
					uintptr_t designerSymbolAddr = reinterpret_cast<uintptr_t>(&designerSymbol);

					DEV_LOG("[CHEAT_ENGINE] Name Symbol Address: %p\n", reinterpret_cast<void*>(nameSymbolAddr));
					DEV_LOG("[CHEAT_ENGINE] Designer Symbol Address: %p\n", reinterpret_cast<void*>(designerSymbolAddr));
					DEV_LOG("[CHEAT_ENGINE] Name().String() returns: '%s' (ptr=%p)\n", nameSymbol.String(), nameSymbol.String());
					DEV_LOG("[CHEAT_ENGINE] DesingerName().String() returns: '%s' (ptr=%p)\n", designerSymbol.String(), designerSymbol.String());
					DEV_LOG("[CHEAT_ENGINE] ========================================\n");
					DEV_LOG("[CHEAT_ENGINE] INSTRUCTIONS:\n");
					DEV_LOG("[CHEAT_ENGINE] 1. Copy Ability Address above\n");
					DEV_LOG("[CHEAT_ENGINE] 2. Add it to Cheat Engine\n");
					DEV_LOG("[CHEAT_ENGINE] 3. Browse memory region\n");
					DEV_LOG("[CHEAT_ENGINE] 4. Search for 'meepo_earthbind' or other ability names\n");
					DEV_LOG("[CHEAT_ENGINE] 5. Calculate offset: StringAddress - AbilityAddress\n");
					DEV_LOG("[CHEAT_ENGINE] ========================================\n");
					s_bLoggedAddresses = true;
				}

				name = id->Name().String();

				// If name is empty, try DesingerName() as fallback
				if (name.empty())
				{
					name = id->DesingerName().String();
				}

				if (i < 4 && !name.empty())
					DEV_LOG("[meepo] CUtlVector[%d]: Found ability '%s' (len=%zu) at %p\n", i, name.c_str(), name.length(), ability);
			}

			// Final fallback (match InvokerController exactly)
			if (name.empty())
			{
				name = "unknown_" + std::to_string(i);
				if (i < 4) // Log first 4 for debugging
					DEV_LOG("[meepo] CUtlVector[%d]: Using fallback name '%s' (ability=%p, entityIdentity=%p)\n", i, name.c_str(), ability, ability->pEntityIdentity());
			}

			info.name = name;

			// Get ability properties using schema offsets
			// NOTE: Schema offsets for ability properties are NOT FOUND in current build
			// These will need to be found manually using Cheat Engine (like m_hAbilities offset)
			// For now, properties will remain at default values (0, 0.0f, false)
			uint32_t propOffset = 0;
			static bool s_bWarnedAboutOffsets = false;

			if (!s_bWarnedAboutOffsets && i == 0)
			{
				DEV_LOG("[meepo] WARNING: Ability property offsets not found - properties will show as Unknown\n");
				DEV_LOG("[meepo] Need to find offsets manually: m_iLevel, m_flCooldown, m_flCooldownLength, m_iManaCost, m_flCastRange, m_bIsActivated\n");
				s_bWarnedAboutOffsets = true;
			}

			if (GetSchemaOffset()->TryGetOffset("C_DOTABaseAbility", "m_iLevel", propOffset) && propOffset > 0)
			{
				try { info.level = *reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(ability) + propOffset); }
				catch (...) {}
			}

			if (GetSchemaOffset()->TryGetOffset("C_DOTABaseAbility", "m_flCooldown", propOffset) && propOffset > 0)
			{
				try { info.cooldown = *reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(ability) + propOffset); }
				catch (...) {}
			}

			if (GetSchemaOffset()->TryGetOffset("C_DOTABaseAbility", "m_flCooldownLength", propOffset) && propOffset > 0)
			{
				try { info.cooldownLength = *reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(ability) + propOffset); }
				catch (...) {}
			}

			if (GetSchemaOffset()->TryGetOffset("C_DOTABaseAbility", "m_iManaCost", propOffset) && propOffset > 0)
			{
				try { info.manaCost = *reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(ability) + propOffset); }
				catch (...) {}
			}

			if (GetSchemaOffset()->TryGetOffset("C_DOTABaseAbility", "m_flCastRange", propOffset) && propOffset > 0)
			{
				try { info.castRange = *reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(ability) + propOffset); }
				catch (...) {}
			}

			if (GetSchemaOffset()->TryGetOffset("C_DOTABaseAbility", "m_bIsActivated", propOffset) && propOffset > 0)
			{
				try { info.isActivated = *reinterpret_cast<bool*>(reinterpret_cast<uint8_t*>(ability) + propOffset); }
				catch (...) {}
			}

			fresh.push_back(info);
		}

		m_Abilities.swap(fresh);
		if (!m_Abilities.empty())
		{
			static bool loggedSuccess = false;
			if (!loggedSuccess)
			{
				DEV_LOG("[meepo] Successfully loaded %zu abilities using CUtlVector approach\n", m_Abilities.size());
				loggedSuccess = true;
			}
		}
		return !m_Abilities.empty();
	}
	catch (...)
	{
		DEV_LOG( "[meepo] Failed to access m_vecAbilities (offset: 0x%X)\n" , offset );
		return false;
	}
}




