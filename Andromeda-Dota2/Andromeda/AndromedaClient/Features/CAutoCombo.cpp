#include "CAutoCombo.hpp"

#include <AndromedaClient/CAndromedaClient.hpp>
#include <AndromedaClient/CAndromedaGUI.hpp>
#include <AndromedaClient/Heroes/Invoker/CInvokerController.hpp>
#include <AndromedaClient/Data/AbilityDamageData.hpp>
#include <AndromedaClient/Settings/Settings.hpp>
#include <Common/DevLog.hpp>
#include <Dota2/SDK/CSchemaOffset.hpp>
#include <Dota2/SDK/Interface/CGameEntitySystem.hpp>
#include <Dota2/SDK/Interface/CLocalHeroResolver.hpp>
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

namespace
{
	enum class ComboToolKind : uint8_t
	{
		Ability,
		Item,
		Attack
	};

	struct AutoComboOffsets
	{
		uint32_t health = 0;
		uint32_t maxHealth = 0;
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
		uint32_t damageMin = 0;
		uint32_t damageBonus = 0;
		uint32_t attackRange = 0;
		uint32_t team = 0;
		bool hasInventory = false;
		bool hasAbilityActivated = false;
		bool hasDamageBonus = false;
		bool hasAttackRange = false;
		bool hasTeam = false;
		bool resolved = false;
	};

	struct NetworkHandleVector
	{
		int32_t size = 0;
		int32_t pad = 0;
		const CHandle* data = nullptr;
		int32_t allocationCount = 0;
		int32_t growSize = 0;
	};

	struct UnitSnapshot
	{
		C_BaseEntity* entity = nullptr;
		Vector3 origin{};
		int health = 0;
		float mana = 0.f;
		float attackDamage = 0.f;
		float attackRange = 150.f;
		uint8_t team = 0;
	};

	struct ComboTool
	{
		ComboToolKind kind = ComboToolKind::Ability;
		std::string name;
		float castRange = 0.f;
		WORD key = 0;
		// Ability slot 0..5 (Q W E D F R) for kind==Ability; -1 for items/attack.
		int slot = -1;
		bool noTarget = false;
		bool unitTarget = false;
		bool pointTarget = false;
		bool hasDamage = false;
		uint32_t delayMs = 0;
	};

	auto IsReadableRuntimeMemory( const void* ptr , size_t size = 1 ) -> bool
	{
		if ( !ptr || size == 0 )
			return false;

		MEMORY_BASIC_INFORMATION mbi{};
		if ( !VirtualQuery( ptr , &mbi , sizeof( mbi ) ) )
			return false;

		if ( mbi.State != MEM_COMMIT || ( mbi.Protect & ( PAGE_NOACCESS | PAGE_GUARD ) ) )
			return false;

		const DWORD readable =
			PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
			PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
		if ( !( mbi.Protect & readable ) )
			return false;

		const auto start = reinterpret_cast<uintptr_t>( ptr );
		const auto regionStart = reinterpret_cast<uintptr_t>( mbi.BaseAddress );
		const auto regionEnd = regionStart + mbi.RegionSize;
		return start >= regionStart && start + size <= regionEnd && start + size >= start;
	}

	// Validated read - VirtualQuery per call, so keep it OFF hot paths. Only
	// for pointers that were themselves read out of game memory (handle-vector
	// data, inventory arrays), where a stale/garbage pointer is plausible.
	template <typename T>
	auto TryRead( const void* address , T& out ) -> bool
	{
		if ( !IsReadableRuntimeMemory( address , sizeof( T ) ) )
			return false;
		std::memcpy( &out , address , sizeof( T ) );
		return true;
	}

	// Plain dereference, no VirtualQuery - `base` is always the game's own live
	// entity pointer (or an offset a few hundred bytes into it), never
	// attacker-controlled or independently computed. Matches CKillStealer's
	// ReadField: routing the entity-scan loops through VirtualQuery instead
	// meant tens of thousands of kernel calls in a single render-thread frame,
	// which froze the game for noticeable fractions of a second on the combo
	// keypress.
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

	auto ToLower( const std::string& in ) -> std::string
	{
		std::string out = in;
		std::transform( out.begin() , out.end() , out.begin() , []( unsigned char c ) { return static_cast<char>( std::tolower( c ) ); } );
		return out;
	}

	auto EntityName( C_BaseEntity* entity , CEntityIdentity* identity ) -> std::string
	{
		if ( identity )
		{
			if ( const char* name = identity->Name().String(); name && name[0] )
				return name;
			if ( const char* name = identity->DesingerName().String(); name && name[0] )
				return name;
		}
		if ( entity )
		{
			if ( const char* name = entity->GetSchemaClassName(); name && name[0] )
				return name;
		}
		return {};
	}

	// identityOut comes from the entity's own m_pEntity schema field (matching
	// CKillStealer's entity->pEntityIdentity()) rather than being left null -
	// a null identity here silently made EntityName() fall back to
	// GetSchemaClassName(), which returns null for entities reached via an
	// identity-chunk walk, so every caller (ability/item lookup, hero
	// detection) was matching against an empty name.
	auto TryEntityAtIndex( CGameEntitySystem* entitySystem , int index , CEntityIdentity*& identityOut , C_BaseEntity*& entityOut ) -> bool
	{
		identityOut = nullptr;
		entityOut = nullptr;
		if ( !entitySystem || index < 0 )
			return false;

		auto* entity = entitySystem->GetBaseEntity( index );
		if ( !entity || !IsReadableRuntimeMemory( entity , sizeof( void* ) ) )
			return false;

		entityOut = entity;
		identityOut = entity->pEntityIdentity();
		return true;
	}

