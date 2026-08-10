#include "CSchemaOffset.hpp"

#include <Dota2/SDK/SDK.hpp>
#include <Dota2/SDK/Interface/CShemaSystemSDK.hpp>

#include <cstring>
#include <algorithm>
#include <unordered_set>
#include <vector>

static CShcemaOffset g_CShcemaOffset{};

namespace
{
	inline bool IsReadable( const void* ptr , size_t size = 1 )
	{
		if ( !ptr )
			return false;

		MEMORY_BASIC_INFORMATION mbi{};

		if ( !VirtualQuery( ptr , &mbi , sizeof( mbi ) ) )
			return false;

		const DWORD protect = mbi.Protect & ~( PAGE_GUARD | PAGE_NOACCESS );

		const bool committed = mbi.State == MEM_COMMIT;
		const bool canRead =
			protect == PAGE_READONLY ||
			protect == PAGE_READWRITE ||
			protect == PAGE_WRITECOPY ||
			protect == PAGE_EXECUTE_READ ||
			protect == PAGE_EXECUTE_READWRITE ||
			protect == PAGE_EXECUTE_WRITECOPY;

		const auto regionEnd = reinterpret_cast<uintptr_t>( mbi.BaseAddress ) + mbi.RegionSize;
		const auto ptrEnd = reinterpret_cast<uintptr_t>( ptr ) + size;

		return committed && canRead && ptrEnd <= regionEnd;
	}

	template<typename T, size_t BucketCount = 256>
	auto CollectUtlTsHashElements( const UtlTsHash<T, BucketCount>* hash ) -> std::vector<T*>
	{
		std::vector<T*> elements;

		if ( !hash || !IsReadable( hash , sizeof( *hash ) ) )
			return elements;

		const int32_t usedCount = hash->m_entryMem.m_blocksAllocated;
		const int32_t freeCount = hash->m_entryMem.m_peakAllocated;

		elements.reserve( static_cast<size_t>( usedCount + freeCount ) );

		for ( const auto& bucket : hash->m_buckets )
		{
			auto* node = bucket.m_pFirstUncommitted;

			while ( node && IsReadable( node , sizeof( *node ) ) )
			{
				if ( node->m_pData && IsReadable( node->m_pData , sizeof( T ) ) )
					elements.push_back( node->m_pData );

				if ( elements.size() >= static_cast<size_t>( usedCount ) )
					break;

				node = node->m_pNext;
			}
		}

		auto* blob = reinterpret_cast<UtlTsHashAllocatedBlob<T>*>( hash->m_entryMem.m_freeBlocks.m_head.m_pNext );

		while ( blob && IsReadable( blob , sizeof( *blob ) ) )
		{
			if ( blob->m_pData && IsReadable( blob->m_pData , sizeof( T ) ) )
				elements.push_back( blob->m_pData );

			if ( elements.size() >= static_cast<size_t>( usedCount + freeCount ) )
				break;

			blob = blob->m_pNext;
		}

		std::unordered_set<uintptr_t> seen;

		elements.erase(
			std::remove_if( elements.begin() , elements.end() , [&]( T* ptr )
			{
				const auto address = reinterpret_cast<uintptr_t>( ptr );

				if ( seen.contains( address ) )
					return true;

				seen.insert( address );
				return false;
			} ) ,
			elements.end() );

		return elements;
	}

