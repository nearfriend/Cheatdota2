#include "CInvokerController.hpp"

#include <AndromedaClient/CAndromedaGUI.hpp>
#include <AndromedaClient/Scripting/LuaManager.hpp>
#include <AndromedaClient/Settings/Heroes/Invoker.hpp>

#include <Dota2/SDK/Interface/CGameEntitySystem.hpp>
#include <Dota2/SDK/Math/Math.hpp>
#include <Dota2/SDK/Types/CEntityData.hpp>
#include <Dota2/SDK/Types/CHandle.hpp>
#include <Dota2/SDK/CSchemaOffset.hpp>
#include <Dota2/SDK/Update/AbilityOffsets.hpp>
#include <Dota2/SDK/SDK.hpp>
#include <Dota2/SDK/Update/CDOTAInput.hpp>
#include <Dota2/SDK/Update/CUserCmd.hpp>

#include <ImGui/imgui.h>
#include <Windows.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstring>
#include <string>

static constexpr const char* kInvokerHeroName = "invoker";
static constexpr int kMaxAbilitySlots = 24;

// Fixed Invoker ability-bar layout (not user-remappable in Dota 2): slots 0-2 are
// the orbs, 3 is Invoke, 4/5 are the two currently-invoked spell slots. Matches the
// game's default keybinds Q/W/E/R/D/F, which is what CollectTools (CKillStealer.cpp)
// already assumes for the generic Q/W/E/D/F/R ability-key layout.
static constexpr uint16_t kQuasKey = 'Q';
static constexpr uint16_t kWexKey = 'W';
static constexpr uint16_t kExortKey = 'E';
static constexpr uint16_t kInvokeKey = 'R';
static constexpr uint16_t kSpellSlotKeys[2] = { 'D' , 'F' };
static constexpr int kSpellSlotIndices[2] = { 4 , 5 };

struct InvokerSpellInfo
{
	const char* displayName;
	const char* abilityName;
	int quas;
	int wex;
	int exort;
	bool pointTarget;
	bool unitTarget;
};

// Orb multiset -> resulting spell. Order of Q/W/E presses doesn't matter, only the
// count of each among Invoker's last three orb activations.
static constexpr InvokerSpellInfo kInvokerSpells[] =
{
	{ "Cold Snap"       , "invoker_cold_snap"       , 3 , 0 , 0 , false , true  },
	{ "Ghost Walk"      , "invoker_ghost_walk"      , 2 , 1 , 0 , false , false },
	{ "Ice Wall"        , "invoker_ice_wall"        , 2 , 0 , 1 , true  , false },
	{ "EMP"             , "invoker_emp"             , 1 , 2 , 0 , true  , false },
	{ "Tornado"         , "invoker_tornado"         , 0 , 3 , 0 , true  , false },
	{ "Alacrity"        , "invoker_alacrity"        , 0 , 2 , 1 , false , true  },
	{ "Sun Strike"      , "invoker_sun_strike"      , 1 , 1 , 1 , true  , false },
	{ "Forge Spirit"    , "invoker_forge_spirit"    , 0 , 1 , 2 , true  , false },
	{ "Chaos Meteor"    , "invoker_chaos_meteor"    , 1 , 0 , 2 , true  , false },
	{ "Deafening Blast" , "invoker_deafening_blast" , 0 , 0 , 3 , true  , false },
};
static constexpr int kInvokerSpellCount = static_cast<int>( std::size( kInvokerSpells ) );

namespace
{
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

	template <typename T>
	auto TryReadField( const void* base , uint32_t offset , T& out ) -> bool
	{
		if ( !base || !offset )
			return false;

		MEMORY_BASIC_INFORMATION mbi{};
		const auto* addr = reinterpret_cast<const void*>( reinterpret_cast<uintptr_t>( base ) + offset );
		if ( !VirtualQuery( addr , &mbi , sizeof( mbi ) ) || mbi.State != MEM_COMMIT )
			return false;

		std::memcpy( &out , addr , sizeof( T ) );
		return true;
	}
}

