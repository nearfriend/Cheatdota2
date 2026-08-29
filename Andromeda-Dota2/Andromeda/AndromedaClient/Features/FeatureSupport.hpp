#pragma once

#include <AndromedaClient/Data/AbilityDamageData.hpp>
#include <Dota2/SDK/Math/Math.hpp>
#include <Dota2/SDK/Types/CEntityData.hpp>
#include <Dota2/SDK/Types/CHandle.hpp>

#include <ImGui/imgui.h>
#include <Windows.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

class CGameEntitySystem;

// Feature-agnostic plumbing shared by the automation features.
//
// Every function here is an extraction of code that was already proven in
// CKillStealer.cpp / CAutoCombo.cpp - the entity-chunk walking, the
// identity-based name lookup, the inventory/ability handle-vector layout and
// the scan-code keyboard input all carry build-specific fixes that took real
// debugging to find (see the comments on each). New features must call these
// instead of re-deriving them; CKillStealer and CAutoCombo deliberately keep
// their own copies so that extracting this stayed zero-risk to two features
// that already work.
namespace FeatureSupport
{
	// Schema offsets for units, their abilities and their inventory. One
	// shared resolve for every feature that reads them.
	struct UnitOffsets
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
		// m_bInAbilityPhase - true from the start of an ability's cast point
		// until it actually fires. This is the earliest client-visible signal
		// that an enemy is casting something, which is what makes reactive
		// dodging possible at all.
		uint32_t abilityInPhase = 0;
		uint32_t abilityChannelStart = 0;
		uint32_t sceneNode = 0;
		uint32_t absOrigin = 0;
		uint32_t rotation = 0;
		uint32_t isIllusion = 0;
		uint32_t isClone = 0;
		uint32_t waitingToSpawn = 0;
		// Unit state. Optional - a build that does not replicate these simply
		// leaves the has* flag false and callers fall back to not knowing.
		//
		// m_nUnitState64 is the one that actually resolves on this build: the
		// individual m_bStunned / m_bMagicImmune / m_bInvulnerable booleans all
		// came back unresolved in a live capture (the dodger logged
		// state-gates=000), so every state guard was silently inert.
		uint32_t unitState = 0;
		uint32_t magicImmune = 0;
		uint32_t invulnerable = 0;
		uint32_t stunned = 0;
		bool hasInventory = false;
		bool hasAbilityActivated = false;
		bool hasAbilityInPhase = false;
		bool hasAbilityChannelStart = false;
		bool hasRotation = false;
		bool hasIsIllusion = false;
		bool hasIsClone = false;
		bool hasWaitingToSpawn = false;
		bool hasUnitState = false;
		bool hasMagicImmune = false;
		bool hasInvulnerable = false;
		bool hasStunned = false;
		bool resolved = false;
	};

	// Retries internally (throttled) until the schema system is up, so it is
	// safe to call every tick.
	auto ResolveOffsets() -> const UnitOffsets&;

	// Bit positions in m_nUnitState64 (Dota's MODIFIER_STATE enum).
	//
	// These indices are not guessed: the enum's names live in client.dll as a
	// contiguous string table in declaration order, starting at
	// MODIFIER_STATE_ROOTED, so the position of each name in that table IS its
	// value. Re-derive with:
	//   grep -a -b -o "MODIFIER_STATE_[A-Z_]*" client.dll | sort -n
	enum UnitStateBit : uint32_t
	{
		kStateRooted = 0,
		kStateDisarmed = 1,
		kStateAttackImmune = 2,
		kStateSilenced = 3,
		kStateMuted = 4,
		kStateStunned = 5,
		kStateHexed = 6,
		kStateInvisible = 7,
		kStateInvulnerable = 8,
		kStateMagicImmune = 9,
		kStateNightmared = 11,
		kStateFrozen = 19,
		kStateCommandRestricted = 20,
	};

	// Returns false when this build does not replicate the mask at all, so
	// callers can tell "no states set" from "we cannot see states".
	auto TryReadUnitState( C_BaseEntity* unit , const UnitOffsets& offsets , uint64_t& out ) -> bool;
	auto HasUnitState( uint64_t stateMask , UnitStateBit bit ) -> bool;

	auto IsReadableRuntimeMemory( const void* ptr , size_t size = 1 ) -> bool;

	// Plain dereference, no VirtualQuery - `base` is always the game's own live
	// entity pointer (or an offset into it), never attacker-controlled. Routing
	// entity-scan loops through VirtualQuery instead means tens of thousands of
	// kernel calls per render-thread frame, which visibly freezes the game.
	template <typename T>
	auto TryReadField( const void* base , uint32_t offset , T& out ) -> bool
	{
		if ( !base || !offset )
			return false;
		std::memcpy( &out , reinterpret_cast<const uint8_t*>( base ) + offset , sizeof( T ) );
		return true;
	}

	template <typename T>
	auto ReadField( const void* base , uint32_t offset , T fallback = T{} ) -> T
	{
		T value{};
		return TryReadField( base , offset , value ) ? value : fallback;
	}

	// Validated read - one VirtualQuery per call, so keep it OFF hot paths.
	// Only for pointers that were themselves read out of game memory
	// (handle-vector data, inventory arrays), where a stale pointer is
	// plausible.
	template <typename T>
	auto TryRead( const void* address , T& out ) -> bool
	{
		if ( !IsReadableRuntimeMemory( address , sizeof( T ) ) )
			return false;
		std::memcpy( &out , address , sizeof( T ) );
		return true;
	}

	auto ToLower( const std::string& value ) -> std::string;
	auto IsPlayableTeam( uint8_t team ) -> bool;
	auto Distance2D( const Vector3& left , const Vector3& right ) -> float;

	// Names must come from the entity's own identity: GetSchemaClassName()
	// returns null for entities reached through an identity-chunk walk, which
	// silently made every name comparison match the empty string.
	auto EntityName( C_BaseEntity* entity , CEntityIdentity* identity ) -> std::string;
	auto TryEntityAtIndex( CGameEntitySystem* entitySystem , int index , CEntityIdentity*& identityOut , C_BaseEntity*& entityOut ) -> bool;
	auto EntityFromHandle( CGameEntitySystem* entitySystem , CHandle handle , CEntityIdentity** identityOut = nullptr ) -> C_BaseEntity*;

	auto LooksLikeHeroEntity( C_BaseEntity* entity , const std::string& name ) -> bool;
	// Lane creeps only - melee, ranged and siege - never jungle camps, Roshan,
	// buildings, wards, couriers, summons or heroes. The needle lists are the
	// ones CLastHitAssistant.cpp arrived at the hard way; see IsLaneCreep
	// there for why identity has to come from the name on this build and why
	// nothing may be admitted on its stats.
	auto LooksLikeLaneCreep( C_BaseEntity* entity , const std::string& name , uint8_t team ) -> bool;
	auto TryReadOrigin( C_BaseEntity* entity , const UnitOffsets& offsets , Vector3& out ) -> bool;
	// Facing angle in degrees (scene node yaw), for direction-aware checks.
	auto TryReadYaw( C_BaseEntity* entity , const UnitOffsets& offsets , float& out ) -> bool;

	auto ReadHandleVector( const void* field , int maxCount , std::vector<CHandle>& out ) -> bool;
	auto ReadAbilityHandles( C_BaseEntity* unit , const UnitOffsets& offsets , std::vector<CHandle>& out ) -> bool;
	auto ReadInventoryHandles( C_BaseEntity* unit , const UnitOffsets& offsets , std::vector<CHandle>& out ) -> bool;
	auto ReadCastRange( C_BaseEntity* ability , const UnitOffsets& offsets , float fallback ) -> float;

	auto FindAbilityEntry( const std::string& name ) -> const AbilityDamageEntry*;
	auto PreferredSlotForAbility( const std::string& name ) -> int;

	// Returns the Dota window only when it is focused and our menu is closed -
	// i.e. when injecting input is actually safe.
	auto WindowReadyForInput() -> HWND;
	// Key events must carry a scan code, not just a virtual key: Dota
	// (Source 2 / SDL) consumes raw input, where a wVk-only event has no
	// usable scan code and is ignored outright.
	auto SendKeyPress( WORD key ) -> bool;
	auto SendLeftClick() -> bool;
	// A right click on empty ground is a move order, and on an enemy unit an
	// attack order - the two things automation needs that a left click cannot
	// express.
	auto SendRightClick() -> bool;
	// Absolute cursor move through SendInput, NOT SetCursorPos - the game only
	// sees the former. Use this for restoring the cursor too, so the game's
	// crosshair goes back with the Windows one.
	auto MoveCursorToScreen( int screenX , int screenY ) -> bool;
	auto MoveCursorToClientPoint( HWND window , const ImVec2& screen , POINT& previousOut ) -> bool;
	// Projects a world point and reports whether it is genuinely visible and
	// clickable - inside the client area and clear of the bottom HUD band -
	// rather than clamped to an edge the way AimCursorAtWorld does. Check this
	// before any cast that ends in a click on a unit.
	auto ProjectWorldToClient( HWND window , const Vector3& worldPoint , bool groundTargeted , ImVec2& outScreen ) -> bool;
	// groundTargeted=false raises the aim point onto the unit's model;
	// ground-targeted casts must aim at the origin itself or they land behind
	// the target.
	auto AimCursorAtWorld( HWND window , const Vector3& worldPoint , bool groundTargeted ) -> bool;
}