	auto StoreClassBinding( CShcemaOffset* schema , CSchemaClassBinding* binding ) -> void
	{
		if ( !binding || !IsReadable( binding , sizeof( *binding ) ) )
			return;

		const char* className = binding->m_bindingName();

		if ( !className || !IsReadable( className , 256 ) )
			return;

		const int16_t fieldCount = binding->m_fieldCount();
		SchemaClassFieldData_t* fields = binding->m_fields();

		if ( !fields || fieldCount <= 0 || fieldCount > 4096 || !IsReadable( fields , sizeof( SchemaClassFieldData_t ) * fieldCount ) )
			return;

#if DUMP_SCHEMA_ALL_OFFSET == 1
		const char* dllName = binding->m_dllName();
		DEV_LOG( "class %s // %s\n{\n" , className , dllName && IsReadable( dllName , 256 ) ? dllName : "unknown" );
#endif

		for ( int16_t idx = 0; idx < fieldCount; ++idx )
		{
			const auto& field = fields[idx];

			if ( !field.m_pName || !IsReadable( field.m_pName , 256 ) )
				continue;

			const char* fieldType = nullptr;

			if ( field.m_pType && IsReadable( field.m_pType , sizeof( *field.m_pType ) ) )
				fieldType = field.m_pType->m_szTypeName;

			if ( !fieldType || !IsReadable( fieldType , 256 ) )
				continue;

#if DUMP_SCHEMA_ALL_OFFSET == 1
			DEV_LOG( "\t%s %s; // 0x%04X\n" , fieldType , field.m_pName , field.m_nOffset );
#endif

			schema->StoreOffset( className , field.m_pName , static_cast<uint32_t>( field.m_nOffset ) );
		}

#if DUMP_SCHEMA_ALL_OFFSET == 1
		DEV_LOG( "};\n\n" );
#endif
	}

	auto StoreEnumBinding( CSchemaEnumBinding* binding ) -> void
	{
		if ( !binding || !IsReadable( binding , sizeof( *binding ) ) )
			return;

		const char* enumName = binding->m_bindingName();

		if ( !enumName || !IsReadable( enumName , 256 ) )
			return;

		const uint16_t enumeratorCount = binding->m_enumeratorCount();
		SchemaEnumeratorInfoData_t* enumerators = binding->m_enumerators();

		if ( !enumerators || enumeratorCount == 0 || enumeratorCount > 4096 || !IsReadable( enumerators , sizeof( SchemaEnumeratorInfoData_t ) * enumeratorCount ) )
			return;

#if DUMP_SCHEMA_ALL_OFFSET == 1
		DEV_LOG( "enum %s\n{\n" , enumName );
#endif

		for ( uint16_t idx = 0; idx < enumeratorCount; ++idx )
		{
			const auto& enumerator = enumerators[idx];

			if ( !enumerator.m_pName || !IsReadable( enumerator.m_pName , 256 ) )
				continue;

#if DUMP_SCHEMA_ALL_OFFSET == 1
			switch ( binding->m_TypeSize() )
			{
				case 1:
					DEV_LOG( "\t%s = 0x%02X ,\n" , enumerator.m_pName , enumerator.m_nValueChar );
					break;
				case 2:
					DEV_LOG( "\t%s = 0x%04X ,\n" , enumerator.m_pName , enumerator.m_nValueShort );
					break;
				case 4:
					DEV_LOG( "\t%s = 0x%08X ,\n" , enumerator.m_pName , enumerator.m_nValueInt );
					break;
				case 8:
					DEV_LOG( "\t%s = 0x%p ,\n" , enumerator.m_pName , reinterpret_cast<void*>( enumerator.m_nValue ) );
					break;
				default:
					break;
			}
#endif
		}

#if DUMP_SCHEMA_ALL_OFFSET == 1
		DEV_LOG( "};\n\n" );
#endif
	}
}