	auto EntityFromHandle( CGameEntitySystem* entitySystem , CHandle handle , CEntityIdentity** identityOut = nullptr ) -> C_BaseEntity*
	{
		if ( identityOut )
			*identityOut = nullptr;
		if ( !handle.IsValid() )
			return nullptr;

		CEntityIdentity* identity = nullptr;
		C_BaseEntity* entity = nullptr;
		if ( !TryEntityAtIndex( entitySystem , handle.GetEntryIndex() , identity , entity ) )
			return nullptr;

		if ( identityOut )
			*identityOut = identity;
		return entity;
	}

	auto TryReadOrigin( C_BaseEntity* entity , const AutoComboOffsets& offsets , Vector3& out ) -> bool
	{
		void* sceneNode = nullptr;
		if ( !TryReadField( entity , offsets.sceneNode , sceneNode ) || !sceneNode )
			return false;
		if ( !TryReadField( sceneNode , offsets.absOrigin , out ) )
			return false;
		return std::isfinite( out.m_x ) && std::isfinite( out.m_y ) && std::isfinite( out.m_z );
	}

	auto ReadHandleVector( const void* field , int maxCount , std::vector<CHandle>& out ) -> bool
	{
		out.clear();
		NetworkHandleVector vector{};
		if ( !TryRead( field , vector ) )
			return false;

		if ( vector.size <= 0 || vector.size > maxCount || !vector.data ||
			!IsReadableRuntimeMemory( vector.data , sizeof( CHandle ) * static_cast<size_t>( vector.size ) ) )
			return false;

		out.resize( vector.size );
		std::memcpy( out.data() , vector.data , sizeof( CHandle ) * static_cast<size_t>( vector.size ) );
		return true;
	}

	auto ReadInventoryHandles( C_BaseEntity* hero , const AutoComboOffsets& offsets , std::vector<CHandle>& out ) -> bool
	{
		out.clear();
		if ( !hero || !offsets.hasInventory )
			return false;

		const uintptr_t itemsBase = reinterpret_cast<uintptr_t>( hero ) + offsets.inventory + offsets.inventoryItems;
		int32_t reportedSize = 0;
		if ( !TryRead( reinterpret_cast<const void*>( itemsBase ) , reportedSize ) )
			return false;

		const CHandle* handles = nullptr;
		int count = 0;
		constexpr int kMaxInventorySlots = 27;

		if ( reportedSize > 0 && reportedSize <= kMaxInventorySlots )
		{
			handles = reinterpret_cast<const CHandle*>( itemsBase + sizeof( int32_t ) );
			count = reportedSize;
		}
		else
		{
			handles = reinterpret_cast<const CHandle*>( itemsBase );
			count = 6;
		}

		if ( !IsReadableRuntimeMemory( handles , sizeof( CHandle ) * static_cast<size_t>( count ) ) )
			return false;

		out.assign( handles , handles + count );
		return true;
	}

	auto ReadCastRange( C_BaseEntity* ability , const AutoComboOffsets& offsets , float fallback ) -> float
	{
		float castRange = fallback;
		if ( offsets.abilityCastRange )
		{
			int rangeInt = 0;
			if ( TryReadField( ability , offsets.abilityCastRange , rangeInt ) && rangeInt >= 50 && rangeInt <= 25000 )
				castRange = static_cast<float>( rangeInt );
			else
			{
				float rangeFloat = 0.f;
				if ( TryReadField( ability , offsets.abilityCastRange , rangeFloat ) &&
					std::isfinite( rangeFloat ) && rangeFloat >= 50.f && rangeFloat <= 25000.f )
					castRange = rangeFloat;
			}
		}
		if ( castRange <= 0.f )
			castRange = 800.f;
		return castRange;
	}

	auto FindDamageEntry( const std::string& name ) -> const AbilityDamageEntry*
	{
		auto* data = GetAbilityDamageData();
		if ( !data )
			return nullptr;
		if ( const auto* entry = data->Find( name ) )
			return entry;
		const std::string lowered = ToLower( name );
		if ( lowered != name )
			return data->Find( lowered );
		return nullptr;
	}

	auto PreferredSlotForAbility( const std::string& name ) -> int
	{
		auto* data = GetAbilityDamageData();
		if ( !data )
			return -1;
		const int direct = data->PreferredSlot( name );
		if ( direct >= 0 )
			return direct;
		const std::string lowered = ToLower( name );
		if ( lowered != name )
			return data->PreferredSlot( lowered );
		return -1;
	}

	auto Distance2D( const Vector3& left , const Vector3& right ) -> float
	{
		const float dx = left.m_x - right.m_x;
		const float dy = left.m_y - right.m_y;
		return std::sqrt( dx * dx + dy * dy );
	}

