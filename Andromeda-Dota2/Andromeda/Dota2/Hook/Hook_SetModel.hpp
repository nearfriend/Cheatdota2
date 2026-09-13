#pragma once

#include <Common/Common.hpp>

// CSkeletonInstance::SetModel( CSkeletonInstance* skeleton , ResourceBinding* vmdl ).
//
// Every model the client installs on anything passes through here with its
// resource binding in hand. Hooking it gives the skin changer two things the
// entity-scan approach could not: a free, passive record of every binding the
// game ever loads (loading screen, menus, other heroes' cosmetics), and the
// chance to substitute a binding at the moment the model is actually decided,
// instead of patching an entity after the fact.
auto Hook_SetModel( void* skeleton , void* binding ) -> void;

using SetModel_t = decltype( &Hook_SetModel );
inline SetModel_t SetModel_o = nullptr;

// Verified model-combiner entry and econ-item model-path lookup.
auto Hook_BuildCombinedModel( void* hero , const void* items ) -> bool;
auto Hook_GetItemModel( void* itemView , int modelVariant ) -> const char*;
inline decltype( &Hook_BuildCombinedModel ) BuildCombinedModel_o = nullptr;
inline decltype( &Hook_GetItemModel ) GetItemModel_o = nullptr;
