#include "CDodger.hpp"

#include "FeatureSupport.hpp"

#include <AndromedaClient/Data/AbilityDamageData.hpp>
#include <AndromedaClient/Settings/Settings.hpp>
#include <Common/DevLog.hpp>
#include <Dota2/SDK/Interface/CGameEntitySystem.hpp>
#include <Dota2/SDK/Interface/CLocalHeroResolver.hpp>
#include <Dota2/SDK/Math/Math.hpp>
#include <Dota2/SDK/SDK.hpp>

#include <ImGui/imgui.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

// How this detects a cast without a projectile or net-message hook
// ----------------------------------------------------------------
// There is no client-side hook here for dota_linear_projectile /
// TrackedProjectile, and the schema exposes no "what is this unit casting"
// pointer we could trust. What it does expose, per ability entity, is
// m_bInAbilityPhase (true for the duration of the cast point) and m_flCooldown
// (remaining seconds). So a cast is a *transition*:
//
//   1. m_bInAbilityPhase false -> true. This is the earliest possible signal,
//      firing at the START of the cast point, i.e. before the spell exists.
//      This is what makes a dodge actually beat the spell.
//   2. m_flCooldown ~0 -> >0. This fires when the ability leaves the ready
//      state, which is the moment it goes off. Slightly later than (1), but it
//      only depends on a field every other feature here already reads
//      successfully, so it is the fallback that keeps the feature working if a
//      future build renames or drops m_bInAbilityPhase.
//
// Both feed the same handler, deduplicated per ability entity so one cast can
// never trigger two dodges.
//
// The same watch loop covers enemy INVENTORY entities, so Hex, Atos, Dagon and
// friends trigger a dodge exactly like abilities do.
//
// Choosing the answer
// -------------------
// Two layers, in this order:
//   1. kPreferredCounters - explicit "this spell, that counter" pairs, e.g.
//      Anti-Mage's Counterspell for a Finger of Death, Manta Style for a
//      Reverse Polarity, Eul's for a Thundergod's Wrath. Whenever we own the
//      listed counter and it is usable, that is what fires.
//   2. Flag matching - a threat carries what it does (magical / physical /
//      pure / disable / travels / pierces immunity / instant) and each
//      response says what it answers, so anything not in the pair table still
//      gets a sensible counter.
// The kInstant flag is what stops the second layer picking nonsense: a spell
// that resolves the moment it is cast cannot be blinked away from, so
// positional answers are excluded from those entirely.

namespace
{
	namespace FS = FeatureSupport;

	constexpr uint32_t kThinkIntervalMs = 30;
	// Cursor settle time around a targeted cast. Dota consumes injected input
	// on its own message loop, not synchronously with SendInput, so the cursor
	// has to still be on the aim point when the game gets around to the key.
	constexpr uint32_t kCastSettleMs = 40;
	constexpr uint32_t kCastExpiryMs = 900;
	// After a counter fires, hold off - otherwise the follow-up cooldown edge
	// of the same spell (or the next spell of the same combo) burns every
	// escape item we own on one initiation.
	constexpr uint32_t kDodgeCooldownMs = 1500;
	// One cast must not be reported twice by the two detectors above.
	constexpr uint32_t kCastDedupeMs = 1000;
	constexpr uint32_t kWatchExpiryMs = 15000;
	constexpr uint32_t kThreatMarkerMs = 1400;
	// Slack added to a spell's reach before it counts as "can hit me". Cast
	// ranges here are the ability's own, so this only covers the caster still
	// walking toward us during the cast point.
	constexpr float kThreatMargin = 150.f;
	// Reach assumed for a dangerous no-target ability whose radius we don't
	// know (Ravage-likes are in the table below with a real radius).
	constexpr float kDefaultNoTargetReach = 600.f;
	constexpr float kAllySaveRange = 900.f;
	// A panic save that fires again a second later just feeds the next item
	// into the same losing fight, so it holds much longer than a spell dodge.
	constexpr uint32_t kPanicHoldMs = 6000;
	// How close a blink has to land before it counts as an initiation on us.
	constexpr float kBlinkThreatRange = 700.f;

	enum ThreatFlag : uint8_t
	{
		kMagical = 1 << 0,
		kPhysical = 1 << 1,
		kPure = 1 << 2,
		// Stun / hex / root / silence - worth answering even at zero damage.
		kDisable = 1 << 3,
		// Travels or has a wind-up, so moving the hero (blink, force staff,
		// Eul's) actually removes it.
		kProjectile = 1 << 4,
		// Goes through spell immunity: never answer these with a BKB.
		kPierces = 1 << 5,
		// Lands on its target the instant it is cast - no travel, no area to
		// step out of. Moving the hero (blink, force staff) or breaking
		// targeting (manta, glimmer) cannot answer these; only a status
		// answer can - immunity, a reflect, a spell block, or Eul's.
		kInstant = 1 << 6,
	};

	struct DangerEntry
	{
		const char* name;
		uint8_t flags;
		// Effective reach when the ability has no meaningful cast range of its
		// own (no-target ults, ground AoE). 0 = use the ability's cast range.
		float reach;
	};