	auto ResolveOffsets() -> AutoComboOffsets&
	{
		static AutoComboOffsets offsets{};
		if ( offsets.resolved )
			return offsets;

		static ULONGLONG lastAttemptTick = 0;
		const ULONGLONG now = GetTickCount64();
		if ( lastAttemptTick && now - lastAttemptTick < 500 )
			return offsets;
		lastAttemptTick = now;

		auto* schema = GetSchemaOffset();
		if ( !schema )
			return offsets;

		const bool hasHealth = schema->TryGetOffset( "C_BaseEntity" , "m_iHealth" , offsets.health );
		const bool hasMaxHealth = schema->TryGetOffset( "C_BaseEntity" , "m_iMaxHealth" , offsets.maxHealth );
		offsets.hasTeam = schema->TryGetOffset( "C_BaseEntity" , "m_iTeamNum" , offsets.team );
		const bool hasMana = schema->TryGetOffset( "C_DOTA_BaseNPC" , "m_flMana" , offsets.mana );
		const bool hasAbilities = schema->TryGetOffset( "C_DOTA_BaseNPC" , "m_vecAbilities" , offsets.abilities );
		const bool hasSceneNode = schema->TryGetOffset( "C_BaseEntity" , "m_pGameSceneNode" , offsets.sceneNode );
		const bool hasAbsOrigin = schema->TryGetOffset( "CGameSceneNode" , "m_vecAbsOrigin" , offsets.absOrigin );
		const bool hasAbilityLevel = schema->TryGetOffset( "C_DOTABaseAbility" , "m_iLevel" , offsets.abilityLevel );
		const bool hasAbilityCooldown = schema->TryGetOffset( "C_DOTABaseAbility" , "m_flCooldown" , offsets.abilityCooldown ) ||
			schema->TryGetOffset( "C_DOTABaseAbility" , "m_fCooldown" , offsets.abilityCooldown );
		const bool hasAbilityMana = schema->TryGetOffset( "C_DOTABaseAbility" , "m_iManaCost" , offsets.abilityManaCost );
		schema->TryGetOffset( "C_DOTABaseAbility" , "m_flCastRange" , offsets.abilityCastRange );
		offsets.hasAbilityActivated = schema->TryGetOffset( "C_DOTABaseAbility" , "m_bIsActivated" , offsets.abilityActivated ) ||
			schema->TryGetOffset( "C_DOTABaseAbility" , "m_bActivated" , offsets.abilityActivated );

		const bool hasInventoryContainer = schema->TryGetOffset( "C_DOTA_BaseNPC" , "m_Inventory" , offsets.inventory );
		const bool hasNestedItems = schema->TryGetOffset( "C_DOTA_UnitInventory" , "m_hItems" , offsets.inventoryItems ) ||
			schema->TryGetOffset( "CDOTA_UnitInventory" , "m_hItems" , offsets.inventoryItems );
		if ( hasNestedItems )
			offsets.hasInventory = hasInventoryContainer;
		else
			offsets.hasInventory = schema->TryGetOffset( "C_DOTA_BaseNPC" , "m_hItems" , offsets.inventoryItems );

		schema->TryGetOffset( "C_DOTA_BaseNPC" , "m_iDamageMin" , offsets.damageMin );
		offsets.hasDamageBonus = schema->TryGetOffset( "C_DOTA_BaseNPC" , "m_iDamageBonus" , offsets.damageBonus );
		offsets.hasAttackRange = schema->TryGetOffset( "C_DOTA_BaseNPC" , "m_iAttackRange" , offsets.attackRange );

		offsets.resolved = hasHealth && hasMaxHealth && hasMana && hasAbilities &&
			hasSceneNode && hasAbsOrigin && hasAbilityLevel && hasAbilityCooldown && hasAbilityMana;

		return offsets;
	}

	auto IsPlayableTeam( uint8_t team ) -> bool
	{
		return team == 2 || team == 3;
	}

	auto LooksLikeHeroEntity( C_BaseEntity* entity , const std::string& name ) -> bool
	{
		if ( !entity )
			return false;
		const std::string lower = ToLower( name );
		if ( lower.find( "npc_dota_hero_" ) != std::string::npos )
			return true;
		const char* className = entity->GetSchemaClassName();
		return className && ( std::strstr( className , "DOTA_BaseNPC_Hero" ) || std::strstr( className , "DOTA_Unit_Hero" ) );
	}

	// Which entity is our hero is resolved by the shared CLocalHeroResolver
	// (see Dota2/SDK/Interface/CLocalHeroResolver.hpp) - it already carries
	// the fix for GetLocalPlayerController() never succeeding on this build,
	// originally worked out debugging CKillStealer. This just reads out the
	// AutoCombo-specific fields (mana, attack damage/range) once an entity is
	// found.
	auto ResolveLocalUnit( CGameEntitySystem* entitySystem , const AutoComboOffsets& offsets , UnitSnapshot& out ) -> bool
	{
		C_BaseEntity* entity = nullptr;
		int entIndex = -1;
		if ( !CLocalHeroResolver::Resolve( entitySystem , entity , entIndex ) )
			return false;

		const int health = ReadField<int>( entity , offsets.health , 0 );
		if ( health <= 0 )
			return false;

		Vector3 origin{};
		if ( !TryReadOrigin( entity , offsets , origin ) )
			return false;

		out.entity = entity;
		out.origin = origin;
		out.health = health;
		out.mana = ReadField<float>( entity , offsets.mana , 0.f );
		if ( !std::isfinite( out.mana ) || out.mana < 0.f )
			out.mana = 0.f;
		out.attackDamage = static_cast<float>( (std::max)( 1 , ReadField<int>( entity , offsets.damageMin , 0 ) +
			( offsets.hasDamageBonus ? ReadField<int>( entity , offsets.damageBonus , 0 ) : 0 ) ) );
		out.attackRange = offsets.hasAttackRange ? static_cast<float>( (std::max)( 150 , ReadField<int>( entity , offsets.attackRange , 150 ) ) ) : 150.f;
		out.team = offsets.hasTeam ? ReadField<uint8_t>( entity , offsets.team , 0 ) : 0;
		return true;
	}