auto CShcemaOffset::Init() -> void
{
#if DUMP_SCHEMA_ALL_OFFSET == 1
	DEV_LOG( "[schema] Waiting for game to stabilize before schema scan...\n" );
	Sleep( 3000 );
#endif

	std::vector<CSchemaSystemTypeScope*> scopeList;

	auto* pSchemaSystem = SDK::Interfaces::SchemaSystem();

	if ( !pSchemaSystem )
	{
		DEV_LOG( "[schema] Schema system not available yet\n" );
		return;
	}

	if ( IsReadable( pSchemaSystem , sizeof( *pSchemaSystem ) ) )
	{
		if ( auto* pGlobal = pSchemaSystem->GlobalTypeScope() )
			scopeList.push_back( pGlobal );
	}

	CSchemaSystemTypeScope** ppAllScopes = nullptr;
	uint16_t scopeCount = 0;

	if ( IsReadable( pSchemaSystem , sizeof( *pSchemaSystem ) ) )
	{
		ppAllScopes = pSchemaSystem->GetAllTypeScope();
		scopeCount = pSchemaSystem->GetAllTypeScopeSize();
	}

	if ( ppAllScopes && IsReadable( ppAllScopes , sizeof( CSchemaSystemTypeScope* ) * scopeCount ) )
	{
		for ( size_t idx = 0; idx < scopeCount && idx < 1000u; ++idx )
		{
			auto* pScope = ppAllScopes[idx];

			if ( pScope && IsReadable( pScope , sizeof( *pScope ) ) )
				scopeList.push_back( pScope );
		}
	}

#if DUMP_SCHEMA_SCOPE_LIST == 1
	DEV_LOG( "\n" );

	for ( auto* scope : scopeList )
		DEV_LOG( "Scope: %p\n" , scope );

	DEV_LOG( "\n" );
#endif

#if DUMP_SCHEMA_ALL_OFFSET == 1
	DEV_LOG( "\n========================================\n" );
	DEV_LOG( "SCHEMA OFFSET DUMP START\n" );
	DEV_LOG( "========================================\n\n" );
	DEV_LOG( "[schema] Scanning %zu scopes via UtlTsHash...\n\n" , scopeList.size() );
#endif

	size_t totalClasses = 0;
	size_t totalEnums = 0;

	for ( auto* scope : scopeList )
	{
		if ( !scope || !IsReadable( scope , sizeof( *scope ) ) )
			continue;

		const auto classBindings = CollectUtlTsHashElements( scope->GetClassBindingsHash() );

#if DUMP_SCHEMA_ALL_OFFSET == 1
		DEV_LOG( "[schema] Scope %p: %zu class bindings\n" , scope , classBindings.size() );
#endif

		for ( auto* binding : classBindings )
		{
			StoreClassBinding( this , binding );
			++totalClasses;
		}

#if DUMP_SCHEMA_ALL_OFFSET == 1
		const auto enumBindings = CollectUtlTsHashElements( scope->GetEnumBindingsHash() );
		DEV_LOG( "[schema] Scope %p: %zu enum bindings\n" , scope , enumBindings.size() );

		for ( auto* binding : enumBindings )
		{
			StoreEnumBinding( binding );
			++totalEnums;
		}
#else
		const auto enumBindings = CollectUtlTsHashElements( scope->GetEnumBindingsHash() );
		totalEnums += enumBindings.size();
#endif
	}

#if DUMP_SCHEMA_ALL_OFFSET == 1
	DEV_LOG( "\n========================================\n" );
	DEV_LOG( "SCHEMA OFFSET DUMP COMPLETE\n" );
	DEV_LOG( "Processed %zu classes, %zu enums\n" , totalClasses , totalEnums );
	DEV_LOG( "========================================\n\n" );

	DEV_LOG( "[schema] KEY OFFSETS SUMMARY:\n" );
	DEV_LOG( "========================================\n" );

	DEV_LOG( "\nC_DOTABaseAbility offsets:\n" );
	const char* abilityProps[] = { "m_iLevel" , "m_flCooldown" , "m_fCooldown" , "m_flCooldownLength" , "m_iManaCost" , "m_bActivated" };

	for ( const char* prop : abilityProps )
	{
		uint32_t offset = 0;

		if ( this->TryGetOffset( "C_DOTABaseAbility" , prop , offset ) )
			DEV_LOG( "  %s: 0x%04X\n" , prop , offset );
		else
			DEV_LOG( "  %s: NOT FOUND\n" , prop );
	}

	DEV_LOG( "\nC_DOTA_BaseNPC offsets:\n" );
	uint32_t offset = 0;

	if ( this->TryGetOffset( "C_DOTA_BaseNPC" , "m_flMana" , offset ) )
		DEV_LOG( "  m_flMana: 0x%04X\n" , offset );
	else
		DEV_LOG( "  m_flMana: NOT FOUND\n" );

	if ( this->TryGetOffset( "C_DOTA_BaseNPC" , "m_flMaxMana" , offset ) )
		DEV_LOG( "  m_flMaxMana: 0x%04X\n" , offset );
	else
		DEV_LOG( "  m_flMaxMana: NOT FOUND\n" );

	if ( this->TryGetOffset( "C_DOTA_BaseNPC" , "m_vecAbilities" , offset ) )
		DEV_LOG( "  m_vecAbilities: 0x%04X\n" , offset );
	else
		DEV_LOG( "  m_vecAbilities: NOT FOUND\n" );

	DEV_LOG( "\n========================================\n\n" );
#endif
}

