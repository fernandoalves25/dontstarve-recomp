-- Soak test for the recompiled build: days pass every 20 s (each new day
-- autosaves, seasons turn over), the player cannot die.
local days = 0
local function tick()
    local p = GLOBAL.GetPlayer()
    if p then
        if p.components.health then p.components.health:SetInvincible(true) end
        if p.components.hunger then p.components.hunger:SetPercent(1) end
        if p.components.sanity then p.components.sanity:SetPercent(1) end
    end
    local clock = GLOBAL.GetClock and GLOBAL.GetClock()
    if clock and clock.MakeNextDay then
        clock:MakeNextDay()
        days = days + 1
        print("[soak] day skipped: " .. days .. ", world day " .. tostring(clock.numcycles))
    end
end
AddSimPostInit(function(inst)
    inst:DoPeriodicTask(20, tick)
end)