	// Curated because the JSON catalog carries damage and cast range but not
	// "is this a stun" - a zero-damage hard disable (Chronosphere, Duel,
	// Berserker's Call) is exactly what this feature exists to answer. Spells
	// not listed still trigger through the damage heuristic in ClassifyThreat.
	constexpr DangerEntry kDangerAbilities[] =
	{
		{ "pudge_meat_hook" , kProjectile | kDisable | kPure | kPierces , 0.f },
		{ "mirana_arrow" , kProjectile | kDisable | kMagical , 3000.f },
		{ "lion_impale" , kProjectile | kDisable | kMagical , 0.f },
		{ "lion_finger_of_death" , kMagical | kInstant , 0.f },
		{ "lina_laguna_blade" , kMagical | kInstant , 0.f },
		{ "lina_light_strike_array" , kMagical | kDisable | kProjectile , 0.f },
		{ "sniper_assassinate" , kProjectile | kPhysical , 3000.f },
		{ "invoker_sun_strike" , kPure | kProjectile | kPierces , 0.f },
		{ "invoker_tornado" , kMagical | kDisable | kProjectile , 3000.f },
		{ "invoker_chaos_meteor" , kMagical | kProjectile , 1800.f },
		{ "invoker_deafening_blast" , kMagical | kDisable | kProjectile , 1000.f },
		{ "zuus_thundergods_wrath" , kMagical | kInstant , 25000.f },
		{ "sven_storm_bolt" , kProjectile | kDisable | kMagical , 0.f },
		{ "shadow_shaman_ether_shock" , kMagical | kProjectile , 0.f },
		{ "shadow_shaman_shackles" , kDisable | kMagical | kInstant , 0.f },
		{ "shadow_shaman_voodoo" , kDisable | kMagical | kInstant , 0.f },
		{ "bane_fiends_grip" , kDisable | kMagical | kInstant , 0.f },
		{ "bane_nightmare" , kDisable | kMagical | kInstant , 0.f },
		{ "beastmaster_primal_roar" , kDisable | kMagical , 0.f },
		{ "tidehunter_ravage" , kDisable | kMagical , 1250.f },
		{ "enigma_black_hole" , kDisable | kMagical , 420.f },
		{ "enigma_malefice" , kDisable | kMagical | kInstant , 0.f },
		{ "faceless_void_chronosphere" , kDisable | kPierces , 425.f },
		{ "magnataur_reverse_polarity" , kDisable | kMagical , 410.f },
		{ "earthshaker_echo_slam" , kMagical , 600.f },
		{ "earthshaker_fissure" , kDisable | kMagical | kProjectile , 0.f },
		{ "axe_berserkers_call" , kDisable | kPierces , 315.f },
		{ "legion_commander_duel" , kDisable | kPierces | kInstant , 0.f },
		{ "doom_bringer_doom" , kMagical | kDisable | kPierces | kInstant , 0.f },
		{ "chaos_knight_chaos_bolt" , kProjectile | kDisable | kMagical , 0.f },
		{ "vengefulspirit_magic_missile" , kProjectile | kDisable | kMagical , 0.f },
		{ "skywrath_mage_mystic_flare" , kMagical , 200.f },
		{ "witch_doctor_paralyzing_cask" , kProjectile | kDisable | kMagical , 0.f },
		{ "witch_doctor_death_ward" , kPhysical , 700.f },
		{ "lich_chain_frost" , kProjectile | kMagical , 0.f },
		{ "crystal_maiden_frostbite" , kDisable | kMagical | kInstant , 0.f },
		{ "crystal_maiden_freezing_field" , kMagical , 835.f },
		{ "jakiro_ice_path" , kDisable | kMagical | kProjectile , 0.f },
		{ "puck_dream_coil" , kDisable | kMagical , 375.f },
		{ "windrunner_shackleshot" , kProjectile | kDisable , 0.f },
		{ "windrunner_powershot" , kProjectile | kMagical , 2600.f },
		{ "kunkka_torrent" , kDisable | kMagical | kProjectile , 0.f },
		{ "kunkka_ghostship" , kMagical | kDisable | kProjectile , 0.f },
		{ "tiny_avalanche" , kDisable | kMagical | kProjectile , 0.f },
		{ "tiny_toss" , kDisable | kMagical | kInstant , 0.f },
		{ "slardar_slithereen_crush" , kDisable | kMagical , 350.f },
		{ "sandking_burrowstrike" , kDisable | kMagical | kProjectile , 0.f },
		{ "sandking_epicenter" , kMagical , 600.f },
		{ "storm_spirit_electric_vortex" , kDisable | kPierces | kInstant , 0.f },
		{ "morphling_adaptive_strike_str" , kProjectile | kDisable | kPhysical , 0.f },
		{ "ancient_apparition_ice_blast" , kProjectile | kMagical , 25000.f },
		{ "ancient_apparition_chilling_touch" , kMagical | kProjectile , 0.f },
		{ "disruptor_glimpse" , kDisable | kPierces | kInstant , 0.f },
		{ "grimstroke_soul_chain" , kDisable | kMagical | kInstant , 0.f },
		{ "rattletrap_hookshot" , kProjectile | kDisable | kMagical , 0.f },
		{ "batrider_flaming_lasso" , kDisable | kPierces | kInstant , 0.f },
		{ "mars_spear" , kProjectile | kDisable | kMagical , 0.f },
		{ "dragon_knight_dragon_tail" , kDisable | kMagical , 0.f },
		{ "elder_titan_echo_stomp" , kDisable | kMagical , 500.f },
		{ "abyssal_underlord_pit_of_malice" , kDisable | kMagical , 375.f },
		{ "obsidian_destroyer_astral_imprisonment" , kDisable | kMagical | kInstant , 0.f },
		{ "silencer_last_word" , kDisable | kMagical | kInstant , 0.f },
		{ "silencer_global_silence" , kDisable | kMagical | kInstant , 25000.f },
		{ "necrolyte_reapers_scythe" , kDisable | kMagical | kInstant , 0.f },
		{ "riki_smoke_screen" , kDisable | kMagical , 375.f },
		{ "shadow_demon_disruption" , kDisable | kPierces | kInstant , 0.f },
		{ "spirit_breaker_charge_of_darkness" , kProjectile | kDisable | kPierces , 25000.f },
		{ "spirit_breaker_nether_strike" , kDisable | kMagical | kInstant , 0.f },
		{ "primal_beast_onslaught" , kProjectile | kDisable | kMagical , 1100.f },
		{ "primal_beast_pulverize" , kDisable | kMagical | kInstant , 0.f },
		{ "hoodwink_sharpshooter" , kProjectile | kDisable | kPhysical , 3000.f },
		{ "marci_grapple" , kDisable | kMagical | kInstant , 0.f },
		{ "nevermore_requiem" , kMagical , 1000.f },
		{ "pugna_life_drain" , kMagical | kInstant , 0.f },
		{ "night_stalker_crippling_fear" , kDisable | kMagical , 400.f },
		{ "phantom_assassin_stifling_dagger" , kProjectile | kPhysical , 0.f },
		{ "queenofpain_sonic_wave" , kMagical | kProjectile , 0.f },
		{ "leshrac_split_earth" , kDisable | kMagical | kProjectile , 0.f },
		{ "warlock_upheaval" , kDisable | kMagical , 0.f },
		{ "medusa_stone_gaze" , kDisable | kMagical , 1200.f },
		// Enemy items - same detector, same answer.
		{ "item_sheepstick" , kDisable | kMagical | kInstant , 0.f },
		{ "item_rod_of_atos" , kDisable | kProjectile | kMagical , 0.f },
		{ "item_gungir" , kDisable | kProjectile | kMagical , 0.f },
		{ "item_abyssal_blade" , kDisable | kPhysical | kPierces | kInstant , 0.f },
		{ "item_bloodthorn" , kDisable | kMagical | kInstant , 0.f },
		{ "item_orchid" , kDisable | kMagical | kInstant , 0.f },
		{ "item_nullifier" , kDisable | kMagical | kInstant , 0.f },
		{ "item_ethereal_blade" , kMagical | kInstant , 0.f },
		{ "item_dagon" , kMagical | kInstant , 0.f },
		{ "item_dagon_2" , kMagical | kInstant , 0.f },
		{ "item_dagon_3" , kMagical | kInstant , 0.f },
		{ "item_dagon_4" , kMagical | kInstant , 0.f },
		{ "item_dagon_5" , kMagical | kInstant , 0.f },
		{ "item_cyclone" , kDisable | kMagical | kInstant , 0.f },
		{ "item_wind_waker" , kDisable | kMagical | kInstant , 0.f },
	};

	enum class ResponseKind : uint8_t
	{
		// Fire and forget - fastest possible answer, no cursor involved.
		NoTarget,
		// Unit-target cast aimed at our own hero (or at the ally being saved).
		UnitTarget,
		// Point-target cast aimed directly away from the caster.
		BlinkAway,
	};

	struct ResponseDef
	{
		const char* name;
		ResponseKind kind;
		// Threat flags this answers. Selection needs a non-empty intersection.
		uint8_t counters;
		// Lower goes first.
		int priority;
		// Can be cast on a threatened ally, not just ourselves.
		bool allyCapable;
		// A blink escape, gated by the "Escape With Blink" switch.
		bool isBlink;
		// Only stops magic - never chosen against pure/physical damage or a
		// spell-immunity-piercing disable.
		bool magicOnly;
		// A hero ability rather than an item (gated by "Use Abilities").
		bool isAbility;
		// Works by moving the hero or breaking the caster's targeting. Useless
		// against a kInstant threat, which has already landed on you by the
		// time anything moves.
		bool positional;
	};

