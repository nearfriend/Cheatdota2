#pragma once

#include "CUserCmd.hpp"

#include <Dota2/SDK/FunctionListSDK.hpp>

class CDOTAInput
{
public:
	auto GetSequenceNumber( C_DOTAPlayerController* pLocalPlayerController ) -> uint32_t
	{
		int32_t OutputTick = 0;
		GetCUserCmdTick( pLocalPlayerController , &OutputTick );

		if ( OutputTick <= 0 )
			return 0;

		auto** ppFirstCmd = SDK::Pointers::GetFirstCUserCmdArray();

		if ( ppFirstCmd )
		{
			const int32_t Tick = OutputTick - 1;

			auto* pCUserCmdArray = GetCUserCmdArray( ppFirstCmd , Tick );

			if ( pCUserCmdArray )
				return pCUserCmdArray->m_nSequenceNumber();
		}
		else
		{
			static bool s_Logged = false;

			if ( !s_Logged )
			{
				DEV_LOG( "[warn] GetFirstCUserCmdArray unavailable, using tick as sequence number\n" );
				s_Logged = true;
			}
		}

		// Fallback: use tick as sequence number when array pointer not resolved.
		return static_cast<uint32_t>( OutputTick );

		return 0;
	}

	auto GetUserCmd( C_DOTAPlayerController* pLocalPlayerController ) -> CUserCmd*
	{
		const auto SequenceNumber = GetSequenceNumber( pLocalPlayerController );

		if ( SequenceNumber )
			return GetCUserCmdBySequenceNumber( pLocalPlayerController , SequenceNumber );

		return nullptr;
	}
};
