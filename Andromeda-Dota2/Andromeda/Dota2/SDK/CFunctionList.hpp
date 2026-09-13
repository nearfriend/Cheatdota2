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

	// CSkeletonInstance::SetModel - installs an ALREADY-RESIDENT model on a
	// skeleton instance. Located by its own __FUNCTION__ string (skeletoninstance
	// .cpp:5530) and confirmed by caller 0x18A90F0, which type-checks the handle
	// against 'vmdl' before calling. VERIFIED ABI - note this is NOT a path setter:
	//   void __fastcall fn( void* skeletonInstance , void* vmdlResourceBinding );
	// `skeletonInstance` is the entity's m_pGameSceneNode, and the binding is the
	// same CStrongHandle value m_modelState.m_hModel already holds, so both are
	// things CCosmeticChanger reads today. Passing a path here would crash.
	// Its own assert - "SetModel to nonresident asset %s" - is why a swap to an
	// unloaded cosmetic needs PrecacheResource below first.
	CBasePattern CSkeletonInstance_SetModel = { "CSkeletonInstance::SetModel" , "40 55 53 56 57 41 56 48 8D AC 24 00 FC FF FF" , CLIENT_DLL , 0 , SEARCH_TYPE_NONE };
	// ResourcePath constructor from UTF-8, client RVA 0x3A1DC80 on the verified
	// build. bool(ResourcePath*, const char*); fills the normalized path and hashes.
	CBasePattern ResourcePath_Init = { "ResourcePath::Init" , "48 89 5C 24 10 57 48 83 EC 30 8B 41 04 48 8D 79 08 48 8B D9 A9 FF FF FF 3F" , CLIENT_DLL , 0 , SEARCH_TYPE_NONE };

	// PrecacheResource - makes a resource resident by name. Identified by its own
	// error string ("Attempting to precache resource, but resource name is NULL or
	// empty") and its 'vmdl' extension test. VERIFIED ABI:
	//   void __fastcall fn( const char* resourceName , void* context );
	// `context` must be a LIVE precache context. It looks null-tolerant - it falls
	// back to [[0x6531B20]+8] - but that global only holds a context during the
	// engine's precache phase, so on a game tick the fallback is null too and the
	// function faults dereferencing it (c0000005 at +0x19B0162). See the guard in
	// SDK_PrecacheResource; we have no context source yet, so it never fires.
	CBasePattern PrecacheResource = { "PrecacheResource" , "48 89 5C 24 08 57 48 83 EC 20 48 8B FA 48 8B D9 48 85 C9 ? ? 80 39 00" , CLIENT_DLL , 0 , SEARCH_TYPE_NONE };
};

auto GetFunctionList() -> CFunctionList*;
