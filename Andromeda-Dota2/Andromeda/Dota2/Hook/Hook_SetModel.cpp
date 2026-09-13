#include "Hook_SetModel.hpp"

#include <AndromedaClient/CAndromedaClient.hpp>

namespace
{
	thread_local void* combiningHero = nullptr;
	struct CombineScope
	{
		void* previous = combiningHero;
		explicit CombineScope( void* hero ) { combiningHero = hero; }
		~CombineScope() { combiningHero = previous; }
	};
}

auto Hook_BuildCombinedModel( void* hero , const void* items ) -> bool
{
	if ( !BuildCombinedModel_o ) return false;
	CombineScope scope( hero );
	return BuildCombinedModel_o( hero , items );
}

auto Hook_GetItemModel( void* itemView , int modelVariant ) -> const char*
{
	// NO combiningHero gate here. It restricted substitution to builds we start
	// ourselves, and every one of those is refused by the engine - while the
	// rebuilds that DO complete are the engine's own, during which combiningHero is
	// null. Gating on it meant never substituting at all.
	//
	// `combiningHero` is still passed through, because the changer uses it to tell
	// the two cases apart: a build of ours qualifies on the hero, an engine rebuild
	// qualifies only if the item view is one of the local hero's own. Inventory
	// panels, previews and other players' heroes match neither and are untouched.
	if ( auto* client = GetAndromedaClient() )
		if ( const char* model = client->GetCosmeticChanger().GetCombinedModelOverride( combiningHero , itemView ) )
			return model;
	return GetItemModel_o ? GetItemModel_o( itemView , modelVariant ) : nullptr;
}

auto Hook_SetModel( void* skeleton , void* binding ) -> void
{
	// This runs on whatever thread the game loads models on, potentially many
	// times a frame. Everything the changer does here is one lock and two hash
	// operations; path resolution and logging happen later, on the tick thread.
	void* install = binding;
	if ( auto* client = GetAndromedaClient() )
		install = client->GetCosmeticChanger().OnSkeletonSetModel( skeleton , binding );

	if ( SetModel_o )
		SetModel_o( skeleton , install );
}
