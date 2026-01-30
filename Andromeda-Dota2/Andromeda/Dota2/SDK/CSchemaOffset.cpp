#include "CSchemaOffset.hpp"

#include <Dota2/SDK/SDK.hpp>
#include <Dota2/SDK/Interface/CShemaSystemSDK.hpp>

#include <vector>
#include <array>
#include <cstring>  // For strstr

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
}

auto CShcemaOffset::Init() -> void
{
#if DUMP_SCHEMA_ALL_OFFSET == 1
	// Add delay to ensure game is fully initialized before schema scanning
	// Schema dump can be very heavy and might cause issues if done too early
	DEV_LOG( "[schema] Waiting for game to stabilize before schema scan...\n" );
	Sleep( 3000 );  // Wait 3 seconds for game to stabilize
#endif

	std::vector<CSchemaSystemTypeScope*> m_ScopeList;

	auto* pSchemaSystem = SDK::Interfaces::SchemaSystem();

	if ( !pSchemaSystem )
	{
		DEV_LOG( "[schema] Schema system not available yet\n" );
		return;
	}

	// Safely get global type scope
	CSchemaSystemTypeScope* pGlobal = nullptr;
	try
	{
		if ( IsReadable( pSchemaSystem , sizeof( *pSchemaSystem ) ) )
			pGlobal = pSchemaSystem->GlobalTypeScope();
	}
	catch ( ... )
	{
		DEV_LOG( "[schema] Failed to get global type scope\n" );
		pGlobal = nullptr;
	}
	
	if ( pGlobal && IsReadable( pGlobal , sizeof( *pGlobal ) ) )
		m_ScopeList.push_back( pGlobal );

	// Safely get all type scopes
	CSchemaSystemTypeScope** ppAllScopes = nullptr;
	uint32_t scopeCount = 0;
	
	try
	{
		if ( IsReadable( pSchemaSystem , sizeof( *pSchemaSystem ) ) )
		{
			ppAllScopes = pSchemaSystem->GetAllTypeScope();
			scopeCount = pSchemaSystem->GetAllTypeScopeSize();
		}
	}
	catch ( ... )
	{
		DEV_LOG( "[schema] Failed to get all type scopes\n" );
		ppAllScopes = nullptr;
		scopeCount = 0;
	}

	if ( ppAllScopes && IsReadable( ppAllScopes , sizeof( CSchemaSystemTypeScope* ) * scopeCount ) )
	{
		for ( size_t idx = 0; idx < scopeCount && idx < 1000u; idx++ )  // Limit to 1000 scopes max
		{
			CSchemaSystemTypeScope* pScope = nullptr;
			
			try
			{
				if ( IsReadable( &ppAllScopes[idx] , sizeof( CSchemaSystemTypeScope* ) ) )
					pScope = ppAllScopes[idx];
			}
			catch ( ... )
			{
				break;
			}

			if ( pScope && IsReadable( pScope , sizeof( *pScope ) ) )
				m_ScopeList.push_back( pScope );
		}
	}

#if DUMP_SCHEMA_SCOPE_LIST == 1

	DEV_LOG( "\n" );

	for ( auto& Scope : m_ScopeList )
		DEV_LOG( "Scope: %p\n" , Scope );

	DEV_LOG( "\n" );

#endif

#if DUMP_SCHEMA_ALL_OFFSET == 1
	DEV_LOG( "\n" );
	DEV_LOG( "========================================\n" );
	DEV_LOG( "SCHEMA OFFSET DUMP START\n" );
	DEV_LOG( "========================================\n\n" );
	DEV_LOG( "[schema] Scanning %zu scopes for class definitions...\n\n" , m_ScopeList.size() );
#endif

	// Track important classes for summary
	std::vector<std::string> importantClasses;
	const std::vector<std::string> targetClasses = {
		"C_DOTABaseAbility",
		"C_DOTA_BaseNPC_Hero",
		"C_DOTA_BaseNPC",
		"C_DOTAPlayerController",
		"C_BaseEntity"
	};

	for ( auto& Scope : m_ScopeList )
	{
		if ( !Scope || !IsReadable( Scope , sizeof( *Scope ) ) )
			continue;

		CSchemaList<CSchemaClassBinding>* pClassContainer = nullptr;
		
		// Safely get class container
		try
		{
			if ( IsReadable( Scope , sizeof( *Scope ) ) )
				pClassContainer = Scope->GetClassContainer();
		}
		catch ( ... )
		{
			// Skip this scope if we can't read it
			continue;
		}

		if ( pClassContainer && IsReadable( pClassContainer , sizeof( *pClassContainer ) ) )
		{
#if DUMP_SCHEMA_ALL_OFFSET == 1
			DEV_LOG( "[schema] Found class container in scope %p\n" , Scope );
#endif
			int BlockIndex = 0;
			int maxSchema = 0;
			
			// Safely get schema count
			try
			{
				if ( IsReadable( pClassContainer , sizeof( *pClassContainer ) ) )
					maxSchema = pClassContainer->GetNumSchema();
			}
			catch ( ... )
			{
				maxSchema = 10000;  // Safe default
			}

			// Safely iterate block containers
			try
			{
				const CSchemaList<CSchemaClassBinding>::BlockContainers& blockContainers = pClassContainer->GetBlockContainers();
				
#if DUMP_SCHEMA_ALL_OFFSET == 1
				DEV_LOG( "[schema] Block containers size: %zu, maxSchema: %d\n" , blockContainers.size() , maxSchema );
#endif
				
				for ( size_t containerIdx = 0; containerIdx < blockContainers.size(); containerIdx++ )
				{
					const CSchemaList<CSchemaClassBinding>::BlockContainer& SchemaBlock = blockContainers[containerIdx];
					
					if ( !IsReadable( &SchemaBlock , sizeof( SchemaBlock ) ) )
					{
#if DUMP_SCHEMA_ALL_OFFSET == 1
						if ( containerIdx < 3 )  // Only log first few
							DEV_LOG( "[schema] Container %zu: SchemaBlock not readable, skipping\n" , containerIdx );
#endif
						continue;
					}
					
					CSchemaList<CSchemaClassBinding>::SchemaBlock* Block = nullptr;
					
					try
					{
						if ( IsReadable( &SchemaBlock , sizeof( SchemaBlock ) ) )
							Block = SchemaBlock.GetFirstBlock();
					}
					catch ( ... )
					{
#if DUMP_SCHEMA_ALL_OFFSET == 1
						if ( containerIdx < 3 )
							DEV_LOG( "[schema] Container %zu: Exception getting first block, skipping\n" , containerIdx );
#endif
						continue;
					}
					
#if DUMP_SCHEMA_ALL_OFFSET == 1
					if ( containerIdx < 3 )
						DEV_LOG( "[schema] Container %zu: First block = %p\n" , containerIdx , Block );
#endif
					
					for ( ; Block && BlockIndex < maxSchema; BlockIndex++ )
					{
						if ( !IsReadable( Block , sizeof( *Block ) ) )
						{
#if DUMP_SCHEMA_ALL_OFFSET == 1
							DEV_LOG( "[schema] Block %d: Block not readable, breaking\n" , BlockIndex );
#endif
							break;
						}

						// Debug: Read raw block structure
#if DUMP_SCHEMA_ALL_OFFSET == 1
						if ( BlockIndex < 3 && containerIdx < 3 )
						{
							// SchemaBlock structure: void* unkn0, SchemaBlock* m_nextBlock, T* m_classBinding
							uint64_t* rawBlock = reinterpret_cast<uint64_t*>( Block );
							DEV_LOG( "[schema] Block %d (container %zu): raw[0]=%p, raw[1]=%p, raw[2]=%p\n" , 
								BlockIndex , containerIdx , 
								reinterpret_cast<void*>( rawBlock[0] ) , 
								reinterpret_cast<void*>( rawBlock[1] ) , 
								reinterpret_cast<void*>( rawBlock[2] ) );
						}
#endif

						CSchemaClassBinding* pBinding = nullptr;
						
						try
						{
							if ( IsReadable( Block , sizeof( *Block ) ) )
								pBinding = Block->GetBinding();
						}
						catch ( ... )
						{
#if DUMP_SCHEMA_ALL_OFFSET == 1
							DEV_LOG( "[schema] Block %d: Exception getting binding, breaking\n" , BlockIndex );
#endif
							break;
						}

						if ( !pBinding || !IsReadable( pBinding , sizeof( *pBinding ) ) )
						{
#if DUMP_SCHEMA_ALL_OFFSET == 1
							if ( BlockIndex < 5 )  // Only log first few to avoid spam
							{
								DEV_LOG( "[schema] Block %d: Binding null or not readable (pBinding=%p), skipping to next block\n" , BlockIndex , pBinding );
							}
#endif
							// CRITICAL: Continue to next block even if binding is null
							// Some blocks may have null bindings but valid bindings appear later in the chain
							try
							{
								if ( IsReadable( Block , sizeof( *Block ) ) )
								{
									auto nextBlock = Block->Next();
#if DUMP_SCHEMA_ALL_OFFSET == 1
									if ( BlockIndex < 5 )
										DEV_LOG( "[schema] Block %d: Next block pointer = %p\n" , BlockIndex , nextBlock );
#endif
									// Validate next block pointer before using it
									// Next() might return invalid pointers (like 0x2000) which are flags, not addresses
									if ( nextBlock && reinterpret_cast<uintptr_t>( nextBlock ) > 0x10000 && IsReadable( nextBlock , sizeof( *nextBlock ) ) )
									{
										Block = nextBlock;
										BlockIndex--;  // Compensate for the increment at loop start
									}
									else
									{
#if DUMP_SCHEMA_ALL_OFFSET == 1
										if ( BlockIndex < 5 )
											DEV_LOG( "[schema] Block %d: Next block invalid (%p), breaking chain\n" , BlockIndex , nextBlock );
#endif
										break;  // Invalid next block pointer, stop iteration
									}
								}
								else
								{
									break;  // Block not readable, can't continue
								}
							}
							catch ( ... )
							{
								break;  // Exception getting next block, stop iteration
							}
							continue;
						}

					// Safely get class name and DLL name
					const char* className = nullptr;
					const char* dllName = nullptr;
					
					// Validate pointers before dereferencing
					try
					{
						if ( IsReadable( pBinding , sizeof( *pBinding ) ) )
						{
							className = pBinding->m_bindingName();
							dllName = pBinding->m_dllName();
						}
					}
					catch ( ... )
					{
						// Skip this binding if we can't read the name
						try
						{
							if ( IsReadable( Block , sizeof( *Block ) ) )
								Block = Block->Next();
						}
						catch ( ... )
						{
							break;
						}
						continue;
					}
					
					// Validate string pointers are readable
					if ( !className || !IsReadable( className , 256 ) )
					{
#if DUMP_SCHEMA_ALL_OFFSET == 1
						if ( BlockIndex < 5 )  // Only log first few
							DEV_LOG( "[schema] Block %d: className invalid (ptr=%p), skipping\n" , BlockIndex , className );
#endif
						try
						{
							if ( IsReadable( Block , sizeof( *Block ) ) )
								Block = Block->Next();
						}
						catch ( ... )
						{
							break;
						}
						continue;
					}
					if ( !dllName || !IsReadable( dllName , 256 ) )
					{
#if DUMP_SCHEMA_ALL_OFFSET == 1
						if ( BlockIndex < 5 )  // Only log first few
							DEV_LOG( "[schema] Block %d: dllName invalid (ptr=%p), skipping\n" , BlockIndex , dllName );
#endif
						try
						{
							if ( IsReadable( Block , sizeof( *Block ) ) )
								Block = Block->Next();
						}
						catch ( ... )
						{
							break;
						}
						continue;
					}
					
#if DUMP_SCHEMA_ALL_OFFSET == 1
					// Check if this is an important class
					bool isImportant = false;
					for ( const auto& targetClass : targetClasses )
					{
						if ( className && strstr( className , targetClass.c_str() ) != nullptr )
						{
							isImportant = true;
							importantClasses.push_back( className );
							break;
						}
					}
					
					// Only dump important classes or all classes (for now, dump all)
					// You can change this to only dump important classes for smaller logs
					bool shouldDump = true;  // Set to 'isImportant' to filter
					
					if ( shouldDump )
					{
						try
						{
							if ( IsReadable( pBinding , sizeof( *pBinding ) ) )
							{
								auto pBaseClass = pBinding->m_baseClass();

								if ( pBaseClass && IsReadable( pBaseClass , sizeof( *pBaseClass ) ) )
								{
									auto pBaseClassInfo = pBaseClass->m_classInfo();

									if ( pBaseClassInfo && IsReadable( pBaseClassInfo , sizeof( *pBaseClassInfo ) ) )
									{
										const char* baseClassName = nullptr;
										try
										{
											baseClassName = pBaseClassInfo->m_bindingName();
										}
										catch ( ... )
										{
											baseClassName = "Unknown";
										}
										
										if ( baseClassName && IsReadable( baseClassName , 256 ) )
										{
											DEV_LOG( "class %s : public %s // %s\n{\n" , className , baseClassName , dllName );
										}
										else
										{
											DEV_LOG( "class %s : public <unknown> // %s\n{\n" , className , dllName );
										}
									}
									else
									{
										DEV_LOG( "class %s // %s\n{\n" , className , dllName );
									}
								}
								else
								{
									DEV_LOG( "class %s // %s\n{\n" , className , dllName );
								}
							}
						}
						catch ( ... )
						{
							// If we can't read base class info, just log the class name
							DEV_LOG( "class %s // %s\n{\n" , className , dllName );
						}
					}
#endif

					// Safely get data array
					uint32_t dataSize = 0;
					SchemaClassFieldDataArray_t* pDataArray = nullptr;
					
					try
					{
						if ( IsReadable( pBinding , sizeof( *pBinding ) ) )
						{
							dataSize = pBinding->m_DataArraySize();
							pDataArray = pBinding->m_DataArray();
						}
					}
					catch ( ... )
					{
						// Skip if we can't read data array info
						try
						{
							if ( IsReadable( Block , sizeof( *Block ) ) )
								Block = Block->Next();
						}
						catch ( ... )
						{
							break;
						}
						continue;
					}

					if ( !pDataArray || dataSize <= 0 || dataSize > 4096 || !IsReadable( pDataArray , sizeof( SchemaClassFieldDataArray_t ) * dataSize ) )
					{
#if DUMP_SCHEMA_ALL_OFFSET == 1
						if ( BlockIndex < 5 )  // Only log first few
							DEV_LOG( "[schema] Block %d: Data array invalid for class %s (pDataArray=%p, dataSize=%u), skipping\n" , BlockIndex , className , pDataArray , dataSize );
#endif
						try
						{
							if ( IsReadable( Block , sizeof( *Block ) ) )
								Block = Block->Next();
						}
						catch ( ... )
						{
							break;
						}
						continue;
					}

					for ( uint32_t idx = 0; idx < dataSize; idx++ )
					{
						SchemaClassFieldDataArray_t pClassData = {};
						
						// Safely read class data
						try
						{
							if ( IsReadable( &pDataArray[idx] , sizeof( SchemaClassFieldDataArray_t ) ) )
								pClassData = pDataArray[idx];
						}
						catch ( ... )
						{
							// Skip this field if we can't read it
							continue;
						}

						// Validate field pointers
						if ( !pClassData.FieldName || !IsReadable( pClassData.FieldName , 256 ) )
							continue;
						if ( !pClassData.FieldType || !IsReadable( pClassData.FieldType , sizeof( *pClassData.FieldType ) ) )
							continue;
						
						const char* fieldName = pClassData.FieldName;
						const char* fieldType = nullptr;
						
						try
						{
							if ( IsReadable( pClassData.FieldType , sizeof( *pClassData.FieldType ) ) )
								fieldType = pClassData.FieldType->szTypeName;
						}
						catch ( ... )
						{
							continue;
						}
						
						if ( !fieldType || !IsReadable( fieldType , 256 ) )
							continue;
						
						uint32_t offset = pClassData.FieldOffset;
						
#if DUMP_SCHEMA_ALL_OFFSET == 1
						// Check if we should dump this class (same logic as above)
						bool shouldDumpClass = true;  // Set to 'isImportant' to filter only important classes
						if ( shouldDumpClass )
						{
							DEV_LOG( "\t%s %s; // 0x%04X\n" , fieldType , fieldName , offset );
						}
#endif

						// Store the offset data
						m_SchemaData[className][fieldName].m_ClassName = className;
						m_SchemaData[className][fieldName].m_PropertyName = fieldName;
						m_SchemaData[className][fieldName].m_Offset = offset;
					}
					
					// Move to next block
					try
					{
						if ( IsReadable( Block , sizeof( *Block ) ) )
							Block = Block->Next();
					}
					catch ( ... )
					{
						break;
					}

#if DUMP_SCHEMA_ALL_OFFSET == 1
					bool shouldDumpClass = true;  // Set to 'isImportant' to filter only important classes
					if ( shouldDumpClass )
					{
						DEV_LOG( "};\n\n" );
					}
#endif
				}
				}
			}
			catch ( ... )
			{
				// If we can't iterate blocks, skip this container
				DEV_LOG( "[schema] Error iterating blocks in scope, skipping...\n" );
			}
		}

#if DUMP_SCHEMA_ALL_OFFSET == 1
		CSchemaList<CSchemaEnumBinding>* pEnumContainer = nullptr;
		
		try
		{
			if ( IsReadable( Scope , sizeof( *Scope ) ) )
				pEnumContainer = Scope->GetEnumContainer();
		}
		catch ( ... )
		{
			pEnumContainer = nullptr;
		}

		if ( pEnumContainer && IsReadable( pEnumContainer , sizeof( *pEnumContainer ) ) )
		{
			int blockIndex = 0;
			int maxEnumSchema = 0;
			
			try
			{
				if ( IsReadable( pEnumContainer , sizeof( *pEnumContainer ) ) )
					maxEnumSchema = pEnumContainer->GetNumSchema();
			}
			catch ( ... )
			{
				maxEnumSchema = 10000;
			}

			try
			{
				const CSchemaList<CSchemaEnumBinding>::BlockContainers& enumBlockContainers = pEnumContainer->GetBlockContainers();
				
				for ( size_t enumContainerIdx = 0; enumContainerIdx < enumBlockContainers.size(); enumContainerIdx++ )
				{
					const CSchemaList<CSchemaEnumBinding>::BlockContainer& schema_block = enumBlockContainers[enumContainerIdx];
					
					if ( !IsReadable( &schema_block , sizeof( schema_block ) ) )
						continue;
					
					CSchemaList<CSchemaEnumBinding>::SchemaBlock* block = nullptr;
					
					try
					{
						if ( IsReadable( &schema_block , sizeof( schema_block ) ) )
							block = schema_block.GetFirstBlock();
					}
					catch ( ... )
					{
						continue;
					}
					
					for ( ; block && blockIndex < maxEnumSchema; ++blockIndex )
					{
						if ( !IsReadable( block , sizeof( *block ) ) )
							break;
						
						CSchemaEnumBinding* binding = nullptr;
						
						try
						{
							if ( IsReadable( block , sizeof( *block ) ) )
								binding = block->GetBinding();
						}
						catch ( ... )
						{
							try
							{
								if ( IsReadable( block , sizeof( *block ) ) )
									block = block->Next();
							}
							catch ( ... )
							{
								break;
							}
							continue;
						}
						
						if ( !binding || !IsReadable( binding , sizeof( *binding ) ) )
						{
							try
							{
								if ( IsReadable( block , sizeof( *block ) ) )
									block = block->Next();
							}
							catch ( ... )
							{
								break;
							}
							continue;
						}
						
						const char* enumName = nullptr;
						const char* enumType = nullptr;
						
						try
						{
							if ( IsReadable( binding , sizeof( *binding ) ) )
							{
								enumName = binding->m_bindingName();
								enumType = binding->GenerateTypeStorage();
							}
						}
						catch ( ... )
						{
							try
							{
								if ( IsReadable( block , sizeof( *block ) ) )
									block = block->Next();
							}
							catch ( ... )
							{
								break;
							}
							continue;
						}
						
						if ( !enumName || !IsReadable( enumName , 256 ) )
						{
							try
							{
								if ( IsReadable( block , sizeof( *block ) ) )
									block = block->Next();
							}
							catch ( ... )
							{
								break;
							}
							continue;
						}
						
						if ( !enumType || !IsReadable( enumType , 256 ) )
							enumType = "int";
						
						DEV_LOG( "enum %s : %s\n{\n" , enumName , enumType );

						uint32_t enumDataSize = 0;
						SchemaEnumFieldDataArray_t* pEnumDataArray = nullptr;
						
						try
						{
							if ( IsReadable( binding , sizeof( *binding ) ) )
							{
								enumDataSize = binding->m_DataArraySize();
								pEnumDataArray = binding->m_DataArray();
							}
						}
						catch ( ... )
						{
							try
							{
								if ( IsReadable( block , sizeof( *block ) ) )
									block = block->Next();
							}
							catch ( ... )
							{
								break;
							}
							continue;
						}

						if ( pEnumDataArray && enumDataSize > 0 && enumDataSize <= 4096 && IsReadable( pEnumDataArray , sizeof( SchemaEnumFieldDataArray_t ) * enumDataSize ) )
						{
							for ( uint32_t idx = 0; idx < enumDataSize; idx++ )
							{
								SchemaEnumFieldDataArray_t enum_data = {};
								
								try
								{
									if ( IsReadable( &pEnumDataArray[idx] , sizeof( SchemaEnumFieldDataArray_t ) ) )
										enum_data = pEnumDataArray[idx];
								}
								catch ( ... )
								{
									continue;
								}

								if ( enum_data.FieldName && IsReadable( enum_data.FieldName , 256 ) )
								{
									uint32_t typeSize = 0;
									
									try
									{
										if ( IsReadable( binding , sizeof( *binding ) ) )
											typeSize = binding->m_TypeSize();
									}
									catch ( ... )
									{
										typeSize = 4;
									}
									
									switch ( typeSize )
									{
										case 1:
											DEV_LOG( "\t%s = 0x%02X ,\n" , enum_data.FieldName , enum_data.FieldData );
											break;
										case 2:
											DEV_LOG( "\t%s = 0x%04X ,\n" , enum_data.FieldName , enum_data.FieldData );
											break;
										case 4:
											DEV_LOG( "\t%s = 0x%08X ,\n" , enum_data.FieldName , enum_data.FieldData );
											break;
										case 8:
											DEV_LOG( "\t%s = 0x%p ,\n" , enum_data.FieldName , enum_data.FieldData );
											break;
										default:
											break;
									}
								}
							}
						}

						DEV_LOG( "};\n\n" );
						
						try
						{
							if ( IsReadable( block , sizeof( *block ) ) )
								block = block->Next();
						}
						catch ( ... )
						{
							break;
						}
					}
				}
			}
			catch ( ... )
			{
				// Skip enum container if error
			}
		}
#endif
	}

#if DUMP_SCHEMA_ALL_OFFSET == 1
	DEV_LOG( "\n========================================\n" );
	DEV_LOG( "SCHEMA OFFSET DUMP COMPLETE\n" );
	DEV_LOG( "========================================\n\n" );
	
	// Print summary of important classes found
	if ( !importantClasses.empty() )
	{
		DEV_LOG( "[schema] IMPORTANT CLASSES FOUND:\n" );
		for ( const auto& className : importantClasses )
		{
			DEV_LOG( "  - %s\n" , className.c_str() );
		}
		DEV_LOG( "\n" );
	}
	
	// Print key offsets summary
	DEV_LOG( "[schema] KEY OFFSETS SUMMARY:\n" );
	DEV_LOG( "========================================\n" );
	
	// C_DOTABaseAbility offsets
	DEV_LOG( "\nC_DOTABaseAbility offsets:\n" );
	const char* abilityProps[] = { "m_iLevel" , "m_flCooldown" , "m_flCooldownLength" , "m_iManaCost" , "m_flCastRange" , "m_bIsActivated" };
	for ( const char* prop : abilityProps )
	{
		uint32_t offset = 0;
		if ( this->TryGetOffset( "C_DOTABaseAbility" , prop , offset ) )
		{
			DEV_LOG( "  %s: 0x%04X\n" , prop , offset );
		}
		else
		{
			DEV_LOG( "  %s: NOT FOUND\n" , prop );
		}
	}
	
	// C_DOTA_BaseNPC_Hero offsets
	DEV_LOG( "\nC_DOTA_BaseNPC_Hero offsets:\n" );
	const char* heroProps[] = { "m_flMana" , "m_flMaxMana" };
	for ( const char* prop : heroProps )
	{
		uint32_t offset = 0;
		if ( this->TryGetOffset( "C_DOTA_BaseNPC_Hero" , prop , offset ) )
		{
			DEV_LOG( "  %s: 0x%04X\n" , prop , offset );
		}
		else
		{
			DEV_LOG( "  %s: NOT FOUND\n" , prop );
		}
	}
	
	// C_DOTA_BaseNPC offsets
	DEV_LOG( "\nC_DOTA_BaseNPC offsets:\n" );
	uint32_t abilitiesOffset = 0;
	if ( this->TryGetOffset( "C_DOTA_BaseNPC" , "m_hAbilities" , abilitiesOffset ) )
	{
		DEV_LOG( "  m_hAbilities: 0x%04X\n" , abilitiesOffset );
	}
	else
	{
		DEV_LOG( "  m_hAbilities: NOT FOUND\n" );
	}
	
	DEV_LOG( "\n========================================\n\n" );
#endif
}

auto CShcemaOffset::GetOffset( std::string ClassName , std::string PropertyName ) -> uint32_t
{
	return m_SchemaData[ClassName][PropertyName].m_Offset;
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
	
	// Critical offsets we need for ability system
	struct CriticalOffset
	{
		const char* className;
		const char* propertyName;
		const char* description;
	};
	
	const CriticalOffset criticalOffsets[] = {
		{ "C_DOTABaseAbility" , "m_iLevel" , "Ability level" },
		{ "C_DOTABaseAbility" , "m_flCooldown" , "Ability cooldown" },
		{ "C_DOTABaseAbility" , "m_iManaCost" , "Ability mana cost" },
		{ "C_DOTA_BaseNPC_Hero" , "m_flMana" , "Hero current mana" },
		{ "C_DOTA_BaseNPC_Hero" , "m_flMaxMana" , "Hero max mana" },
		{ "C_DOTA_BaseNPC" , "m_hAbilities" , "Ability handle array" }
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
	
	DEV_LOG( "[schema] Verification %s\n\n" , allFound ? "PASSED" : "FAILED" );
	
	return allFound;
}

auto GetSchemaOffset() -> CShcemaOffset*
{
	return &g_CShcemaOffset;
}
