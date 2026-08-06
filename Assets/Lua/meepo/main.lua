local state = {
    combo_target = -1,
    last_tick_log = 0,
}

function on_tick(dt)
    state.last_tick_log = state.last_tick_log + dt
    if state.last_tick_log > 2.0 then
        print("[meepo] on_tick, target=" .. tostring(state.combo_target))
        state.last_tick_log = 0
    end
end

function on_combo(target_index)
    state.combo_target = target_index or -1
    print("[meepo] combo triggered, target=" .. tostring(state.combo_target))
end
