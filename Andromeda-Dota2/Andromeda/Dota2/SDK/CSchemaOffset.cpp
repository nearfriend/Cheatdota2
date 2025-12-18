#include "CSchemaOffset.hpp"

#include <Dota2/SDK/SDK.hpp>
#include <Dota2/SDK/Interface/CShemaSystemSDK.hpp>

#include <vector>
#include <array>

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
	std::vector<CSchemaSystemTypeScope*> m_ScopeList;

	auto* pSchemaSystem = SDK::Interfaces::SchemaSystem();

	if ( !pSchemaSystem )
		return;

	if ( auto* pGlobal = pSchemaSystem->GlobalTypeScope() )
		m_ScopeList.push_back( pGlobal );

	auto ppAllScopes = pSchemaSystem->GetAllTypeScope();
	const auto scopeCount = pSchemaSystem->GetAllTypeScopeSize();

	for ( auto idx = 0; idx < scopeCount; idx++ )
	{
		auto* pScope = ppAllScopes[idx];

		if ( pScope )
			m_ScopeList.push_back( pScope );
	}

#if DUMP_SCHEMA_SCOPE_LIST == 1

	DEV_LOG( "\n" );

	for ( auto& Scope : m_ScopeList )
		DEV_LOG( "Scope: %p\n" , Scope );

	DEV_LOG( "\n" );

#endif

#if DUMP_SCHEMA_ALL_OFFSET == 1
	DEV_LOG( "\n" );
#endif

	for ( auto& Scope : m_ScopeList )
	{
		if ( !Scope )
			continue;

		auto pClassContainer = Scope->GetClassContainer();

		if ( pClassContainer && IsReadable( pClassContainer ) )
		{
			int BlockIndex = 0;

			for ( auto& SchemaBlock : pClassContainer->GetBlockContainers() )
			{
				for ( auto Block = SchemaBlock.GetFirstBlock(); Block && BlockIndex < pClassContainer->GetNumSchema(); Block = Block->Next() , BlockIndex++ )
				{
					if ( !IsReadable( Block , sizeof( *Block ) ) )
						break;

					auto pBinding = Block->GetBinding();

					if ( !pBinding || !IsReadable( pBinding , sizeof( *pBinding ) ) )
						continue;

#if DUMP_SCHEMA_ALL_OFFSET == 1

					auto pBaseClass = pBinding->m_baseClass();

					if ( pBaseClass )
					{
						auto pBaseClassInfo = pBaseClass->m_classInfo();

						if ( pBaseClassInfo )
						{
							DEV_LOG( "class %s : public %s // %s\n{\n" , pBinding->m_bindingName() , pBaseClassInfo->m_bindingName() , pBinding->m_dllName() );
						}
					}
					else
					{
						DEV_LOG( "class %s // %s\n{\n" , pBinding->m_bindingName() , pBinding->m_dllName() );
					}

#endif

					const auto dataSize = pBinding->m_DataArraySize();
					auto pDataArray = pBinding->m_DataArray();

					if ( !pDataArray || dataSize <= 0 || dataSize > 4096 || !IsReadable( pDataArray , sizeof( SchemaClassFieldDataArray_t ) * dataSize ) )
						continue;

					for ( auto idx = 0; idx < dataSize; idx++ )
					{
						auto pClassData = pDataArray[idx];

						if ( pClassData.FieldName && pClassData.FieldType )
						{
#if DUMP_SCHEMA_ALL_OFFSET == 1

							DEV_LOG( "\t%s %s; // 0x%04X\n" , 
									 pClassData.FieldType->szTypeName , pClassData.FieldName , pClassData.FieldOffset );

#endif

							m_SchemaData[pBinding->m_bindingName()][pClassData.FieldName].m_ClassName = pBinding->m_bindingName();
							m_SchemaData[pBinding->m_bindingName()][pClassData.FieldName].m_PropertyName = pClassData.FieldName;
							m_SchemaData[pBinding->m_bindingName()][pClassData.FieldName].m_Offset = pClassData.FieldOffset;
						}
					}

#if DUMP_SCHEMA_ALL_OFFSET == 1
					DEV_LOG( "};\n" );
#endif
				}
			}
		}

#if DUMP_SCHEMA_ALL_OFFSET == 1
		auto pEnumContainer = Scope->GetEnumContainer();

		if ( pEnumContainer )
		{
			int blockIndex = 0;

			for ( auto& schema_block : pEnumContainer->GetBlockContainers() )
			{
				for ( auto block = schema_block.GetFirstBlock(); block && blockIndex < pEnumContainer->GetNumSchema(); block = block->Next() , ++blockIndex )
				{
					auto binding = block->GetBinding();

					if ( !binding )
						continue;

					DEV_LOG( "enum %s : %s\n{\n" , binding->m_bindingName() , binding->GenerateTypeStorage() );

					for ( auto idx = 0; idx < binding->m_DataArraySize(); idx++ )
					{
						auto enum_data = binding->m_DataArray()[idx];

						if ( enum_data.FieldName )
						{
							switch ( binding->m_TypeSize() )
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

					DEV_LOG( "};\n\n" );
				}
			}
		}
#endif
	}
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

auto GetSchemaOffset() -> CShcemaOffset*
{
	return &g_CShcemaOffset;
}
