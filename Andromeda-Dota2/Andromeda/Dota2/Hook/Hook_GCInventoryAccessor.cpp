#include "Hook_GCInventoryAccessor.hpp"

#include <Dota2/Hook/Hook_GetProtoCDOTAGameAccountPlus.hpp>

auto Hook_GCInventoryAccessor( CDOTAGCClientSystem* pCDOTAGCClientSystem ) -> void*
{
	CacheGCClientSystem( pCDOTAGCClientSystem );

	if ( !GCInventoryAccessor_o )
		return nullptr;

	return GCInventoryAccessor_o( pCDOTAGCClientSystem );
}
