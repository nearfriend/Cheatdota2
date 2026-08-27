#pragma once

#include <Dota2/SDK/Math/Vector3.hpp>

#include <Windows.h>

#include <cstdint>
#include <string>
#include <unordered_map>

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

private:
	enum class CastPhase : uint8_t
	{
		Aim,
		Cast,
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
		CastPhase phase = CastPhase::Aim;
		WORD key = 0;
		std::string name;
		Vector3 aimPoint{};
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

	std::unordered_map<uintptr_t , AbilityWatch> m_Watches{};
	std::unordered_map<int , PositionSample> m_HeroPositions{};
	ThreatInfo m_Threat{};
	std::string m_Status = "Idle";
	uint32_t m_NextThinkTick = 0;
	uint32_t m_NextPruneTick = 0;
	uint32_t m_NextDodgeTick = 0;
	uint32_t m_NextPanicTick = 0;
};
