#include "Hook_GetProtoCDOTAGameAccountPlus.hpp"

#include <AndromedaClient/Settings/Settings.hpp>
#include <Dota2/SDK/FunctionListSDK.hpp>

namespace
{
	constexpr ptrdiff_t kDefaultOffset = offsetof( CDOTAGameAccountPlus , m_nStatus ); // 0x10

	ptrdiff_t g_StatusOffset = kDefaultOffset;
	bool g_OffsetConfirmed = false;
	bool g_WarnedNotFound = false;
	bool g_LoggedGCClientSystem = false;

	// Cached gateway to the GC SharedObject cache; see header. Set on every hook
	// call regardless of the DotaPlus toggle, so the inventory path can use it.
	CDOTAGCClientSystem* g_pGCClientSystem = nullptr;
}

auto CacheGCClientSystem( CDOTAGCClientSystem* pCDOTAGCClientSystem ) -> void
{
	if ( !pCDOTAGCClientSystem )
		return;

	g_pGCClientSystem = pCDOTAGCClientSystem;

	if ( !g_LoggedGCClientSystem )
	{
		DEV_LOG( "[cosmetic] GC client system captured: %p (armory path can begin)\n" , pCDOTAGCClientSystem );
		g_LoggedGCClientSystem = true;
	}
}

auto Hook_GetProtoCDOTAGameAccountPlus( CDOTAGCClientSystem* pCDOTAGCClientSystem ) -> CDOTAGameAccountPlus*
{
	// Keep this legacy DotaPlus path useful as a fallback GC-system capture.
	CacheGCClientSystem( pCDOTAGCClientSystem );

	auto pResult = GetProtoCDOTAGameAccountPlus_o( pCDOTAGCClientSystem );

	if ( pResult && Settings::DotaPlus::Enable )
	{
		auto* pBase = reinterpret_cast<uint8_t*>( pResult );

		// При первом вызове (после возможных обновлений) пытаемся найти смещение статуса по значениям.
		if ( !g_OffsetConfirmed )
		{
			constexpr size_t kScanBytes = 0x100;

			for ( size_t offset = 0; offset + sizeof( int32_t ) <= kScanBytes; offset += sizeof( int32_t ) )
			{
				auto* pField = reinterpret_cast<int32_t*>( pBase + offset );
				const auto value = *pField;

				if ( value == CDOTAGameAccountPlus::EDotaPlusStatus::STATUS_UNSUBSCRIBED ||
					 value == CDOTAGameAccountPlus::EDotaPlusStatus::STATUS_INVALID ||
					 value == 0 )
				{
					g_StatusOffset = static_cast<ptrdiff_t>( offset );
					g_OffsetConfirmed = true;

					if ( g_StatusOffset != kDefaultOffset )
						DEV_LOG( "[DotaPlus] Found status offset 0x%zX (default 0x%zX)\n" , offset , static_cast<size_t>( kDefaultOffset ) );
					break;
				}
			}

			if ( !g_OffsetConfirmed && !g_WarnedNotFound )
			{
				DEV_LOG( "[DotaPlus] Status offset not found in first 0x%X bytes, using default 0x%zX\n" , static_cast<unsigned>( kScanBytes ) , static_cast<size_t>( kDefaultOffset ) );
				g_WarnedNotFound = true;
			}
		}

		auto* pStatus = reinterpret_cast<int32_t*>( pBase + g_StatusOffset );
		*pStatus = CDOTAGameAccountPlus::EDotaPlusStatus::STATUS_SUBSCRIBED;
	}

	return pResult;
}

auto GetDotaPlusStatusOffset() -> ptrdiff_t
{
	return g_StatusOffset;
}

auto IsDotaPlusStatusOffsetConfirmed() -> bool
{
	return g_OffsetConfirmed;
}

auto GetCachedGCClientSystem() -> CDOTAGCClientSystem*
{
	// Prefer the hook-captured pointer; if the DotaPlus getter has not fired yet
	// (e.g. never visited the main menu), fall back to the optional signature-based
	// getter. That getter returns null until a verified sig is filled in, so this
	// is a no-op on an unconfigured build and never costs a bad call.
	if ( !g_pGCClientSystem )
	{
		if ( auto* p = SDK_GetDOTAGCClientSystem() )
			CacheGCClientSystem( p );
	}
	return g_pGCClientSystem;
}
