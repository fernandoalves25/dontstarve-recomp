-- Every HUD root, the map overlay and the follow text (hover and speech) take
-- their size from FrontEnd:GetHUDScale(), so one multiplier enlarges them all.
-- 1.5x is the most a 4:3 640x480 screen fits (the inventory bar's width).
local FrontEnd = GLOBAL.FrontEnd
local base = FrontEnd.GetHUDScale
local mult = GetModConfigData("scale") or 1.4
FrontEnd.GetHUDScale = function(self)
    return base(self) * mult
end

-- Every text in the game (HUD items, recipe popups, menus, dialogs) is a
-- widgets/text: its size is set once in the constructor and on every
-- SetSize, so scaling both enlarges all of them by the same factor. Fixed
-- text regions may wrap one line sooner; 1.25x keeps the stock layouts.
local tmult = GetModConfigData("text") or 1.25
if tmult ~= 1 then
    local Text = GLOBAL.require "widgets/text"
    local set_size = Text.SetSize
    Text.SetSize = function(self, sz)
        return set_size(self, sz * tmult)
    end
    AddClassPostConstruct("widgets/text", function(self, font, size)
        if size then self:SetSize(size) end           -- the constructor's size, scaled
    end)
end

-- Optional 30 fps cap (the engine's own TheSim:SetTargetFrameRate, which no
-- stock script calls). The simulation runs at 30 Hz either way.
local fps = GetModConfigData("fps") or 60
if fps ~= 60 and GLOBAL.TheSim.SetTargetFrameRate then
    GLOBAL.TheSim:SetTargetFrameRate(fps)
end
