#include "FeatureSupport.hpp"

#include <AndromedaClient/CAndromedaGUI.hpp>
#include <Dota2/SDK/CSchemaOffset.hpp>
#include <Dota2/SDK/Interface/CGameEntitySystem.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>

namespace
{
	// Layout of a replicated CNetworkUtlVectorBase<CHandle> - size, padding,
	// then the heap pointer.
	struct NetworkHandleVector
	{
		int32_t size = 0;
		int32_t pad = 0;
		const CHandle* data = nullptr;
		int32_t allocationCount = 0;
		int32_t growSize = 0;
	};
}

auto FeatureSupport::IsReadableRuntimeMemory( const void* ptr , size_t size ) -> bool
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

auto FeatureSupport::ResolveOffsets() -> const UnitOffsets&
{
	static UnitOffsets offsets{};
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
	const bool hasTeam = schema->TryGetOffset( "C_BaseEntity" , "m_iTeamNum" , offsets.team );
	const bool hasMana = schema->TryGetOffset( "C_DOTA_BaseNPC" , "m_flMana" , offsets.mana );
	const bool hasAbilities = schema->TryGetOffset( "C_DOTA_BaseNPC" , "m_vecAbilities" , offsets.abilities );
	const bool hasSceneNode = schema->TryGetOffset( "C_BaseEntity" , "m_pGameSceneNode" , offsets.sceneNode );
	const bool hasAbsOrigin = schema->TryGetOffset( "CGameSceneNode" , "m_vecAbsOrigin" , offsets.absOrigin );
	const bool hasAbilityLevel = schema->TryGetOffset( "C_DOTABaseAbility" , "m_iLevel" , offsets.abilityLevel );
	const bool hasAbilityCooldown = schema->TryGetOffset( "C_DOTABaseAbility" , "m_flCooldown" , offsets.abilityCooldown ) ||
		schema->TryGetOffset( "C_DOTABaseAbility" , "m_fCooldown" , offsets.abilityCooldown );
	const bool hasAbilityMana = schema->TryGetOffset( "C_DOTABaseAbility" , "m_iManaCost" , offsets.abilityManaCost );

	// m_nCastRange first, and it is not a guess: grepping this build's
	// client.dll for schema field names finds m_nCastRange (plus its
	// neighbours m_nCastRangeBuffer / m_bCastRangeRequired) and finds NO
	// m_flCastRange at all. CKillStealer and CAutoCombo both ask only for
	// m_flCastRange, so their cast ranges have always been the JSON catalog
	// fallback rather than the live value. The other spellings stay in the
	// chain in case a future build renames it back.
	schema->TryGetOffset( "C_DOTABaseAbility" , "m_nCastRange" , offsets.abilityCastRange ) ||
		schema->TryGetOffset( "C_DOTABaseAbility" , "m_flCastRange" , offsets.abilityCastRange ) ||
		schema->TryGetOffset( "C_DOTABaseAbility" , "m_iCastRange" , offsets.abilityCastRange );
	offsets.hasAbilityActivated = schema->TryGetOffset( "C_DOTABaseAbility" , "m_bIsActivated" , offsets.abilityActivated ) ||
		schema->TryGetOffset( "C_DOTABaseAbility" , "m_bActivated" , offsets.abilityActivated );
	offsets.hasAbilityInPhase = schema->TryGetOffset( "C_DOTABaseAbility" , "m_bInAbilityPhase" , offsets.abilityInPhase );
	offsets.hasAbilityChannelStart = schema->TryGetOffset( "C_DOTABaseAbility" , "m_flChannelStartTime" , offsets.abilityChannelStart );
	offsets.hasRotation = schema->TryGetOffset( "CGameSceneNode" , "m_angRotation" , offsets.rotation );

	const bool hasInventoryContainer = schema->TryGetOffset( "C_DOTA_BaseNPC" , "m_Inventory" , offsets.inventory );
	const bool hasNestedItems = schema->TryGetOffset( "C_DOTA_UnitInventory" , "m_hItems" , offsets.inventoryItems ) ||
		schema->TryGetOffset( "CDOTA_UnitInventory" , "m_hItems" , offsets.inventoryItems );
	if ( hasNestedItems )
		offsets.hasInventory = hasInventoryContainer;
	else
		offsets.hasInventory = schema->TryGetOffset( "C_DOTA_BaseNPC" , "m_hItems" , offsets.inventoryItems );

	offsets.hasIsIllusion = schema->TryGetOffset( "C_DOTA_BaseNPC" , "m_bIsIllusion" , offsets.isIllusion );
	offsets.hasIsClone = schema->TryGetOffset( "C_DOTA_BaseNPC" , "m_bIsClone" , offsets.isClone );
	offsets.hasWaitingToSpawn = schema->TryGetOffset( "C_DOTA_BaseNPC" , "m_bIsWaitingToSpawn" , offsets.waitingToSpawn );
	// The real one: a live capture showed none of the individual state
	// booleans below resolving, while Dota keeps every unit state in this
	// single 64-bit mask.
	offsets.hasUnitState = schema->TryGetOffset( "C_DOTA_BaseNPC" , "m_nUnitState64" , offsets.unitState ) ||
		schema->TryGetOffset( "C_DOTA_BaseNPC" , "m_nUnitState" , offsets.unitState );

	// Both spellings appear in this build's client.dll strings; which one (if
	// either) is a replicated C_DOTA_BaseNPC field is a runtime question, so
	// ask for both and let the has* flag say whether anyone answered.
	offsets.hasMagicImmune = schema->TryGetOffset( "C_DOTA_BaseNPC" , "m_bMagicImmune" , offsets.magicImmune ) ||
		schema->TryGetOffset( "C_DOTA_BaseNPC" , "m_bSpellImmunity" , offsets.magicImmune );
	offsets.hasInvulnerable = schema->TryGetOffset( "C_DOTA_BaseNPC" , "m_bInvulnerable" , offsets.invulnerable ) ||
		schema->TryGetOffset( "C_BaseEntity" , "m_bInvulnerable" , offsets.invulnerable );
	offsets.hasStunned = schema->TryGetOffset( "C_DOTA_BaseNPC" , "m_bStunned" , offsets.stunned );

	offsets.resolved = hasHealth && hasMaxHealth && hasTeam && hasMana && hasAbilities &&
		hasSceneNode && hasAbsOrigin && hasAbilityLevel && hasAbilityCooldown && hasAbilityMana;

	return offsets;
}