	constexpr ResponseDef kResponses[] =
	{
		{ "item_black_king_bar" , ResponseKind::NoTarget , kMagical | kDisable , 10 , false , false , true , false , false },
		{ "item_cyclone" , ResponseKind::UnitTarget , kMagical | kPhysical | kPure | kDisable | kProjectile , 20 , true , false , false , false , false },
		{ "item_wind_waker" , ResponseKind::UnitTarget , kMagical | kPhysical | kPure | kDisable | kProjectile , 21 , true , false , false , false , false },
		{ "item_manta" , ResponseKind::NoTarget , kProjectile | kDisable , 30 , false , false , false , false , true },
		{ "item_blink" , ResponseKind::BlinkAway , kProjectile | kDisable | kMagical | kPhysical | kPure , 40 , false , true , false , false , true },
		{ "item_swift_blink" , ResponseKind::BlinkAway , kProjectile | kDisable | kMagical | kPhysical | kPure , 41 , false , true , false , false , true },
		{ "item_arcane_blink" , ResponseKind::BlinkAway , kProjectile | kDisable | kMagical | kPhysical | kPure , 42 , false , true , false , false , true },
		{ "item_overwhelming_blink" , ResponseKind::BlinkAway , kProjectile | kDisable | kMagical | kPhysical | kPure , 43 , false , true , false , false , true },
		{ "item_lotus_orb" , ResponseKind::UnitTarget , kDisable | kMagical , 50 , true , false , false , false , false },
		{ "item_force_staff" , ResponseKind::UnitTarget , kProjectile | kDisable , 60 , true , false , false , false , true },
		{ "item_hurricane_pike" , ResponseKind::UnitTarget , kProjectile | kDisable , 61 , true , false , false , false , true },
		{ "item_glimmer_cape" , ResponseKind::UnitTarget , kProjectile | kMagical , 70 , true , false , false , false , true },
		{ "item_ghost" , ResponseKind::NoTarget , kPhysical , 80 , false , false , false , false , false },
		// Spell blocks and phase-outs: these beat even an instant targeted
		// nuke, which is what makes Anti-Mage's Counterspell the right answer
		// to a Finger of Death and nothing else in this list is.
		{ "antimage_counterspell" , ResponseKind::NoTarget , kMagical | kDisable , 11 , false , false , true , true , false },
		{ "antimage_counterspell_ally" , ResponseKind::NoTarget , kMagical | kDisable , 11 , false , false , true , true , false },
		{ "nyx_assassin_spiked_carapace" , ResponseKind::NoTarget , kMagical | kPhysical | kDisable , 17 , false , false , false , true , false },
		{ "templar_assassin_refraction" , ResponseKind::NoTarget , kMagical | kPhysical | kPure , 18 , false , false , false , true , false },
		{ "void_spirit_dissimilate" , ResponseKind::NoTarget , kMagical | kPhysical | kPure | kDisable | kProjectile , 19 , false , false , false , true , true },
		{ "dark_willow_shadow_realm" , ResponseKind::NoTarget , kMagical | kPhysical | kPure | kDisable | kProjectile , 22 , false , false , false , true , true },
		// Self-preservation abilities that need no target and no aim.
		{ "puck_phase_shift" , ResponseKind::NoTarget , kMagical | kPhysical | kPure | kDisable | kProjectile , 15 , false , false , false , true , false },
		{ "juggernaut_blade_fury" , ResponseKind::NoTarget , kMagical , 16 , false , false , true , true , false },
		{ "weaver_shukuchi" , ResponseKind::NoTarget , kProjectile | kPhysical , 25 , false , false , false , true , true },
		{ "life_stealer_rage" , ResponseKind::NoTarget , kMagical | kDisable , 26 , false , false , true , true , false },
		{ "phantom_lancer_doppelwalk" , ResponseKind::NoTarget , kProjectile | kDisable , 27 , false , false , false , true , true },
		{ "slark_dark_pact" , ResponseKind::NoTarget , kDisable | kMagical , 28 , false , false , false , true , false },
		{ "windrunner_windrun" , ResponseKind::NoTarget , kPhysical | kProjectile , 29 , false , false , false , true , false },
		{ "omniknight_guardian_angel" , ResponseKind::NoTarget , kPhysical , 31 , false , false , false , true , false },
		{ "antimage_blink" , ResponseKind::BlinkAway , kProjectile | kDisable | kMagical | kPhysical | kPure , 44 , false , true , false , true , true },
		{ "queenofpain_blink" , ResponseKind::BlinkAway , kProjectile | kDisable | kMagical | kPhysical | kPure , 45 , false , true , false , true , true },
		// Ally saves (all also work on ourselves).
		{ "dazzle_shallow_grave" , ResponseKind::UnitTarget , kMagical | kPhysical | kPure | kDisable , 12 , true , false , false , true , false },
		{ "oracle_false_promise" , ResponseKind::UnitTarget , kMagical | kPhysical | kPure | kDisable , 13 , true , false , false , true , false },
		{ "oracle_fates_edict" , ResponseKind::UnitTarget , kMagical , 13 , true , false , true , true , false },
		{ "abaddon_aphotic_shield" , ResponseKind::UnitTarget , kMagical | kPhysical | kDisable , 14 , true , false , false , true , false },
		{ "winter_wyvern_cold_embrace" , ResponseKind::UnitTarget , kPhysical , 14 , true , false , false , true , false },
	};

	// Per-spell answers, best first, for the cases where the generic flag
	// matching would pick something technically eligible but worse. These are
	// the "this spell, that counter" pairings:
	//   - Lion's Finger of Death is a targeted magical nuke, so Anti-Mage's
	//     Counterspell blocks it outright.
	//   - Magnus's Reverse Polarity is answered with Manta Style.
	//   - Zeus's global ult is ridden out inside Eul's.
	// A preferred counter still has to be off cooldown, affordable and (for an
	// ally save) in range - if none of the list is usable, selection falls
	// back to the generic flag matching in SelectResponse.
	struct CounterPreference
	{
		const char* threat;
		const char* counters[5];
	};

	constexpr CounterPreference kPreferredCounters[] =
	{
		{ "lion_finger_of_death" , { "antimage_counterspell" , "antimage_counterspell_ally" , "item_lotus_orb" , "item_black_king_bar" , "item_cyclone" } },
		{ "lina_laguna_blade" , { "antimage_counterspell" , "antimage_counterspell_ally" , "item_lotus_orb" , "item_black_king_bar" , "item_cyclone" } },
		{ "necrolyte_reapers_scythe" , { "antimage_counterspell" , "item_lotus_orb" , "item_black_king_bar" , "item_cyclone" , nullptr } },
		{ "doom_bringer_doom" , { "antimage_counterspell" , "item_lotus_orb" , "item_cyclone" , "item_wind_waker" , nullptr } },
		{ "legion_commander_duel" , { "antimage_counterspell" , "item_lotus_orb" , "item_cyclone" , nullptr , nullptr } },
		{ "item_sheepstick" , { "antimage_counterspell" , "item_lotus_orb" , "item_black_king_bar" , "item_cyclone" , nullptr } },
		{ "magnataur_reverse_polarity" , { "item_manta" , "item_black_king_bar" , "item_cyclone" , "item_blink" , nullptr } },
		{ "zuus_thundergods_wrath" , { "item_cyclone" , "item_wind_waker" , "item_black_king_bar" , nullptr , nullptr } },
		{ "faceless_void_chronosphere" , { "item_blink" , "item_cyclone" , "item_manta" , nullptr , nullptr } },
		{ "axe_berserkers_call" , { "item_blink" , "item_cyclone" , "item_manta" , nullptr , nullptr } },
		{ "tidehunter_ravage" , { "item_blink" , "item_black_king_bar" , "item_cyclone" , "item_manta" , nullptr } },
		{ "enigma_black_hole" , { "item_blink" , "item_black_king_bar" , "item_cyclone" , nullptr , nullptr } },
		{ "pudge_meat_hook" , { "item_cyclone" , "item_blink" , "item_manta" , nullptr , nullptr } },
		{ "mirana_arrow" , { "item_blink" , "item_cyclone" , "item_manta" , nullptr , nullptr } },
	};

	struct HeroSnapshot
	{
		C_BaseEntity* entity = nullptr;
		int entIndex = -1;
		std::string name;
		Vector3 origin{};
		uint8_t team = 0;
		int health = 0;
		int maxHealth = 0;
		float mana = 0.f;
	};

	struct CastEvent
	{
		C_BaseEntity* ability = nullptr;
		std::string name;
		int level = 1;
	};

	struct ReadyResponse
	{
		const ResponseDef* def = nullptr;
		WORD key = 0;
		float castRange = 0.f;
	};

	auto FindDangerEntry( const std::string& loweredName ) -> const DangerEntry*
	{
		for ( const auto& entry : kDangerAbilities )
		{
			if ( loweredName == entry.name )
				return &entry;
		}
		return nullptr;
	}

	auto FindResponseDef( const std::string& loweredName ) -> const ResponseDef*
	{
		for ( const auto& entry : kResponses )
		{
			if ( loweredName == entry.name )
				return &entry;
		}
		return nullptr;
	}

	auto DamageTypeFlag( AbilityDamageType type ) -> uint8_t
	{
		switch ( type )
		{
		case AbilityDamageType::Physical: return kPhysical;
		case AbilityDamageType::Pure: return kPure;
		default: return kMagical;
		}
	}