static bool HeroNameContains( C_DOTA_BaseNPC_Hero* pHero , const char* needle )
{
	if ( !pHero || !needle || !needle[0] )
		return false;

	auto matches = [needle]( const char* s ) -> bool
	{
		if ( !s || !s[0] )
			return false;

		std::string lower( s );
		std::transform( lower.begin() , lower.end() , lower.begin() , []( unsigned char c ) { return static_cast<char>( std::tolower( c ) ); } );
		return lower.find( needle ) != std::string::npos;
	};

	if ( matches( pHero->GetSchemaClassName() ) )
		return true;

	if ( auto* id = pHero->pEntityIdentity() )
	{
		if ( matches( id->DesingerName().String() ) )
			return true;
		if ( matches( id->Name().String() ) )
			return true;
	}

	return false;
}

void CInvokerController::OnCreateMove( CDOTAInput* /*pCDOTAInput*/ , CUserCmd* /*pCUserCmd*/ )
{
	if ( !ResolveLocalHero() )
		return;

	// Only run Invoker Lua / ability scrape when local hero is actually Invoker.
	if ( !HeroNameContains( m_pHero , kInvokerHeroName ) )
		return;

	if ( !EnsureAbilityOffsets() )
		return;

	if ( RefreshAbilityList() )
		LogAbilitiesOnce();

	TickLua();
	AdvanceCombo();
}

bool CInvokerController::ResolveLocalHero()
{
	auto* pController = CGameEntitySystem::GetLocalPlayerController();
	if ( !pController )
	{
		m_pHero = nullptr;
		return false;
	}

	const auto heroHandle = pController->m_hAssignedHero();
	if ( !heroHandle.IsValid() )
	{
		m_pHero = nullptr;
		m_HeroClassificationComplete = false;
		return false;
	}

	if ( m_HeroClassificationComplete && m_LastHeroHandle == heroHandle.m_Index )
		return m_pHero != nullptr;

	m_LastHeroHandle = heroHandle.m_Index;
	m_HeroClassificationComplete = true;

	auto* hero = static_cast<C_DOTA_BaseNPC_Hero*>( SDK::Interfaces::GameEntitySystem()->GetBaseEntityFromHandle( heroHandle ) );

	if ( !HeroNameContains( hero , kInvokerHeroName ) )
	{
		m_pHero = nullptr;
		return false;
	}

	m_pHero = hero;
	
	// Log once when the assigned hero handle changes, not once per game tick.
	if ( m_pHero )
		DEV_LOG( "[CHEAT_ENGINE] Hero address: %p\n" , m_pHero );
	
	return m_pHero != nullptr;
}

bool CInvokerController::EnsureAbilityOffsets()
{
	if ( m_OffsetsReady )
		return true;
	
	uint32_t offset = 0;
	if ( !GetSchemaOffset()->TryGetOffset( "C_DOTA_BaseNPC" , "m_vecAbilities" , offset ) )
	{
		offset = BaseNPCOffsets::m_vecAbilities;
		static bool warned = false;
		if ( !warned )
		{
			DEV_LOG( "[invoker] m_vecAbilities not in schema, using fallback 0x%04X\n" , offset );
			DEV_LOG( "[invoker] Available classes: %zu\n" , GetSchemaOffset()->GetClassCount() );
			warned = true;
		}
	}

	if ( offset == 0 )
		return false;

	m_AbilityArrayOffset = offset;
	m_OffsetsReady = true;
	DEV_LOG( "[invoker] m_vecAbilities offset: 0x%04X\n" , offset );
	return true;
}

bool CInvokerController::RefreshAbilityList()
{
	if ( !m_pHero || m_AbilityArrayOffset == 0 )
		return false;

	auto* abilityHandles = reinterpret_cast<CHandle*>( reinterpret_cast<uint8_t*>( m_pHero ) + m_AbilityArrayOffset );
	std::vector<AbilityEntry> fresh;
	fresh.reserve( kMaxAbilitySlots );

	for ( int slot = 0; slot < kMaxAbilitySlots; ++slot )
	{
		const auto handle = abilityHandles[slot];
		if ( !handle.IsValid() )
			continue;

		auto* ability = static_cast<C_DOTABaseAbility*>( SDK::Interfaces::GameEntitySystem()->GetBaseEntityFromHandle( handle ) );
		if ( !ability )
			continue;

		std::string name;
		if ( auto* id = ability->pEntityIdentity() )
		{
			name = id->Name().String();
		}

		if ( name.empty() )
			name = "unknown_" + std::to_string( slot );

		fresh.push_back( { slot , name } );
	}

	const bool changed = fresh != m_Abilities;
	if ( changed )
		m_Abilities.swap( fresh );

	return changed;
}

