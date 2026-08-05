#include "CFunctionList.hpp"

static CFunctionList g_CFunctionList{};

auto CFunctionList::OnInit() -> bool
{
	// Do not call the pattern-scanned GetLocalPlayerController - it crashes on current builds.
	CGameEntitySystem_GetLocalPlayerController.Search( true );

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
