#pragma once

#include <Dota2/SDK/Math/Vector3.hpp>

#include <Windows.h>

#include <cstdint>
#include <string>

// Body-blocks your own creep wave on the way to lane, for as long as a key is
// held.
//
// Blocking is a positioning trick, not a cast: you repeatedly walk to a point
// just in front of the leading creep so your hero's collision holds the wave
// back, and you keep cutting across their path as they try to walk around you.
// That is exactly what this issues - a move order every ~180 ms at a point
// ahead of the front creep, alternating sides each time.
//
// See CCreepBlocker.cpp for how the wave's direction of travel is read and why
// the order is a right click rather than a keyed command.
class CCreepBlocker final
{
public:
	auto OnRender() -> void;

	// Shown live in the menu page, so the user can tell "no wave found" from
	// "key not bound" without reading a log.
	auto GetStatus() const -> const std::string& { return m_Status; }

private:
	// Dota consumes injected input asynchronously, so an order is three ticks,
	// never one burst: aim the cursor, click on a later frame, then put the
	// cursor back once the click has actually gone out. Restoring it in the
	// same breath as the click races the click itself - the order then lands
	// wherever the player's cursor was.
	enum class OrderPhase : uint8_t
	{
		Idle,
		Click,
		Restore
	};

	auto AdvanceOrder( uint32_t now ) -> void;
	auto TryIssueBlockOrder( uint32_t now ) -> bool;

	OrderPhase m_Phase = OrderPhase::Idle;
	uint32_t m_NextPhaseTick = 0;
	uint32_t m_NextOrderTick = 0;
	POINT m_PreviousCursor{};
	std::string m_Status = "Idle";
};