	// 0 = harmless / not worth an item. Anything in the curated table above is
	// dangerous by definition; everything else has to clear the damage bar the
	// user set.
	auto ClassifyThreat( const std::string& loweredName , const AbilityDamageEntry* entry , int level , float& outReach ) -> uint8_t
	{
		outReach = 0.f;

		if ( const auto* danger = FindDangerEntry( loweredName ) )
		{
			outReach = danger->reach;
			return danger->flags;
		}

		if ( !entry || !entry->targetEnemy || !entry->IsUsableDamage() )
			return 0;
		if ( entry->DamageForLevel( level ) < Settings::Dodger::MinDangerDamage )
			return 0;

		uint8_t flags = DamageTypeFlag( entry->damageType );
		// A unit-targeted nuke almost always arrives as a projectile, so a
		// blink/Eul's genuinely removes it; a point/no-target one lands where
		// it was aimed.
		if ( entry->unitTarget )
			flags |= kProjectile;
		return flags;
	}

	auto ScanHeroes( CGameEntitySystem* entitySystem , const FS::UnitOffsets& offsets ) -> std::vector<HeroSnapshot>
	{
		std::vector<HeroSnapshot> heroes;
		if ( !entitySystem )
			return heroes;

		heroes.reserve( 12 );
		// Identity-chunk walk rather than an index-bounded loop: see
		// CKillStealer::ScanHeroes - GetHighestEntityIndex() is build-dependent
		// and has been observed missing live heroes for a whole session.
		for ( int chunkIndex = 0; chunkIndex < MAX_ENTITY_LISTS; ++chunkIndex )
		{
			auto* chunk = entitySystem->m_pIdentityChunks[chunkIndex];
			if ( !chunk )
				continue;

			for ( int entryIndex = 0; entryIndex < MAX_ENTITIES_IN_LIST; ++entryIndex )
			{
				auto& identity = chunk->m_pIdentities[entryIndex];
				auto* entity = identity.pBaseEntity();
				if ( !entity )
					continue;

				const int health = FS::ReadField<int>( entity , offsets.health , 0 );
				if ( health <= 0 )
					continue;

				const uint8_t team = FS::ReadField<uint8_t>( entity , offsets.team , 0 );
				if ( !FS::IsPlayableTeam( team ) )
					continue;

				if ( offsets.hasIsIllusion && FS::ReadField<bool>( entity , offsets.isIllusion , false ) )
					continue;
				if ( offsets.hasWaitingToSpawn && FS::ReadField<bool>( entity , offsets.waitingToSpawn , false ) )
					continue;

				const int entIndex = chunkIndex * MAX_ENTITIES_IN_LIST + entryIndex;
				// The identity is already in hand from the chunk, so the name
				// costs nothing extra - no re-lookup, and no VirtualQuery per
				// creep on the map (every lane creep also has health and a
				// playable team, so this loop sees all of them).
				const std::string name = FS::EntityName( entity , &identity );
				if ( !FS::LooksLikeHeroEntity( entity , name ) )
					continue;

				Vector3 origin{};
				if ( !FS::TryReadOrigin( entity , offsets , origin ) )
					continue;
				// Exact (0,0,0) is the never-replicated placeholder, and it
				// sits near mid - an unseen hero parked there would otherwise
				// read as standing right next to whoever is fighting there.
				if ( origin.m_x == 0.f && origin.m_y == 0.f && origin.m_z == 0.f )
					continue;

				HeroSnapshot snapshot{};
				snapshot.entity = entity;
				snapshot.entIndex = entIndex;
				snapshot.name = name;
				snapshot.origin = origin;
				snapshot.team = team;
				snapshot.health = health;
				snapshot.maxHealth = FS::ReadField<int>( entity , offsets.maxHealth , 0 );
				snapshot.mana = FS::ReadField<float>( entity , offsets.mana , 0.f );
				heroes.push_back( std::move( snapshot ) );
			}
		}

		return heroes;
	}

	auto CollectResponses( CGameEntitySystem* entitySystem , const FS::UnitOffsets& offsets , const HeroSnapshot& local ) -> std::vector<ReadyResponse>
	{
		std::vector<ReadyResponse> ready;
		static constexpr std::array<WORD , 6> kItemKeys = { 'Z' , 'X' , 'C' , 'V' , 'B' , 'N' };
		static constexpr std::array<WORD , 6> kAbilityKeys = { 'Q' , 'W' , 'E' , 'D' , 'F' , 'R' };

		auto Usable = [&]( C_BaseEntity* castable , const AbilityDamageEntry* entry , int level ) -> bool
		{
			const float cooldown = FS::ReadField<float>( castable , offsets.abilityCooldown , 0.f );
			if ( std::isfinite( cooldown ) && cooldown > 0.15f )
				return false;

			int manaCost = FS::ReadField<int>( castable , offsets.abilityManaCost , 0 );
			if ( manaCost <= 0 && entry )
				manaCost = entry->ManaForLevel( level );
			return manaCost <= static_cast<int>( local.mana + 0.5f );
		};

		if ( Settings::Dodger::UseItems )
		{
			std::vector<CHandle> itemHandles;
			if ( FS::ReadInventoryHandles( local.entity , offsets , itemHandles ) )
			{
				const int slotLimit = (std::min)( static_cast<int>( itemHandles.size() ) , static_cast<int>( kItemKeys.size() ) );
				for ( int slot = 0; slot < slotLimit; ++slot )
				{
					CEntityIdentity* identity = nullptr;
					auto* item = FS::EntityFromHandle( entitySystem , itemHandles[slot] , &identity );
					if ( !item )
						continue;

					const std::string itemName = FS::ToLower( FS::EntityName( item , identity ) );
					const auto* def = FindResponseDef( itemName );
					if ( !def || def->isAbility )
						continue;

					const auto* entry = FS::FindAbilityEntry( itemName );
					const int level = (std::max)( 1 , FS::ReadField<int>( item , offsets.abilityLevel , 1 ) );
					if ( !Usable( item , entry , level ) )
						continue;

					ReadyResponse response{};
					response.def = def;
					response.key = kItemKeys[slot];
					response.castRange = FS::ReadCastRange( item , offsets , entry ? entry->castRange : 0.f );
					ready.push_back( response );
				}
			}
		}

		if ( Settings::Dodger::UseAbilities )
		{
			std::vector<CHandle> abilityHandles;
			if ( FS::ReadAbilityHandles( local.entity , offsets , abilityHandles ) )
			{
				int fallbackSlot = 0;
				for ( const auto& handle : abilityHandles )
				{
					CEntityIdentity* identity = nullptr;
					auto* ability = FS::EntityFromHandle( entitySystem , handle , &identity );
					if ( !ability )
						continue;

					const std::string abilityName = FS::ToLower( FS::EntityName( ability , identity ) );
					if ( abilityName.empty() || abilityName.rfind( "special_bonus_" , 0 ) == 0 )
						continue;

					int slot = FS::PreferredSlotForAbility( abilityName );
					if ( slot < 0 || slot >= static_cast<int>( kAbilityKeys.size() ) )
						slot = fallbackSlot;
					++fallbackSlot;

					const auto* def = FindResponseDef( abilityName );
					if ( !def || !def->isAbility )
						continue;
					if ( slot < 0 || slot >= static_cast<int>( kAbilityKeys.size() ) )
						continue;

					const int level = FS::ReadField<int>( ability , offsets.abilityLevel , 0 );
					if ( level <= 0 )
						continue;
					if ( offsets.hasAbilityActivated && !FS::ReadField<bool>( ability , offsets.abilityActivated , true ) )
						continue;

					const auto* entry = FS::FindAbilityEntry( abilityName );
					if ( !Usable( ability , entry , level ) )
						continue;

					ReadyResponse response{};
					response.def = def;
					response.key = kAbilityKeys[slot];
					response.castRange = FS::ReadCastRange( ability , offsets , entry ? entry->castRange : 0.f );
					ready.push_back( response );
				}
			}
		}

		return ready;
	}

