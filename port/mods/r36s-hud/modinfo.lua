name = "Bigger HUD for small screens"
description = "Scales the in-game HUD (inventory, status, crafting tabs, map) and the hover/speech text, for 3.5\" handheld screens like the R36S. Stacks with the game's own HUD Size option."
author = "dontstarve-recomp"
version = "1.0"
forumthread = ""
api_version = 6
dont_starve_compatible = true
reign_of_giants_compatible = true
shipwrecked_compatible = true
hamlet_compatible = true
icon_atlas = nil
icon = nil
configuration_options = {
    {
        name = "scale",
        label = "HUD scale",
        options = {
            { description = "1.0x", data = 1.0 },
            { description = "1.2x", data = 1.2 },
            { description = "1.3x", data = 1.3 },
            { description = "1.4x", data = 1.4 },
            { description = "1.5x", data = 1.5 },
        },
        default = 1.4,
    },
}
