-- Loaded after the verified hardware bring-up configuration.
hl.config({ general = { gaps_in = 0, gaps_out = 6 } })

-- Each finger's moves and lift go to the surface it went down on
-- (touch-fingers, which install.sh builds against this Hyprland).
local touch_fingers = os.getenv("HOME") .. "/.local/lib/omarchy-mobile/touch-fingers.so"
local plugin = io.open(touch_fingers, "r")
if plugin then
    plugin:close()
    hl.plugin.load(touch_fingers)
end

-- Pair press/release so the release that wakes the phone cannot sleep it again.
hl.unbind("XF86PowerOff")
hl.bind("XF86PowerOff", hl.dsp.exec_cmd("$HOME/.local/bin/omarchy-mobile-power press"),
    { locked = true, description = "Arm phone power button" })
hl.bind("XF86PowerOff", hl.dsp.exec_cmd("$HOME/.local/bin/omarchy-mobile-power release"),
    { release = true, locked = true, description = "Sleep or wake phone" })

-- Standard media key names; each board exposes its own physical input device.
for key, action in pairs({ XF86AudioRaiseVolume = "up", XF86AudioLowerVolume = "down", XF86AudioMute = "mute" }) do
    hl.unbind(key)
    hl.bind(key, hl.dsp.exec_cmd("quickshell ipc -p $HOME/.config/quickshell/omarchy-mobile/shell.qml call mobile volume " .. action),
        { locked = true, repeating = action ~= "mute", description = "Phone volume " .. action })
end