auto CShcemaOffset::GetOffset( std::string ClassName , std::string PropertyName ) -> uint32_t
{
	return m_SchemaData[ClassName][PropertyName].m_Offset;
}

auto CShcemaOffset::StoreOffset( const char* className , const char* propertyName , uint32_t offset ) -> void
{
	if ( !className || !propertyName )
		return;

	m_SchemaData[className][propertyName].m_ClassName = className;
	m_SchemaData[className][propertyName].m_PropertyName = propertyName;
	m_SchemaData[className][propertyName].m_Offset = offset;
}

auto CShcemaOffset::TryGetOffset( const std::string& ClassName , const std::string& PropertyName , uint32_t& outOffset ) const -> bool
{
	const auto classIt = m_SchemaData.find( ClassName );

	if ( classIt == m_SchemaData.end() )
		return false;

	const auto propIt = classIt->second.find( PropertyName );

	if ( propIt == classIt->second.end() )
		return false;

	outOffset = propIt->second.m_Offset;
	return true;
}

auto CShcemaOffset::VerifyCriticalOffsets() const -> bool
{
	uint32_t offset = 0;

	const struct CriticalOffset
	{
		const char* className;
		const char* propertyName;
		const char* description;
	};

	const CriticalOffset criticalOffsets[] = {
		{ "C_DOTABaseAbility" , "m_iLevel" , "Ability level" },
		{ "C_DOTABaseAbility" , "m_fCooldown" , "Ability cooldown" },
		{ "C_DOTABaseAbility" , "m_iManaCost" , "Ability mana cost" },
		{ "C_BaseEntity" , "m_iHealth" , "Hero current health" },
		{ "C_BaseEntity" , "m_iMaxHealth" , "Hero max health" },
		{ "C_BaseEntity" , "m_iTeamNum" , "Hero team" },
		{ "C_DOTA_BaseNPC" , "m_flMana" , "Hero current mana" },
		{ "C_DOTA_BaseNPC" , "m_flMaxMana" , "Hero max mana" },
		{ "C_DOTA_BaseNPC" , "m_vecAbilities" , "Ability handle array" },
		{ "C_DOTAPlayerController" , "m_hAssignedHero" , "Assigned hero handle" },
	};

	bool allFound = true;

	DEV_LOG( "[schema] Verifying critical offsets...\n" );

	for ( const auto& crit : criticalOffsets )
	{
		if ( TryGetOffset( crit.className , crit.propertyName , offset ) )
		{
			DEV_LOG( "  [OK] %s::%s = 0x%04X (%s)\n" ,
			         crit.className , crit.propertyName , offset , crit.description );
		}
		else
		{
			DEV_LOG( "  [FAIL] %s::%s NOT FOUND (%s)\n" ,
			         crit.className , crit.propertyName , crit.description );
			allFound = false;
		}
	}

	const bool hasPlayerId = TryGetOffset( "C_DOTAPlayerController" , "m_nPlayerID" , offset ) ||
		TryGetOffset( "C_DOTAPlayerController" , "m_iPlayerID" , offset );
	if ( hasPlayerId )
		DEV_LOG( "  [OK] C_DOTAPlayerController::playerID = 0x%04X (Player roster slot)\n" , offset );
	else
	{
		DEV_LOG( "%s" , "  [WARN] C_DOTAPlayerController player ID not found; roster will use controller order\n" );
	}

	DEV_LOG( "[schema] Verification %s\n\n" , allFound ? "PASSED" : "FAILED" );

	return allFound;
}

auto GetSchemaOffset() -> CShcemaOffset*
{
	return &g_CShcemaOffset;
}
