#pragma once

#include <Common/Common.hpp>
#include <Common/MemoryEngine.hpp>
#include <cstddef>

class CDOTAGCClientSystem;

class CDOTAGameAccountPlus
{
private:
	PAD( 0x10 ); // m_nStatus находится на смещении 0x10 (по дампу из hook)
public:
	enum EDotaPlusStatus : int32_t
	{
		STATUS_INVALID = -1,
		STATUS_SUBSCRIBED = 1,
		STATUS_UNSUBSCRIBED = 2,
	};

	int32_t m_nStatus;
};

static_assert( offsetof( CDOTAGameAccountPlus , m_nStatus ) == 0x10 , "CDOTAGameAccountPlus::m_nStatus offset mismatch" );

auto Hook_GetProtoCDOTAGameAccountPlus( CDOTAGCClientSystem* pCDOTAGCClientSystem ) -> CDOTAGameAccountPlus*;

auto GetDotaPlusStatusOffset() -> ptrdiff_t;
auto IsDotaPlusStatusOffsetConfirmed() -> bool;

// Last live CDOTAGCClientSystem* seen by the hook. This is the gateway to the
// GC SharedObject cache (owned-item inventory / armory catalog). Null until the
// hooked getter has fired at least once. Consumers must treat it as best-effort
// and validate before dereferencing - it is only cached, never owned.
auto GetCachedGCClientSystem() -> CDOTAGCClientSystem*;
auto CacheGCClientSystem( CDOTAGCClientSystem* pCDOTAGCClientSystem ) -> void;

using GetProtoCDOTAGameAccountPlus_t = decltype( &Hook_GetProtoCDOTAGameAccountPlus );
inline GetProtoCDOTAGameAccountPlus_t GetProtoCDOTAGameAccountPlus_o = nullptr;
