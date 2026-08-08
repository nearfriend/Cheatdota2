#include "Hook_OnAddEntity.hpp"

#include <AndromedaClient/CAndromedaClient.hpp>
#include <Dota2/SDK/Types/CEntityData.hpp>

#include <cstring>
#include <cctype>

namespace
{
	auto LooksLikeFogControllerName( const char* name ) -> bool
	{
		if ( !name || !name[0] )
			return false;

		auto contains = [name]( const char* word )
		{
			const size_t nameLength = std::strlen( name );
			const size_t wordLength = std::strlen( word );

			for ( size_t index = 0; index + wordLength <= nameLength; ++index )
			{
				size_t offset = 0;
				for ( ; offset < wordLength; ++offset )
				{
					const unsigned char value = static_cast<unsigned char>( name[index + offset] );
					if ( static_cast<char>( std::tolower( value ) ) != word[offset] )
						break;
				}

				if ( offset == wordLength )
					return true;
			}

			return false;
		};

		return contains( "fog" ) && contains( "controller" );
	}
}

auto Hook_OnAddEntity( CGameEntitySystem* pCGameEntitySystem , CEntityInstance* pInst , CHandle handle ) -> void
{
	// Entity construction must never wait on application-side classification.
	// Call Dota first, then perform only the constant-time fog-controller check.
	if ( OnAddEntity_o )
		OnAddEntity_o( pCGameEntitySystem , pInst , handle );

	if ( !pInst )
		return;

	const char* className = pInst->GetSchemaClassName();
	bool isFogController = LooksLikeFogControllerName( className );

	if ( !isFogController )
	{
		if ( auto* identity = pInst->pEntityIdentity() )
		{
			isFogController = LooksLikeFogControllerName( identity->DesingerName().String() ) ||
				LooksLikeFogControllerName( identity->Name().String() );
		}
	}

	if ( isFogController )
	{
		if ( auto* client = GetAndromedaClient() )
			client->RegisterFogController( pInst );
	}
}
