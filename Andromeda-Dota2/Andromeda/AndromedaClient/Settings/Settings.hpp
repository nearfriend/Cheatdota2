#pragma once

#include <Common/Common.hpp>
#include <AndromedaClient/Settings/Heroes/Invoker.hpp>

namespace Settings
{
	namespace Camera
	{
		inline auto Enable = true;
		inline auto Distance = 1200.f; // Default
		inline auto SmoothZoom = true;
		inline auto SmoothnessDuration = 1.4f;
		inline auto ZoomUsingWheel = 0;
		inline auto ZoomSpeed = 50.f;
	}
	namespace Menu
	{
		inline auto MenuAlpha = 200;
		inline auto MenuAlphaMin = 0.85f;
	}
	namespace InfoOverlay
	{
		inline bool TopOverlayEnabled = true;
		inline bool ShowOnAllies = true;
		inline bool ShowDangerousAbilityTimer = true;
		inline bool TopOverlayElements[6] = { true, true, true, true, true, true };

		inline bool SidePanelsEnabled = true;
		inline bool ShowItems = true;
		inline bool ShowNetworth = true;
		inline bool ScanGlyphInfo = true;

		inline bool WardTrackerEnabled = false;
		inline int WardShowMode = 2;
		inline int WardWorldRenderMode = 2;
	}
	namespace VisibleByEnemy
	{
		inline bool Enable = true;
	}
	namespace DotaPlus
	{
		inline auto Enable = true;
	}
	namespace KillStealer
	{
		inline bool Enable = false;
		inline bool UseAbilities = true;
		inline bool UseItems = true;
		inline bool UseAutoAttack = true;
		inline bool DrawKillableMarkers = true;
		inline bool DrawDebugInfo = false;
		inline bool QuickCast = true;
		inline float HealthBuffer = 1.f;
		inline int AttackKey = 'A';
	}
	namespace LastHitAssistant
	{
		inline bool Enable = false;
		inline float HealthBuffer = 120.f;
		inline float DetectRange = 950.f; // Sniper-style max lane scan range.
	}
	namespace Heroes
	{
		namespace Meepo
		{
			// Virtual-key code for combo activation (0 = none).
			inline int ComboKey = 0;
			inline int TargetEntIndex = -1;
		}
	}
}
