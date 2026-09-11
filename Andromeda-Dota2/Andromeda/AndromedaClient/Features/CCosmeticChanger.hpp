#pragma once

#include <cstdint>

// Local, view-only cosmetic changer. Rewrites the wearable entities attached to
// the LOCAL hero so his skin shows differently on this client only - no other
// player sees it, since their clients render our real equipped items.
//
// Step 1 (this file, for now): resolve the wearable schema and enumerate the
// hero's current wearables, logging each slot's item-definition index. That
// confirms the fields resolve and gives the defindexes we later rewrite.
class CCosmeticChanger final
{
public:
	auto OnRender() -> void;

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
		bool hasWearables = false;
		bool hasDefIndex = false;
		bool resolved = false;
	};

	auto ResolveOffsets() -> bool;

	Offsets m_Offsets{};
	bool m_ResolveTried = false;
	bool m_LoggedSchema = false;
	uint32_t m_LastLogTick = 0;
};