	auto ResolveTargetUnit( CGameEntitySystem* entitySystem , const AutoComboOffsets& offsets , int entIndex , UnitSnapshot& out ) -> bool
	{
		if ( !entitySystem || entIndex < 0 )
			return false;

		auto* entity = entitySystem->GetBaseEntity( entIndex );
		if ( !entity || !IsReadableRuntimeMemory( entity , sizeof( void* ) ) )
			return false;

		const int health = ReadField<int>( entity , offsets.health , 0 );
		if ( health <= 0 )
			return false;

		Vector3 origin{};
		if ( !TryReadOrigin( entity , offsets , origin ) )
			return false;

		out.entity = entity;
		out.origin = origin;
		out.health = health;
		out.team = offsets.hasTeam ? ReadField<uint8_t>( entity , offsets.team , 0 ) : 0;
		return true;
	}

	// Walking allocated identity chunks directly (rather than an index-bounded
	// loop up to GetHighestEntityIndex) mirrors CKillStealer's ScanHeroes fix -
	// see that file's comment for why the index-bounded approach silently
	// missed live heroes in a real capture.
	auto FindNearestEnemyHero( CGameEntitySystem* entitySystem , const AutoComboOffsets& offsets , const UnitSnapshot& localUnit , float maxRange ) -> int
	{
		if ( !entitySystem || !offsets.hasTeam || !IsPlayableTeam( localUnit.team ) )
			return -1;

		int bestIndex = -1;
		float bestDistance = maxRange;

		for ( int chunkIndex = 0; chunkIndex < MAX_ENTITY_LISTS; ++chunkIndex )
		{
			auto* chunk = entitySystem->m_pIdentityChunks[chunkIndex];
			if ( !chunk )
				continue;

			for ( int entryIndex = 0; entryIndex < MAX_ENTITIES_IN_LIST; ++entryIndex )
			{
				auto* entity = chunk->m_pIdentities[entryIndex].pBaseEntity();
				if ( !entity || entity == localUnit.entity )
					continue;

				const int health = ReadField<int>( entity , offsets.health , 0 );
				if ( health <= 0 )
					continue;

				const uint8_t team = ReadField<uint8_t>( entity , offsets.team , 0 );
				if ( !IsPlayableTeam( team ) || team == localUnit.team )
					continue;

				CEntityIdentity* identity = nullptr;
				C_BaseEntity* namedEntity = nullptr;
				const int entIndex = chunkIndex * MAX_ENTITIES_IN_LIST + entryIndex;
				if ( !TryEntityAtIndex( entitySystem , entIndex , identity , namedEntity ) )
					continue;

				const std::string name = EntityName( namedEntity , identity );
				if ( !LooksLikeHeroEntity( entity , name ) )
					continue;

				Vector3 origin{};
				if ( !TryReadOrigin( entity , offsets , origin ) )
					continue;
				if ( origin.m_x == 0.f && origin.m_y == 0.f && origin.m_z == 0.f )
					continue;

				const float distance = Distance2D( localUnit.origin , origin );
				if ( distance < bestDistance )
				{
					bestDistance = distance;
					bestIndex = entIndex;
				}
			}
		}

		return bestIndex;
	}

	// Names the local hero's ability slots (0..5 = Q W E D F R) for the menu's
	// spell-order editor. Unlike CollectTools this does not filter on the
	// damage-data catalog, level, cooldown or mana - the user should see and be
	// able to disable every spell the hero has, usable right now or not.
	auto CollectSlotNames( CGameEntitySystem* entitySystem , const UnitSnapshot& localUnit ,
		const AutoComboOffsets& offsets , std::array<std::string , 6>& out ) -> void
	{
		out = {};
		std::vector<CHandle> abilityHandles;
		const auto* field = reinterpret_cast<const void*>( reinterpret_cast<uintptr_t>( localUnit.entity ) + offsets.abilities );
		if ( !ReadHandleVector( field , 48 , abilityHandles ) )
			return;

		int fallbackSlot = 0;
		for ( const auto& handle : abilityHandles )
		{
			CEntityIdentity* identity = nullptr;
			auto* ability = EntityFromHandle( entitySystem , handle , &identity );
			if ( !ability )
				continue;

			const std::string name = EntityName( ability , identity );
			if ( name.empty() ||
				name.rfind( "special_bonus_" , 0 ) == 0 ||
				name.rfind( "generic_" , 0 ) == 0 ||
				name.rfind( "ability_" , 0 ) == 0 )
				continue;

			int slot = PreferredSlotForAbility( name );
			if ( slot < 0 || slot >= static_cast<int>( out.size() ) )
				slot = fallbackSlot;
			++fallbackSlot;
			if ( slot >= 0 && slot < static_cast<int>( out.size() ) && out[slot].empty() )
				out[slot] = name;
		}
	}