auto FeatureSupport::TryReadUnitState( C_BaseEntity* unit , const UnitOffsets& offsets , uint64_t& out ) -> bool
{
	out = 0;
	if ( !unit || !offsets.hasUnitState )
		return false;
	return TryReadField( unit , offsets.unitState , out );
}

auto FeatureSupport::HasUnitState( uint64_t stateMask , UnitStateBit bit ) -> bool
{
	return ( stateMask & ( 1ULL << static_cast<uint32_t>( bit ) ) ) != 0;
}

auto FeatureSupport::ToLower( const std::string& value ) -> std::string
{
	std::string out = value;
	std::transform( out.begin() , out.end() , out.begin() ,
		[]( unsigned char character ) { return static_cast<char>( std::tolower( character ) ); } );
	return out;
}

auto FeatureSupport::IsPlayableTeam( uint8_t team ) -> bool
{
	return team == 2 || team == 3;
}

auto FeatureSupport::Distance2D( const Vector3& left , const Vector3& right ) -> float
{
	const float dx = left.m_x - right.m_x;
	const float dy = left.m_y - right.m_y;
	return std::sqrt( dx * dx + dy * dy );
}

auto FeatureSupport::EntityName( C_BaseEntity* entity , CEntityIdentity* identity ) -> std::string
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

auto FeatureSupport::TryEntityAtIndex( CGameEntitySystem* entitySystem , int index ,
	CEntityIdentity*& identityOut , C_BaseEntity*& entityOut ) -> bool
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

auto FeatureSupport::EntityFromHandle( CGameEntitySystem* entitySystem , CHandle handle , CEntityIdentity** identityOut ) -> C_BaseEntity*
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