	// Checks everything that is true of a usable answer regardless of WHY it
	// was picked (explicit preference or flag match): the user's switches, who
	// it can be cast on, and whether moving the hero can help at all.
	auto ResponseIsApplicable( const ReadyResponse& candidate , uint8_t threatFlags , bool againstAlly , float allyDistance ) -> bool
	{
		const auto* def = candidate.def;
		if ( def->isBlink && !Settings::Dodger::UseBlink )
			return false;
		// Blinking away or breaking targeting does nothing about a spell that
		// resolves on you the moment it is cast.
		if ( def->positional && ( threatFlags & kInstant ) != 0 )
			return false;
		if ( againstAlly )
		{
			if ( !def->allyCapable )
				return false;
			const float range = candidate.castRange > 0.f ? candidate.castRange : kAllySaveRange;
			if ( allyDistance > range + 50.f )
				return false;
		}
		return true;
	}

	// The explicit "this spell, that counter" table. Returns nullptr when the
	// threat has no entry, or when nothing on its list is usable right now.
	// Linken's Sphere is passive, so it never shows up as a response - but
	// whether it is ready decides if a response is needed at all.
	auto HasReadyLinkens( CGameEntitySystem* entitySystem , const FS::UnitOffsets& offsets , C_BaseEntity* localEntity ) -> bool
	{
		std::vector<CHandle> itemHandles;
		if ( !FS::ReadInventoryHandles( localEntity , offsets , itemHandles ) )
			return false;

		const int slotLimit = (std::min)( static_cast<int>( itemHandles.size() ) , 6 );
		for ( int slot = 0; slot < slotLimit; ++slot )
		{
			CEntityIdentity* identity = nullptr;
			auto* item = FS::EntityFromHandle( entitySystem , itemHandles[slot] , &identity );
			if ( !item )
				continue;
			if ( FS::ToLower( FS::EntityName( item , identity ) ) != "item_sphere" )
				continue;

			const float cooldown = FS::ReadField<float>( item , offsets.abilityCooldown , 0.f );
			return !std::isfinite( cooldown ) || cooldown <= 0.15f;
		}
		return false;
	}

	auto SelectPreferredResponse( const std::vector<ReadyResponse>& ready , const std::string& threatName ,
		uint8_t threatFlags , bool againstAlly , float allyDistance ) -> const ReadyResponse*
	{
		const CounterPreference* preference = nullptr;
		for ( const auto& entry : kPreferredCounters )
		{
			if ( threatName == entry.threat )
			{
				preference = &entry;
				break;
			}
		}
		if ( !preference )
			return nullptr;

		for ( const char* wanted : preference->counters )
		{
			if ( !wanted )
				continue;
			for ( const auto& candidate : ready )
			{
				if ( std::strcmp( candidate.def->name , wanted ) != 0 )
					continue;
				if ( !ResponseIsApplicable( candidate , threatFlags , againstAlly , allyDistance ) )
					continue;
				return &candidate;
			}
		}
		return nullptr;
	}

	auto SelectResponse( const std::vector<ReadyResponse>& ready , uint8_t threatFlags , bool againstAlly , float allyDistance ) -> const ReadyResponse*
	{
		const ReadyResponse* best = nullptr;
		for ( const auto& candidate : ready )
		{
			const auto* def = candidate.def;
			if ( ( def->counters & threatFlags ) == 0 )
				continue;
			if ( !ResponseIsApplicable( candidate , threatFlags , againstAlly , allyDistance ) )
				continue;
			// A BKB (or any magic-only answer) does nothing about pure/physical
			// damage, and nothing at all about a disable that pierces spell
			// immunity - Hook, Duel, Chronosphere, Doom.
			if ( def->magicOnly && ( threatFlags & ( kPure | kPhysical | kPierces ) ) != 0 )
				continue;
			if ( !best || def->priority < best->def->priority )
				best = &candidate;
		}
		return best;
	}

	// A skillshot only threatens what the caster is pointed at. Dota turns the
	// hero to face the cast direction BEFORE the cast point begins, so the
	// facing we read at detection time already is the direction the spell will
	// travel - which is what keeps a Hook thrown at someone else from burning
	// our BKB. Unit-targeted projectiles (they home) and anything whose
	// targeting we can't look up are never gated by this.
	auto CasterFacesVictim( const Vector3& caster , float casterYaw , const Vector3& victim ) -> bool
	{
		const float dx = victim.m_x - caster.m_x;
		const float dy = victim.m_y - caster.m_y;
		if ( std::fabs( dx ) < 1.f && std::fabs( dy ) < 1.f )
			return true;

		const float toVictim = RAD2DEG( std::atan2( dy , dx ) );
		constexpr float kFacingToleranceDegrees = 50.f;
		return std::fabs( Math::AngleNormalize( toVictim - casterYaw ) ) <= kFacingToleranceDegrees;
	}

	auto BlinkEscapePoint( const Vector3& local , const Vector3& caster , float range ) -> Vector3
	{
		float dx = local.m_x - caster.m_x;
		float dy = local.m_y - caster.m_y;
		const float length = std::sqrt( dx * dx + dy * dy );
		if ( !std::isfinite( length ) || length < 1.f )
		{
			dx = 1.f;
			dy = 0.f;
		}
		else
		{
			dx /= length;
			dy /= length;
		}

		// Stay inside the item's real range - an over-range blink lands short,
		// which on a Blink Dagger is only ~960 units and may not clear the AoE.
		const float distance = std::clamp( range - 50.f , 200.f , 1150.f );
		Vector3 target = local;
		target.m_x += dx * distance;
		target.m_y += dy * distance;
		return target;
	}
}

auto CDodger::CancelCast( const char* reason ) -> void
{
	if ( m_Cast.hasPrevCursor )
		SetCursorPos( m_Cast.prevCursorX , m_Cast.prevCursorY );
	m_Cast = {};
	if ( reason )
	{
		m_Status = reason;
		DEV_LOG( "[dodger] cast cancelled: %s\n" , reason );
	}
}

auto CDodger::AdvanceCast( uint32_t now ) -> void
{
	if ( !m_Cast.active )
		return;

	if ( now >= m_Cast.expiresAt )
	{
		CancelCast( "Cast timed out" );
		return;
	}
	if ( now < m_Cast.nextTick )
		return;

	const HWND window = FS::WindowReadyForInput();
	if ( !window || m_Cast.key == 0 )
	{
		CancelCast( "Game not focused" );
		return;
	}

	if ( !m_Cast.needsAim )
	{
		// Item and ability hotkeys apply to the CURRENT selection, so a hero
		// left unselected by an earlier click means the BKB press does nothing
		// at all. One extra key event, no measurable latency.
		FS::SendKeyPress( VK_F1 );
		if ( !FS::SendKeyPress( m_Cast.key ) )
		{
			CancelCast( "Cast failed" );
			return;
		}
		const std::string name = m_Cast.name;
		m_Cast = {};
		m_NextDodgeTick = now + kDodgeCooldownMs;
		m_Status = "Used " + name;
		DEV_LOG( "[dodger] fired no-target response %s\n" , name.c_str() );
		return;
	}

	switch ( m_Cast.phase )
	{
	case CastPhase::Aim:
	{
		// Re-select our own hero first: any earlier click may have left an
		// enemy unit selected, and then every hotkey applies to nothing.
		FS::SendKeyPress( VK_F1 );

		POINT previous{};
		GetCursorPos( &previous );
		if ( !FS::AimCursorAtWorld( window , m_Cast.aimPoint , m_Cast.groundAim ) )
		{
			CancelCast( "Aim failed" );
			break;
		}
		m_Cast.prevCursorX = previous.x;
		m_Cast.prevCursorY = previous.y;
		m_Cast.hasPrevCursor = true;
		m_Cast.phase = CastPhase::Cast;
		m_Cast.nextTick = now + kCastSettleMs;
		break;
	}
	case CastPhase::Cast:
	{
		FS::AimCursorAtWorld( window , m_Cast.aimPoint , m_Cast.groundAim );
		if ( !FS::SendKeyPress( m_Cast.key ) || ( m_Cast.needsClick && !FS::SendLeftClick() ) )
		{
			CancelCast( "Cast failed" );
			break;
		}
		m_Status = "Used " + m_Cast.name;
		DEV_LOG( "[dodger] fired targeted response %s\n" , m_Cast.name.c_str() );
		m_Cast.phase = CastPhase::Restore;
		m_Cast.nextTick = now + kCastSettleMs;
		break;
	}
	case CastPhase::Restore:
	{
		if ( m_Cast.hasPrevCursor )
		{
			SetCursorPos( m_Cast.prevCursorX , m_Cast.prevCursorY );
			m_Cast.hasPrevCursor = false;
		}
		// The confirming click landed on a unit - our own hero for a self
		// cast, the ally for a save. A click on an ally SELECTS the ally, and
		// every hotkey the player presses next would go to it, so hand the
		// selection back before getting out of the way.
		FS::SendKeyPress( VK_F1 );
		const std::string name = m_Cast.name;
		m_Cast = {};
		m_NextDodgeTick = now + kDodgeCooldownMs;
		m_Status = "Used " + name;
		break;
	}
	}
}

