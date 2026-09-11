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

// True when a verified SetModel signature is compiled in and resolved, i.e. when
// the cosmetic changer can actually swap a wearable's model instead of only
// previewing the selection. Callers must check this before offering to apply.
inline bool SDK_SetModelAvailable()
{
	auto& pattern = GetFunctionList()->SetModel;
	return pattern.HasPattern() && pattern.GetFunction() != nullptr;
}

// Guarded CBaseModelEntity::SetModel. Loads the .vmdl at `vmdlPath` if needed and
// installs it on `entity`. Returns false (doing nothing) when the signature is
// absent/unresolved or the arguments are empty, so calling this on an
// unconfigured build is always a safe no-op rather than a jump to a bad address.
inline bool SDK_SetEntityModel( void* entity , const char* vmdlPath )
{
	if ( !entity || !vmdlPath || !vmdlPath[0] )
		return false;
	if ( !SDK_SetModelAvailable() )
		return false;

	using Fn = void ( __fastcall* )( void* , const char* );
	reinterpret_cast<Fn>( GetFunctionList()->SetModel.GetFunction() )( entity , vmdlPath );
	return true;
}

DECLARATE_CS2_FUNCTION_SDK_FASTCALL( C_DOTAPlayerController* , CGameEntitySystem_GetLocalPlayerController , ( int Unk = 0 ) , ( int ) , ( Unk ) );
DECLARATE_CS2_FUNCTION_SDK_FASTCALL( void , GetCUserCmdTick , ( C_DOTAPlayerController* pPlayerController , int32_t* pOutputTick ) , ( C_DOTAPlayerController* , int32_t* ) , ( pPlayerController , pOutputTick ) );
DECLARATE_CS2_FUNCTION_SDK_FASTCALL( CUserCmdArray* , GetCUserCmdArray , ( CUserCmd** ppCUserCmd , int Tick ) , ( CUserCmd** , int ) , ( ppCUserCmd , Tick ) );
DECLARATE_CS2_FUNCTION_SDK_FASTCALL( CUserCmd* , GetCUserCmdBySequenceNumber , ( C_DOTAPlayerController* pPlayerController , uint32_t SequenceNumber ) , ( C_DOTAPlayerController* , uint32_t ) , ( pPlayerController , SequenceNumber ) );