auto FeatureSupport::LooksLikeHeroEntity( C_BaseEntity* entity , const std::string& name ) -> bool
{
	if ( !entity )
		return false;
	const std::string lower = ToLower( name );
	if ( lower.find( "npc_dota_hero_" ) != std::string::npos )
		return true;
	const char* className = entity->GetSchemaClassName();
	return className && ( std::strstr( className , "DOTA_BaseNPC_Hero" ) || std::strstr( className , "DOTA_Unit_Hero" ) );
}

auto FeatureSupport::LooksLikeLaneCreep( C_BaseEntity* entity , const std::string& name , uint8_t team ) -> bool
{
	if ( !entity || !IsPlayableTeam( team ) )
		return false;

	const std::string lower = ToLower( name );
	if ( lower.empty() )
		return false;

	auto Contains = [&lower]( const char* needle )
	{
		return lower.find( needle ) != std::string::npos;
	};

	// Jungle camps and Roshan are on the neutral team, so they never reach
	// here; the rest of this list is what shares the two playing teams with a
	// lane creep. Summons mostly carry the "npc_dota_unit_" prefix, but the
	// ones that do not are named individually.
	static constexpr const char* kDenyNeedles[] =
	{
		"neutral" , "roshan" , "ancient" ,
		"tower" , "fort" , "barracks" , "rax" , "building" , "filler" ,
		"healer" , "shrine" , "outpost" , "watch" , "effigy" ,
		"ward" , "courier" , "hero" , "thinker" , "additive" , "player" ,
		"npc_dota_unit_" , "bear" , "wolf" , "treant" , "spiderling" ,
		"eidolon" , "familiar" , "zombie" , "golem" , "boar" , "hawk" ,
		"necronomicon" , "illusion" , "clone" , "spirit"
	};

	for ( const char* needle : kDenyNeedles )
	{
		if ( Contains( needle ) )
			return false;
	}

	return Contains( "npc_dota_creep" ) || Contains( "creep_lane" ) || Contains( "creep_siege" ) ||
		Contains( "creep_melee" ) || Contains( "creep_ranged" ) || Contains( "basenpc_creep" ) ||
		Contains( "flagbearer" ) || Contains( "siege" ) ||
		Contains( "goodguys_melee" ) || Contains( "goodguys_ranged" ) ||
		Contains( "badguys_melee" ) || Contains( "badguys_ranged" );
}

auto FeatureSupport::TryReadOrigin( C_BaseEntity* entity , const UnitOffsets& offsets , Vector3& out ) -> bool
{
	void* sceneNode = nullptr;
	if ( !TryReadField( entity , offsets.sceneNode , sceneNode ) || !sceneNode )
		return false;
	if ( !TryReadField( sceneNode , offsets.absOrigin , out ) )
		return false;
	return std::isfinite( out.m_x ) && std::isfinite( out.m_y ) && std::isfinite( out.m_z );
}

auto FeatureSupport::TryReadYaw( C_BaseEntity* entity , const UnitOffsets& offsets , float& out ) -> bool
{
	if ( !offsets.hasRotation )
		return false;

	void* sceneNode = nullptr;
	if ( !TryReadField( entity , offsets.sceneNode , sceneNode ) || !sceneNode )
		return false;

	// m_angRotation is a QAngle: pitch, yaw, roll.
	float angles[3] = { 0.f , 0.f , 0.f };
	if ( !TryReadField( sceneNode , offsets.rotation , angles ) )
		return false;
	if ( !std::isfinite( angles[1] ) )
		return false;

	out = angles[1];
	return true;
}

auto FeatureSupport::ReadHandleVector( const void* field , int maxCount , std::vector<CHandle>& out ) -> bool
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

auto FeatureSupport::ReadAbilityHandles( C_BaseEntity* unit , const UnitOffsets& offsets , std::vector<CHandle>& out ) -> bool
{
	out.clear();
	if ( !unit || !offsets.abilities )
		return false;

	const auto* field = reinterpret_cast<const void*>( reinterpret_cast<uintptr_t>( unit ) + offsets.abilities );
	return ReadHandleVector( field , 48 , out );
}

