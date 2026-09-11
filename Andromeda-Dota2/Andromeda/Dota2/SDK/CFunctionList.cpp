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

	// Optional order-injection function (Phase 2b). Searched only when a real
	// signature has been filled in - the default is an empty placeholder, which
	// must NOT be searched (an empty IDA pattern resolves to the first code
	// address, not null). SkipError so a blank/failed search is silent and never
	// affects cheat init; GetFunction() then stays null and SendMoveOrder no-ops.
	if ( PrepareUnitOrders.HasPattern() )
		PrepareUnitOrders.Search( true );

	// Optional GC-system getter for the armory path. Same rule: only searched once
	// a real signature is present, so the empty default stays null and the armory
	// path silently falls back to the DotaPlus-hook-captured pointer.
	if ( GetDOTAGCClientSystem.HasPattern() )
		GetDOTAGCClientSystem.Search( true );

	// Optional model setter for the local skin changer. Same rule: only searched
	// once a real signature is present, so the empty default stays null and the
	// cosmetic apply path remains preview-only instead of calling a bad address.
	if ( SetModel.HasPattern() )
		SetModel.Search( true );

	return searched;
}

auto GetFunctionList() -> CFunctionList*
{
	return &g_CFunctionList;
}
