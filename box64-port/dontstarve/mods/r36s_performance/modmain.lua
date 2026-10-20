local G = GLOBAL

local function opt(name, default)
	local value = GetModConfigData(name)
	if value == nil then
		return default
	end
	return value
end

-- Options the launcher writes from port.cfg into this folder
local port = {}
local port_chunk = G.kleiloadlua(MODROOT .. "r36s_options.lua")
if type(port_chunk) == "function" then
	port = port_chunk() or {}
end

-- 30 Hz simulation tick and frame cap (what the Android/iOS builds force).
-- The engine stores it in [MISC] netbook_mode of settings.ini.
if opt("netbook", true) then
	G.TheSim:SetNetbookMode(true)
end

-- Klei's metrics endpoints only add HTTP work on a handheld.
G.METRICS_ENABLED = false
G.STATS_ENABLE = false

if not opt("season_dsp", false) then
	G.USE_SEASON_DSP = false
end

-- playerhud.lua checks this when the HUD is built.
if opt("hide_vignette", true) then
	G.TheConfig:Enable("hide_vignette")
end

local no_postfx = opt("no_postfx", true)
if no_postfx then
	G.Profile:SetBloomEnabled(false)
	G.Profile:SetDistortionEnabled(false)
end

if opt("no_canopy", true) and G.Profile.RenderCanopy then
	G.Profile:RenderCanopy(false)
end

-- Handhelds have no mouse: switch the gamepad on. The game only offers that through a
-- one-time popup (marked as seen in the port's settings.ini), so enable every connected
-- controller when startup checks them, after the profile's saved controls were applied.
-- On-screen FPS counter (SHOW_FPS=1 in port.cfg): frames drawn per second, top left
-- (the top right corner holds the day clock)
-- With DEBUG_FPS=1 the 10 s average also goes to log.txt as "[R36S] fps N"
local fps_label
if port.show_fps or port.log_fps then
	local frames, elapsed = 0, 0
	local log_frames, log_elapsed = 0, 0
	local _WallUpdate = G.WallUpdate
	G.WallUpdate = function(dt, ...)
		frames, log_frames = frames + 1, log_frames + 1
		elapsed, log_elapsed = elapsed + (dt or 0), log_elapsed + (dt or 0)
		if elapsed >= 0.5 then
			if fps_label then
				fps_label:SetString(string.format("%d FPS", frames / elapsed + 0.5))
			end
			frames, elapsed = 0, 0
		end
		if port.log_fps and log_elapsed >= 10 then
			print(string.format("[R36S] fps %.1f", log_frames / log_elapsed))
			log_frames, log_elapsed = 0, 0
		end
		return _WallUpdate(dt, ...)
	end
end

local function add_fps_label()
	-- The front end's overlay layer is drawn above every screen, the in-game HUD included
	local Widget = G.require("widgets/widget")
	local Text = G.require("widgets/text")
	local root = G.TheFrontEnd.overlayroot:AddChild(Widget("r36s_fps"))
	root:SetScaleMode(G.SCALEMODE_PROPORTIONAL)
	root:SetHAnchor(G.ANCHOR_LEFT)
	root:SetVAnchor(G.ANCHOR_TOP)
	fps_label = root:AddChild(Text(G.NUMBERFONT, 34))
	fps_label:SetPosition(60, -20, 0)
	fps_label:SetColour(1, 1, 0.4, 1)
	fps_label:SetString("-- FPS")
end

-- Runs from Start() once the front end exists, in the menu and again when a world loads
local _CheckControllers = G.CheckControllers
if _CheckControllers then
	G.CheckControllers = function(...)
		G.TheInput:EnableAllControllers()
		if port.show_fps and G.TheFrontEnd then
			add_fps_label()
		end
		return _CheckControllers(...)
	end
end

-- Gamepad focus can't land on a disabled menu item, so disabled buttons strand it. (New Game
-- with the DLCs installed: Start, Character and World stay disabled until a world is picked.
-- The menu's initial focus goes to Start and fails, leaving nothing focused, and Cancel's
-- neighbours are disabled too.) Focus the nearest enabled item and link items past the
-- disabled ones.
local Menu = G.require("widgets/menu")
local _MenuSetFocus = Menu.SetFocus
Menu.SetFocus = function(self, index)
	local items = self.items
	index = index or (self.reverse and -1 or 1)
	if index < 0 then
		index = index + #items + 1
	end
	if items[index] and not items[index].enabled then
		for i = 1, #items do
			if items[index + i] and items[index + i].enabled then
				index = index + i
				break
			elseif items[index - i] and items[index - i].enabled then
				index = index - i
				break
			end
		end
	end
	return _MenuSetFocus(self, index)
end

local _DoFocusHookups = Menu.DoFocusHookups
Menu.DoFocusHookups = function(self, ...)
	_DoFocusHookups(self, ...)
	local fwd, back
	if self.horizontal then
		fwd, back = self.offset > 0 and G.MOVE_RIGHT or G.MOVE_LEFT, self.offset > 0 and G.MOVE_LEFT or G.MOVE_RIGHT
	else
		fwd, back = self.offset > 0 and G.MOVE_UP or G.MOVE_DOWN, self.offset > 0 and G.MOVE_DOWN or G.MOVE_UP
	end
	local function nearest(k, step)
		local items = self.items
		local i = k + step
		while items[i] do
			if items[i].enabled and items[i]:IsVisible() then
				return items[i]
			end
			i = i + step
		end
	end
	for k = 1, #self.items do
		local item = self.items[k]
		if k > 1 then
			item:SetFocusChangeDir(back, function() return nearest(k, -1) end)
		end
		if k < #self.items then
			item:SetFocusChangeDir(fwd, function() return nearest(k, 1) end)
		end
	end
end

AddSimPostInit(function()
	if no_postfx and G.TheFrontEnd then
		local gopts = G.TheFrontEnd:GetGraphicsOptions()
		gopts:SetBloomEnabled(false)
		gopts:SetDistortionEnabled(false)
	end
end)

-- Weather emitters spawn particles from Lua every tick (rain also spawns a
-- splash entity per drop). Scale the per-tick rates only while the emitter
-- update runs, so whatever code sets the rates keeps working unchanged.
local scale = opt("weather_particles", 0.3)
if scale < 1 then
	local function find_emitter(inst)
		local em = G.EmitterManager
		for _, group in ipairs({ em.awakeEmitters, em.sleepingEmitters }) do
			local data = group.infiniteLifetimes[inst] or group.limitedLifetimes[inst]
			if data then
				return data
			end
		end
	end

	local function scale_emitter(inst)
		local data = find_emitter(inst)
		if not data or data.r36s_scaled then
			return
		end
		data.r36s_scaled = true
		local update = data.updateFunc
		data.updateFunc = function()
			local particles, splashes = inst.particles_per_tick, inst.splashes_per_tick
			if particles then inst.particles_per_tick = particles * scale end
			if splashes then inst.splashes_per_tick = splashes * scale end
			update()
			inst.particles_per_tick, inst.splashes_per_tick = particles, splashes
		end
	end

	for _, prefab in ipairs({ "rain", "snow" }) do
		AddPrefabPostInit(prefab, scale_emitter)
	end
end