	// Heroes whose spells can't be cast by static slot hotkeys. Invoker's real
	// spells are invoked dynamically into D/F via orb sequences - pressing
	// Q/W/E just switches orbs - so his spell casting is delegated to
	// CInvokerController's orb-sequence combo instead.
	auto IsSlotlessKitHero( const std::string& heroName ) -> bool
	{
		return heroName.find( "npc_dota_hero_invoker" ) != std::string::npos;
	}

	auto LocalHeroName( const UnitSnapshot& localUnit ) -> std::string
	{
		if ( !localUnit.entity )
			return {};
		return EntityName( localUnit.entity , localUnit.entity->pEntityIdentity() );
	}

	// includeAbilities=false leaves only items and the auto-attack in the plan
	// (used for slotless-kit heroes, whose spells go through their own
	// controller).
	auto CollectTools( CGameEntitySystem* entitySystem , const UnitSnapshot& localUnit , const AutoComboOffsets& offsets , bool includeAbilities ) -> std::vector<ComboTool>
	{
		std::vector<ComboTool> tools;
		static constexpr std::array<WORD, 6> kAbilityKeys = { 'Q' , 'W' , 'E' , 'D' , 'F' , 'R' };
		static constexpr std::array<WORD, 6> kItemKeys = { 'Z' , 'X' , 'C' , 'V' , 'B' , 'N' };

		if ( includeAbilities && Settings::AutoCombo::UseAbilities )
		{
			std::vector<CHandle> abilityHandles;
			const auto* field = reinterpret_cast<const void*>( reinterpret_cast<uintptr_t>( localUnit.entity ) + offsets.abilities );
			if ( ReadHandleVector( field , 48 , abilityHandles ) )
			{
				int fallbackSlot = 0;
				for ( const auto& handle : abilityHandles )
				{
					CEntityIdentity* identity = nullptr;
					auto* ability = EntityFromHandle( entitySystem , handle , &identity );
					if ( !ability )
						continue;

					const std::string abilityName = EntityName( ability , identity );
					const auto* data = FindDamageEntry( abilityName );
					if ( !data || ( !data->unitTarget && !data->noTarget && !data->pointTarget ) )
						continue;

					const int level = ReadField<int>( ability , offsets.abilityLevel , 0 );
					if ( level <= 0 )
						continue;

					if ( offsets.hasAbilityActivated && !ReadField<bool>( ability , offsets.abilityActivated , true ) )
						continue;

					const float cooldown = ReadField<float>( ability , offsets.abilityCooldown , 0.f );
					if ( std::isfinite( cooldown ) && cooldown > 0.15f )
						continue;

					int manaCost = ReadField<int>( ability , offsets.abilityManaCost , data->ManaForLevel( level ) );
					if ( manaCost <= 0 )
						manaCost = data->ManaForLevel( level );
					if ( manaCost > static_cast<int>( localUnit.mana + 0.5f ) )
						continue;

					int preferredSlot = PreferredSlotForAbility( abilityName );
					if ( preferredSlot < 0 || preferredSlot >= static_cast<int>( kAbilityKeys.size() ) )
						preferredSlot = fallbackSlot;
					++fallbackSlot;
					if ( preferredSlot < 0 || preferredSlot >= static_cast<int>( kAbilityKeys.size() ) )
						continue;

					// Per-slot opt-out from the menu's spell-order editor.
					if ( !Settings::AutoCombo::SpellEnabled[preferredSlot] )
						continue;

					ComboTool tool{};
					tool.kind = ComboToolKind::Ability;
					tool.name = abilityName;
					tool.castRange = data->noTarget ? 25000.f : ReadCastRange( ability , offsets , data->castRange );
					tool.key = kAbilityKeys[preferredSlot];
					tool.slot = preferredSlot;
					tool.noTarget = data->noTarget;
					tool.unitTarget = data->unitTarget;
					tool.pointTarget = data->pointTarget;
					tool.hasDamage = data->IsUsableDamage();
					tool.delayMs = data->noTarget ? 90u : ( Settings::AutoCombo::QuickCast ? 120u : 180u );
					tools.push_back( tool );
				}
			}
		}

		if ( Settings::AutoCombo::UseItems )
		{
			std::vector<CHandle> itemHandles;
			if ( ReadInventoryHandles( localUnit.entity , offsets , itemHandles ) )
			{
				const int slotLimit = (std::min)( static_cast<int>( itemHandles.size() ) , static_cast<int>( kItemKeys.size() ) );
				for ( int slot = 0; slot < slotLimit; ++slot )
				{
					CEntityIdentity* identity = nullptr;
					auto* item = EntityFromHandle( entitySystem , itemHandles[slot] , &identity );
					if ( !item )
						continue;

					const std::string itemName = EntityName( item , identity );
					const auto* data = FindDamageEntry( itemName );
					if ( !data || ( !data->unitTarget && !data->noTarget && !data->pointTarget ) )
						continue;

					const int level = (std::max)( 1 , ReadField<int>( item , offsets.abilityLevel , 1 ) );
					const float cooldown = ReadField<float>( item , offsets.abilityCooldown , 0.f );
					if ( std::isfinite( cooldown ) && cooldown > 0.15f )
						continue;

					int manaCost = ReadField<int>( item , offsets.abilityManaCost , data->ManaForLevel( level ) );
					if ( manaCost <= 0 )
						manaCost = data->ManaForLevel( level );
					if ( manaCost > static_cast<int>( localUnit.mana + 0.5f ) )
						continue;

					ComboTool tool{};
					tool.kind = ComboToolKind::Item;
					tool.name = itemName;
					tool.castRange = data->noTarget ? 25000.f : ReadCastRange( item , offsets , data->castRange );
					tool.key = kItemKeys[slot];
					tool.noTarget = data->noTarget;
					tool.unitTarget = data->unitTarget;
					tool.pointTarget = data->pointTarget;
					tool.hasDamage = data->IsUsableDamage();
					tool.delayMs = data->noTarget ? 90u : ( Settings::AutoCombo::QuickCast ? 120u : 180u );
					tools.push_back( tool );
				}
			}
		}

		if ( Settings::AutoCombo::UseAutoAttack && localUnit.attackDamage > 0.f && Settings::AutoCombo::AttackKey > 0 )
		{
			ComboTool tool{};
			tool.kind = ComboToolKind::Attack;
			tool.name = "auto_attack";
			tool.castRange = localUnit.attackRange + 75.f;
			tool.key = static_cast<WORD>( Settings::AutoCombo::AttackKey );
			tool.unitTarget = true;
			tool.delayMs = 220u;
			tools.push_back( tool );
		}

		// Abilities cast in the user's per-slot order (Settings::AutoCombo::SpellOrder,
		// edited in the menu's spell-order editor - Zeus players can put E before R
		// before W, etc). Items follow after every spell, the auto-attack finisher
		// goes last. Stable sort keeps item slot order among themselves.
		std::stable_sort( tools.begin() , tools.end() , []( const ComboTool& a , const ComboTool& b )
		{
			auto rank = []( const ComboTool& t ) -> int
			{
				if ( t.kind == ComboToolKind::Attack )
					return 200;
				if ( t.kind == ComboToolKind::Item )
					return 100;
				for ( int position = 0; position < Settings::AutoCombo::SpellSlotCount; ++position )
				{
					if ( Settings::AutoCombo::SpellOrder[position] == t.slot )
						return position;
				}
				return Settings::AutoCombo::SpellSlotCount;
			};
			return rank( a ) < rank( b );
		} );

		return tools;
	}

