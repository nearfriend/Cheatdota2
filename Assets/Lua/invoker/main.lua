local state = {
    combo_target = -1,
    last_tick_log = 0,
}

-- Вызывается каждый тик (см. LuaManager::TickHero).
function on_tick(dt)
    state.last_tick_log = state.last_tick_log + dt

    -- Пример: раз в ~2 секунды пишем, что скрипт жив.
    if state.last_tick_log > 2.0 then
        print("[invoker] on_tick, target=" .. tostring(state.combo_target))
        state.last_tick_log = 0
    end
end

-- Вызывается при нажатии ComboKey (см. CInvokerController).
function on_combo(target_index)
    state.combo_target = target_index or -1
    print("[invoker] combo triggered, target=" .. tostring(state.combo_target))
end