void CInvokerController::TickLua()
{
	auto* lua = GetLuaManager();

	if ( lua )
		lua->TickHero( kInvokerHeroName , 1.f / 60.f );

	const bool comboKeyDown = Settings::Heroes::Invoker::ComboKey > 0 &&
		( GetAsyncKeyState( Settings::Heroes::Invoker::ComboKey ) & 0x8000 ) != 0;

	if ( comboKeyDown && !m_ComboKeyWasDown )
	{
		if ( lua )
			lua->TriggerHeroCombo( kInvokerHeroName );

		if ( !StartCombo( Settings::Heroes::Invoker::ComboSpell ) )
			DEV_LOG( "[invoker] combo not started: %s\n" , m_ComboStatus.c_str() );
	}

	m_ComboKeyWasDown = comboKeyDown;
}

void CInvokerController::LogAbilitiesOnce()
{
	if ( m_LoggedAbilities || m_Abilities.empty() )
		return;

	DEV_LOG( "[invoker] abilities found (%zu):\n" , m_Abilities.size() );
	for ( const auto& entry : m_Abilities )
	{
		DEV_LOG( "[invoker] slot %d -> %s\n" , entry.slot , entry.name.c_str() );
	}

	m_LoggedAbilities = true;
}

bool CInvokerController::EnsureOriginOffsets()
{
	if ( m_OriginOffsetsReady )
		return true;

	auto* schema = GetSchemaOffset();
	if ( !schema )
		return false;

	uint32_t sceneNodeOffset = 0;
	uint32_t absOriginOffset = 0;
	if ( !schema->TryGetOffset( "C_BaseEntity" , "m_pGameSceneNode" , sceneNodeOffset ) ||
		!schema->TryGetOffset( "CGameSceneNode" , "m_vecAbsOrigin" , absOriginOffset ) )
		return false;

	m_SceneNodeOffset = sceneNodeOffset;
	m_AbsOriginOffset = absOriginOffset;
	m_OriginOffsetsReady = true;
	return true;
}

void CInvokerController::CancelCombo( const char* reason )
{
	m_Combo = {};
	m_ComboStatus = reason ? reason : "Idle";
	if ( reason )
		DEV_LOG( "[invoker] combo cancelled: %s\n" , reason );
}

bool CInvokerController::StartCombo( int spellIndex )
{
	if ( m_Combo.active )
	{
		m_ComboStatus = "Already running";
		return false;
	}

	if ( spellIndex < 0 || spellIndex >= kInvokerSpellCount )
	{
		m_ComboStatus = "Invalid spell selection";
		return false;
	}

	if ( !EnsureOriginOffsets() )
	{
		m_ComboStatus = "Origin offsets unresolved";
		return false;
	}

	const auto& info = kInvokerSpells[spellIndex];

	if ( ( info.pointTarget || info.unitTarget ) && Settings::Heroes::Invoker::TargetEntIndex < 0 )
	{
		m_ComboStatus = "No target set";
		return false;
	}

	m_Combo = {};
	m_Combo.active = true;
	m_Combo.phase = ComboPhase::PressingOrbs;
	m_Combo.spellIndex = spellIndex;

	for ( int i = 0; i < info.quas; ++i ) m_Combo.orbKeys.push_back( kQuasKey );
	for ( int i = 0; i < info.wex; ++i ) m_Combo.orbKeys.push_back( kWexKey );
	for ( int i = 0; i < info.exort; ++i ) m_Combo.orbKeys.push_back( kExortKey );

	m_Combo.nextActionTick = GetTickCount();
	m_ComboStatus = std::string( "Casting " ) + info.displayName;
	DEV_LOG( "[invoker] combo started: %s\n" , info.displayName );
	return true;
}

bool CInvokerController::ResolveFinalKey( const char* abilityName , uint16_t& outKey ) const
{
	for ( const auto& entry : m_Abilities )
	{
		for ( int i = 0; i < 2; ++i )
		{
			if ( entry.slot != kSpellSlotIndices[i] )
				continue;

			std::string lower = entry.name;
			std::transform( lower.begin() , lower.end() , lower.begin() , []( unsigned char c ) { return static_cast<char>( std::tolower( c ) ); } );
			if ( lower == abilityName )
			{
				outKey = kSpellSlotKeys[i];
				return true;
			}
		}
	}
	return false;
}

