#pragma once

class CGameEntitySystem;
class C_BaseEntity;

// Resolves which live entity is the local player's hero.
//
// CGameEntitySystem::GetLocalPlayerController()->m_hAssignedHero() (see
// CGameEntitySystem.hpp) has never resolved on this game build - neither its
// Steam-ID match nor its m_bIsLocalPlayerController flag fallback finds
// anything. The only method proven to work - first discovered debugging
// CKillStealer, later confirmed again debugging CAutoCombo - is matching the
// engine's local player slot (IVEngineClient2::GetLocalPlayer) against each
// hero's replicated m_iPlayerID / m_nPlayerOwnerID. This class is the single
// shared home for that logic so future features don't have to re-derive it
// (or re-hit the same bugs) a third time.
//
// Extracted from CKillStealer's ResolveLocalHero/ResolveLocalHeroByPlayerId/
// TryResolveViaController - CKillStealer.cpp keeps its own copy rather than
// switching to this one, so that change stays zero-risk to a feature that
// already works; new callers (starting with CAutoCombo) should use this one.
class CLocalHeroResolver
{
public:
	// On success, outEntity/outEntIndex identify the local hero. Safe to call
	// every frame - internally throttled/cached like GetLocalPlayerController().
	static auto Resolve( CGameEntitySystem* entitySystem , C_BaseEntity*& outEntity , int& outEntIndex ) -> bool;

	// Player-id-only resolution, no controller fallback and no cache. This is
	// the variant CGameEntitySystem::GetLocalPlayerController() itself may
	// call (its hero-match strategy) - the full Resolve() falls back to
	// GetLocalPlayerController() and would recurse.
	static auto ResolveByPlayerIdOnly( CGameEntitySystem* entitySystem , C_BaseEntity*& outEntity , int& outEntIndex ) -> bool;
};
