#pragma once

#include <Common/Common.hpp>
#include <Dota2/SDK/CSchemaOffset.hpp>

/**
 * Ability and Hero property offsets from current client schema (GameTracking / dota2dumped Aug 2026).
 * Schema discovery at runtime should override these when available.
 */

namespace AbilityOffsets
{
	constexpr uint32_t m_iLevel = 0x0628;
	constexpr uint32_t m_flCooldown = 0x0638;
	constexpr uint32_t m_fCooldown = 0x0638;
	constexpr uint32_t m_flCooldownLength = 0x063C;
	constexpr uint32_t m_iManaCost = 0x0640;
	constexpr uint32_t m_bActivated = 0x0619;
}

namespace HeroOffsets
{
	constexpr uint32_t m_flMana = 0x0C04;
	constexpr uint32_t m_flMaxMana = 0x0C08;
}

namespace BaseNPCOffsets
{
	constexpr uint32_t m_vecAbilities = 0x0C30;
}

inline bool VerifyCriticalOffsets()
{
	auto* pSchema = GetSchemaOffset();

	if ( !pSchema )
		return false;

	uint32_t offset = 0;

	const bool hasLevel = pSchema->TryGetOffset( "C_DOTABaseAbility" , "m_iLevel" , offset );
	const bool hasCooldown = pSchema->TryGetOffset( "C_DOTABaseAbility" , "m_fCooldown" , offset ) ||
		pSchema->TryGetOffset( "C_DOTABaseAbility" , "m_flCooldown" , offset );
	const bool hasManaCost = pSchema->TryGetOffset( "C_DOTABaseAbility" , "m_iManaCost" , offset );
	const bool hasHeroMana = pSchema->TryGetOffset( "C_DOTA_BaseNPC" , "m_flMana" , offset );
	const bool hasAbilities = pSchema->TryGetOffset( "C_DOTA_BaseNPC" , "m_vecAbilities" , offset );

	return hasLevel && hasCooldown && hasManaCost && hasHeroMana && hasAbilities;
}
