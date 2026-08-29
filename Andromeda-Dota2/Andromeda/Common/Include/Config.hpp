#pragma once

// Project Configuration:

#define LOG_FILE					"debug.log"
#define GUI_FILE					"gui.ini"

#define CHEAT_NAME					"Andromeda [Dota2 Plus + Camera Distance Changer]"
#define CHEAT_VERSION				"1.0.1"

// Stamped into dwExtraInfo on every SendInput event the features generate, so
// GUI_WndProc can tell our own synthetic input apart from the player's.
//
// Why it exists: the window-procedure hook drops any MOUSE message ImGui
// consumed while io.WantCaptureMouse is set - and that is set whenever the
// cursor sits over ANY overlay window, including the small hero-tracker
// windows that float over heroes in the world. Keyboard messages are only
// dropped while the menu is actually open. That asymmetry meant every
// keypress-only cast worked while every cast that needed a confirming click
// was silently eaten by our own overlay before Dota ever saw it.
#define ANDROMEDA_INJECTED_INPUT_TAG	( (ULONG_PTR)0xA11D0D6E )

// Project Buid Config:

#ifdef RELEASE_BUILD

#define ENABLE_CONSOLE_DEBUG		1
#define ENABLE_CPP_EH_EXCEPTION		0

#define ENABLE_XOR_STR				0

#define LOG_SDK						1
#define LOG_SDK_PATTERN				1

#define DUMP_SCHEMA_SCOPE_LIST		0
// Schema Dump Configuration:
// Set to 1 to enable schema offset discovery (for Phase 1)
// WARNING: Generates VERY large log file (50+ MB) and may cause delays/crashes
// If game crashes on injection, try setting this to 0 temporarily
#define DUMP_SCHEMA_ALL_OFFSET		0

#endif // RELEASE_BUILD
