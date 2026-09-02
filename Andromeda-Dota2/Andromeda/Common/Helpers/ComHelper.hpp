#pragma once

#include <objbase.h>

// Injected DLLs render from a hooked thread (the game's own Present callback)
// that never goes through a normal app's CoInitializeEx startup path. Without
// this, CoCreateInstance(CLSID_WICImagingFactory, ...) fails with
// CO_E_NOTINITIALIZED and every WIC-backed texture load (icons, logo, hero
// portraits) silently fails while non-COM rendering (ImGui text, vector
// icons) keeps working - call this once before any WIC use on a given thread.
inline void EnsureComInitializedOnThisThread()
{
	thread_local bool initialized = false;
	if ( initialized )
		return;
	initialized = true;

	// S_OK/S_FALSE: this call owns/shares the apartment. RPC_E_CHANGED_MODE:
	// the host already initialized COM on this thread in a different mode -
	// still fine for CoCreateInstance, just leave it alone either way.
	CoInitializeEx( nullptr , COINIT_MULTITHREADED );
}
