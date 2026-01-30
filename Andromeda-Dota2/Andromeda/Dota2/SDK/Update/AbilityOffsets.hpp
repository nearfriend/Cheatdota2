#pragma once

#include <Common/Common.hpp>
#include <Dota2/SDK/CSchemaOffset.hpp>

/**
 * Ability and Hero Property Offsets
 * 
 * These offsets are discovered via schema dump (Phase 1).
 * Update these values after running schema dump and finding actual offsets.
 * 
 * IMPORTANT: These are placeholder values. You MUST run schema dump
 * and update these with actual offsets from debug.log before using.
 */

namespace AbilityOffsets
{
	// C_DOTABaseAbility offsets
	// These will be populated from schema dump
	constexpr uint32_t m_iLevel = 0x0000;  // TODO: Replace with actual offset from schema dump
	constexpr uint32_t m_flCooldown = 0x0000;  // TODO: Replace with actual offset
	constexpr uint32_t m_flCooldownLength = 0x0000;  // TODO: Replace with actual offset
	constexpr uint32_t m_iManaCost = 0x0000;  // TODO: Replace with actual offset
	constexpr uint32_t m_flCastRange = 0x0000;  // TODO: Replace with actual offset
	constexpr uint32_t m_bIsActivated = 0x0000;  // TODO: Replace with actual offset
	constexpr uint32_t m_iAbilityIndex = 0x0000;  // TODO: Replace with actual offset (if exists)
}

namespace HeroOffsets
{
	// C_DOTA_BaseNPC_Hero offsets
	constexpr uint32_t m_flMana = 0x0000;  // TODO: Replace with actual offset from schema dump
	constexpr uint32_t m_flMaxMana = 0x0000;  // TODO: Replace with actual offset
}

namespace BaseNPCOffsets
{
	// C_DOTA_BaseNPC offsets
	constexpr uint32_t m_hAbilities = 0x0000;  // TODO: Replace with actual offset from schema dump
}

/**
 * Helper function to verify offsets are discovered.
 * Call this after schema initialization to check if offsets are found.
 */
inline bool VerifyCriticalOffsets()
{
	auto* pSchema = GetSchemaOffset();
	if ( !pSchema )
		return false;
	
	uint32_t offset = 0;
	
	// Check ability offsets
	bool hasLevel = pSchema->TryGetOffset( "C_DOTABaseAbility" , "m_iLevel" , offset );
	bool hasCooldown = pSchema->TryGetOffset( "C_DOTABaseAbility" , "m_flCooldown" , offset );
	bool hasManaCost = pSchema->TryGetOffset( "C_DOTABaseAbility" , "m_iManaCost" , offset );
	
	// Check hero offsets
	bool hasHeroMana = pSchema->TryGetOffset( "C_DOTA_BaseNPC_Hero" , "m_flMana" , offset );
	
	// Check base NPC offsets
	bool hasAbilities = pSchema->TryGetOffset( "C_DOTA_BaseNPC" , "m_hAbilities" , offset );
	
	return hasLevel && hasCooldown && hasManaCost && hasHeroMana && hasAbilities;
}

