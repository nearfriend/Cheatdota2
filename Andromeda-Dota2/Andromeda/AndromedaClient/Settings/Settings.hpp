#pragma once

#include <Common/Common.hpp>
#include <AndromedaClient/Settings/Heroes/Invoker.hpp>

#include <string>
#include <vector>

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
		// User-authored "this enemy spell, that answer of mine" pairings, built
		// in the menu's combination editor from the spells the enemy heroes
		// actually have and the counters our hero actually owns.
		//
		// These outrank CDodger's built-in pair table, which in turn outranks
		// flag matching - so the editor is an override, not a replacement: a
		// spell with no rule still gets answered automatically.
		struct SpellRule
		{
			// Ability/item entity name of the ENEMY spell, e.g. lion_impale.
			std::string spell;
			// Ability/item entity name of OUR answer. Empty = decide
			// automatically, only the ignore flag matters.
			std::string counter;
			// Never dodge this spell at all.
			bool ignore = false;
		};

		inline std::vector<SpellRule> Rules;

		// Returns the rule for a spell, or nullptr when the user has not
		// expressed an opinion about it.
		inline auto FindRule( const std::string& spell ) -> SpellRule*
		{
			for ( auto& rule : Rules )
			{
				if ( rule.spell == spell )
					return &rule;
			}
			return nullptr;
		}

		inline auto RuleFor( const std::string& spell ) -> SpellRule&
		{
			if ( auto* existing = FindRule( spell ) )
				return *existing;
			Rules.push_back( SpellRule{ spell , {} , false } );
			return Rules.back();
		}

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
		// How a cast on OURSELVES is issued (Eul's, Lotus, Glimmer on self).
		//
		// Off (default): aim at our own hero and click it, centring the camera
		// on the hero first when it is off screen so the click always has a
		// model to land on. This depends on no Dota option.
		//
		// On: double tap the key, which is Dota's own self-cast. Cleaner when
		// it works, but it needs Dota's "double tap ability to self cast"
		// option enabled - a live capture with it disabled showed the two
		// presses going out and the item never leaving cooldown-ready, so it
		// cannot be the default.
		inline bool PreferDoubleTapSelfCast = false;
		// Casters farther than this are ignored even if the spell could
		// technically reach - a global ult from across the map is not
		// something an item reaction is going to help with.
		inline float TriggerRange = 1500.f;
		// Health-driven rescue for the cases no cast detection can catch: the
		// burst already landed, or the fight is simply lost. Fires for us and
		// (when Save Allies is on) for a nearby ally, but only with an enemy
		// hero close by - dying to a tower is not an item's problem.
		inline bool PanicSaveEnable = true;
		// Deliberate pause between spotting the cast and answering it. 0 is
		// the fastest and least human-looking.
		inline float ReactionDelayMs = 60.f;
	}
	namespace LastHitAssistant
	{
		inline bool Enable = false;
		inline bool EnableAutoAttack = true;
	}
	namespace CreepBlocker
	{
		inline bool Enable = false;
		// Held, not toggled: blocking is something you do for the ten seconds
		// of the walk to lane, and a hold reads the same as doing it by hand.
		inline int Key = 0;
		// How far in front of the leading creep the hero is sent. Too short
		// and the order resolves behind the creep so it walks past; too long
		// and the hero runs ahead of the wave instead of standing in it.
		inline float BlockAhead = 110.f;
		// How far sideways a single order may pull the hero while it tracks the
		// leading creep's line. Blocking means standing in the creep's path and
		// following it across when it tries to walk around; this caps how much
		// of that correction happens per order, so a creep wide of the hero
		// does not send it sprinting across the lane in one go.
		inline float SideStep = 45.f;
		// Circle-and-line marker on the point the hero is being sent to.
		inline bool DrawBlockMarker = true;
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
