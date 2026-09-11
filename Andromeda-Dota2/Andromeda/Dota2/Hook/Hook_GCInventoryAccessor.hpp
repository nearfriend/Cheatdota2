#pragma once

#include <Common/Common.hpp>

class CDOTAGCClientSystem;

auto Hook_GCInventoryAccessor( CDOTAGCClientSystem* pCDOTAGCClientSystem ) -> void*;

using GCInventoryAccessor_t = decltype( &Hook_GCInventoryAccessor );
inline GCInventoryAccessor_t GCInventoryAccessor_o = nullptr;
