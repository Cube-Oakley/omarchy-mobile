-- Bounded software-rendering feasibility test, not the shared Omarchy desktop.
hl.monitor({ output = "DSI-1", mode = "preferred", position = "auto", scale = 3 })
hl.config({
    general = { gaps_in = 0, gaps_out = 0, border_size = 1 },
    decoration = { rounding = 0, shadow = { enabled = false }, blur = { enabled = false } },
    animations = { enabled = false },
    -- Touch only: never draw a pointer, like the OnePlus configuration.
    cursor = { invisible = true, no_hardware_cursors = 1 },
    misc = { disable_hyprland_logo = true, disable_splash_rendering = true,
             disable_watchdog_warning = true },
})
hl.bind("SUPER + Q", hl.dsp.exec_cmd("weston-terminal"))
hl.bind("SUPER + C", hl.dsp.window.close())
