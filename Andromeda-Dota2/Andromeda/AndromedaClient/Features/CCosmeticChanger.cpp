#include "CCosmeticChanger.hpp"

#include <AndromedaClient/Features/FeatureSupport.hpp>
#include <Common/DevLog.hpp>
#include <Dota2/SDK/CSchemaOffset.hpp>
#include <Dota2/SDK/Interface/CGameEntitySystem.hpp>
#include <Dota2/SDK/Interface/CLocalHeroResolver.hpp>
#include <Dota2/SDK/SDK.hpp>
#include <Dota2/SDK/Types/CHandle.hpp>

#include <Windows.h>

#include <cstdint>

namespace
{
	// Source 2 C_NetworkUtlVectorBase / CUtlVector: size@0x00, pad@0x04,
	// data ptr@0x08. Mirrors NetworkHandleVector in CAndromedaClient.cpp.
	struct NetworkHandleVector
	{
		int32_t size = 0;
		int32_t pad = 0;
		const CHandle* data = nullptr;
	};

	template <typename T>
	auto ReadAt( const void* base , uint32_t offset ) -> T
	{
		T value{};
		if ( base && offset )
			std::memcpy( &value , reinterpret_cast<const uint8_t*>( base ) + offset , sizeof( T ) );
		return value;
	}
}

auto CCosmeticChanger::ResolveOffsets() -> bool
{
	if ( m_Offsets.resolved )
		return true;
	if ( m_ResolveTried && !m_Offsets.resolved )
	{
		// Retry occasionally in case the schema was not up on the first tick.
	}
	m_ResolveTried = true;

	auto* schema = GetSchemaOffset();
	if ( !schema )
		return false;

	// The wearable handle vector hangs off the combat-character base; some
	// builds surface it on the DOTA npc binding instead.
	m_Offsets.hasWearables =
		schema->TryGetOffset( "C_BaseCombatCharacter" , "m_hMyWearables" , m_Offsets.myWearables ) ||
		schema->TryGetOffset( "C_DOTA_BaseNPC" , "m_hMyWearables" , m_Offsets.myWearables );

	// defindex = wearable + m_AttributeManager + m_Item + m_iItemDefinitionIndex.
	const bool hasAttr =
		schema->TryGetOffset( "C_EconEntity" , "m_AttributeManager" , m_Offsets.attributeManager );
	const bool hasItem =
		schema->TryGetOffset( "C_AttributeContainer" , "m_Item" , m_Offsets.item );
	const bool hasDef =
		schema->TryGetOffset( "C_EconItemView" , "m_iItemDefinitionIndex" , m_Offsets.itemDefinitionIndex );
	m_Offsets.hasDefIndex = hasAttr && hasItem && hasDef;

	m_Offsets.resolved = m_Offsets.hasWearables && m_Offsets.hasDefIndex;

	if ( !m_LoggedSchema )
	{
		DEV_LOG( "[cosmetic] schema wearables=%d(0x%X) attrMgr=%d(0x%X) item=%d(0x%X) defIdx=%d(0x%X)\n" ,
			m_Offsets.hasWearables , m_Offsets.myWearables ,
			hasAttr , m_Offsets.attributeManager , hasItem , m_Offsets.item ,
			hasDef , m_Offsets.itemDefinitionIndex );
		m_LoggedSchema = true;
	}

	return m_Offsets.resolved;
}

auto CCosmeticChanger::OnRender() -> void
{
	const uint32_t now = static_cast<uint32_t>( GetTickCount64() );
	// Throttle the whole pass; this is a read-only diagnostic for now.
	if ( now - m_LastLogTick < 1000 )
		return;
	m_LastLogTick = now;

	if ( !ResolveOffsets() )
		return;

	auto* entitySystem = SDK::Interfaces::GameEntitySystem();
	if ( !entitySystem )
		return;

	C_BaseEntity* hero = nullptr;
	int heroIndex = -1;
	if ( !CLocalHeroResolver::Resolve( entitySystem , hero , heroIndex ) || !hero )
		return;

	const uintptr_t vecBase = reinterpret_cast<uintptr_t>( hero ) + m_Offsets.myWearables;
	const auto* vec = reinterpret_cast<const NetworkHandleVector*>( vecBase );
	if ( !vec || vec->size <= 0 || vec->size > 32 || !vec->data )
	{
		DEV_LOG( "[cosmetic] hero has no readable wearable vector (size=%d)\n" , vec ? vec->size : -1 );
		return;
	}

	DEV_LOG( "[cosmetic] hero wearables=%d:\n" , vec->size );
	for ( int i = 0; i < vec->size; ++i )
	{
		if ( !vec->data[i].IsValid() )
			continue;
		auto* wearable = entitySystem->GetBaseEntityFromHandle( vec->data[i] );
		if ( !wearable )
			continue;

		// wearable -> m_AttributeManager -> m_Item -> m_iItemDefinitionIndex.
		// The attribute manager and item are embedded structs, so their offsets
		// add on top of the wearable pointer (no pointer hops).
		const uint32_t defOffset = m_Offsets.attributeManager + m_Offsets.item + m_Offsets.itemDefinitionIndex;
		const uint16_t defIndex = ReadAt<uint16_t>( wearable , defOffset );
		DEV_LOG( "  [cosmetic] slot=%d defindex=%u\n" , i , static_cast<unsigned>( defIndex ) );
	}
}
