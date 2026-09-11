#pragma once

#include <vector>
#include <Common/Common.hpp>

#include <Dota2/SDK/SDK.hpp>
#include <Dota2/CBasePattern.hpp>

#define DECLARATE_CS2_FUNCTION_SDK_FASTCALL(Ret,Function,Param,UsingParam,CallParam)\
inline Ret Function Param\
{\
	using Fn = Ret ( __fastcall* ) UsingParam;\
	Fn Original = static_cast<Fn>( GetFunctionList()->##Function##.GetFunction() );\
	return Original##CallParam##;\
}

class CFunctionList final
{
public:
	auto OnInit() -> bool;

public:
	CBasePattern CGameEntitySystem_GetLocalPlayerController = { "CGameEntitySystem::GetLocalPlayerController" , "E8 ? ? ? ? 48 89 45 ? 4C 8B F8 48 85 C0 0F 84 ? ? ? ? 48 89 B4 24" , CLIENT_DLL , 0 , SEARCH_TYPE_CALL };
	CBasePattern GetCUserCmdTick = { "GetCUserCmdTick" , "48 83 EC ? 4C 8B 0D ? ? ? ? 4C 8B DA" , CLIENT_DLL , 0 , SEARCH_TYPE_NONE };
	CBasePattern GetCUserCmdArray = { "GetCUserCmdArray" , "48 89 4C 24 ? 41 56 41 57" , CLIENT_DLL , 0 , SEARCH_TYPE_NONE };
	CBasePattern GetCUserCmdBySequenceNumber = { "GetCUserCmdBySequenceNumber" , "40 53 48 83 EC ? 8B DA E8 ? ? ? ? 4C 8B C0" , CLIENT_DLL , 0 , SEARCH_TYPE_NONE };

	// PrepareUnitOrders - the client function that issues a real unit order
	// (move/attack/cast) the same way the game does when you click, bypassing the
	// simulated-mouse click-grab that stops the creep blocker from ever reaching
	// body contact (see CCreepBlocker's Phase-2b notes). The order path is only
	// used when this resolves; the pattern below is INTENTIONALLY EMPTY so it
	// never resolves on an unverified build - filling it in is what turns Phase 2b
	// on. The byte signature is build-specific and MUST be verified against the
	// running client.dll (unique match, lands on a function prologue) before use;
	// a wrong pattern that happens to match would call a garbage address and crash
	// the game. NOT added to the required-pattern list in OnInit, so a blank/failed
	// search never blocks cheat init. See FeatureSupport::SendMoveOrder.
	CBasePattern PrepareUnitOrders = { "PrepareUnitOrders" , "" , CLIENT_DLL , 0 , SEARCH_TYPE_NONE };

	// Static getter returning the CDOTAGCClientSystem singleton - the GC gateway
	// to the SharedObject inventory cache (owned-cosmetics / armory catalog).
	// INTENTIONALLY EMPTY until a verified signature for the running build is
	// filled in, same discipline as PrepareUnitOrders above: an empty pattern must
	// never be Search()ed (an empty IDA pattern resolves to the first code address,
	// not null). When resolved it lets the armory path obtain the GC system without
	// waiting for the DotaPlus hook to fire. Expected ABI:
	//   CDOTAGCClientSystem* __fastcall fn();   // no args, returns the singleton
	// Verify the sig is a UNIQUE match landing on a function prologue and that the
	// returned pointer is stable before trusting it. NOT in the required list, so a
	// blank/failed search never blocks cheat init.
	CBasePattern GetDOTAGCClientSystem = { "GetDOTAGCClientSystem" , "" , CLIENT_DLL , 0 , SEARCH_TYPE_NONE };

	// CBaseModelEntity::SetModel - the client function that resolves a .vmdl path,
	// LOADS the model resource if it is not already resident, and installs it on the
	// entity. This is the missing link for the local skin changer: an in-match test
	// proved that rewriting m_iItemDefinitionIndex on a spawned wearable does NOT
	// swap the rendered mesh (Source 2 caches the model handle at spawn), and the
	// target cosmetic's model is normally not resident at all, so nothing short of a
	// real load-and-set can change the skin. With this resolved, CCosmeticChanger can
	// point any wearable at any catalog model path locally.
	// INTENTIONALLY EMPTY until a verified signature for the running build is filled
	// in - same discipline as the two placeholders above. Expected ABI:
	//   void __fastcall fn( void* entity /*CBaseModelEntity*/ , const char* vmdlPath );
	// Verify the sig is a UNIQUE match on a function prologue before enabling; a
	// wrong address here is called with a live entity and will crash the game.
	CBasePattern SetModel = { "SetModel" , "" , CLIENT_DLL , 0 , SEARCH_TYPE_NONE };
};

auto GetFunctionList() -> CFunctionList*;
