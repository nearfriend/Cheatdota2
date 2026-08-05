#pragma once

#include <Common/Common.hpp>
#include <Common/MemoryEngine.hpp>

#include <array>
#include <cstdint>
#include <unordered_set>
#include <vector>

#include <Dota2/CBasePattern.hpp>
#include <Dota2/SDK/SDK.hpp>
#include <Dota2/SDK/Update/VMT_Index.hpp>

#define SCHEMA_SYSTEM_INTERFACE_VERSION "SchemaSystem_001"

namespace GeneratorOffset
{
	// CSchemaSystemTypeScope::class_bindings (UtlTsHash)
	constexpr auto ClassBindings = 0x560;
	// CSchemaSystemTypeScope::enum_bindings (UtlTsHash)
	constexpr auto EnumBindings = 0x1DD0;
}

class CSchemaClassBinding;
class CSchemaType;
class CSchemaSystemTypeScope;

struct SchemaClassFieldData_t
{
	const char* m_pName;
	CSchemaType* m_pType;
	int32_t m_nOffset;
	int32_t m_nMetadataCount;
	void* m_pMetadata;
};

struct SchemaEnumeratorInfoData_t
{
	const char* m_pName;
	union
	{
		uint8_t m_nValueChar;
		uint16_t m_nValueShort;
		uint32_t m_nValueInt;
		uint64_t m_nValue;
	};
};

class CSchemaType
{
public:
	void* m_pVtable;
	const char* m_szTypeName;
};

class CSchemaClassBinding
{
public:
	CUSTOM_OFFSET_FIELD( void* , m_pBase , 0x0 );
	CUSTOM_OFFSET_FIELD( const char* , m_bindingName , 0x8 );
	CUSTOM_OFFSET_FIELD( const char* , m_binaryName , 0x10 );
	CUSTOM_OFFSET_FIELD( const char* , m_dllName , 0x18 );
	CUSTOM_OFFSET_FIELD( int32_t , m_SizeOf , 0x20 );
	CUSTOM_OFFSET_FIELD( int16_t , m_fieldCount , 0x24 );
	CUSTOM_OFFSET_FIELD( int16_t , m_staticMetadataCount , 0x26 );
	CUSTOM_OFFSET_FIELD( SchemaClassFieldData_t* , m_fields , 0x30 );
	CUSTOM_OFFSET_FIELD( void* , m_baseClasses , 0x40 );
	CUSTOM_OFFSET_FIELD( void* , m_staticMetadata , 0x48 );
	CUSTOM_OFFSET_FIELD( CSchemaSystemTypeScope* , m_TypeScope , 0x58 );
	CUSTOM_OFFSET_FIELD( CSchemaType* , m_Type , 0x60 );

	auto m_DataArray() -> SchemaClassFieldData_t* { return m_fields(); }
	auto m_DataArraySize() -> int16_t { return m_fieldCount(); }
};

class CSchemaEnumBinding
{
public:
	CUSTOM_OFFSET_FIELD( void* , m_pBase , 0x0 );
	CUSTOM_OFFSET_FIELD( const char* , m_bindingName , 0x8 );
	CUSTOM_OFFSET_FIELD( const char* , m_dllName , 0x10 );
	CUSTOM_OFFSET_FIELD( uint8_t , m_size , 0x18 );
	CUSTOM_OFFSET_FIELD( uint8_t , m_alignment , 0x19 );
	CUSTOM_OFFSET_FIELD( uint16_t , m_enumeratorCount , 0x1C );
	CUSTOM_OFFSET_FIELD( SchemaEnumeratorInfoData_t* , m_enumerators , 0x20 );

	auto m_DataArray() -> SchemaEnumeratorInfoData_t* { return m_enumerators(); }
	auto m_DataArraySize() -> uint16_t { return m_enumeratorCount(); }
	auto m_TypeSize() -> int8_t { return static_cast<int8_t>( m_size() ); }
};

struct TsListHead
{
	void* m_pNext;
};

struct TsListBase
{
	TsListHead m_head;
};

struct UtlMemoryPool
{
	int32_t m_blockSize;
	int32_t m_blocksPerBlob;
	uint32_t m_growMode;
	int32_t m_blocksAllocated;
	int32_t m_peakAllocated;
	uint16_t m_alignment;
	uint16_t m_blobCount;
	PAD( 0x2 );
	TsListBase m_freeBlocks;
	PAD( 0x20 );
	void* m_blobHead;
	int32_t m_totalSize;
	PAD( 0xC );
};

template<typename T>
struct UtlTsHashFixedData
{
	uint64_t m_uiKey;
	UtlTsHashFixedData<T>* m_pNext;
	T* m_pData;
};

template<typename T>
struct UtlTsHashBucket
{
	uintptr_t m_addLock;
	UtlTsHashFixedData<T>* m_pFirst;
	UtlTsHashFixedData<T>* m_pFirstUncommitted;
};

template<typename T, size_t BucketCount = 256>
struct UtlTsHash
{
	UtlMemoryPool m_entryMem;
	std::array<UtlTsHashBucket<T> , BucketCount> m_buckets;
	bool m_needsCommit;
	PAD( 0x3 );
	int32_t m_contentionCheck;
	PAD( 0x8 );
};

template<typename T, size_t BucketCount = 256>
struct UtlTsHashAllocatedBlob
{
	UtlTsHashAllocatedBlob<T>* m_pNext;
	PAD( 0x8 );
	T* m_pData;
	PAD( 0x18 );
};

class CSchemaSystemTypeScope
{
public:
	auto FindRawClassBinding( const char* className ) -> CSchemaClassBinding*
	{
		VirtualFn( CSchemaClassBinding* )( CSchemaSystemTypeScope* , const char* );
		return vget<Fn>( this , SDK::VMT_Index::CSchemaSystemTypeScope::FindRawClassBinding )( this , className );
	}

	auto GetClassBindingsHash() -> UtlTsHash<CSchemaClassBinding>*
	{
		return CUSTOM_OFFSET_RAW( UtlTsHash<CSchemaClassBinding> , GeneratorOffset::ClassBindings );
	}

	auto GetEnumBindingsHash() -> UtlTsHash<CSchemaEnumBinding>*
	{
		return CUSTOM_OFFSET_RAW( UtlTsHash<CSchemaEnumBinding> , GeneratorOffset::EnumBindings );
	}
};

namespace CSchemaSystem_Search
{
	inline CBasePattern GetAllTypeScopeFn = { "CSchemaSystem::GetAllTypeScope" , "48 8B 05 ? ? ? ? 48 8B D6 0F B7 CB 48 8B 3C C8" , SCHEMASYSTEM_DLL , 0 , SEARCH_TYPE_PTR2 };
}

class CSchemaSystem
{
public:
	auto GlobalTypeScope() -> CSchemaSystemTypeScope*
	{
		VirtualFn( CSchemaSystemTypeScope* )( CSchemaSystem* );
		return vget<Fn>( this , SDK::VMT_Index::CSchemaSystem::GlobalTypeScope )( this );
	}

	auto GetAllTypeScope() -> CSchemaSystemTypeScope**
	{
		return *reinterpret_cast<CSchemaSystemTypeScope***>( CSchemaSystem_Search::GetAllTypeScopeFn.GetFunction() );
	}

	auto GetAllTypeScopeSize() -> uint16_t
	{
		return *reinterpret_cast<uint16_t*>( reinterpret_cast<uintptr_t>( CSchemaSystem_Search::GetAllTypeScopeFn.GetFunction() ) - 0x8 );
	}
};
