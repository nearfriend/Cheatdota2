#include "Hook_GetProtoCDOTAGameAccountPlus.hpp"

#include <AndromedaClient/Settings/Settings.hpp>

namespace
{
	constexpr ptrdiff_t kDefaultOffset = offsetof( CDOTAGameAccountPlus , m_nStatus ); // 0x10

	ptrdiff_t g_StatusOffset = kDefaultOffset;
	bool g_OffsetConfirmed = false;
	bool g_WarnedNotFound = false;
}

auto Hook_GetProtoCDOTAGameAccountPlus( CDOTAGCClientSystem* pCDOTAGCClientSystem ) -> CDOTAGameAccountPlus*
{
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
