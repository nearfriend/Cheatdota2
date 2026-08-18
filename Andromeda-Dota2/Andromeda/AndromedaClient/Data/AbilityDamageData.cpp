#include "AbilityDamageData.hpp"

#include <Common/DevLog.hpp>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <fstream>
#include <sstream>
#include <vector>

static CAbilityDamageData g_AbilityDamageData{};

namespace
{
	auto ToLower( std::string value ) -> std::string
	{
		for ( auto& character : value )
			character = static_cast<char>( std::tolower( static_cast<unsigned char>( character ) ) );
		return value;
	}

	auto ExtractFirstQuoted( const std::string& line , size_t offset = 0 ) -> std::string
	{
		const size_t first = line.find( '"' , offset );
		const size_t second = first == std::string::npos ? std::string::npos : line.find( '"' , first + 1 );
		if ( first == std::string::npos || second == std::string::npos )
			return {};
		return line.substr( first + 1 , second - first - 1 );
	}

	auto ExtractJsonStringValue( const std::string& line ) -> std::string
	{
		const size_t colon = line.find( ':' );
		if ( colon == std::string::npos )
			return {};
		return ExtractFirstQuoted( line , colon + 1 );
	}

	auto BraceDelta( const std::string& line ) -> int
	{
		int delta = 0;
		bool inString = false;
		bool escaped = false;

		for ( const char character : line )
		{
			if ( escaped )
			{
				escaped = false;
				continue;
			}
			if ( character == '\\' && inString )
			{
				escaped = true;
				continue;
			}
			if ( character == '"' )
			{
				inString = !inString;
				continue;
			}
			if ( inString )
				continue;
			if ( character == '{' )
				++delta;
			else if ( character == '}' )
				--delta;
		}

		return delta;
	}

	auto ParseNumbers( const std::string& text ) -> std::vector<float>
	{
		std::string cleaned;
		cleaned.reserve( text.size() );
		for ( const char character : text )
		{
			const unsigned char value = static_cast<unsigned char>( character );
			if ( std::isdigit( value ) || character == '.' || character == '-' )
				cleaned.push_back( character );
			else
				cleaned.push_back( ' ' );
		}

		std::vector<float> result;
		std::istringstream stream( cleaned );
		float value = 0.f;
		while ( stream >> value )
			result.push_back( value );
		return result;
	}

	template<typename T , size_t Count>
	auto CopyNumbers( std::array<T , Count>& out , int& outCount , const std::vector<float>& values ) -> void
	{
		out = {};
		outCount = static_cast<int>( (std::min)( values.size() , Count ) );
		for ( int index = 0; index < outCount; ++index )
			out[index] = static_cast<T>( values[index] );
	}

	auto MaxValue( const std::vector<float>& values ) -> float
	{
		if ( values.empty() )
			return 0.f;
		return *std::max_element( values.begin() , values.end() );
	}

	auto IsLikelyDirectDamageKey( std::string key ) -> bool
	{
		key = ToLower( key );

		if ( key == "damage" || key == "damage_min" || key == "damage_max" ||
			 key == "base_damage" || key == "impact_damage" || key == "bolt_damage" ||
			 key == "strike_damage" || key == "nuke_damage" || key == "blast_damage" ||
			 key == "arrow_damage" || key == "projectile_damage" || key == "initial_damage" ||
			 key == "hero_damage" || key == "tooltip_damage" || key == "damage_tooltip" ||
			 key == "abilitydamage" || key == "bonus_damage" || key == "punch_bonus_damage_base" )
			return true;

		if ( key.find( "damage" ) == std::string::npos )
			return false;

		const char* rejectParts[] =
		{
			"pct" , "percent" , "reduction" , "amp" , "duration" , "delay" , "radius" ,
			"range" , "interval" , "per_second" , "per_tick" , "per_kill" , "cleave" ,
			"chance" , "factor" , "multiplier" , "count"
		};

		for ( const char* reject : rejectParts )
		{
			if ( key.find( reject ) != std::string::npos )
				return false;
		}

		return true;
	}

	auto IsCastRangeKey( std::string key ) -> bool
	{
		key = ToLower( key );
		return key == "cast_range" || key == "abilitycastrange";
	}

	auto AssignDamageCandidate( AbilityDamageEntry& entry , const std::vector<float>& values ) -> void
	{
		if ( values.empty() || MaxValue( values ) <= 0.f )
			return;

		std::vector<float> current;
		for ( int index = 0; index < entry.damageCount; ++index )
			current.push_back( entry.damage[index] );

		if ( entry.damageCount == 0 || MaxValue( values ) > MaxValue( current ) )
			CopyNumbers( entry.damage , entry.damageCount , values );
	}

