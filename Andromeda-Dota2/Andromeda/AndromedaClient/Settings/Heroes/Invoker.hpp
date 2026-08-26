#pragma once

#include <Common/Common.hpp>

namespace Settings
{
	namespace Heroes
	{
		namespace Invoker
		{
			// Кнопка для вызова прокаста/комбо (0 = выключено).
			inline int ComboKey = 0;
			// Цель для прокаста (entity index), можно выставлять из UI/Lua.
			inline int TargetEntIndex = -1;
			// Index into the hardcoded 10-spell orb table (see CInvokerController.cpp).
			// Legacy single-spell selection - the combo now casts the SpellOrder
			// sequence below; this remains only for the old menu dropdown.
			inline int ComboSpell = 6; // Sun Strike by default.

			// Full-combo sequence, edited from the Auto Combo page. SpellOrder is
			// a permutation of 0..9 (indices into kInvokerSpells: 0 Cold Snap,
			// 1 Ghost Walk, 2 Ice Wall, 3 EMP, 4 Tornado, 5 Alacrity, 6 Sun Strike,
			// 7 Forge Spirit, 8 Chaos Meteor, 9 Deafening Blast); element 0 casts
			// first. Spells with SpellEnabled[spell] false are skipped. Default:
			// the classic Tornado -> EMP -> Sun Strike -> Meteor -> Blast.
			inline constexpr int SpellCount = 10;
			inline constexpr int DefaultSpellOrder[SpellCount] = { 4 , 3 , 6 , 8 , 9 , 0 , 2 , 5 , 7 , 1 };
			inline constexpr bool DefaultSpellEnabled[SpellCount] = { false , false , false , true , true , false , true , false , true , true };
			inline int SpellOrder[SpellCount] = { 4 , 3 , 6 , 8 , 9 , 0 , 2 , 5 , 7 , 1 };
			inline bool SpellEnabled[SpellCount] = { false , false , false , true , true , false , true , false , true , true };
		}
	}
}
