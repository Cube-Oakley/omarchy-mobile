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

-- The power key goes to the shell (PowerKey.qml) as a global shortcut: its
-- press and release arrive at once, with no process to start (the earlier
-- omarchy-mobile-power took about 0.4 s for each).
hl.unbind("XF86PowerOff")
hl.bind("XF86PowerOff", hl.dsp.global("omarchy-mobile:power"),
    { locked = true, description = "Phone power button" })

-- Standard media key names; each board exposes its own physical input device.
-- The volume keys go to the shell the same way: Power + Volume Down is a
-- screenshot there, which needs both keys' presses and releases at once, and
-- the shell repeats a held key itself.
for key, name in pairs({ XF86AudioRaiseVolume = "volume-up", XF86AudioLowerVolume = "volume-down", XF86AudioMute = "volume-mute" }) do
    hl.unbind(key)
    hl.bind(key, hl.dsp.global("omarchy-mobile:" .. name), { locked = true, description = "Phone " .. name })
end

-- The lock screen (LockScreen.qml) holds the session lock; Hyprland shows
-- nothing else but these: as pictures (1), or with touch (2) for the
-- always-on display (double tap, the fingerprint sensor), the call screens,
-- the alarm screen, the power menu and the keyboard for a password.
for namespace, mode in pairs({
    ["omarchy-mobile-ambient"] = 2, ["omarchy-mobile-crt"] = 1, ["omarchy-mobile-volume"] = 1,
    ["omarchy-mobile-status"] = 1, ["omarchy-mobile-call"] = 2, ["omarchy-mobile-call-cover"] = 2,
    ["omarchy-mobile-power-menu"] = 2, ["omarchy-mobile-alarm"] = 2, ["wvkbd"] = 2,
    ["omarchy-mobile-keyboard"] = 2,
}) do
    hl.layer_rule({ match = { namespace = "^" .. namespace .. "$" }, above_lock = mode })
end
-- Android's input method is the keyboard bridge (waydroid/keyboard-bridge):
-- its one-pixel window only tells the shell to show the phone's keyboard, so
-- it stays out of sight and focus instead of tiling beside the Android app.
hl.window_rule({
    name  = "android-keyboard-bridge",
    match = { class = "^waydroid\\.InputMethod$" },
    workspace        = "special:android-input silent",
    no_initial_focus = true,
    no_focus         = true,
    no_anim          = true,
})
-- The bridge's other window: the blank screen Android shows just before it
-- sleeps (keyboard-bridge's SleepActivity), so its screen-off black lands
-- there instead of in the app's window. Never seen.
hl.window_rule({
    name  = "android-sleep-screen",
    match = { class = "^waydroid\\.org\\.omarchy\\.mobile\\.keyboard$" },
    workspace        = "special:android-input silent",
    no_initial_focus = true,
    no_focus         = true,
    no_anim          = true,
})
-- A shell that restarts while locked takes the lock again (Hyprland keeps
-- the session locked in between).
hl.config({ misc = { allow_session_lock_restore = true } })