auto CDodger::DrawThreatMarker() const -> void
{
	if ( !Settings::Dodger::DrawThreatMarkers || m_Threat.abilityName.empty() )
		return;
	if ( GetTickCount() >= m_Threat.expiresAt )
		return;

	ImVec2 casterScreen{};
	Vector3 above = m_Threat.casterOrigin;
	above.m_z += 160.f;
	if ( !Math::WorldToScreen( above , casterScreen ) )
		return;

	auto* drawList = ImGui::GetForegroundDrawList();
	const ImU32 color = m_Threat.againstAlly ? IM_COL32( 255 , 190 , 60 , 235 ) : IM_COL32( 235 , 70 , 80 , 235 );
	drawList->AddCircle( casterScreen , 15.f , color , 20 , 2.f );

	const std::string label = ( m_Threat.againstAlly ? "SAVE: " : "DODGE: " ) + m_Threat.abilityName;
	const ImVec2 textSize = ImGui::CalcTextSize( label.c_str() );
	const ImVec2 textPos( casterScreen.x - textSize.x * 0.5f , casterScreen.y - 34.f );
	drawList->AddRectFilled( ImVec2( textPos.x - 4.f , textPos.y - 2.f ) ,
		ImVec2( textPos.x + textSize.x + 4.f , textPos.y + textSize.y + 2.f ) , IM_COL32( 10 , 11 , 13 , 200 ) , 3.f );
	drawList->AddText( textPos , color , label.c_str() );

	ImVec2 victimScreen{};
	if ( Math::WorldToScreen( m_Threat.victimOrigin , victimScreen ) )
		drawList->AddLine( casterScreen , victimScreen , color , 1.5f );
}

