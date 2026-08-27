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
		// When on, CastPlanAction skips the confirm-click after the ability
		// hotkey and relies on the press alone firing at the cursor - correct
		// ONLY if Quickcast is enabled for that ability in Dota's own options.
		// Defaults off: a stray confirm-click is harmless when Dota-side
		// Quickcast IS on (it just selects the already-cast-at unit), but
		// skipping the click when Dota-side Quickcast is OFF means the ability
		// only ever enters targeting mode and never fires - which reads as
		// "the spell didn't kill" when the plan and damage math were correct
		// the whole time. Off is the setting that can't silently no-op casts.
		inline bool QuickCast = false;
		inline float HealthBuffer = 20.f;
		// Extra percentage of the target's current HP required on top of
		// HealthBuffer before a plan counts as lethal - see LethalThreshold in
		// CKillStealer.cpp.
		inline float SafetyMarginPercent = 25.f;
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
	namespace Dodger
	{
		// Reactive defense (see CDodger.cpp): answers an enemy cast that can
		// reach us - or a low ally - with the best counter we own.
		inline bool Enable = false;
		inline bool DodgeSelf = true;
		// React to an enemy blinking onto us (detected as a position jump, see
		// CDodger.cpp) as well as to the spells themselves.
		inline bool DodgeEnemyBlink = true;
		inline bool SaveAllies = true;
		inline bool UseItems = true;
		inline bool UseAbilities = true;
		// Blinking away is the loudest reaction there is (it moves the hero),
		// so it gets its own switch even though blink items are just items.
		inline bool UseBlink = true;
		inline bool DrawThreatMarkers = true;
		// Same meaning as KillStealer::QuickCast: on, the confirming click
		// after the hotkey is skipped, which is only correct when Dota's own
		// Quickcast is enabled for that item/ability. Off can never silently
		// no-op a cast, so off is the default.
		inline bool QuickCast = false;
		// Casters farther than this are ignored even if the spell could
		// technically reach - a global ult from across the map is not
		// something an item reaction is going to help with.
		inline float TriggerRange = 1500.f;
		// Only save an ally already below this much of their max health.
		inline float AllySaveHealthPercent = 40.f;
		// Health-driven rescue for the cases no cast detection can catch: the
		// burst already landed, or the fight is simply lost. Fires for us and
		// (when Save Allies is on) for a nearby ally, but only with an enemy
		// hero close by - dying to a tower is not an item's problem.
		inline bool PanicSaveEnable = true;
		inline float PanicHealthPercent = 18.f;
		// Deliberate pause between spotting the cast and answering it. 0 is
		// the fastest and least human-looking.
		inline float ReactionDelayMs = 60.f;
		// Damage bar an ability not in CDodger's curated danger table has to
		// clear before it is worth an item.
		inline float MinDangerDamage = 150.f;
		// A damage-only spell (no stun/hex/root) is answered only when its
		// catalog damage is at least this share of the health we have left -
		// otherwise a Zeus ult at full health would cost a Eul's every time.
		// Disables ignore this and are always answered.
		inline float MinThreatHealthPercent = 35.f;
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

		// Per-spell cast order and enable flags, edited from the menu. Indices
		// are ability slots 0..5 (hotkeys Q W E D F R). SpellOrder is the cast
		// sequence - element 0 casts first - and is always kept a permutation
		// of 0..5 (the UI only swaps neighbors). A slot with SpellEnabled[slot]
		// false is skipped by the combo entirely. Items (if enabled) cast after
		// all spells, the auto-attack finisher last.
		inline constexpr int SpellSlotCount = 6;
		inline int SpellOrder[SpellSlotCount] = { 0 , 1 , 2 , 3 , 4 , 5 };
		inline bool SpellEnabled[SpellSlotCount] = { true , true , true , true , true , true };
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
