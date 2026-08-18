#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>

enum class AbilityDamageType : uint8_t
{
	Magical,
	Physical,
	Pure
};

struct AbilityDamageEntry
{
	std::string name;
	std::array<float , 8> damage{};
	int damageCount = 0;
	float castRange = 0.f;
	std::array<int , 8> manaCost{};
	int manaCostCount = 0;
	std::array<float , 8> cooldown{};
	int cooldownCount = 0;
	AbilityDamageType damageType = AbilityDamageType::Magical;
	bool unitTarget = false;
	bool pointTarget = false;
	bool noTarget = false;
	bool targetEnemy = false;

	auto IsUsableDamage() const -> bool { return damageCount > 0 && damage[0] > 0.f; }
	auto DamageForLevel( int level ) const -> float;
	auto ManaForLevel( int level ) const -> int;
	auto CooldownForLevel( int level ) const -> float;
};

class CAbilityDamageData final
{
public:
	auto LoadFromFile( const std::string& path ) -> bool;
	auto LoadHeroSkillBarFromFile( const std::string& path ) -> bool;
	auto Find( std::string_view name ) const -> const AbilityDamageEntry*;
	auto PreferredSlot( std::string_view abilityName ) const -> int;
	auto LoadedCount() const -> size_t { return m_Entries.size(); }
	auto HeroSkillSlotCount() const -> size_t { return m_PreferredSlots.size(); }

private:
	std::unordered_map<std::string , AbilityDamageEntry> m_Entries;
	std::unordered_map<std::string , int> m_PreferredSlots;
};

auto GetAbilityDamageData() -> CAbilityDamageData*;