	auto WindowReadyForInput() -> HWND
	{
		auto* gui = GetAndromedaGUI();
		const HWND window = gui ? gui->m_hCS2Window : nullptr;
		if ( !window || GetForegroundWindow() != window || gui->IsVisible() )
			return nullptr;
		return window;
	}

	auto MoveCursorToClientPoint( HWND window , const ImVec2& screen , POINT& previousOut ) -> bool
	{
		RECT client{};
		if ( !GetClientRect( window , &client ) || client.right <= 0 || client.bottom <= 0 )
			return false;

		const int x = std::clamp( static_cast<int>( std::lround( screen.x ) ) , 0 , static_cast<int>( client.right ) - 1 );
		const int y = std::clamp( static_cast<int>( std::lround( screen.y ) ) , 0 , static_cast<int>( client.bottom ) - 1 );
		POINT target{ x , y };
		if ( !ClientToScreen( window , &target ) )
			return false;

		GetCursorPos( &previousOut );
		return SetCursorPos( target.x , target.y ) != FALSE;
	}

	// Key events must carry a scan code, not just a virtual key: Dota
	// (Source 2 / SDL) consumes raw input, where a wVk-only event has no
	// usable scan code and gets ignored - see CInvokerController.cpp, where
	// this silently swallowed every orb/Invoke press.
	auto SendKeyPress( WORD key ) -> bool
	{
		auto MakeKeyInput = []( WORD vk , bool keyUp )
		{
			INPUT input{};
			input.type = INPUT_KEYBOARD;
			input.ki.wVk = 0;
			input.ki.wScan = static_cast<WORD>( MapVirtualKeyW( vk , MAPVK_VK_TO_VSC ) );
			input.ki.dwFlags = KEYEVENTF_SCANCODE | ( keyUp ? KEYEVENTF_KEYUP : 0 );
			return input;
		};
		INPUT inputs[2] = { MakeKeyInput( key , false ) , MakeKeyInput( key , true ) };
		return SendInput( static_cast<UINT>( std::size( inputs ) ) , inputs , sizeof( INPUT ) ) == std::size( inputs );
	}

	auto SendLeftClick() -> bool
	{
		INPUT inputs[2]{};
		inputs[0].type = INPUT_MOUSE;
		inputs[0].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
		inputs[1].type = INPUT_MOUSE;
		inputs[1].mi.dwFlags = MOUSEEVENTF_LEFTUP;
		return SendInput( static_cast<UINT>( std::size( inputs ) ) , inputs , sizeof( INPUT ) ) == std::size( inputs );
	}

	// Raising the aim point helps land the cursor on a UNIT's model, but for a
	// ground-targeted spell it is actively wrong: a point above the hero
	// projects higher on screen, and the ground under that screen pixel lies
	// further from the camera - so the spell lands BEHIND the target.
	auto ProjectTargetScreen( const Vector3& origin , bool groundTargeted , ImVec2& out ) -> bool
	{
		if ( !groundTargeted )
		{
			Vector3 targetPoint = origin;
			targetPoint.m_z += 80.f;
			if ( Math::WorldToScreen( targetPoint , out ) )
				return true;
		}
		return Math::WorldToScreen( origin , out );
	}

