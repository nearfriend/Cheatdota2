#include "CInvokerController.hpp"

#include <AndromedaClient/Scripting/LuaManager.hpp>
#include <AndromedaClient/Settings/Heroes/Invoker.hpp>

#include <Dota2/SDK/Interface/CGameEntitySystem.hpp>
#include <Dota2/SDK/Types/CEntityData.hpp>
#include <Dota2/SDK/Types/CHandle.hpp>
#include <Dota2/SDK/CSchemaOffset.hpp>
#include <Dota2/SDK/SDK.hpp>
#include <Dota2/SDK/Update/CDOTAInput.hpp>
#include <Dota2/SDK/Update/CUserCmd.hpp>

#include <algorithm>
#include <array>

static constexpr const char* kInvokerHeroName = "invoker";
static constexpr int kMaxAbilitySlots = 24;

void CInvokerController::OnCreateMove( CDOTAInput* /*pCDOTAInput*/ , CUserCmd* /*pCUserCmd*/ )
{
	TickLua();

	if ( !ResolveLocalHero() )
		return;

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
		return false;
	}

	m_pHero = static_cast<C_DOTA_BaseNPC_Hero*>( SDK::Interfaces::GameEntitySystem()->GetBaseEntityFromHandle( heroHandle ) );
	
	// Log hero address for Cheat Engine (to find m_hAbilities offset)
	if ( m_pHero )
		DEV_LOG( "[CHEAT_ENGINE] Hero address: %p\n" , m_pHero );
	
	return m_pHero != nullptr;
}

bool CInvokerController::EnsureAbilityOffsets()
{
	if ( m_OffsetsReady )
		return true;
	
	uint32_t offset = 0;
	if ( !GetSchemaOffset()->TryGetOffset( "C_DOTA_BaseNPC" , "m_hAbilities" , offset ) )
	{
		static bool warned = false;
		if ( !warned )
		{
			DEV_LOG( "[invoker] m_hAbilities offset not found in schema\n" );
			DEV_LOG( "[invoker] Available classes: %zu\n" , GetSchemaOffset()->GetClassCount() );
			warned = true;
		}
		return false;
	}
	
	m_AbilityArrayOffset = offset;
	m_OffsetsReady = true;
	DEV_LOG( "[invoker] m_hAbilities offset found: 0x%04X\n" , offset );
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
