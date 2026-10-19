-- Every HUD root, the map overlay and the follow text (hover and speech) take
-- their size from FrontEnd:GetHUDScale(), so one multiplier enlarges them all.
-- 1.5x is the most a 4:3 640x480 screen fits (the inventory bar's width).
local FrontEnd = GLOBAL.FrontEnd
local base = FrontEnd.GetHUDScale
local mult = GetModConfigData("scale") or 1.4
FrontEnd.GetHUDScale = function(self)
    return base(self) * mult
end

-- Optional 30 fps cap (the engine's own TheSim:SetTargetFrameRate, which no
-- stock script calls). The simulation runs at 30 Hz either way.
local fps = GetModConfigData("fps") or 60
if fps ~= 60 and GLOBAL.TheSim.SetTargetFrameRate then
    GLOBAL.TheSim:SetTargetFrameRate(fps)
end