	auto AssignCastRange( AbilityDamageEntry& entry , const std::vector<float>& values ) -> void
	{
		if ( values.empty() )
			return;
		entry.castRange = (std::max)( entry.castRange , MaxValue( values ) );
	}

	auto AssignMana( AbilityDamageEntry& entry , const std::vector<float>& values ) -> void
	{
		if ( values.empty() )
			return;
		CopyNumbers( entry.manaCost , entry.manaCostCount , values );
	}

	auto AssignCooldown( AbilityDamageEntry& entry , const std::vector<float>& values ) -> void
	{
		if ( values.empty() )
			return;
		CopyNumbers( entry.cooldown , entry.cooldownCount , values );
	}

	auto ShouldKeepEntry( const AbilityDamageEntry& entry ) -> bool
	{
		if ( !entry.IsUsableDamage() )
			return false;
		if ( entry.name.rfind( "special_bonus_" , 0 ) == 0 )
			return false;
		if ( entry.name.rfind( "generic_" , 0 ) == 0 || entry.name.rfind( "dota_" , 0 ) == 0 )
			return false;
		return entry.targetEnemy || entry.unitTarget || entry.pointTarget || entry.noTarget;
	}

	auto SelectLevelValue( const std::array<float , 8>& values , int count , int level ) -> float
	{
		if ( count <= 0 )
			return 0.f;
		if ( level <= 0 )
			level = 1;
		const int index = (std::min)( level - 1 , count - 1 );
		return values[index];
	}

	auto SelectLevelValue( const std::array<int , 8>& values , int count , int level ) -> int
	{
		if ( count <= 0 )
			return 0;
		if ( level <= 0 )
			level = 1;
		const int index = (std::min)( level - 1 , count - 1 );
		return values[index];
	}
}

auto AbilityDamageEntry::DamageForLevel( int level ) const -> float
{
	return SelectLevelValue( damage , damageCount , level );
}

auto AbilityDamageEntry::ManaForLevel( int level ) const -> int
{
	return SelectLevelValue( manaCost , manaCostCount , level );
}

auto AbilityDamageEntry::CooldownForLevel( int level ) const -> float
{
	return SelectLevelValue( cooldown , cooldownCount , level );
}

auto CAbilityDamageData::LoadFromFile( const std::string& path ) -> bool
{
	std::ifstream input( path );
	if ( !input.is_open() )
		return false;

	std::unordered_map<std::string , AbilityDamageEntry> parsed;
	AbilityDamageEntry current;
	std::string currentAttribKey;
	std::string collectingKey;
	std::vector<float> collectingValues;
	int objectDepth = 0;
	std::string line;

	auto FinishCollection = [&]()
	{
		if ( collectingKey.empty() )
			return;
		if ( IsLikelyDirectDamageKey( collectingKey ) )
			AssignDamageCandidate( current , collectingValues );
		else if ( IsCastRangeKey( collectingKey ) )
			AssignCastRange( current , collectingValues );
		else if ( collectingKey == "mc" )
			AssignMana( current , collectingValues );
		else if ( collectingKey == "cd" )
			AssignCooldown( current , collectingValues );

		collectingKey.clear();
		collectingValues.clear();
	};

	while ( std::getline( input , line ) )
	{
		const int depthBefore = objectDepth;

		if ( current.name.empty() && depthBefore == 1 )
		{
			const size_t keyStart = line.find( '"' );
			const size_t keyEnd = keyStart == std::string::npos ? std::string::npos : line.find( '"' , keyStart + 1 );
			const size_t objectStart = keyEnd == std::string::npos ? std::string::npos : line.find( '{' , keyEnd + 1 );
			if ( keyStart != std::string::npos && keyEnd != std::string::npos && objectStart != std::string::npos )
			{
				current = {};
				current.name = line.substr( keyStart + 1 , keyEnd - keyStart - 1 );
				currentAttribKey.clear();
				collectingKey.clear();
				collectingValues.clear();
			}
		}

		if ( !current.name.empty() )
		{
			if ( !collectingKey.empty() )
			{
				const auto values = ParseNumbers( line );
				collectingValues.insert( collectingValues.end() , values.begin() , values.end() );
				if ( line.find( ']' ) != std::string::npos )
					FinishCollection();
			}
			else
			{
				if ( line.find( "\"Unit Target\"" ) != std::string::npos || line.find( "\"Unit Target\"" ) != std::string::npos )
					current.unitTarget = true;
				if ( line.find( "\"Point Target\"" ) != std::string::npos )
					current.pointTarget = true;
				if ( line.find( "\"No Target\"" ) != std::string::npos )
					current.noTarget = true;
				if ( line.find( "\"target_team\"" ) != std::string::npos )
				{
					const std::string targetTeam = ToLower( ExtractJsonStringValue( line ) );
					current.targetEnemy = targetTeam.find( "enemy" ) != std::string::npos;
				}
				if ( line.find( "\"dmg_type\"" ) != std::string::npos )
				{
					const std::string type = ToLower( ExtractJsonStringValue( line ) );
					if ( type.find( "pure" ) != std::string::npos )
						current.damageType = AbilityDamageType::Pure;
					else if ( type.find( "physical" ) != std::string::npos )
						current.damageType = AbilityDamageType::Physical;
					else
						current.damageType = AbilityDamageType::Magical;
				}

				if ( line.find( "\"key\"" ) != std::string::npos )
					currentAttribKey = ExtractJsonStringValue( line );

				if ( !currentAttribKey.empty() && line.find( "\"value\"" ) != std::string::npos )
				{
					if ( line.find( '[' ) != std::string::npos && line.find( ']' ) == std::string::npos )
					{
						collectingKey = currentAttribKey;
						collectingValues = ParseNumbers( line );
					}
					else
					{
						const auto values = ParseNumbers( line );
						if ( IsLikelyDirectDamageKey( currentAttribKey ) )
							AssignDamageCandidate( current , values );
						else if ( IsCastRangeKey( currentAttribKey ) )
							AssignCastRange( current , values );
					}
				}

				if ( line.find( "\"mc\"" ) != std::string::npos )
				{
					if ( line.find( '[' ) != std::string::npos && line.find( ']' ) == std::string::npos )
					{
						collectingKey = "mc";
						collectingValues = ParseNumbers( line );
					}
					else
					{
						AssignMana( current , ParseNumbers( line ) );
					}
				}
				else if ( line.find( "\"cd\"" ) != std::string::npos )
				{
					if ( line.find( '[' ) != std::string::npos && line.find( ']' ) == std::string::npos )
					{
						collectingKey = "cd";
						collectingValues = ParseNumbers( line );
					}
					else
					{
						AssignCooldown( current , ParseNumbers( line ) );
					}
				}
			}
		}

		objectDepth += BraceDelta( line );

		if ( !current.name.empty() && depthBefore > 1 && objectDepth == 1 )
		{
			FinishCollection();
			if ( ShouldKeepEntry( current ) )
				parsed[current.name] = current;
			current = {};
			currentAttribKey.clear();
		}
	}

	if ( parsed.empty() )
		return false;

	m_Entries.swap( parsed );
	return true;
}

