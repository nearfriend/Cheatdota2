#pragma once

#include <Common/Common.hpp>
#include <atomic>
#include <cstdint>
#include <vector>
#include <string>

class CDOTAInput;
class CUserCmd;
class C_DOTA_BaseNPC_Hero;
class C_DOTABaseAbility;

class CInvokerController
{
public:
	void OnCreateMove( CDOTAInput* pCDOTAInput , CUserCmd* pCUserCmd );

	// UI status helpers (Heroes tab).
	std::string GetComboStatus() const { return m_ComboStatus; }

	// Fire the Invoker combo sequence (every enabled spell in the configured
	// order) without sharing its hotkey - used by CAutoCombo when the local
	// hero is Invoker, whose spells can't be cast by slot hotkeys. Callable
	// from any thread; the request is consumed on the next OnCreateMove tick.
	void RequestCombo() { m_PendingComboRequest.store( true , std::memory_order_relaxed ); }

	// Spell table accessors for the menu's Invoker spell-order editor.
	static int GetSpellCount();
	static const char* GetSpellDisplayName( int index );

	struct AbilityEntry
	{
		int slot = -1;
		std::string name;
		// Live ability entity, refreshed every tick alongside the name. Used to
		// read cooldown/mana; not part of identity comparison, since the
		// pointer changing does not mean the ability bar changed.
		C_DOTABaseAbility* ability = nullptr;

		bool operator==( const AbilityEntry& other ) const noexcept
		{
			return slot == other.slot && name == other.name;
		}
	};

	const std::vector<AbilityEntry>& GetAbilities() const { return m_Abilities; }

private:
	// The targeted final cast spans three phases (aim -> press -> restore)
	// rather than one tick: Dota consumes injected input asynchronously, so
	// the cursor must still be on the target when the game processes the
	// key/click - the old single-tick move+press+restore made targeted spells
	// resolve at the pre-aim cursor position (same bug CAutoCombo had).
	enum class ComboPhase : uint8_t
	{
		Idle,
		SelectingHero,
		PressingOrbs,
		Invoking,
		WaitingForSlot,
		CastingFinal,
		FinalPress,
		FinalRestore
	};

	struct ComboState
	{
		bool active = false;
		ComboPhase phase = ComboPhase::Idle;
		int spellIndex = -1;
		std::vector<uint16_t> orbKeys;
		size_t orbIndex = 0;
		uint32_t nextActionTick = 0;
		uint32_t phaseDeadline = 0;
		// Invoke gets re-pressed on this cadence while waiting for the spell to
		// appear: a press made while Invoke is on cooldown is simply eaten by
		// the game, so one press is not enough.
		uint32_t nextInvokeRetryTick = 0;
		uint16_t finalKey = 0;
		// A key currently held down, released once releaseTick passes. Sending
		// down+up in one batch gives a zero-length press that an input consumer
		// sampling per frame can miss entirely.
		uint16_t heldKey = 0;
		uint32_t releaseTick = 0;
		// Set when the spell is already invoked, so after re-selecting the hero
		// the combo jumps straight to casting instead of redoing the orbs.
		bool castDirectly = false;
		// How many times the invoke wait has been extended because Invoke was
		// still on cooldown. Bounded so a genuinely stuck combo still fails.
		int invokeWaitExtensions = 0;
		int32_t prevCursorX = 0;
		int32_t prevCursorY = 0;
		bool hasPrevCursor = false;
	};

private:
	bool ResolveLocalHero();
	bool EnsureAbilityOffsets();
	bool EnsureOriginOffsets();
	bool RefreshAbilityList();
	void TickLua();
	void LogAbilitiesOnce();

	bool StartCombo( int spellIndex );
	void StartSequence();
	void TickSequence();
	void AdvanceCombo();
	void CancelCombo( const char* reason );
	bool ResolveFinalKey( const char* abilityName , uint16_t& outKey ) const;
	// True when the named ability is off cooldown and affordable. An invoked
	// spell stays in its D/F slot after being cast, so slot presence alone
	// does not mean it can be cast again.
	bool IsSpellReady( const char* abilityName ) const;
	// Remaining cooldown in seconds for an ability in slots 0..5, 0 when ready
	// or unknown. Used to wait out Invoke between consecutive spells instead of
	// declaring the spell uninvokable.
	float AbilityCooldownRemaining( const char* abilityName ) const;
	// Level of one of the three orb abilities by name. 0 = no skill points
	// spent, meaning orb instances of that type cannot be created at all, so
	// any spell needing them is impossible to invoke right now.
	int OrbLevel( const char* orbAbilityName ) const;
	// Projects the combo target and parks the cursor on it. Returns false when
	// the target/its origin/projection is unavailable (caller cancels).
	bool AimAtComboTarget( HWND window );
	// Releases a key left held by the hold-to-press mechanism. MUST run before
	// any reset of m_Combo: dropping the state with a key still down leaves it
	// stuck down in the game, which silently swallows every later press of
	// that key (a stuck Invoke key made all subsequent invokes no-ops).
	void ReleaseHeldKey();

private:
	C_DOTA_BaseNPC_Hero* m_pHero = nullptr;
	int m_LastHeroEntIndex = -1;
	bool m_HeroClassificationComplete = false;
	uint32_t m_AbilityArrayOffset = 0;
	uint32_t m_AbilityLevelOffset = 0;
	uint32_t m_AbilityCooldownOffset = 0;
	uint32_t m_AbilityManaCostOffset = 0;
	uint32_t m_HeroManaOffset = 0;
	bool m_OffsetsReady = false;
	bool m_LoggedAbilities = false;
	std::vector<AbilityEntry> m_Abilities;

	uint32_t m_SceneNodeOffset = 0;
	uint32_t m_AbsOriginOffset = 0;
	bool m_OriginOffsetsReady = false;

	ComboState m_Combo{};
	std::string m_ComboStatus = "Idle";
	bool m_ComboKeyWasDown = false;
	std::atomic<bool> m_PendingComboRequest{ false };

	// Multi-spell sequence state: the queue of spell indices being cast one
	// after another, each through the full orb -> Invoke -> cast pipeline.
	std::vector<int> m_SequenceQueue;
	size_t m_SequenceIndex = 0;
	bool m_SequenceActive = false;
	bool m_AwaitingComboResult = false;
	bool m_LastComboSucceeded = false;
	// Set when a cast was abandoned only because the game window wasn't
	// accepting input (alt-tabbed, or our menu open). That is a pause, not a
	// spell failure, so it must not consume the spell's retry budget.
	bool m_ComboBlockedByWindow = false;
	int m_SpellRetries = 0;
	uint32_t m_NextSequenceTick = 0;
	uint32_t m_SequenceDeadline = 0;
};