auto CDodger::OnRender() -> void
{
	const uint32_t now = GetTickCount();

	if ( !Settings::Dodger::Enable )
	{
		if ( m_Cast.active )
			CancelCast();
		if ( !m_Watches.empty() )
			m_Watches.clear();
		if ( !m_HeroPositions.empty() )
			m_HeroPositions.clear();
		m_Threat = {};
		m_Status = "Disabled";
		return;
	}

	// Drawing and the in-flight cast run every frame - throttling either one
	// costs exactly the milliseconds this feature exists to save.
	DrawThreatMarker();
	AdvanceCast( now );

	if ( now < m_NextThinkTick )
		return;
	m_NextThinkTick = now + kThinkIntervalMs;

	const auto& offsets = FS::ResolveOffsets();
	auto* entitySystem = SDK::Interfaces::GameEntitySystem();
	if ( !entitySystem || !offsets.resolved )
	{
		m_Status = "Waiting for offsets";
		return;
	}

	// Which detector is actually live matters when reading a debug log: without
	// m_bInAbilityPhase the feature still works, but it only ever sees a cast
	// at the moment the ability goes on cooldown, i.e. a cast point later.
	static bool loggedDetectors = false;
	if ( !loggedDetectors )
	{
		loggedDetectors = true;

		// A counter named in kPreferredCounters that is not in kResponses can
		// never be selected - the pairing just silently does nothing, which is
		// exactly the kind of edit that looks correct in a diff. Say so once
		// instead of leaving it to be discovered in a match.
		for ( const auto& preference : kPreferredCounters )
		{
			for ( const char* wanted : preference.counters )
			{
				if ( !wanted )
					continue;
				const bool known = std::any_of( std::begin( kResponses ) , std::end( kResponses ) ,
					[&]( const ResponseDef& def ) { return std::strcmp( def.name , wanted ) == 0; } );
				if ( !known )
					DEV_LOG( "[dodger] BUG: preferred counter %s for %s is not in kResponses\n" , wanted , preference.threat );
			}
		}

		DEV_LOG( "[dodger] detectors: cast-phase=%s cooldown-edge=yes facing-gate=%s cast-range=%s state-gates=%d%d%d\n" ,
			offsets.hasAbilityInPhase ? "yes" : "NO (m_bInAbilityPhase unresolved)" ,
			offsets.hasRotation ? "yes" : "no" ,
			offsets.abilityCastRange ? "live" : "catalog-only" ,
			offsets.hasMagicImmune ? 1 : 0 , offsets.hasInvulnerable ? 1 : 0 , offsets.hasStunned ? 1 : 0 );
	}

	C_BaseEntity* localEntity = nullptr;
	int localEntIndex = -1;
	if ( !CLocalHeroResolver::Resolve( entitySystem , localEntity , localEntIndex ) )
	{
		m_Status = "Local hero unresolved";
		return;
	}

	const auto heroes = ScanHeroes( entitySystem , offsets );
	const HeroSnapshot* local = nullptr;
	for ( const auto& hero : heroes )
	{
		if ( hero.entity == localEntity )
		{
			local = &hero;
			break;
		}
	}
	if ( !local )
	{
		// ScanHeroes drops dead and pre-spawn heroes, so the common reason to
		// land here is simply being dead - say that rather than something that
		// reads like a resolver failure.
		m_Status = FS::ReadField<int>( localEntity , offsets.health , 0 ) <= 0 ? "Hero is dead" : "Local hero not in scan";
		return;
	}

	if ( now >= m_NextPruneTick )
	{
		m_NextPruneTick = now + 5000;
		for ( auto it = m_Watches.begin(); it != m_Watches.end(); )
			it = ( now - it->second.lastSeenTick > kWatchExpiryMs ) ? m_Watches.erase( it ) : std::next( it );
		for ( auto it = m_HeroPositions.begin(); it != m_HeroPositions.end(); )
			it = ( now - it->second.tick > kWatchExpiryMs ) ? m_HeroPositions.erase( it ) : std::next( it );
	}

	// Nothing we press lands while the hero is stunned/hexed, and nothing
	// needs pressing while they are invulnerable - spending the escape item
	// there is strictly worse than keeping it.
	//
	// This deliberately does NOT return early. The watch loop below has to keep
	// sampling cooldowns even while we cannot act: skipping it for the two
	// seconds of a stun would leave every enemy ability holding a two-second-old
	// sample, and the first tick afterwards would read that as a fresh cast and
	// dodge a spell that already resolved.
	const bool stunned = offsets.hasStunned && FS::ReadField<bool>( local->entity , offsets.stunned , false );
	const bool invulnerable = offsets.hasInvulnerable && FS::ReadField<bool>( local->entity , offsets.invulnerable , false );
	const bool canAct = !stunned && !invulnerable;
	if ( !canAct )
		m_Status = stunned ? "Stunned - cannot act" : "Invulnerable";
	// Already under a BKB: only damage/disables that go through spell immunity
	// are still worth answering.
	const bool alreadyImmune = offsets.hasMagicImmune &&
		FS::ReadField<bool>( local->entity , offsets.magicImmune , false );

	const float triggerRange = Settings::Dodger::TriggerRange;
	bool threatHandled = false;

	// Resolved at most once per think tick, and only if some threat is
	// actually single-target - the inventory walk is not worth doing for the
	// ticks where nothing is being cast at us.
	bool linkensChecked = false;
	bool linkensReady = false;
	auto LinkensWillBlock = [&]( const AbilityDamageEntry* entry ) -> bool
	{
		if ( !entry || !entry->unitTarget )
			return false;
		if ( !linkensChecked )
		{
			linkensChecked = true;
			linkensReady = HasReadyLinkens( entitySystem , offsets , local->entity );
		}
		return linkensReady;
	};

	// Registers the threat (for the marker) and, if we are not already busy or
	// on the post-dodge hold, picks and starts the counter. ally == nullptr
	// means the threat is against us.
	auto ArmResponse = [&]( const std::string& threatName , const HeroSnapshot& caster ,
		const HeroSnapshot* ally , uint8_t flags , float allyDistance ) -> void
	{
		const bool againstAlly = ally != nullptr;

		m_Threat = {};
		m_Threat.abilityName = threatName;
		m_Threat.casterName = caster.name;
		m_Threat.casterOrigin = caster.origin;
		m_Threat.victimOrigin = againstAlly ? ally->origin : local->origin;
		m_Threat.againstAlly = againstAlly;
		m_Threat.flags = flags;
		m_Threat.detectedTick = now;
		m_Threat.expiresAt = now + kThreatMarkerMs;

		// The marker above is still worth drawing while we cannot answer - it
		// is the difference between "the dodger missed this" and "the dodger
		// saw it and had nothing to do about it".
		if ( !canAct || m_Cast.active || now < m_NextDodgeTick )
			return;

		const auto ready = CollectResponses( entitySystem , offsets , *local );
		// The explicit per-spell answer wins when we own it; the flag matcher
		// is the fallback for everything not in that table.
		const auto* chosen = SelectPreferredResponse( ready , threatName , flags , againstAlly , allyDistance );
		const bool fromPreference = chosen != nullptr;
		if ( !chosen )
			chosen = SelectResponse( ready , flags , againstAlly , allyDistance );
		if ( !chosen )
		{
			m_Status = "No counter for " + threatName;
			DEV_LOG( "[dodger] %s: %s (flags=%u) - nothing available to answer it\n" ,
				caster.name.c_str() , threatName.c_str() , static_cast<unsigned>( flags ) );
			return;
		}

		m_Cast = {};
		m_Cast.active = true;
		m_Cast.key = chosen->key;
		m_Cast.name = chosen->def->name;
		m_Cast.startTick = now;
		m_Cast.nextTick = now + static_cast<uint32_t>( (std::max)( 0.f , Settings::Dodger::ReactionDelayMs ) );
		m_Cast.expiresAt = m_Cast.nextTick + kCastExpiryMs;
		m_Cast.phase = CastPhase::Aim;

		switch ( chosen->def->kind )
		{
		case ResponseKind::NoTarget:
			m_Cast.needsAim = false;
			break;
		case ResponseKind::UnitTarget:
			m_Cast.needsAim = true;
			m_Cast.groundAim = false;
			m_Cast.needsClick = !Settings::Dodger::QuickCast;
			m_Cast.aimPoint = againstAlly ? ally->origin : local->origin;
			break;
		case ResponseKind::BlinkAway:
			m_Cast.needsAim = true;
			m_Cast.groundAim = true;
			m_Cast.needsClick = !Settings::Dodger::QuickCast;
			m_Cast.aimPoint = BlinkEscapePoint( local->origin , caster.origin ,
				chosen->castRange > 0.f ? chosen->castRange : 1200.f );
			break;
		}

		m_Status = std::string( againstAlly ? "Saving ally from " : "Dodging " ) + threatName;
		DEV_LOG( "[dodger] %s: %s (flags=%u) - answering with %s (%s)\n" ,
			caster.name.c_str() , threatName.c_str() , static_cast<unsigned>( flags ) , m_Cast.name.c_str() ,
			fromPreference ? "known pair" : "flag match" );
	};

	for ( const auto& enemy : heroes )
	{
		if ( enemy.team == local->team || enemy.entity == local->entity )
			continue;
		// Deliberately NOT gated on distance. A global ult (Thundergod's
		// Wrath, Ice Blast, Assassinate) is cast from anywhere on the map and
		// still has to be answered, so skipping far-away casters here would
		// silently make those undodgeable. Cost is bounded anyway: this is at
		// most five heroes, each contributing a handful of field reads.

		// Blink initiation. No ability watch can see this coming: the blink
		// item's own cast tells us nothing about where it lands, and the
		// follow-up spell is only cast once the enemy is already on top of us.
		// What it does leave is a position jump no hero can walk - 500+ units
		// between two 30ms ticks - that ends next to us after starting well
		// away from us.
		auto& track = m_HeroPositions[enemy.entIndex];
		const Vector3 previousOrigin = track.origin;
		const uint32_t previousTick = track.tick;
		track.origin = enemy.origin;
		track.tick = now;

		if ( Settings::Dodger::DodgeEnemyBlink && Settings::Dodger::DodgeSelf && !threatHandled &&
			!alreadyImmune && previousTick != 0 && now - previousTick <= 300 )
		{
			const float jump = FS::Distance2D( previousOrigin , enemy.origin );
			const float distanceAfter = FS::Distance2D( enemy.origin , local->origin );
			const float distanceBefore = FS::Distance2D( previousOrigin , local->origin );
			// Requiring the jump to START far away is what keeps a hero
			// popping back into vision (their fogged position can snap) from
			// reading as an initiation - a real blink onto us always closes a
			// gap that was there a moment ago.
			if ( jump >= 500.f && distanceAfter <= kBlinkThreatRange && distanceBefore > kBlinkThreatRange + 400.f )
			{
				ArmResponse( "blink initiation" , enemy , nullptr , kMagical | kDisable | kProjectile , 0.f );
				threatHandled = true;
				break;
			}
		}

		std::vector<CastEvent> casts;
		auto WatchCastable = [&]( C_BaseEntity* castable , CEntityIdentity* identity , bool isItem )
		{
			if ( !castable )
				return;

			const std::string name = FS::ToLower( FS::EntityName( castable , identity ) );
			if ( name.empty() || name.rfind( "special_bonus_" , 0 ) == 0 )
				return;

			const int level = isItem
				? (std::max)( 1 , FS::ReadField<int>( castable , offsets.abilityLevel , 1 ) )
				: FS::ReadField<int>( castable , offsets.abilityLevel , 0 );
			if ( level <= 0 )
				return;

			const float cooldown = FS::ReadField<float>( castable , offsets.abilityCooldown , 0.f );
			const bool inPhase = offsets.hasAbilityInPhase &&
				FS::ReadField<bool>( castable , offsets.abilityInPhase , false );

			auto& watch = m_Watches[reinterpret_cast<uintptr_t>( castable )];
			const bool hadSample = watch.initialized;
			const float previousCooldown = watch.cooldown;
			const bool previousPhase = watch.inPhase;

			watch.cooldown = std::isfinite( cooldown ) ? cooldown : 0.f;
			watch.inPhase = inPhase;
			watch.initialized = true;
			watch.lastSeenTick = now;

			if ( !hadSample )
				return;

			// (1) cast point started, or (2) the ability just left the ready
			// state. Either way it is being cast right now.
			const bool phaseEdge = inPhase && !previousPhase;
			const bool cooldownEdge = !inPhase && watch.cooldown > 0.2f && previousCooldown <= 0.15f;
			if ( !phaseEdge && !cooldownEdge )
				return;
			// One cast, one trigger: both edges fire for the same cast, roughly
			// a cast point apart, and the second one must not re-arm a dodge.
			if ( watch.lastFiredTick != 0 && now - watch.lastFiredTick < kCastDedupeMs )
				return;
			watch.lastFiredTick = now;

			CastEvent event{};
			event.ability = castable;
			event.name = name;
			event.level = level;
			casts.push_back( std::move( event ) );
		};

		std::vector<CHandle> abilityHandles;
		if ( FS::ReadAbilityHandles( enemy.entity , offsets , abilityHandles ) )
		{
			for ( const auto& handle : abilityHandles )
			{
				CEntityIdentity* identity = nullptr;
				auto* ability = FS::EntityFromHandle( entitySystem , handle , &identity );
				WatchCastable( ability , identity , false );
			}
		}

		std::vector<CHandle> itemHandles;
		if ( FS::ReadInventoryHandles( enemy.entity , offsets , itemHandles ) )
		{
			const int slotLimit = (std::min)( static_cast<int>( itemHandles.size() ) , 6 );
			for ( int slot = 0; slot < slotLimit; ++slot )
			{
				CEntityIdentity* identity = nullptr;
				auto* item = FS::EntityFromHandle( entitySystem , itemHandles[slot] , &identity );
				WatchCastable( item , identity , true );
			}
		}

		for ( const auto& cast : casts )
		{
			float tableReach = 0.f;
			const auto* entry = FS::FindAbilityEntry( cast.name );
			const uint8_t flags = ClassifyThreat( cast.name , entry , cast.level , tableReach );
			if ( flags == 0 )
				continue;
			// A magical nuke or a normal disable is already handled by the
			// spell immunity we are standing in.
			if ( alreadyImmune && ( flags & ( kPure | kPhysical | kPierces ) ) == 0 )
				continue;

			float reach = FS::ReadCastRange( cast.ability , offsets , entry ? entry->castRange : 0.f );
			reach = (std::max)( reach , tableReach );
			if ( reach <= 0.f )
				reach = kDefaultNoTargetReach;
			reach = (std::min)( reach , 25000.f ) + kThreatMargin;

			// Skillshot = travels, but does not home on a unit. Those are the
			// only casts where the caster's facing tells us who is in danger.
			const bool isSkillshot = ( flags & kProjectile ) != 0 && entry && !entry->unitTarget;
			float casterYaw = 0.f;
			const bool hasFacing = isSkillshot && FS::TryReadYaw( enemy.entity , offsets , casterYaw );

			// Trigger Range exists to ignore casters too far away to matter,
			// but a global ult is exactly the case where distance is not the
			// point: Zeus hitting the whole map from his fountain has to be
			// answered the same as one cast next to us.
			const bool isGlobal = tableReach >= 5000.f;

			// A pure-damage threat is only worth an item if it actually takes a
			// meaningful bite out of the health we have right now - otherwise
			// every Zeus ult in the game would cost us a Eul's. Disables skip
			// this: being stunned at full health is the thing to avoid. The
			// damage here is the catalog's pre-mitigation number, so this is a
			// proportion check, not a lethality calculation.
			bool worthAnItem = true;
			if ( ( flags & kDisable ) == 0 && entry && entry->IsUsableDamage() && local->health > 0 )
			{
				const float damage = entry->DamageForLevel( cast.level );
				const float percentOfHealth = 100.f * damage / static_cast<float>( local->health );
				worthAnItem = percentOfHealth >= Settings::Dodger::MinThreatHealthPercent;
			}

			const float distanceToLocal = FS::Distance2D( enemy.origin , local->origin );
			const bool reachesLocal = Settings::Dodger::DodgeSelf && worthAnItem &&
				distanceToLocal <= reach && ( isGlobal || distanceToLocal <= triggerRange ) &&
				( !hasFacing || CasterFacesVictim( enemy.origin , casterYaw , local->origin ) );

			// A Linken's Sphere that is off cooldown eats the next single-target
			// spell aimed at us by itself. Spending a BKB or a Eul's on one it
			// is about to block is pure waste, so let it do its job. (It cannot
			// help against ground/AoE casts, hence the unitTarget test.)
			const bool threatensLocal = reachesLocal && !LinkensWillBlock( entry );
			if ( reachesLocal && !threatensLocal )
			{
				// The cast is spoken for: it is single-target, it was aimed at
				// us, and Linken's is about to eat it. Falling through to the
				// ally search below would answer a spell that is never going
				// to reach the ally either.
				DEV_LOG( "[dodger] %s cast %s - leaving it to Linken's Sphere\n" ,
					enemy.name.c_str() , cast.name.c_str() );
				continue;
			}

			const HeroSnapshot* ally = nullptr;
			float allyDistance = 0.f;
			if ( !threatensLocal && Settings::Dodger::SaveAllies )
			{
				for ( const auto& candidate : heroes )
				{
					if ( candidate.team != local->team || candidate.entity == local->entity || candidate.maxHealth <= 0 )
						continue;
					const float healthPercent = 100.f * static_cast<float>( candidate.health ) / static_cast<float>( candidate.maxHealth );
					if ( healthPercent > Settings::Dodger::AllySaveHealthPercent )
						continue;
					if ( FS::Distance2D( enemy.origin , candidate.origin ) > reach )
						continue;
					if ( hasFacing && !CasterFacesVictim( enemy.origin , casterYaw , candidate.origin ) )
						continue;
					const float distance = FS::Distance2D( local->origin , candidate.origin );
					if ( distance > kAllySaveRange )
						continue;
					if ( !ally || distance < allyDistance )
					{
						ally = &candidate;
						allyDistance = distance;
					}
				}
			}

			if ( !threatensLocal && !ally )
				continue;

			ArmResponse( cast.name , enemy , threatensLocal ? nullptr : ally , flags , allyDistance );
			threatHandled = true;
			break;
		}

		if ( threatHandled )
			break;
	}

	// Panic save. Nothing detectable was cast, but somebody is about to die
	// anyway - the burst already landed, a channel we could not classify is
	// draining them, or they are simply losing a fight. This is the half of
	// "save an ally" that works off health rather than off a specific spell,
	// and it is what makes the feature useful when the threat is not a single
	// identifiable cast.
	if ( Settings::Dodger::PanicSaveEnable && !threatHandled && !m_Cast.active &&
		now >= m_NextDodgeTick && now >= m_NextPanicTick )
	{
		// Dying with no enemy hero anywhere near is a tower or a creep wave,
		// not something an escape item is the answer to.
		constexpr float kPanicEnemyRange = 1200.f;
		constexpr uint8_t kPanicFlags = kMagical | kPhysical | kPure | kDisable;

		auto HealthPercent = []( const HeroSnapshot& hero ) -> float
		{
			return hero.maxHealth > 0 ? 100.f * static_cast<float>( hero.health ) / static_cast<float>( hero.maxHealth ) : 100.f;
		};

		auto NearestEnemyTo = [&]( const Vector3& point ) -> const HeroSnapshot*
		{
			const HeroSnapshot* best = nullptr;
			float bestDistance = kPanicEnemyRange;
			for ( const auto& candidate : heroes )
			{
				if ( candidate.team == local->team )
					continue;
				const float distance = FS::Distance2D( candidate.origin , point );
				if ( distance < bestDistance )
				{
					bestDistance = distance;
					best = &candidate;
				}
			}
			return best;
		};

		if ( Settings::Dodger::DodgeSelf && HealthPercent( *local ) <= Settings::Dodger::PanicHealthPercent )
		{
			if ( const auto* source = NearestEnemyTo( local->origin ) )
			{
				ArmResponse( "critical health" , *source , nullptr , kPanicFlags , 0.f );
				if ( m_Cast.active )
					m_NextPanicTick = now + kPanicHoldMs;
				threatHandled = true;
			}
		}

		if ( !threatHandled && Settings::Dodger::SaveAllies )
		{
			const HeroSnapshot* ally = nullptr;
			float allyDistance = 0.f;
			for ( const auto& candidate : heroes )
			{
				if ( candidate.team != local->team || candidate.entity == local->entity )
					continue;
				if ( HealthPercent( candidate ) > Settings::Dodger::PanicHealthPercent )
					continue;
				const float distance = FS::Distance2D( local->origin , candidate.origin );
				if ( distance > kAllySaveRange )
					continue;
				if ( !ally || distance < allyDistance )
				{
					ally = &candidate;
					allyDistance = distance;
				}
			}

			if ( ally )
			{
				if ( const auto* source = NearestEnemyTo( ally->origin ) )
				{
					ArmResponse( "ally at critical health" , *source , ally , kPanicFlags , allyDistance );
					// Only start the hold if something actually fired -
					// otherwise a panic that found no usable item would lock
					// the next six seconds out for no reason.
					if ( m_Cast.active )
						m_NextPanicTick = now + kPanicHoldMs;
					threatHandled = true;
				}
			}
		}
	}

	// Idle status, but never on top of a status that explains why we are idle
	// (waiting for offsets, stunned, invulnerable) or of a threat still being
	// shown on screen.
	if ( canAct && !threatHandled && !m_Cast.active && now >= m_Threat.expiresAt )
		m_Status = "Watching";
}
