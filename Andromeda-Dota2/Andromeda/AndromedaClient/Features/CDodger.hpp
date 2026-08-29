#pragma once

#include <Dota2/SDK/Math/Vector3.hpp>

#include <Windows.h>

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

// Reactive defense: watches every enemy hero's abilities and items for the
// moment a cast starts, decides whether that cast can reach us (or a low ally),
// and answers it with the best counter we own - a magic-immunity/dispel item,
// a blink away, or a save cast on the ally.
//
// See CDodger.cpp for how a cast is detected without any projectile/net-message
// hook.
class CDodger final
{
public:
	auto OnRender() -> void;

	auto GetStatus() const -> const std::string& { return m_Status; }

	// Live match data for the menu's combination editor. Both are refreshed
	// only while the menu is open (casting is blocked then anyway) on the same
	// render thread the menu draws on, so no locking is involved.
	struct EnemySpell
	{
		std::string hero;
		std::string spell;
		// True when the dodger would treat this cast as a threat today.
		bool dangerous = false;
	};

	struct OwnCounter
	{
		std::string name;
		bool isItem = false;
		char key = 0;
	};

	auto GetEnemySpells() const -> const std::vector<EnemySpell>& { return m_EnemySpells; }
	auto GetOwnCounters() const -> const std::vector<OwnCounter>& { return m_OwnCounters; }

private:
	// Four separate ticks, because Dota consumes injected input asynchronously:
	// aim, press, THEN click on a later frame (the game needs a beat to enter
	// targeting mode first), then restore the cursor.
	enum class CastPhase : uint8_t
	{
		Aim,
		CenterCamera,
		Cast,
		SelfTap,
		Click,
		Restore
	};

	// One in-flight counter-cast. Dodging is always a single action - if the
	// first answer doesn't land there is no time for a second one anyway.
	struct CastState
	{
		bool active = false;
		bool needsAim = false;
		bool needsClick = false;
		// Ground-targeted (blink) casts must aim at the ground point itself;
		// unit-targeted ones aim slightly above the model.
		bool groundAim = false;
		// Aimed at our own hero, so Dota's own self-cast (double-tapping the
		// key) can replace the click entirely - no camera, no cursor.
		bool selfCast = false;
		bool doubleTap = false;
		// Set once we have already tried centring the camera, so a hero that
		// still will not project cannot loop.
		bool cameraCentred = false;
		CastPhase phase = CastPhase::Aim;
		WORD key = 0;
		std::string name;
		Vector3 aimPoint{};
		// Where a ground-targeted escape starts from, so the point can be
		// pulled in toward the hero when the full-length one is off screen.
		Vector3 aimOrigin{};
		uint32_t startTick = 0;
		uint32_t nextTick = 0;
		uint32_t expiresAt = 0;
		bool hasPrevCursor = false;
		int prevCursorX = 0;
		int prevCursorY = 0;
	};

	// Last observed state of one enemy ability/item, keyed by its entity
	// address. A cast is a transition in these values, so the previous sample
	// is the whole detector.
	struct AbilityWatch
	{
		float cooldown = 0.f;
		bool inPhase = false;
		bool initialized = false;
		uint32_t lastSeenTick = 0;
		// Guards against the cast-point edge and the cooldown edge of one cast
		// both reporting it.
		uint32_t lastFiredTick = 0;
	};

	struct ThreatInfo
	{
		std::string abilityName;
		std::string casterName;
		Vector3 casterOrigin{};
		Vector3 victimOrigin{};
		bool againstAlly = false;
		uint32_t detectedTick = 0;
		uint32_t expiresAt = 0;
		uint8_t flags = 0;
	};

	auto CancelCast( const char* reason = nullptr ) -> void;
	auto AdvanceCast( uint32_t now ) -> void;
	auto DrawThreatMarker() const -> void;

	CastState m_Cast{};
	// Previous position of each enemy hero, so a blink initiation shows up as
	// a position jump no ability watch could have told us about.
	struct PositionSample
	{
		Vector3 origin{};
		uint32_t tick = 0;
	};

	// After firing, the log could only say the input was SENT. Re-reading the
	// item/ability a moment later and checking it went on cooldown is the
	// difference between "we pressed the key" and "the game accepted the cast"
	// - the exact ambiguity that hid the SetCursorPos bug for a whole session.
	struct PendingVerify
	{
		std::string name;
		uint32_t at = 0;
	};

	PendingVerify m_Verify{};
	std::unordered_map<uintptr_t , AbilityWatch> m_Watches{};
	std::unordered_map<int , PositionSample> m_HeroPositions{};
	ThreatInfo m_Threat{};
	std::string m_Status = "Idle";
	uint32_t m_NextThinkTick = 0;
	uint32_t m_NextPruneTick = 0;
	uint32_t m_NextDodgeTick = 0;
	uint32_t m_NextPanicTick = 0;
	uint32_t m_NextMenuRefreshTick = 0;
	std::vector<EnemySpell> m_EnemySpells{};
	std::vector<OwnCounter> m_OwnCounters{};
};