	auto AimCursorAtTarget( HWND window , const Vector3& targetOrigin , bool groundTargeted ) -> bool
	{
		ImVec2 screen{};
		if ( !ProjectTargetScreen( targetOrigin , groundTargeted , screen ) )
			return false;
		POINT previous{};
		return MoveCursorToClientPoint( window , screen , previous );
	}

	// How long the cursor is left parked on the target before the key/click is
	// sent, and again before it is restored. Dota consumes injected input on
	// its own message loop, not synchronously with SendInput, so both waits
	// must be at least a frame or the cast resolves at the pre-aim cursor
	// position. One think tick (30ms) plus margin.
	constexpr uint32_t kCastSettleMs = 45;
}

auto CAutoCombo::CancelPlan( const char* reason ) -> void
{
	// Never leave the cursor parked on the target if the plan dies mid-cast.
	if ( m_Plan.hasPrevCursor )
		SetCursorPos( m_Plan.prevCursorX , m_Plan.prevCursorY );
	m_Plan = {};
	if ( reason )
	{
		m_Status = reason;
		DEV_LOG( "[auto-combo] cancelled: %s\n" , reason );
	}
}

auto CAutoCombo::OnRender() -> void
{
	const uint32_t now = GetTickCount();

	if ( !Settings::AutoCombo::Enable )
	{
		if ( m_Plan.active )
			CancelPlan();
		m_Status = "Disabled";
		return;
	}

	constexpr uint32_t kThinkIntervalMs = 30;
	if ( now < m_NextThinkTick )
		return;
	m_NextThinkTick = now + kThinkIntervalMs;

	auto& offsets = ResolveOffsets();
	auto* entitySystem = SDK::Interfaces::GameEntitySystem();
	if ( !entitySystem || !offsets.resolved )
	{
		CancelPlan();
		m_Status = "Waiting for offsets";
		return;
	}

	// Keep the menu's spell-order editor labeled with the hero's real spell
	// names. Only while the menu is open (casting is blocked then anyway) and
	// at a slow cadence - it re-scans the ability list each refresh.
	if ( GetAndromedaGUI()->IsVisible() && now >= m_NextSlotNameRefresh )
	{
		m_NextSlotNameRefresh = now + 500;
		UnitSnapshot localUnit{};
		if ( ResolveLocalUnit( entitySystem , offsets , localUnit ) )
			CollectSlotNames( entitySystem , localUnit , offsets , m_SlotNames );
	}

	// Advance an in-flight plan first.
	if ( m_Plan.active )
	{
		if ( now >= m_Plan.expiresAt )
		{
			CancelPlan( "Expired" );
		}
		else
		{
			UnitSnapshot target{};
			if ( !ResolveTargetUnit( entitySystem , offsets , m_Plan.targetEntIndex , target ) )
			{
				// A dead target is the combo doing its job, not a failure - only
				// a vanished entity (fog, reconnect) is a real "lost".
				auto* entity = entitySystem->GetBaseEntity( m_Plan.targetEntIndex );
				const bool died = entity && ReadField<int>( entity , offsets.health , 0 ) <= 0;
				CancelPlan( died ? "Target died" : "Target lost" );
			}
			else if ( m_Plan.actionIndex >= m_Plan.actions.size() )
			{
				CancelPlan();
				m_Status = "Done";
			}
			else if ( now >= m_Plan.nextActionTick )
			{
				const auto& action = m_Plan.actions[m_Plan.actionIndex];
				const HWND window = WindowReadyForInput();

				auto AdvanceAction = [&]( uint32_t delayMs )
				{
					m_Plan.castPhase = CastPhase::Aim;
					++m_Plan.actionIndex;
					if ( m_Plan.actionIndex >= m_Plan.actions.size() )
					{
						DEV_LOG( "[auto-combo] plan finished: all %zu actions cast vs entindex=%d\n" ,
							m_Plan.actions.size() , m_Plan.targetEntIndex );
						CancelPlan();
						m_Status = "Done";
					}
					else
					{
						m_Plan.nextActionTick = now + delayMs;
					}
				};

				if ( !window || action.key == 0 )
				{
					CancelPlan( "Cast failed" );
				}
				else if ( action.noTarget )
				{
					// Cursor position is irrelevant - fire in one step.
					if ( !SendKeyPress( static_cast<WORD>( action.key ) ) )
						CancelPlan( "Cast failed" );
					else
					{
						m_Status = std::string( "Casting " ) + action.name;
						AdvanceAction( action.delayMs );
					}
				}
				else switch ( m_Plan.castPhase )
				{
				case CastPhase::Aim:
				{
					// Re-select our hero first: the previous action in this plan
					// left-clicked the enemy to aim, and a left-click on an enemy
					// unit selects it - after which every ability hotkey applies
					// to nothing. Dota's default "Select Hero" bind.
					SendKeyPress( VK_F1 );

					POINT previous{};
					GetCursorPos( &previous );
					if ( !AimCursorAtTarget( window , target.origin , action.pointTarget ) )
					{
						CancelPlan( "Cast failed" );
						break;
					}
					m_Plan.prevCursorX = previous.x;
					m_Plan.prevCursorY = previous.y;
					m_Plan.hasPrevCursor = true;
					m_Plan.castPhase = CastPhase::Cast;
					m_Plan.nextActionTick = now + kCastSettleMs;
					break;
				}
				case CastPhase::Cast:
				{
					// Re-aim right before the press - the target may have moved
					// during the settle window.
					AimCursorAtTarget( window , target.origin , action.pointTarget );
					const bool needsClick = action.kind == ComboPlanAction::Kind::Attack || !Settings::AutoCombo::QuickCast;
					if ( !SendKeyPress( static_cast<WORD>( action.key ) ) || ( needsClick && !SendLeftClick() ) )
					{
						CancelPlan( "Cast failed" );
						break;
					}
					m_Status = std::string( "Casting " ) + action.name;
					m_Plan.castPhase = CastPhase::Restore;
					m_Plan.nextActionTick = now + kCastSettleMs;
					break;
				}
				case CastPhase::Restore:
				{
					if ( m_Plan.hasPrevCursor )
					{
						SetCursorPos( m_Plan.prevCursorX , m_Plan.prevCursorY );
						m_Plan.hasPrevCursor = false;
					}
					AdvanceAction( action.delayMs );
					break;
				}
				}
			}
		}
	}

	// Edge-triggered hotkey: start a fresh plan.
	const bool comboKeyDown = Settings::AutoCombo::ComboKey > 0 &&
		( GetAsyncKeyState( Settings::AutoCombo::ComboKey ) & 0x8000 ) != 0;

	if ( comboKeyDown && !m_ComboKeyWasDown && !m_Plan.active )
	{
		UnitSnapshot localUnit{};
		UnitSnapshot target{};

		// Auto-target the nearest enemy hero unless the user pinned a specific
		// entindex (e.g. -1 is the default, so most users never manage to set
		// one, and the combo used to just sit there doing nothing).
		constexpr float kAutoTargetRange = 1800.f;
		int resolvedTargetIndex = Settings::AutoCombo::TargetEntIndex;

		if ( !ResolveLocalUnit( entitySystem , offsets , localUnit ) )
		{
			m_Status = "Local hero unresolved";
		}
		else if ( resolvedTargetIndex < 0 )
		{
			resolvedTargetIndex = FindNearestEnemyHero( entitySystem , offsets , localUnit , kAutoTargetRange );
			if ( resolvedTargetIndex < 0 )
				m_Status = "No enemy hero nearby";
		}

		if ( localUnit.entity && resolvedTargetIndex >= 0 && !ResolveTargetUnit( entitySystem , offsets , resolvedTargetIndex , target ) )
		{
			m_Status = "Target invalid";
		}
		else if ( localUnit.entity && resolvedTargetIndex >= 0 && target.entity )
		{
			// Slotless-kit heroes (Invoker): spells go through the hero's own
			// controller, the generic plan carries only items/auto-attack.
			const bool slotlessKit = IsSlotlessKitHero( LocalHeroName( localUnit ) );
			bool invokerComboRequested = false;
			if ( slotlessKit && Settings::AutoCombo::UseAbilities )
			{
				if ( auto* pClient = GetAndromedaClient() )
				{
					// The Invoker combo aims its final cast at its own target
					// setting (and refuses targeted spells without one), so hand
					// it the target this combo press resolved.
					Settings::Heroes::Invoker::TargetEntIndex = resolvedTargetIndex;
					pClient->GetInvokerController().RequestCombo();
					invokerComboRequested = true;
					DEV_LOG( "[auto-combo] invoker kit detected - delegated spell cast to CInvokerController (target=%d)\n" , resolvedTargetIndex );
				}
			}

			const auto tools = CollectTools( entitySystem , localUnit , offsets , !slotlessKit );
			const float distance = Distance2D( localUnit.origin , target.origin );

			ComboPlanState plan{};
			plan.targetEntIndex = resolvedTargetIndex;

			constexpr size_t kMaxActions = 12;
			for ( const auto& tool : tools )
			{
				if ( plan.actions.size() >= kMaxActions )
					break;
				if ( !tool.noTarget && distance > tool.castRange + 75.f )
					continue;

				ComboPlanAction action{};
				action.kind = tool.kind == ComboToolKind::Item ? ComboPlanAction::Kind::Item :
					tool.kind == ComboToolKind::Attack ? ComboPlanAction::Kind::Attack : ComboPlanAction::Kind::Ability;
				action.name = tool.name;
				action.key = static_cast<uint16_t>( tool.key );
				action.delayMs = tool.delayMs;
				action.noTarget = tool.noTarget;
				action.unitTarget = tool.unitTarget;
				action.pointTarget = tool.pointTarget;
				plan.actions.push_back( action );
			}

			if ( plan.actions.empty() )
			{
				m_Status = invokerComboRequested ? "Invoker combo delegated (no usable items)"
					: "No usable abilities/items in range";
			}
			else
			{
				plan.active = true;
				plan.nextActionTick = now;
				plan.expiresAt = now + 8000u;
				m_Plan = plan;
				m_Status = invokerComboRequested ? "Invoker combo delegated + items started" : "Combo started";
				DEV_LOG( "[auto-combo] plan started with %zu actions vs entindex=%d\n" , plan.actions.size() , plan.targetEntIndex );
			}
		}
	}

	m_ComboKeyWasDown = comboKeyDown;
}
