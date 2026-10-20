name = "Bigger HUD for small screens"
description = "Scales the in-game HUD (inventory, status, crafting tabs, map) and the hover/speech text, for 3.5\" handheld screens like the R36S. Stacks with the game's own HUD Size option. Bigger text everywhere (Text size). Optional 30 fps cap."
author = "dontstarve-recomp"
version = "1.1"
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
    {
        name = "text",
        label = "Text size",
        hover = "Every text in the game: HUD items, recipes, menus, dialogs",
        options = {
            { description = "1.0x", data = 1.0 },
            { description = "1.1x", data = 1.1 },
            { description = "1.25x", data = 1.25 },
            { description = "1.4x", data = 1.4 },
        },
        default = 1.25,
    },
    {
        name = "fps",
        label = "Frame rate cap",
        hover = "30 halves the work per second; the game itself simulates at 30 Hz",
        options = {
            { description = "60", data = 60 },
            { description = "30", data = 30 },
        },
        default = 60,
    },
}