auto FeatureSupport::ReadInventoryHandles( C_BaseEntity* unit , const UnitOffsets& offsets , std::vector<CHandle>& out ) -> bool
{
	out.clear();
	if ( !unit || !offsets.hasInventory )
		return false;

	const uintptr_t itemsBase = reinterpret_cast<uintptr_t>( unit ) + offsets.inventory + offsets.inventoryItems;
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

auto FeatureSupport::ReadCastRange( C_BaseEntity* ability , const UnitOffsets& offsets , float fallback ) -> float
{
	float castRange = fallback;
	if ( offsets.abilityCastRange )
	{
		int rangeInt = 0;
		if ( TryReadField( ability , offsets.abilityCastRange , rangeInt ) && rangeInt >= 50 && rangeInt <= 25000 )
		{
			castRange = static_cast<float>( rangeInt );
		}
		else
		{
			float rangeFloat = 0.f;
			if ( TryReadField( ability , offsets.abilityCastRange , rangeFloat ) &&
				std::isfinite( rangeFloat ) && rangeFloat >= 50.f && rangeFloat <= 25000.f )
				castRange = rangeFloat;
		}
	}
	return castRange;
}

auto FeatureSupport::FindAbilityEntry( const std::string& name ) -> const AbilityDamageEntry*
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

auto FeatureSupport::PreferredSlotForAbility( const std::string& name ) -> int
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

auto FeatureSupport::WindowReadyForInput() -> HWND
{
	auto* gui = GetAndromedaGUI();
	const HWND window = gui ? gui->m_hCS2Window : nullptr;
	if ( !window || GetForegroundWindow() != window || gui->IsVisible() )
		return nullptr;
	return window;
}

auto FeatureSupport::SendKeyPress( WORD key ) -> bool
{
	auto MakeKeyInput = []( WORD vk , bool keyUp )
	{
		INPUT input{};
		input.type = INPUT_KEYBOARD;
		input.ki.wVk = 0;
		input.ki.wScan = static_cast<WORD>( MapVirtualKeyW( vk , MAPVK_VK_TO_VSC ) );
		input.ki.dwFlags = KEYEVENTF_SCANCODE | ( keyUp ? KEYEVENTF_KEYUP : 0 );
		input.ki.dwExtraInfo = ANDROMEDA_INJECTED_INPUT_TAG;
		return input;
	};
	INPUT inputs[2] = { MakeKeyInput( key , false ) , MakeKeyInput( key , true ) };
	return SendInput( static_cast<UINT>( std::size( inputs ) ) , inputs , sizeof( INPUT ) ) == std::size( inputs );
}

auto FeatureSupport::SendLeftClick() -> bool
{
	INPUT inputs[2]{};
	inputs[0].type = INPUT_MOUSE;
	inputs[0].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
	inputs[0].mi.dwExtraInfo = ANDROMEDA_INJECTED_INPUT_TAG;
	inputs[1].type = INPUT_MOUSE;
	inputs[1].mi.dwFlags = MOUSEEVENTF_LEFTUP;
	inputs[1].mi.dwExtraInfo = ANDROMEDA_INJECTED_INPUT_TAG;
	return SendInput( static_cast<UINT>( std::size( inputs ) ) , inputs , sizeof( INPUT ) ) == std::size( inputs );
}

auto FeatureSupport::SendRightClick() -> bool
{
	INPUT inputs[2]{};
	inputs[0].type = INPUT_MOUSE;
	inputs[0].mi.dwFlags = MOUSEEVENTF_RIGHTDOWN;
	inputs[0].mi.dwExtraInfo = ANDROMEDA_INJECTED_INPUT_TAG;
	inputs[1].type = INPUT_MOUSE;
	inputs[1].mi.dwFlags = MOUSEEVENTF_RIGHTUP;
	inputs[1].mi.dwExtraInfo = ANDROMEDA_INJECTED_INPUT_TAG;
	return SendInput( static_cast<UINT>( std::size( inputs ) ) , inputs , sizeof( INPUT ) ) == std::size( inputs );
}

auto FeatureSupport::MoveCursorToScreen( int screenX , int screenY ) -> bool
{
	// SetCursorPos teleports the OS cursor directly - it does NOT go through
	// the same input pipeline as a real mouse move, so it never generates a
	// WM_INPUT / raw-input mouse event. Source 2 reads raw input to decide
	// where the player is pointing, so a SetCursorPos-only move relocates the
	// Windows arrow while the game's own crosshair - the thing that decides
	// where a targeted cast lands - never moves. The targeted cast then
	// resolves at whatever the player's real cursor was sitting on, which
	// reads in game as "the item did nothing".
	//
	// This is the same conclusion CKillStealer and CLastHitAssistant each
	// reached independently; routing the move through SendInput puts it in
	// the exact same synthetic-input stream as the key press and click that
	// follow it.
	const int virtualX = GetSystemMetrics( SM_XVIRTUALSCREEN );
	const int virtualY = GetSystemMetrics( SM_YVIRTUALSCREEN );
	const int virtualW = GetSystemMetrics( SM_CXVIRTUALSCREEN );
	const int virtualH = GetSystemMetrics( SM_CYVIRTUALSCREEN );

	if ( virtualW > 1 && virtualH > 1 )
	{
		INPUT input{};
		input.type = INPUT_MOUSE;
		input.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
		input.mi.dwExtraInfo = ANDROMEDA_INJECTED_INPUT_TAG;
		input.mi.dx = static_cast<LONG>( ( static_cast<long long>( screenX - virtualX ) * 65535LL ) / ( virtualW - 1 ) );
		input.mi.dy = static_cast<LONG>( ( static_cast<long long>( screenY - virtualY ) * 65535LL ) / ( virtualH - 1 ) );
		if ( SendInput( 1 , &input , sizeof( INPUT ) ) == 1 )
			return true;
	}

	// Fall back only when SendInput could not be queued at all (no valid
	// virtual-screen metrics) - a visibly wrong position beats not moving.
	return SetCursorPos( screenX , screenY ) != FALSE;
}

auto FeatureSupport::MoveCursorToClientPoint( HWND window , const ImVec2& screen , POINT& previousOut ) -> bool
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
	return MoveCursorToScreen( target.x , target.y );
}

