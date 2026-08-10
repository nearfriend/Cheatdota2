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

		inline bool SidePanelsEnabled = false;
		inline bool ShowItems = true;
		inline bool ShowNetworth = true;
		inline bool ScanGlyphInfo = true;

		inline bool WardTrackerEnabled = false;
		inline int WardShowMode = 2;
		inline int WardWorldRenderMode = 2;
	}
	namespace DotaPlus
	{
		inline auto Enable = true;
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
