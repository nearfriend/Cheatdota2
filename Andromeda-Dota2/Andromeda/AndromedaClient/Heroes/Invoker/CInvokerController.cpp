#include "CInvokerController.hpp"

#include <AndromedaClient/Scripting/LuaManager.hpp>
#include <AndromedaClient/Settings/Heroes/Invoker.hpp>

#include <Dota2/SDK/Interface/CGameEntitySystem.hpp>
#include <Dota2/SDK/Types/CEntityData.hpp>
#include <Dota2/SDK/Types/CHandle.hpp>
#include <Dota2/SDK/CSchemaOffset.hpp>
#include <Dota2/SDK/Update/AbilityOffsets.hpp>
#include <Dota2/SDK/SDK.hpp>
#include <Dota2/SDK/Update/CDOTAInput.hpp>
#include <Dota2/SDK/Update/CUserCmd.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <string>

static constexpr const char* kInvokerHeroName = "invoker";
static constexpr int kMaxAbilitySlots = 24;

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

	TickLua();

	if ( !EnsureAbilityOffsets() )
		return;

	if ( RefreshAbilityList() )
		LogAbilitiesOnce();
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
	if ( !lua )
		return;

	lua->TickHero( kInvokerHeroName , 1.f / 60.f );

	if ( Settings::Heroes::Invoker::ComboKey > 0 && ( GetAsyncKeyState( Settings::Heroes::Invoker::ComboKey ) & 0x8000 ) )
		lua->TriggerHeroCombo( kInvokerHeroName );
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