auto FeatureSupport::ProjectWorldToClient( HWND window , const Vector3& worldPoint , bool groundTargeted , ImVec2& outScreen ) -> bool
{
	outScreen = ImVec2( 0.f , 0.f );

	ImVec2 screen{};
	bool projected = false;
	if ( !groundTargeted )
	{
		Vector3 raised = worldPoint;
		raised.m_z += 80.f;
		projected = Math::WorldToScreen( raised , screen );
	}
	if ( !projected && !Math::WorldToScreen( worldPoint , screen ) )
		return false;

	RECT client{};
	if ( !GetClientRect( window , &client ) || client.right <= 0 || client.bottom <= 0 )
		return false;

	outScreen = screen;

	// MoveCursorToClientPoint CLAMPS an out-of-view point to the window edge,
	// which is silently wrong for a click: a unit-target cast aimed at a hero
	// the camera is not looking at ends up clicking the screen border, where
	// there is no unit, and the cast never happens. Callers that are about to
	// click need to know the point is genuinely visible first.
	const float width = static_cast<float>( client.right );
	const float height = static_cast<float>( client.bottom );
	// The bottom band is Dota's HUD - a click there hits the shop/inventory
	// panel rather than the world.
	constexpr float kHudTopFraction = 0.80f;
	constexpr float kEdgeMargin = 12.f;
	return screen.x >= kEdgeMargin && screen.x <= width - kEdgeMargin &&
		screen.y >= kEdgeMargin && screen.y <= height * kHudTopFraction;
}

auto FeatureSupport::AimCursorAtWorld( HWND window , const Vector3& worldPoint , bool groundTargeted ) -> bool
{
	ImVec2 screen{};
	bool projected = false;

	if ( !groundTargeted )
	{
		// Raising the aim point helps land the cursor on a unit's model, but
		// for a ground-targeted cast it is actively wrong: a higher point
		// projects to ground further from the camera, so the cast lands behind.
		Vector3 raised = worldPoint;
		raised.m_z += 80.f;
		projected = Math::WorldToScreen( raised , screen );
	}
	if ( !projected && !Math::WorldToScreen( worldPoint , screen ) )
		return false;

	POINT previous{};
	return MoveCursorToClientPoint( window , screen , previous );
}
