#pragma once

#include <Common/Common.hpp>

#include <Dota2/SDK/Types/CBaseTypes.hpp>
#include <Dota2/SDK/CFunctionList.hpp>

class C_DOTAPlayerController;
class CUserCmdArray;
class CUserCmd;
class CDOTAGCClientSystem;

// Guarded call into the optional GC-system getter. Unlike the DECLARATE_ wrappers
// below (which call unconditionally), this returns nullptr whenever the signature
// is absent or unresolved, so it is safe to call before the sig has been filled
// in - it simply never yields a pointer until then.
inline CDOTAGCClientSystem* SDK_GetDOTAGCClientSystem()
{
	auto& pattern = GetFunctionList()->GetDOTAGCClientSystem;
	if ( !pattern.HasPattern() )
		return nullptr;
	void* fn = pattern.GetFunction();
	if ( !fn )
		return nullptr;
	using Fn = CDOTAGCClientSystem* ( __fastcall* )( );
	return reinterpret_cast<Fn>( fn )( );
}

// True when the verified CSkeletonInstance::SetModel signature resolved on this
// build, i.e. when a wearable's model can actually be replaced rather than only
// previewed. Callers must check this before offering to apply.
inline bool SDK_SetModelAvailable()
{
	auto& pattern = GetFunctionList()->CSkeletonInstance_SetModel;
	return pattern.HasPattern() && pattern.GetFunction() != nullptr;
}

// Guarded CSkeletonInstance::SetModel.
//
// IMPORTANT: this takes a resource BINDING, not a path - the game resolves a
// path to a binding elsewhere, and this function only installs one that is
// already resident (its own assert fires on a nonresident asset). `skeleton` is
// the entity's m_pGameSceneNode and `vmdlBinding` is the same CStrongHandle
// value m_modelState.m_hModel holds, so a binding can be harvested from any
// entity that already renders the wanted model. Returns false without calling
// when the signature is unresolved or either argument is null.
inline bool SDK_SkeletonSetModel( void* skeleton , void* vmdlBinding )
{
	if ( !skeleton || !vmdlBinding )
		return false;
	if ( !SDK_SetModelAvailable() )
		return false;

	using Fn = void ( __fastcall* )( void* , void* );
	reinterpret_cast<Fn>( GetFunctionList()->CSkeletonInstance_SetModel.GetFunction() )( skeleton , vmdlBinding );
	return true;
}

inline bool SDK_PrecacheAvailable()
{
	auto& pattern = GetFunctionList()->PrecacheResource;
	return pattern.HasPattern() && pattern.GetFunction() != nullptr;
}

// Guarded PrecacheResource.
//
// `context` MUST be a live precache context - a null one CRASHES the game. The
// function looks like it tolerates null (it falls back to [[0x6531B20]+8]), but
// that global only holds a context during the engine's own precache phase; on a
// game tick it is null, so the fallback yields null too and the next instruction
// dereferences it. Confirmed the hard way: c0000005 at client.dll+0x19B0162,
// which is `mov r9,[rdi]` with rdi = 0.
//
// We have no way to obtain a valid context yet, so every caller currently passes
// null and this refuses. Keep the guard until a real context source exists.
inline bool SDK_PrecacheResource( const char* resourceName , void* context )
{
	if ( !resourceName || !resourceName[0] )
		return false;
	if ( !context )
		return false;
	if ( !SDK_PrecacheAvailable() )
		return false;

	using Fn = void ( __fastcall* )( const char* , void* );
	reinterpret_cast<Fn>( GetFunctionList()->PrecacheResource.GetFunction() )( resourceName , context );
	return true;
}

DECLARATE_CS2_FUNCTION_SDK_FASTCALL( C_DOTAPlayerController* , CGameEntitySystem_GetLocalPlayerController , ( int Unk = 0 ) , ( int ) , ( Unk ) );
DECLARATE_CS2_FUNCTION_SDK_FASTCALL( void , GetCUserCmdTick , ( C_DOTAPlayerController* pPlayerController , int32_t* pOutputTick ) , ( C_DOTAPlayerController* , int32_t* ) , ( pPlayerController , pOutputTick ) );
DECLARATE_CS2_FUNCTION_SDK_FASTCALL( CUserCmdArray* , GetCUserCmdArray , ( CUserCmd** ppCUserCmd , int Tick ) , ( CUserCmd** , int ) , ( ppCUserCmd , Tick ) );
DECLARATE_CS2_FUNCTION_SDK_FASTCALL( CUserCmd* , GetCUserCmdBySequenceNumber , ( C_DOTAPlayerController* pPlayerController , uint32_t SequenceNumber ) , ( C_DOTAPlayerController* , uint32_t ) , ( pPlayerController , SequenceNumber ) );
