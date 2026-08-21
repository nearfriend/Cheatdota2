#include "CAutoCombo.hpp"

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
		bool hasInventory = false;
		bool hasAbilityActivated = false;
		bool hasDamageBonus = false;
		bool hasAttackRange = false;
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
	};

	struct ComboTool
	{
		ComboToolKind kind = ComboToolKind::Ability;
		std::string name;
		float castRange = 0.f;
		WORD key = 0;
		bool noTarget = false;
		bool unitTarget = false;
		bool pointTarget = false;
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

	template <typename T>
	auto TryRead( const void* address , T& out ) -> bool
	{
		if ( !IsReadableRuntimeMemory( address , sizeof( T ) ) )
			return false;
		std::memcpy( &out , address , sizeof( T ) );
		return true;
	}

	template <typename T>
	auto TryReadField( const void* base , uint32_t offset , T& out ) -> bool
	{
		if ( !base || !offset )
			return false;
		return TryRead( reinterpret_cast<const void*>( reinterpret_cast<uintptr_t>( base ) + offset ) , out );
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

	auto ResolveLocalUnit( CGameEntitySystem* entitySystem , const AutoComboOffsets& offsets , UnitSnapshot& out ) -> bool
	{
		auto* controller = CGameEntitySystem::GetLocalPlayerController();
		if ( !controller )
			return false;

		const CHandle heroHandle = controller->m_hAssignedHero();
		if ( !heroHandle.IsValid() )
			return false;

		auto* entity = entitySystem->GetBaseEntityFromHandle( heroHandle );
		if ( !entity )
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
		return true;
	}

	auto CollectTools( CGameEntitySystem* entitySystem , const UnitSnapshot& localUnit , const AutoComboOffsets& offsets ) -> std::vector<ComboTool>
	{
		std::vector<ComboTool> tools;
		static constexpr std::array<WORD, 6> kAbilityKeys = { 'Q' , 'W' , 'E' , 'D' , 'F' , 'R' };
		static constexpr std::array<WORD, 6> kItemKeys = { 'Z' , 'X' , 'C' , 'V' , 'B' , 'N' };

		if ( Settings::AutoCombo::UseAbilities )
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
					if ( !data || !data->IsUsableDamage() || ( !data->unitTarget && !data->noTarget && !data->pointTarget ) )
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

					const float rawDamage = data->DamageForLevel( level );
					if ( rawDamage <= 0.f )
						continue;

					int preferredSlot = PreferredSlotForAbility( abilityName );
					if ( preferredSlot < 0 || preferredSlot >= static_cast<int>( kAbilityKeys.size() ) )
						preferredSlot = fallbackSlot;
					++fallbackSlot;
					if ( preferredSlot < 0 || preferredSlot >= static_cast<int>( kAbilityKeys.size() ) )
						continue;

					ComboTool tool{};
					tool.kind = ComboToolKind::Ability;
					tool.name = abilityName;
					tool.castRange = data->noTarget ? 25000.f : ReadCastRange( ability , offsets , data->castRange );
					tool.key = kAbilityKeys[preferredSlot];
					tool.noTarget = data->noTarget;
					tool.unitTarget = data->unitTarget;
					tool.pointTarget = data->pointTarget;
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
					if ( !data || !data->IsUsableDamage() || ( !data->unitTarget && !data->noTarget && !data->pointTarget ) )
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

					const float rawDamage = data->DamageForLevel( level );
					if ( rawDamage <= 0.f )
						continue;

					ComboTool tool{};
					tool.kind = ComboToolKind::Item;
					tool.name = itemName;
					tool.castRange = data->noTarget ? 25000.f : ReadCastRange( item , offsets , data->castRange );
					tool.key = kItemKeys[slot];
					tool.noTarget = data->noTarget;
					tool.unitTarget = data->unitTarget;
					tool.pointTarget = data->pointTarget;
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

		// No-target/self-cast first, then point-target ground spells, then unit-target
		// spells and the finishing auto-attack - lets buffs/setups land before the
		// unit-target follow-ups that benefit from them.
		std::stable_sort( tools.begin() , tools.end() , []( const ComboTool& a , const ComboTool& b )
		{
			auto rank = []( const ComboTool& t ) -> int
			{
				if ( t.noTarget ) return 0;
				if ( t.pointTarget ) return 1;
				return 2;
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

	auto SendKeyPress( WORD key ) -> bool
	{
		INPUT inputs[2]{};
		inputs[0].type = INPUT_KEYBOARD;
		inputs[0].ki.wVk = key;
		inputs[1] = inputs[0];
		inputs[1].ki.dwFlags = KEYEVENTF_KEYUP;
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

	auto ProjectTargetScreen( const Vector3& origin , ImVec2& out ) -> bool
	{
		Vector3 targetPoint = origin;
		targetPoint.m_z += 80.f;
		if ( Math::WorldToScreen( targetPoint , out ) )
			return true;
		return Math::WorldToScreen( origin , out );
	}

	auto CastPlanAction( const CAutoCombo::ComboPlanAction& action , const Vector3& targetOrigin ) -> bool
	{
		const HWND window = WindowReadyForInput();
		if ( !window || action.key == 0 )
			return false;

		if ( action.noTarget )
			return SendKeyPress( static_cast<WORD>( action.key ) );

		ImVec2 screen{};
		if ( !ProjectTargetScreen( targetOrigin , screen ) )
			return false;

		POINT previous{};
		if ( !MoveCursorToClientPoint( window , screen , previous ) )
			return false;

		const bool needsClick = action.kind == CAutoCombo::ComboPlanAction::Kind::Attack || !Settings::AutoCombo::QuickCast;
		const bool sent = SendKeyPress( static_cast<WORD>( action.key ) ) && ( !needsClick || SendLeftClick() );
		SetCursorPos( previous.x , previous.y );
		return sent;
	}
}

auto CAutoCombo::CancelPlan( const char* reason ) -> void
{
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
				CancelPlan( "Target lost" );
			}
			else if ( m_Plan.actionIndex >= m_Plan.actions.size() )
			{
				CancelPlan();
				m_Status = "Done";
			}
			else if ( now >= m_Plan.nextActionTick )
			{
				const auto& action = m_Plan.actions[m_Plan.actionIndex];
				if ( !CastPlanAction( action , target.origin ) )
				{
					CancelPlan( "Cast failed" );
				}
				else
				{
					m_Status = std::string( "Casting " ) + action.name;
					++m_Plan.actionIndex;
					if ( m_Plan.actionIndex >= m_Plan.actions.size() )
					{
						CancelPlan();
						m_Status = "Done";
					}
					else
					{
						m_Plan.nextActionTick = now + action.delayMs;
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
		if ( Settings::AutoCombo::TargetEntIndex < 0 )
		{
			m_Status = "No target set";
		}
		else if ( !ResolveLocalUnit( entitySystem , offsets , localUnit ) )
		{
			m_Status = "Local hero unresolved";
		}
		else if ( !ResolveTargetUnit( entitySystem , offsets , Settings::AutoCombo::TargetEntIndex , target ) )
		{
			m_Status = "Target invalid";
		}
		else
		{
			const auto tools = CollectTools( entitySystem , localUnit , offsets );
			const float distance = Distance2D( localUnit.origin , target.origin );

			ComboPlanState plan{};
			plan.targetEntIndex = Settings::AutoCombo::TargetEntIndex;

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
				m_Status = "No usable abilities/items in range";
			}
			else
			{
				plan.active = true;
				plan.nextActionTick = now;
				plan.expiresAt = now + 8000u;
				m_Plan = plan;
				m_Status = "Combo started";
				DEV_LOG( "[auto-combo] plan started with %zu actions vs entindex=%d\n" , plan.actions.size() , plan.targetEntIndex );
			}
		}
	}

	m_ComboKeyWasDown = comboKeyDown;
}
