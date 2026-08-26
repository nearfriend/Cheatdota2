#include "CInvokerController.hpp"

#include <AndromedaClient/CAndromedaGUI.hpp>
#include <AndromedaClient/Scripting/LuaManager.hpp>
#include <AndromedaClient/Settings/Heroes/Invoker.hpp>

#include <Dota2/SDK/Interface/CGameEntitySystem.hpp>
#include <Dota2/SDK/Interface/CLocalHeroResolver.hpp>
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

// Dota's default "Select Hero" bind. Every cast is preceded by this because
// the combo left-clicks the enemy to aim targeted spells, and a left-click on
// an enemy unit SELECTS it - deselecting our hero. Ability hotkeys then apply
// to nothing, which is exactly the failure seen in debug.log: orb keys arrive
// at the OS ("key 'E' down -> OS state DOWN") yet the orbs never change.
static constexpr uint16_t kSelectHeroKey = VK_F1;

struct InvokerSpellInfo
{
	const char* displayName;
	const char* abilityName;
	int quas;
	int wex;
	int exort;
	bool pointTarget;
	bool unitTarget;
	// Ally-target buff that belongs on Invoker himself - aimed at our own hero
	// instead of the combo target, and castable with no enemy selected.
	bool selfTarget;
};

// Orb multiset -> resulting spell. Order of Q/W/E presses doesn't matter, only the
// count of each among Invoker's last three orb activations.
//
// These combinations are Dota's, not ours - getting one wrong silently invokes a
// DIFFERENT spell, which then never appears in a slot under the wanted name and
// looks exactly like "Invoke did not resolve spell slot". Three pairs used to be
// transposed here (EMP<->Tornado, Sun Strike<->Deafening Blast, Forge
// Spirit<->Chaos Meteor): asking for Deafening Blast pressed EEE, which is Sun
// Strike - and Sun Strike is precisely what debug.log showed landing in the bar.
static constexpr InvokerSpellInfo kInvokerSpells[] =
{
	//                                                  Q   W   E    point  unit    self
	{ "Cold Snap"       , "invoker_cold_snap"       , 3 , 0 , 0 , false , true  , false }, // QQQ
	{ "Ghost Walk"      , "invoker_ghost_walk"      , 2 , 1 , 0 , false , false , false }, // QQW
	{ "Ice Wall"        , "invoker_ice_wall"        , 2 , 0 , 1 , false , false , false }, // QQE, no target
	{ "EMP"             , "invoker_emp"             , 0 , 3 , 0 , true  , false , false }, // WWW
	{ "Tornado"         , "invoker_tornado"         , 1 , 2 , 0 , true  , false , false }, // QWW
	{ "Alacrity"        , "invoker_alacrity"        , 0 , 2 , 1 , false , true  , true  }, // WWE, buff on ourselves
	{ "Sun Strike"      , "invoker_sun_strike"      , 0 , 0 , 3 , true  , false , false }, // EEE
	{ "Forge Spirit"    , "invoker_forge_spirit"    , 1 , 0 , 2 , false , false , false }, // QEE, no target
	{ "Chaos Meteor"    , "invoker_chaos_meteor"    , 0 , 1 , 2 , true  , false , false }, // WEE
	{ "Deafening Blast" , "invoker_deafening_blast" , 1 , 1 , 1 , true  , false , false }, // QWE
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

	// Injected as a pure SCAN CODE (wVk = 0, KEYEVENTF_SCANCODE). Dota is
	// Source 2 / SDL and reads the keyboard through raw input, which is
	// scan-code based; a virtual-key event is translated by the window message
	// path only, so vk-only injection never reaches the game's input system.
	// Merely filling wScan alongside wVk is NOT enough - without
	// KEYEVENTF_SCANCODE, Windows ignores wScan and re-derives it from wVk.
	auto MakeKeyInput( WORD key , bool keyUp ) -> INPUT
	{
		INPUT input{};
		input.type = INPUT_KEYBOARD;
		input.ki.wVk = 0;
		input.ki.wScan = static_cast<WORD>( MapVirtualKeyW( key , MAPVK_VK_TO_VSC ) );
		input.ki.dwFlags = KEYEVENTF_SCANCODE | ( keyUp ? KEYEVENTF_KEYUP : 0 );
		return input;
	}

	auto SendKeyDown( WORD key ) -> bool
	{
		INPUT input = MakeKeyInput( key , false );
		if ( SendInput( 1 , &input , sizeof( INPUT ) ) != 1 )
		{
			DEV_LOG( "[invoker] SendInput FAILED for vk=0x%02X (err=%lu)\n" , key , GetLastError() );
			return false;
		}
		return true;
	}

	auto SendKeyUp( WORD key ) -> bool
	{
		INPUT input = MakeKeyInput( key , true );
		return SendInput( 1 , &input , sizeof( INPUT ) ) == 1;
	}

	auto SendKeyPress( WORD key ) -> bool
	{
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
	// Throttled visibility into which gate is dropping the pipeline - this
	// path failing silently has already hidden two separate root causes
	// (broken controller resolve, null usercmd upstream).
	static ULONGLONG lastGateLog = 0;
	auto GateLog = [&]( const char* stage )
	{
		const ULONGLONG nowTick = GetTickCount64();
		if ( !lastGateLog || nowTick - lastGateLog >= 5000 )
		{
			lastGateLog = nowTick;
			DEV_LOG( "[invoker] gate: %s\n" , stage );
		}
	};

	if ( !ResolveLocalHero() )
	{
		GateLog( "local hero unresolved" );
		return;
	}

	// Only run Invoker Lua / ability scrape when local hero is actually Invoker.
	if ( !HeroNameContains( m_pHero , kInvokerHeroName ) )
		return;

	if ( !EnsureAbilityOffsets() )
	{
		GateLog( "ability offsets unresolved" );
		return;
	}

	if ( RefreshAbilityList() )
		LogAbilitiesOnce();

	TickLua();
	AdvanceCombo();
	TickSequence();
}

// Local hero identity comes from the shared CLocalHeroResolver - the old
// GetLocalPlayerController()->m_hAssignedHero() path never resolves on this
// build (see CLocalHeroResolver.hpp), which silently disabled the whole
// Invoker pipeline: OnCreateMove bailed here every tick, so TickLua/AdvanceCombo
// never ran and combo hotkey presses (and CAutoCombo delegations) were dropped.
bool CInvokerController::ResolveLocalHero()
{
	auto* entitySystem = SDK::Interfaces::GameEntitySystem();
	if ( !entitySystem )
	{
		m_pHero = nullptr;
		return false;
	}

	// The resolver's primary path is a full identity-chunk scan; OnCreateMove
	// runs every game tick, so re-resolve on a throttle and just revalidate
	// the cached entity pointer in between.
	static ULONGLONG nextResolveTick = 0;
	const ULONGLONG now = GetTickCount64();

	if ( m_HeroClassificationComplete && now < nextResolveTick )
	{
		if ( !m_pHero )
			return false;
		if ( entitySystem->GetBaseEntity( m_LastHeroEntIndex ) == static_cast<C_BaseEntity*>( m_pHero ) )
			return true;
		// Slot no longer holds our hero - fall through to a fresh resolve.
	}
	nextResolveTick = now + 250;

	C_BaseEntity* entity = nullptr;
	int entIndex = -1;
	if ( !CLocalHeroResolver::Resolve( entitySystem , entity , entIndex ) )
	{
		m_pHero = nullptr;
		m_HeroClassificationComplete = false;
		return false;
	}

	const bool sameHero = m_HeroClassificationComplete && entIndex == m_LastHeroEntIndex;
	m_LastHeroEntIndex = entIndex;
	m_HeroClassificationComplete = true;

	auto* hero = static_cast<C_DOTA_BaseNPC_Hero*>( entity );
	if ( !sameHero )
	{
		if ( !HeroNameContains( hero , kInvokerHeroName ) )
		{
			m_pHero = nullptr;
			return false;
		}
		// Log once per hero change, not once per game tick.
		DEV_LOG( "[CHEAT_ENGINE] Hero address: %p\n" , hero );
	}
	else if ( !m_pHero )
	{
		// Same hero as before, already classified as not-Invoker.
		return false;
	}

	m_pHero = hero;
	return true;
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

	// Optional: used only to skip spells that are on cooldown / unaffordable.
	// A missing offset degrades to "always ready", never blocks casting.
	auto* schema = GetSchemaOffset();
	schema->TryGetOffset( "C_DOTABaseAbility" , "m_iLevel" , m_AbilityLevelOffset );
	if ( !schema->TryGetOffset( "C_DOTABaseAbility" , "m_flCooldown" , m_AbilityCooldownOffset ) )
		schema->TryGetOffset( "C_DOTABaseAbility" , "m_fCooldown" , m_AbilityCooldownOffset );
	schema->TryGetOffset( "C_DOTABaseAbility" , "m_iManaCost" , m_AbilityManaCostOffset );
	schema->TryGetOffset( "C_DOTA_BaseNPC" , "m_flMana" , m_HeroManaOffset );

	m_OffsetsReady = true;
	DEV_LOG( "[invoker] m_vecAbilities offset: 0x%04X (cooldown 0x%04X, manaCost 0x%04X, mana 0x%04X)\n" ,
		offset , m_AbilityCooldownOffset , m_AbilityManaCostOffset , m_HeroManaOffset );
	return true;
}

int CInvokerController::OrbLevel( const char* orbAbilityName ) const
{
	// Unknown level offset must not block casting - report "leveled".
	if ( !m_AbilityLevelOffset )
		return 1;

	for ( const auto& entry : m_Abilities )
	{
		if ( !entry.ability || entry.name != orbAbilityName )
			continue;
		int level = 0;
		if ( TryReadField( entry.ability , m_AbilityLevelOffset , level ) )
			return level;
		return 1;
	}
	return 0;
}

float CInvokerController::AbilityCooldownRemaining( const char* abilityName ) const
{
	if ( !m_AbilityCooldownOffset )
		return 0.f;

	auto Lower = []( std::string value ) -> std::string
	{
		std::transform( value.begin() , value.end() , value.begin() , []( unsigned char c ) { return static_cast<char>( std::tolower( c ) ); } );
		return value;
	};

	for ( const auto& entry : m_Abilities )
	{
		if ( entry.slot < 0 || entry.slot > 5 || !entry.ability || Lower( entry.name ) != abilityName )
			continue;
		float cooldown = 0.f;
		if ( TryReadField( entry.ability , m_AbilityCooldownOffset , cooldown ) &&
			std::isfinite( cooldown ) && cooldown > 0.f )
			return cooldown;
		return 0.f;
	}
	return 0.f;
}

bool CInvokerController::IsSpellReady( const char* abilityName ) const
{
	auto Lower = []( std::string value ) -> std::string
	{
		std::transform( value.begin() , value.end() , value.begin() , []( unsigned char c ) { return static_cast<char>( std::tolower( c ) ); } );
		return value;
	};

	for ( const auto& entry : m_Abilities )
	{
		if ( entry.slot < 0 || entry.slot > 5 || !entry.ability || Lower( entry.name ) != abilityName )
			continue;

		if ( m_AbilityCooldownOffset )
		{
			float cooldown = 0.f;
			if ( TryReadField( entry.ability , m_AbilityCooldownOffset , cooldown ) &&
				std::isfinite( cooldown ) && cooldown > 0.15f )
				return false;
		}

		if ( m_AbilityManaCostOffset && m_HeroManaOffset && m_pHero )
		{
			int manaCost = 0;
			float mana = 0.f;
			if ( TryReadField( entry.ability , m_AbilityManaCostOffset , manaCost ) &&
				TryReadField( m_pHero , m_HeroManaOffset , mana ) &&
				std::isfinite( mana ) && manaCost > 0 && static_cast<float>( manaCost ) > mana + 0.5f )
				return false;
		}

		return true;
	}

	// Not in a castable slot at all.
	return false;
}

bool CInvokerController::RefreshAbilityList()
{
	if ( !m_pHero || m_AbilityArrayOffset == 0 )
		return false;

	// m_vecAbilities is a network vector ({size, pad, data*, ...}), NOT an
	// inline handle array - indexing the field directly read the header bytes
	// as handles and produced garbage slots (same layout CAutoCombo's
	// ReadHandleVector already handles).
	struct NetworkHandleVector
	{
		int32_t size = 0;
		int32_t pad = 0;
		const CHandle* data = nullptr;
	};
	NetworkHandleVector vec{};
	std::memcpy( &vec , reinterpret_cast<const uint8_t*>( m_pHero ) + m_AbilityArrayOffset , sizeof( vec ) );
	if ( vec.size <= 0 || vec.size > 64 || !vec.data )
		return false;

	const int slotCount = ( std::min )( vec.size , static_cast<int32_t>( kMaxAbilitySlots ) );
	std::vector<AbilityEntry> fresh;
	fresh.reserve( slotCount );

	for ( int slot = 0; slot < slotCount; ++slot )
	{
		const auto handle = vec.data[slot];
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

		fresh.push_back( { slot , name , ability } );
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
	const bool externalRequest = m_PendingComboRequest.exchange( false , std::memory_order_relaxed );

	if ( ( comboKeyDown && !m_ComboKeyWasDown ) || externalRequest )
	{
		if ( lua )
			lua->TriggerHeroCombo( kInvokerHeroName );

		StartSequence();
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

void CInvokerController::ReleaseHeldKey()
{
	if ( m_Combo.heldKey != 0 )
	{
		SendKeyUp( m_Combo.heldKey );
		m_Combo.heldKey = 0;
	}
}

void CInvokerController::CancelCombo( const char* reason )
{
	// Never leave the cursor parked on the target if the combo dies mid-cast.
	if ( m_Combo.hasPrevCursor )
		SetCursorPos( m_Combo.prevCursorX , m_Combo.prevCursorY );
	ReleaseHeldKey();
	m_Combo = {};
	m_ComboStatus = reason ? reason : "Idle";
	if ( reason )
		DEV_LOG( "[invoker] combo cancelled: %s\n" , reason );
}

bool CInvokerController::AimAtComboTarget( HWND window )
{
	if ( m_Combo.spellIndex < 0 || m_Combo.spellIndex >= kInvokerSpellCount )
		return false;
	const auto& info = kInvokerSpells[m_Combo.spellIndex];

	// Self-buffs aim at our own hero; everything else at the combo target.
	C_BaseEntity* aimEntity = nullptr;
	if ( info.selfTarget )
	{
		aimEntity = static_cast<C_BaseEntity*>( m_pHero );
	}
	else
	{
		auto* entitySystem = SDK::Interfaces::GameEntitySystem();
		aimEntity = entitySystem ? entitySystem->GetBaseEntity( Settings::Heroes::Invoker::TargetEntIndex ) : nullptr;
	}
	if ( !aimEntity )
		return false;

	void* sceneNode = nullptr;
	Vector3 origin{};
	if ( !TryReadField( aimEntity , m_SceneNodeOffset , sceneNode ) || !sceneNode ||
		!TryReadField( sceneNode , m_AbsOriginOffset , origin ) ||
		!std::isfinite( origin.m_x ) || !std::isfinite( origin.m_y ) || !std::isfinite( origin.m_z ) )
		return false;

	ImVec2 screen{};
	Vector3 aimPoint = origin;
	// Raising the aim point helps land the cursor on a UNIT's model, but for a
	// ground-targeted spell it is actively wrong: a point above the hero
	// projects higher on screen, and the ground under that screen pixel lies
	// further from the camera - so the spell lands BEHIND the target. This is
	// what put EMP behind the enemy after a Tornado.
	if ( !info.pointTarget )
		aimPoint.m_z += 80.f;
	if ( !Math::WorldToScreen( aimPoint , screen ) && !Math::WorldToScreen( origin , screen ) )
		return false;

	POINT previous{};
	if ( !MoveCursorToClientPoint( window , screen , previous ) )
		return false;

	// Only the first aim of a cast records the restore position - re-aims
	// would otherwise capture our own parked-on-target coordinates.
	if ( !m_Combo.hasPrevCursor )
	{
		m_Combo.prevCursorX = previous.x;
		m_Combo.prevCursorY = previous.y;
		m_Combo.hasPrevCursor = true;
	}
	return true;
}

static_assert( kInvokerSpellCount == Settings::Heroes::Invoker::SpellCount ,
	"Settings::Heroes::Invoker::SpellCount must match the kInvokerSpells table" );

int CInvokerController::GetSpellCount()
{
	return kInvokerSpellCount;
}

const char* CInvokerController::GetSpellDisplayName( int index )
{
	return index >= 0 && index < kInvokerSpellCount ? kInvokerSpells[index].displayName : "?";
}

// Builds the cast queue from the user's spell order/enable settings and kicks
// off the sequence; each entry then runs the full orb -> Invoke -> cast
// pipeline via TickSequence/StartCombo.
void CInvokerController::StartSequence()
{
	if ( m_SequenceActive || m_Combo.active )
	{
		m_ComboStatus = "Already running";
		return;
	}

	// Refuse rather than queue when input can't be delivered (alt-tabbed, or
	// our menu is open) - otherwise a keypress while configuring the spell
	// order would fire a surprise combo the moment the menu closes.
	if ( !WindowReadyForInput() )
	{
		m_ComboStatus = "Game window not focused";
		return;
	}

	m_SequenceQueue.clear();
	for ( int position = 0; position < Settings::Heroes::Invoker::SpellCount; ++position )
	{
		const int spell = Settings::Heroes::Invoker::SpellOrder[position];
		if ( spell >= 0 && spell < kInvokerSpellCount && Settings::Heroes::Invoker::SpellEnabled[spell] )
			m_SequenceQueue.push_back( spell );
	}

	if ( m_SequenceQueue.empty() )
	{
		m_ComboStatus = "No Invoker spells enabled";
		return;
	}

	const uint32_t now = GetTickCount();
	m_SequenceIndex = 0;
	m_SpellRetries = 0;
	m_AwaitingComboResult = false;
	m_LastComboSucceeded = false;
	m_SequenceActive = true;
	m_NextSequenceTick = now;
	// Generous overall budget: several spells, each with Invoke-cooldown
	// retries, must fit - but a stuck sequence must not run forever.
	m_SequenceDeadline = now + 25000u;
	DEV_LOG( "[invoker] sequence started with %zu spells\n" , m_SequenceQueue.size() );
}

void CInvokerController::TickSequence()
{
	if ( !m_SequenceActive )
		return;

	const uint32_t now = GetTickCount();

	// Freeze the whole sequence while input cannot be delivered (alt-tabbed,
	// or our menu open). Without this the per-attempt cancels still ran the
	// no-progress watchdog down and killed the sequence outright - see the
	// "Window not focused" x8 then "sequence timed out" run in debug.log.
	if ( !WindowReadyForInput() )
	{
		m_SequenceDeadline = now + 25000u;
		m_NextSequenceTick = now + 250u;
		return;
	}

	if ( now >= m_SequenceDeadline )
	{
		m_SequenceActive = false;
		m_ComboStatus = "Sequence timed out";
		DEV_LOG( "[invoker] sequence timed out at spell %zu/%zu\n" , m_SequenceIndex + 1 , m_SequenceQueue.size() );
		return;
	}

	// A single-spell combo is still in flight - let it finish first.
	if ( m_Combo.active )
		return;

	if ( m_AwaitingComboResult )
	{
		m_AwaitingComboResult = false;
		if ( m_LastComboSucceeded )
		{
			++m_SequenceIndex;
			m_SpellRetries = 0;
			m_NextSequenceTick = now + 250u;
			// The deadline is a no-progress watchdog, not a total budget: a
			// long queue whose spells keep landing must not be cut off.
			m_SequenceDeadline = now + 25000u;
		}
		else if ( m_ComboBlockedByWindow )
		{
			// Alt-tabbed or our menu is open: the spell never got a chance, so
			// hold this slot in the queue (no retry consumed) and push the
			// overall deadline out while we wait for input to be possible.
			m_ComboBlockedByWindow = false;
			m_NextSequenceTick = now + 250u;
			m_SequenceDeadline = now + 25000u;
		}
		else if ( ++m_SpellRetries <= 2 )
		{
			// The WaitingForSlot phase already re-presses Invoke for ~6s, so a
			// failure here usually means the spell is genuinely uninvokable
			// right now (orb levels too low for it, silenced). Retry twice,
			// briefly, then move on rather than stalling the whole combo.
			m_NextSequenceTick = now + 400u * static_cast<uint32_t>( m_SpellRetries );
		}
		else
		{
			DEV_LOG( "[invoker] sequence: skipping %s after %d failed attempts\n" ,
				kInvokerSpells[m_SequenceQueue[m_SequenceIndex]].displayName , m_SpellRetries - 1 );
			++m_SequenceIndex;
			m_SpellRetries = 0;
			m_NextSequenceTick = now + 250u;
			m_SequenceDeadline = now + 25000u;
		}
	}

	if ( m_SequenceIndex >= m_SequenceQueue.size() )
	{
		m_SequenceActive = false;
		m_ComboStatus = "Sequence done";
		DEV_LOG( "[invoker] sequence finished (%zu spells)\n" , m_SequenceQueue.size() );
		return;
	}

	if ( now < m_NextSequenceTick )
		return;

	const int spell = m_SequenceQueue[m_SequenceIndex];
	m_LastComboSucceeded = false;
	if ( StartCombo( spell ) )
	{
		m_AwaitingComboResult = true;
		m_ComboStatus = std::to_string( m_SequenceIndex + 1 ) + "/" + std::to_string( m_SequenceQueue.size() ) +
			": " + m_ComboStatus;
	}
	else
	{
		// Could not even start (e.g. targeted spell with no target) - skip it.
		DEV_LOG( "[invoker] sequence: skipping %s (%s)\n" , kInvokerSpells[spell].displayName , m_ComboStatus.c_str() );
		++m_SequenceIndex;
		m_SpellRetries = 0;
		m_NextSequenceTick = now + 100u;
	}
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

	// Self-buffs need no enemy - they are aimed at our own hero.
	if ( ( info.pointTarget || info.unitTarget ) && !info.selfTarget &&
		Settings::Heroes::Invoker::TargetEntIndex < 0 )
	{
		m_ComboStatus = "No target set";
		return false;
	}

	if ( info.selfTarget && !m_pHero )
	{
		m_ComboStatus = "Local hero unresolved";
		return false;
	}

	// Already sitting in a D/F slot (invoked by the previous sequence entry or
	// by the player). Casting an invoked spell does NOT remove it from its
	// slot, so slot presence alone means nothing - it must also be off
	// cooldown and affordable, otherwise the keypress is a no-op in game.
	uint16_t readyKey = 0;
	const bool alreadyInvoked = ResolveFinalKey( info.abilityName , readyKey );

	// An orb with no skill points cannot produce instances, so any spell
	// needing it is impossible to invoke - fail fast instead of pressing keys
	// and waiting out the full invoke timeout for every such spell. (A typical
	// Exort-first Invoker cannot make Tornado or EMP at all early on.)
	if ( !alreadyInvoked )
	{
		const bool orbsAvailable =
			( info.quas == 0 || OrbLevel( "invoker_quas" ) > 0 ) &&
			( info.wex == 0 || OrbLevel( "invoker_wex" ) > 0 ) &&
			( info.exort == 0 || OrbLevel( "invoker_exort" ) > 0 );
		if ( !orbsAvailable )
		{
			m_ComboStatus = std::string( info.displayName ) + ": orbs not leveled";
			return false;
		}
	}

	if ( alreadyInvoked && !IsSpellReady( info.abilityName ) )
	{
		// Re-invoking cannot clear a cooldown, so there is nothing useful to
		// do for this spell right now - let the sequence move on.
		m_ComboStatus = std::string( info.displayName ) + " on cooldown";
		return false;
	}

	// Clear any key still held from a previous combo before discarding its
	// state, so a stuck key can never leak into this one.
	ReleaseHeldKey();
	m_Combo = {};
	m_Combo.active = true;
	m_Combo.spellIndex = spellIndex;
	m_Combo.nextActionTick = GetTickCount();
	m_ComboBlockedByWindow = false;

	// Always re-select the hero first (see kSelectHeroKey): a previous cast in
	// this sequence left-clicked the enemy to aim, which selects that unit and
	// makes every later ability hotkey a no-op.
	m_Combo.phase = ComboPhase::SelectingHero;

	if ( alreadyInvoked )
	{
		// Skipping the orb dance and the Invoke press avoids putting Invoke on
		// cooldown for nothing, which is what made the *next* spell fail.
		m_Combo.finalKey = readyKey;
		m_Combo.castDirectly = true;
		m_ComboStatus = std::string( "Casting " ) + info.displayName;
		DEV_LOG( "[invoker] combo started: %s (already invoked)\n" , info.displayName );
		return true;
	}

	for ( int i = 0; i < info.quas; ++i ) m_Combo.orbKeys.push_back( kQuasKey );
	for ( int i = 0; i < info.wex; ++i ) m_Combo.orbKeys.push_back( kWexKey );
	for ( int i = 0; i < info.exort; ++i ) m_Combo.orbKeys.push_back( kExortKey );

	std::string orbSequence;
	for ( const uint16_t orbKey : m_Combo.orbKeys )
		orbSequence += static_cast<char>( orbKey );

	m_ComboStatus = std::string( "Casting " ) + info.displayName;
	DEV_LOG( "[invoker] combo started: %s (not invoked -> orbs %s, Invoke cd %.1fs)\n" ,
		info.displayName , orbSequence.c_str() , AbilityCooldownRemaining( "invoker_invoke" ) );
	return true;
}

bool CInvokerController::ResolveFinalKey( const char* abilityName , uint16_t& outKey ) const
{
	// The invoked-spell slots are found dynamically rather than hardcoded
	// (a fixed slots-4/5 assumption does not hold across builds): within
	// slots 0..5 they are whatever is not an orb and not
	// Invoke itself. Ascending slot order maps to the D then F hotkeys,
	// matching the game's left-to-right ability bar.
	auto Lower = []( std::string value ) -> std::string
	{
		std::transform( value.begin() , value.end() , value.begin() , []( unsigned char c ) { return static_cast<char>( std::tolower( c ) ); } );
		return value;
	};

	int invokedSlotPosition = 0;
	for ( const auto& entry : m_Abilities )
	{
		if ( entry.slot < 0 || entry.slot > 5 )
			continue;

		const std::string lower = Lower( entry.name );
		if ( lower == "invoker_quas" || lower == "invoker_wex" || lower == "invoker_exort" || lower == "invoker_invoke" )
			continue;

		// This is one of the two invoked-spell slots (holding either a real
		// invoked spell or an invoker_empty placeholder).
		if ( invokedSlotPosition < 2 && lower == abilityName )
		{
			outKey = kSpellSlotKeys[invokedSlotPosition];
			return true;
		}
		++invokedSlotPosition;
	}
	return false;
}

void CInvokerController::AdvanceCombo()
{
	if ( !m_Combo.active )
		return;

	const uint32_t now = GetTickCount();
	const auto& info = kInvokerSpells[m_Combo.spellIndex];

	// Release a held key before anything else, so every phase below sees a
	// settled keyboard state.
	if ( m_Combo.heldKey != 0 )
	{
		if ( now < m_Combo.releaseTick )
			return;
		SendKeyUp( m_Combo.heldKey );
		m_Combo.heldKey = 0;
	}

	// Presses a key and holds it for a few frames. The caller schedules the
	// next step past the release, so the game reliably observes a real press.
	constexpr uint32_t kKeyHoldMs = 45;
	auto HoldKey = [&]( uint16_t key ) -> bool
	{
		if ( !SendKeyDown( key ) )
			return false;
		m_Combo.heldKey = key;
		m_Combo.releaseTick = now + kKeyHoldMs;
		return true;
	};

	switch ( m_Combo.phase )
	{
	case ComboPhase::SelectingHero:
	{
		if ( now < m_Combo.nextActionTick )
			return;

		if ( !WindowReadyForInput() )
		{
			m_ComboBlockedByWindow = true;
			CancelCombo( "Window not focused" );
			return;
		}

		if ( !HoldKey( kSelectHeroKey ) )
		{
			CancelCombo( "Select-hero key press failed" );
			return;
		}

		m_Combo.phase = m_Combo.castDirectly ? ComboPhase::CastingFinal : ComboPhase::PressingOrbs;
		m_Combo.nextActionTick = now + kKeyHoldMs + 80u;
		return;
	}

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
			m_ComboBlockedByWindow = true;
			CancelCombo( "Window not focused" );
			return;
		}

		if ( !HoldKey( m_Combo.orbKeys[m_Combo.orbIndex] ) )
		{
			CancelCombo( "Orb key press failed" );
			return;
		}

		++m_Combo.orbIndex;
		m_Combo.nextActionTick = now + kKeyHoldMs + 90u;
		return;
	}

	case ComboPhase::Invoking:
	{
		if ( now < m_Combo.nextActionTick )
			return;

		if ( !WindowReadyForInput() )
		{
			m_ComboBlockedByWindow = true;
			CancelCombo( "Window not focused" );
			return;
		}

		if ( !HoldKey( kInvokeKey ) )
		{
			CancelCombo( "Invoke key press failed" );
			return;
		}
		{
			std::string orbSequence;
			for ( const uint16_t orbKey : m_Combo.orbKeys )
				orbSequence += static_cast<char>( orbKey );
			DEV_LOG( "[invoker] pressed orbs [%s] (need Q%d W%d E%d; levels Q%d W%d E%d) + Invoke for %s\n" ,
				orbSequence.c_str() , info.quas , info.wex , info.exort ,
				OrbLevel( "invoker_quas" ) , OrbLevel( "invoker_wex" ) , OrbLevel( "invoker_exort" ) ,
				info.displayName );
		}

		m_Combo.phase = ComboPhase::WaitingForSlot;
		m_Combo.nextActionTick = now + kKeyHoldMs + 150u;
		// Long enough to ride out Invoke's cooldown (up to ~4s early game)
		// rather than failing the spell and replaying the whole orb sequence.
		m_Combo.phaseDeadline = now + 6000u;
		m_Combo.nextInvokeRetryTick = now + 800u;
		return;
	}

	case ComboPhase::WaitingForSlot:
	{
		if ( now < m_Combo.nextActionTick )
			return;

		uint16_t key = 0;
		if ( ResolveFinalKey( info.abilityName , key ) )
		{
			// Invoked, but re-invoking a spell does not reset its cooldown -
			// pressing the slot key would be a no-op in game.
			if ( !IsSpellReady( info.abilityName ) )
			{
				CancelCombo( ( std::string( info.displayName ) + " on cooldown" ).c_str() );
				return;
			}
			m_Combo.finalKey = key;
			m_Combo.phase = ComboPhase::CastingFinal;
			m_Combo.nextActionTick = now;
			return;
		}

		if ( now >= m_Combo.phaseDeadline )
		{
			// Invoke itself still on cooldown - typical when the previous spell
			// in the sequence just used it. The spell has not failed, it simply
			// has not had a chance yet, so wait out the real remaining cooldown
			// instead of giving up and replaying the orbs.
			const float invokeCooldown = AbilityCooldownRemaining( "invoker_invoke" );
			if ( invokeCooldown > 0.f && m_Combo.invokeWaitExtensions < 3 )
			{
				++m_Combo.invokeWaitExtensions;
				m_Combo.phaseDeadline = now + static_cast<uint32_t>( invokeCooldown * 1000.f ) + 1500u;
				m_Combo.nextInvokeRetryTick = now + static_cast<uint32_t>( invokeCooldown * 1000.f ) + 100u;
				m_ComboStatus = std::string( "Waiting for Invoke (" ) + info.displayName + ")";
				return;
			}

			// Dump what the ability bar actually holds, so a failure here can
			// be told apart: spell present but unmatched (lookup bug) vs. spell
			// absent entirely (orb/Invoke keypresses not reaching the game).
			std::string slots;
			for ( const auto& entry : m_Abilities )
			{
				if ( entry.slot >= 0 && entry.slot <= 6 )
					slots += std::to_string( entry.slot ) + ":" + entry.name + " ";
			}
			DEV_LOG( "[invoker] invoke timeout for %s (wanted '%s'); bar now: %s\n" ,
				info.displayName , info.abilityName , slots.c_str() );
			CancelCombo( "Invoke did not resolve spell slot" );
			return;
		}

		// The orbs stay set, so re-pressing Invoke is safe and is what
		// actually lands the spell once the cooldown expires.
		if ( now >= m_Combo.nextInvokeRetryTick )
		{
			if ( !WindowReadyForInput() )
			{
				m_ComboBlockedByWindow = true;
				CancelCombo( "Window not focused" );
				return;
			}
			HoldKey( kInvokeKey );
			m_Combo.nextInvokeRetryTick = now + 800u;
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
			m_ComboBlockedByWindow = true;
			CancelCombo( "Window not focused" );
			return;
		}

		if ( !info.pointTarget && !info.unitTarget )
		{
			SendKeyPress( m_Combo.finalKey );
			m_ComboStatus = std::string( "Cast " ) + info.displayName;
			m_LastComboSucceeded = true;
			ReleaseHeldKey();
			m_Combo = {};
			return;
		}

		// Targeted: park the cursor on the target and give the game a tick to
		// register the position before pressing anything.
		if ( !AimAtComboTarget( window ) )
		{
			CancelCombo( "Target/aim failed" );
			return;
		}
		m_Combo.phase = ComboPhase::FinalPress;
		m_Combo.nextActionTick = now + 50u;
		return;
	}

	case ComboPhase::FinalPress:
	{
		if ( now < m_Combo.nextActionTick )
			return;

		const HWND window = WindowReadyForInput();
		if ( !window )
		{
			m_ComboBlockedByWindow = true;
			CancelCombo( "Window not focused" );
			return;
		}

		// Re-aim right before the press - the target may have moved during the
		// settle window.
		AimAtComboTarget( window );
		if ( !SendKeyPress( m_Combo.finalKey ) || !SendLeftClick() )
		{
			CancelCombo( "Final cast failed" );
			return;
		}
		m_Combo.phase = ComboPhase::FinalRestore;
		m_Combo.nextActionTick = now + 50u;
		return;
	}

	case ComboPhase::FinalRestore:
	{
		if ( now < m_Combo.nextActionTick )
			return;

		if ( m_Combo.hasPrevCursor )
			SetCursorPos( m_Combo.prevCursorX , m_Combo.prevCursorY );
		m_ComboStatus = std::string( "Cast " ) + info.displayName;
		m_LastComboSucceeded = true;
		ReleaseHeldKey();
		m_Combo = {};
		return;
	}

	default:
		ReleaseHeldKey();
		m_Combo = {};
		return;
	}
}
