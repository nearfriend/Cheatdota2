#include "CFunctionList.hpp"

static CFunctionList g_CFunctionList{};

auto CFunctionList::OnInit() -> bool
{
	const bool hasLocalPlayerPattern = CGameEntitySystem_GetLocalPlayerController.Search( true );

	if ( !hasLocalPlayerPattern )
		DEV_LOG( "[warn] CBasePattern: CGameEntitySystem::GetLocalPlayerController (using schema fallback)\n" );

	std::vector<CBasePattern*> requiredPatterns =
	{
		&GetCUserCmdTick,
		&GetCUserCmdArray,
		&GetCUserCmdBySequenceNumber,
	};

	auto searched = true;

	for ( auto& Pattern : requiredPatterns )
	{
		if ( !Pattern->Search() )
			searched = false;
	}

	return searched;
}

auto GetFunctionList() -> CFunctionList*
{
	return &g_CFunctionList;
}
