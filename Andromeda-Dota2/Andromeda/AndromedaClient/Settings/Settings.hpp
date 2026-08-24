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
		inline bool TopOverlayElements[6] = {true, true, true, true, true, true};

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
		inline bool DrawDetectRangeCircle = true;
		inline bool DrawDebugInfo = false;
		inline bool QuickCast = true;
		inline float HealthBuffer = 1.f;
		inline int AttackKey = 'A';
		// Lead the plan with Ethereal Blade (doubles magic damage taken) when it
		// helps land the kill in fewer actions, instead of treating it as a
		// no-damage item that gets filtered out.
		inline bool PrioritizeEtherealBlade = true;
		// Ignore enemy heroes farther than this from the local hero. Without a
		// gate here, any enemy your team merely has vision of (anywhere on the
		// map) gets evaluated every tick even though nothing in range could ever
		// reach them - noise at best, wasted work at worst.
		inline float DetectRange = 1200.f;
	}
	namespace LastHitAssistant
	{
		inline bool Enable = false;
		inline bool EnableAutoAttack = true;
	}
	namespace AutoCombo
	{
		// Generic, hero-agnostic burst combo (see CAutoCombo.cpp): fires every
		// off-cooldown, affordable damage ability/item at TargetEntIndex on ComboKey.
		inline bool Enable = false;
		inline bool UseAbilities = true;
		inline bool UseItems = true;
		inline bool UseAutoAttack = false;
		inline bool QuickCast = true;
		inline int ComboKey = 0;
		inline int TargetEntIndex = -1;
		inline int AttackKey = 'A';
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
