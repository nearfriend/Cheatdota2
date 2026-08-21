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
			// Index into the hardcoded 10-spell orb table (see CInvokerController.cpp) selecting which spell ComboKey casts.
			inline int ComboSpell = 6; // Sun Strike by default.
		}
	}
}
