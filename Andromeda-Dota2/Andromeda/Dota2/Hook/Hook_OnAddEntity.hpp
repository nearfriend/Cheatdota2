#pragma once

#include <Common/Common.hpp>

#include <Dota2/SDK/Types/CHandle.hpp>

class CGameEntitySystem;
class CEntityInstance;

auto Hook_OnAddEntity( CGameEntitySystem* pCGameEntitySystem , CEntityInstance* pInst , CHandle handle ) -> void;
auto ProcessPendingEntityAdds( size_t maxEvents ) -> void;

using OnAddEntity_t = decltype( &Hook_OnAddEntity );
inline OnAddEntity_t OnAddEntity_o = nullptr;
