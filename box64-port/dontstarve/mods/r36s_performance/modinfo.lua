name = "R36S Performance"
description = "Performance profile for the R36S port: 30 FPS engine mode, no bloom/distortion/vignette/canopy, lighter weather particles, no online metrics."
author = "dontstarve-r36s port"
version = "1.0"
forumthread = ""

api_version = 6

dont_starve_compatible = true
reign_of_giants_compatible = true
shipwrecked_compatible = true
hamlet_compatible = true

priority = 100
restart_required = false

local function toggle(name, label, default, hover)
	return {
		name = name,
		label = label,
		hover = hover,
		options = { { description = "On", data = true }, { description = "Off", data = false } },
		default = default,
	}
end

configuration_options =
{
	toggle("netbook", "30 FPS mode", true, "Engine netbook mode: simulation and rendering at 30 Hz."),
	toggle("no_postfx", "Disable bloom/distortion", true, "Removes the full-screen post-processing passes."),
	toggle("hide_vignette", "Hide vignette", true, "Removes the full-screen vignette overlay (same as the mobile versions)."),
	toggle("no_canopy", "Disable Hamlet canopy", true, "Skips the jungle canopy shadow layer."),
	toggle("season_dsp", "Seasonal audio filter", false, "Low-pass filter on sound in winter/caves. Costs CPU under emulation."),
	{
		name = "weather_particles",
		label = "Rain/snow particles",
		hover = "Fraction of the normal particle count.",
		options =
		{
			{ description = "100%", data = 1 },
			{ description = "50%", data = 0.5 },
			{ description = "30%", data = 0.3 },
			{ description = "15%", data = 0.15 },
		},
		default = 0.3,
	},
}