auto CAbilityDamageData::LoadHeroSkillBarFromFile( const std::string& path ) -> bool
{
	std::ifstream input( path );
	if ( !input.is_open() )
		return false;

	std::unordered_map<std::string , int> parsed;
	std::string currentHero;
	std::string line;
	bool readingAbilities = false;
	int slot = 0;

	while ( std::getline( input , line ) )
	{
		const size_t heroMarker = line.find( "\"npc_dota_hero_" );
		if ( heroMarker != std::string::npos )
		{
			const size_t endQuote = line.find( '"' , heroMarker + 1 );
			if ( endQuote != std::string::npos )
			{
				currentHero = line.substr( heroMarker + 1 , endQuote - heroMarker - 1 );
				readingAbilities = false;
				slot = 0;
			}
			continue;
		}

		if ( currentHero.empty() )
			continue;

		if ( !readingAbilities && line.find( "\"abilities\"" ) != std::string::npos && line.find( '[' ) != std::string::npos )
		{
			readingAbilities = true;
			slot = 0;
			continue;
		}

		if ( !readingAbilities )
			continue;

		if ( line.find( ']' ) != std::string::npos )
		{
			readingAbilities = false;
			continue;
		}

		const std::string ability = ExtractFirstQuoted( line );
		if ( ability.empty() )
			continue;

		if ( slot < 6 && ability != "generic_hidden" &&
			ability.rfind( "special_bonus_" , 0 ) != 0 &&
			!parsed.contains( ability ) )
		{
			parsed[ability] = slot;
		}
		++slot;
	}

	if ( parsed.empty() )
		return false;

	m_PreferredSlots.swap( parsed );
	return true;
}

auto CAbilityDamageData::Find( std::string_view name ) const -> const AbilityDamageEntry*
{
	const auto it = m_Entries.find( std::string( name ) );
	return it == m_Entries.end() ? nullptr : &it->second;
}

auto CAbilityDamageData::PreferredSlot( std::string_view abilityName ) const -> int
{
	const auto it = m_PreferredSlots.find( std::string( abilityName ) );
	return it == m_PreferredSlots.end() ? -1 : it->second;
}

auto GetAbilityDamageData() -> CAbilityDamageData*
{
	return &g_AbilityDamageData;
}