void CInvokerController::AdvanceCombo()
{
	if ( !m_Combo.active )
		return;

	const uint32_t now = GetTickCount();
	const auto& info = kInvokerSpells[m_Combo.spellIndex];

	switch ( m_Combo.phase )
	{
	case ComboPhase::PressingOrbs:
	{
		if ( now < m_Combo.nextActionTick )
			return;

		if ( m_Combo.orbIndex >= m_Combo.orbKeys.size() )
		{
			m_Combo.phase = ComboPhase::Invoking;
			m_Combo.nextActionTick = now;
			return;
		}

		if ( !WindowReadyForInput() )
		{
			CancelCombo( "Window not focused" );
			return;
		}

		if ( !SendKeyPress( m_Combo.orbKeys[m_Combo.orbIndex] ) )
		{
			CancelCombo( "Orb key press failed" );
			return;
		}

		++m_Combo.orbIndex;
		m_Combo.nextActionTick = now + 120u;
		return;
	}

	case ComboPhase::Invoking:
	{
		if ( now < m_Combo.nextActionTick )
			return;

		if ( !WindowReadyForInput() || !SendKeyPress( kInvokeKey ) )
		{
			CancelCombo( "Invoke key press failed" );
			return;
		}

		m_Combo.phase = ComboPhase::WaitingForSlot;
		m_Combo.nextActionTick = now + 150u;
		m_Combo.phaseDeadline = now + 1500u;
		return;
	}

	case ComboPhase::WaitingForSlot:
	{
		if ( now < m_Combo.nextActionTick )
			return;

		uint16_t key = 0;
		if ( ResolveFinalKey( info.abilityName , key ) )
		{
			m_Combo.finalKey = key;
			m_Combo.phase = ComboPhase::CastingFinal;
			m_Combo.nextActionTick = now;
			return;
		}

		if ( now >= m_Combo.phaseDeadline )
		{
			CancelCombo( "Invoke did not resolve spell slot" );
			return;
		}

		m_Combo.nextActionTick = now + 60u;
		return;
	}

	case ComboPhase::CastingFinal:
	{
		if ( now < m_Combo.nextActionTick )
			return;

		const HWND window = WindowReadyForInput();
		if ( !window )
		{
			CancelCombo( "Window not focused" );
			return;
		}

		if ( !info.pointTarget && !info.unitTarget )
		{
			SendKeyPress( m_Combo.finalKey );
			m_ComboStatus = std::string( "Cast " ) + info.displayName;
			m_Combo = {};
			return;
		}

		auto* entitySystem = SDK::Interfaces::GameEntitySystem();
		auto* target = entitySystem ? entitySystem->GetBaseEntity( Settings::Heroes::Invoker::TargetEntIndex ) : nullptr;
		if ( !target )
		{
			CancelCombo( "Target no longer valid" );
			return;
		}

		void* sceneNode = nullptr;
		Vector3 origin{};
		if ( !TryReadField( target , m_SceneNodeOffset , sceneNode ) || !sceneNode ||
			!TryReadField( sceneNode , m_AbsOriginOffset , origin ) ||
			!std::isfinite( origin.m_x ) || !std::isfinite( origin.m_y ) || !std::isfinite( origin.m_z ) )
		{
			CancelCombo( "Failed to read target origin" );
			return;
		}

		ImVec2 screen{};
		Vector3 aimPoint = origin;
		aimPoint.m_z += 80.f;
		if ( !Math::WorldToScreen( aimPoint , screen ) && !Math::WorldToScreen( origin , screen ) )
		{
			CancelCombo( "Target off-screen" );
			return;
		}

		POINT previous{};
		if ( !MoveCursorToClientPoint( window , screen , previous ) )
		{
			CancelCombo( "Cursor move failed" );
			return;
		}

		const bool sent = SendKeyPress( m_Combo.finalKey ) && SendLeftClick();
		SetCursorPos( previous.x , previous.y );

		m_ComboStatus = sent ? ( std::string( "Cast " ) + info.displayName ) : "Final cast failed";
		m_Combo = {};
		return;
	}

	default:
		m_Combo = {};
		return;
	}
}
