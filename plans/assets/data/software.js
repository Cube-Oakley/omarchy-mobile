/* ==========================================================================
   SOFTWARE page data
   ========================================================================== */
PAGE_SOFTWARE = {
  id: "software",
  nav: "Software",
  cardBlurb: "What you actually touch: the shell, the surfaces, the apps we have to build, and the agent "
           + "layer that makes the whole thing reprogrammable.",
  eyebrow: "Project plan · Software",
  title: "Software",
  blurb: "The Omarchy mobile shell and everything that has to exist for this to be a phone rather than a "
       + "small Linux desktop. The shell is shared (overlay/mobile/); each device adds a small adapter in "
       + "devices/<device>/adapter/. Switch “Status on” to see a single phone.",

  sections: [
    {
      id: "ui",
      title: "Main UI",
      blurb: "The always-present surfaces: bar, shade, launcher, overview, keyboard, gestures.",
      items: [
        { n: "Status bar", s: "ok",
          on: { pixel7pro: { s: "ok", note: "Shows the MAX77759 gauge's battery and charging state and the Wi-Fi signal from real hardware.",
                             ref: "devices/pixel7pro/kernel/battery/README.md" } },
          note: "Battery percentage with charging bolt, Wi-Fi signal, a call chip during calls and an unread-texts "
              + "count; event-driven. The CPU/RAM bars and their 3 s /proc polling are gone: a chip in the "
              + "shade's header opens the performance page instead.",
          ref: "devices/oneplus7pro/docs/sleep-20260927.md" },
        { n: "Notification shade (pull-down)", s: "ok",
          on: { pixel7pro: { s: "ok", note: "The user reports the shade drags smoothly at 120 Hz; its Wi-Fi page lists and joins networks, and the brightness slider and flashlight toggle drive real hardware.",
                             ref: "devices/pixel7pro/docs/status.md" } },
          note: "Drag-down panel with themed detail popups, grouped notifications, heads-up toasts, Wi-Fi/mute/DND "
              + "toggles, battery metrics, Wi-Fi scan and connect, calendar, opt-in weather. Swipe up to close.",
          ref: "devices/oneplus7pro/docs/shell-controls-20260918.md" },
        { n: "App launcher / drawer", s: "ok",
          on: { pixel7pro: { s: "ok", note: "The current alphabetical drawer at a steady 120 fps; the user reports it drags smoothly.",
                             ref: "devices/pixel7pro/docs/smoothness-20260928.md" } },
          note: "Touch launcher on desktop entries, sorted alphabetically (desktop-entry order changed between "
              + "starts); launching through the drawer verified.",
          ref: "devices/oneplus7pro/docs/keyboard-browser-20260918.md" },
        { n: "Workspace & window overview", s: "ok",
          on: { pixel7pro: { s: "ok", note: "Opens smoothly from an app, and from the home screen it now grows in while held; the first open after the shell starts costs one 60–67 ms frame.",
                             ref: "devices/pixel7pro/docs/smoothness-20260928.md" } },
          note: "Card switcher with previews, tap to open, drag onto a card to tile. The swipe up follows the "
              + "finger from the first frame (a recent screen copy, taken only while the phone is in use); "
              + "thumbnails stop copying once taken, and opening another app no longer rebuilds the cards "
              + "mid-animation. Periodic status and theme polling no longer stalls it. Swiping away the last "
              + "app returns to the desktop.",
          ref: "devices/oneplus7pro/docs/smoothness-20260923.md" },
        { n: "Bottom-edge gesture navigation", s: "ok",
          note: "Left = launcher, centre = overview, right = keyboard; finger-tracked sheets with swipe-down "
              + "dismissal. User-confirmed.", ref: "devices/oneplus7pro/docs/mobile-gestures-20260917.md" },
        { n: "On-screen keyboard", s: "ok",
          on: { pixel7pro: { s: "ok", note: "Takes touch input on the SPI touchscreen: the user sent texts from Messages on the Pixel.",
                             ref: "devices/pixel7pro/modem/README.md" } },
          note: "Gesture activation, explicit-tap popup input, swipe-down handle to hide, no duplicate surfaces.",
          ref: "devices/oneplus7pro/docs/keyboard-browser-20260918.md" },
        { n: "Mobile scaling & tiled windows", s: "ok",
          on: { pixel7pro: { s: "ok", note: "Scale 3 on the 1440 × 3120 panel through the Pixel adapter, with several Kitty windows tiled beside the shell surfaces.",
                             ref: "devices/pixel7pro/docs/hyprland-mobile-20260925.md" } },
          note: "Scale appropriate to 1440 × 3120; multiple tiled app windows coexist with the shell surfaces." },
        { n: "Touch with several fingers at once", s: "partial",
          on: { pixel7pro: { s: "partial", note: "Found on the Pixel (a grip on the edge while tapping rotate froze the shell's taps); the plugin loads at every boot, checked over three reboots, but a hand test of the fix is not written up.",
                             ref: "devices/pixel7pro/docs/status.md" } },
          note: "Hyprland 0.56 sends every finger's moves and lift to wherever the last finger landed, which "
              + "stranded the shell's touch and stopped its taps. The shared touch-fingers plugin sends each finger "
              + "to the surface it went down on, and the wallpaper takes stray touches. install.sh builds it "
              + "against the installed Hyprland.",
          ref: "overlay/mobile/README.md" },
        { n: "Shell crash recovery", s: "ok",
          note: "A watchdog starts the shell again about 5 s after it exits, and stops the dead shell's helpers. "
              + "Hyprland 0.56.2 can disconnect the shell when a window closes during a preview capture (a "
              + "compositor bug, still upstream).", ref: "devices/oneplus7pro/docs/shell-fixes-20260924.md" },
        { n: "Notification actions, grouping and dismissal", s: "partial",
          note: "Cards group by app, with expand, per-item and group dismiss, actions, and heads-up toasts; texts "
              + "(with an Open button) and missed calls post notifications. No persistent history, lock-screen "
              + "notifications or banners after reboot.",
          ref: "devices/oneplus7pro/docs/sleep-20260927.md" },
        { n: "Quick-setting toggles (brightness, Bluetooth, DND…)", s: "partial",
          on: { pixel7pro: { s: "partial", note: "The brightness slider and the flashlight toggle work; automatic brightness waits on the stalled light sensor, and Bluetooth pairing is untested by hand.",
                             ref: "devices/pixel7pro/docs/status.md" } },
          note: "Wi-Fi radio, Bluetooth, mute, Do Not Disturb, the flashlight and a brightness slider are in "
              + "the shade; the sun icon beside the slider switches automatic brightness. Bluetooth starts its "
              + "stack when needed. No hotspot yet.",
          ref: "devices/oneplus7pro/docs/controls-20260923.md" },
        { n: "Performance panel", s: "ok",
          note: "A chip in the shade's header opens CPU, memory, load, thermal zones, battery draw and top CPU "
              + "processes. Process ranking is CPU time, not milliwatts.",
          ref: "devices/oneplus7pro/docs/sleep-20260927.md" },
        { n: "Weather tile", s: "partial",
          note: "Current conditions, a 24-hour strip swiped sideways and a five-day forecast from Open-Meteo; the "
              + "location is searched in Settings → Weather. Opt-in.",
          ref: "devices/oneplus7pro/docs/sleep-20260927.md" },
        { n: "New Wi-Fi network entry (password)", s: "partial",
          on: { pixel7pro: { s: "partial", note: "The shade's Wi-Fi page lists and joins networks on the Pixel; a new network's password typed on the phone is not recorded.",
                             ref: "devices/pixel7pro/docs/status.md" } },
          note: "Saved-network activation and HTTPS verified. The password field now takes on-screen keyboard taps "
              + "(the shade no longer grabs exclusive focus); joining a new network by password is not recorded here.",
          ref: "devices/oneplus7pro/docs/notification-shade-20260918.md" },
        { n: "Copy / paste and touch text selection", s: "partial",
          note: "Shell text fields long-press to select, with handles and Copy/Paste/All. Password fields do not "
              + "copy. Kitty long-press selection is in the touch patch but needs a glfw rebuild. Chromium "
              + "selection is unchanged. Desktop sync is not enabled.",
          ref: "devices/oneplus7pro/docs/settings-clipboard-20260919.md" },
        { n: "Touch window resize / move", s: "no", note: "Not implemented." },
        { n: "Auto-rotate", s: "ok",
          on: { pixel7pro: { s: "ok", note: "The same rotate button on the AoC's accelerometer through pixel-sensor-proxy; the screen turns upright both ways, checked by hand.",
                             ref: "devices/pixel7pro/sensors/README.md" } },
          note: "Android's rotate button, by choice: the screen keeps its orientation, and when the phone is "
              + "held another way a button offers to follow it. Both landscape directions checked by hand, touch "
              + "rotating with the picture; upside-down portrait not tried. The accelerometer is released while "
              + "the screen is off.", ref: "devices/oneplus7pro/docs/sensors-20260924.md" },
        { n: "Lock screen, PIN / biometric unlock", s: "no", note: "Not implemented.",
          ref: "docs/mobile-architecture.md" }
      ]
    },

    {
      id: "settings",
      title: "Settings & configuration",
      blurb: "Theme and preference plumbing works, and Settings exists as a themed app. The panel list below "
           + "is what it covers and what it still has to.",
      groups: [
        {
          title: "Working today",
          items: [
            { n: "Theme selection (Omarchy palettes)", s: "ok",
              on: { pixel7pro: { s: "partial", note: "Omarchy theme colours and wallpaper render; switching themes has not been tried on the Pixel.",
                                 ref: "devices/pixel7pro/docs/hyprland-mobile-20260925.md" } },
              note: "Standard Omarchy theme colours apply across the shell, Kitty and keyboard. A theme picked in "
                  + "Settings reaches the shell at once (it watches the palette file); changes made outside the "
                  + "shell are picked up within 30 s.",
              ref: "docs/mobile-architecture.md" },
            { n: "Wallpaper preview, cycling and per-theme choice", s: "ok",
              note: "92 stock images; the chosen wallpaper survives shell restart.",
              ref: "devices/oneplus7pro/docs/wallpaper-switching-20260917.md" },
            { n: "Font handling", s: "ok",
              note: "JetBrainsMono Nerd Font is the default; Appearance in Settings can pick another installed family.",
              ref: "devices/oneplus7pro/docs/settings-clipboard-20260919.md" },
            { n: "Battery detail view", s: "ok",
              on: { pixel7pro: { s: "ok", note: "Percentage, voltage, current, temperature and charge status from the MAX77759 gauge and charger.",
                                 ref: "devices/pixel7pro/kernel/battery/README.md" } },
              note: "Percentage, charge/current direction, voltage and temperature distinguished from input "
                  + "current.", ref: "devices/oneplus7pro/docs/notification-shade-20260918.md" },
            { n: "Volume UI and output routing", s: "partial",
              note: "Android-style panel: the keys change media volume, and an expand button reveals "
                  + "notification, alarm and call volumes as draggable sliders; a tap outside closes it. "
                  + "Connected Bluetooth headphones take over the output with their own volume, which their "
                  + "buttons also drive; the speaker level returns when they leave. No manual output picker.",
              ref: "devices/oneplus7pro/docs/bluetooth-20260922.md" },
            { n: "Choices survive reboot", s: "partial",
              note: "Keyboard, audio and desktop auto-start after reboot is verified; the shade's session-bus "
                  + "startup after reboot is not yet tested.", ref: "devices/oneplus7pro/docs/status.md" }
          ]
        },
        {
          title: "Settings app (to build)",
          note: "Settings exists as a themed app. Rows below are remaining panels.",
          items: [
            { n: "Settings application", s: "partial",
              on: { pixel7pro: { s: "partial", note: "Opens and renders on the Pixel, and Wi-Fi, battery, brightness, audio and Bluetooth now have hardware behind it; its panels have not been checked one by one there.",
                                 ref: "devices/pixel7pro/docs/status.md" } },
              note: "Appearance, Network/Wi-Fi, Display, Sound, Battery, Bluetooth, Weather, About, clipboard and "
                  + "DND. Shade still has the quick toggles. Searchable panels are still ahead.",
              ref: "devices/oneplus7pro/docs/settings-panels-20260919.md" },
            { n: "Panel — Network & Wi-Fi", s: "partial",
              note: "Settings Wi-Fi page plus shade picker, with forget, static DNS, and Automatic (DHCP) or static "
                  + "IPv4 with address, gateway and DNS. Cellular, VPN and hotspot are not. Speed test is HTTP "
                  + "throughput to Cloudflare.",
              ref: "devices/oneplus7pro/docs/calling-20260927.md" },
            { n: "Panel — Display & brightness", s: "partial",
              note: "Settings Display shows the monitor mode, sets the screen timeout (15 s to 10 min, or never) and "
                  + "switches the always-on display. Brightness is read here and changed from the shade.",
              ref: "docs/mobile-architecture.md" },
            { n: "Panel — Sound & output routing", s: "partial",
              note: "Settings Sound and the OSD share the PipeWire volume helper. The OSD's four volume groups "
                  + "are not in Settings yet; no device picker or per-app routing.",
              ref: "devices/oneplus7pro/docs/settings-panels-20260919.md" },
            { n: "Panel — Battery & charging", s: "ok",
              on: { pixel7pro: { s: "ok", note: "The charge limit works on the Pixel: at the limit the phone runs from USB and the battery rests.",
                                 ref: "devices/pixel7pro/kernel/battery/README.md" } },
              note: "Settings Battery shows the shade's metrics, holds charging at a chosen limit (80 % held and "
                  + "resumed on the phone), and sets sleep, the background check and the last sleep's summary.",
              ref: "devices/oneplus7pro/docs/sleep-20260927.md" },
            { n: "Panel — Appearance & theme", s: "ok",
              note: "Theme, wallpaper and font live in the Settings app.",
              ref: "devices/oneplus7pro/docs/settings-clipboard-20260919.md" },
            { n: "Panel — Bluetooth & devices", s: "partial",
              on: { pixel7pro: { s: "partial", note: "The radio is powered at boot and scans; pairing is untested by hand on the Pixel.",
                                 ref: "devices/pixel7pro/kernel/bluetooth/README.md" } },
              note: "Settings Bluetooth powers the radio, scans, pairs, connects, disconnects and forgets; "
                  + "headphones pair and play. Devices that ask for a PIN (most keyboards) cannot pair yet.",
              ref: "devices/oneplus7pro/docs/bluetooth-20260922.md" },
            { n: "Panel — SIM & cellular", s: "no",
              note: "Cellular works on both phones, but Settings has no SIM or cellular page yet: no mobile-data "
                  + "toggle, APN or network-mode choice.",
              ref: "devices/oneplus7pro/docs/cellular-sim-20260926.md" },
            { n: "Panel — Security, lock screen & fingerprint", s: "no",
              note: "Covers credential enrollment and the fingerprint reader, neither of which is enabled.",
              ref: "docs/mobile-roadmap.md" },
            { n: "Panel — Apps & permissions", s: "partial",
              note: "Settings Apps lists installed mobile apps and can revoke a granted permission. There is "
                  + "still no process sandbox.",
              ref: "docs/mobile-architecture.md" },
            { n: "Panel — Storage", s: "partial",
              note: "Settings Storage shows free and used space. Deleting files is done in the Files app.",
              ref: "devices/oneplus7pro/docs/settings-panels-20260919.md" },
            { n: "Panel — Location & sensors", s: "no",
              note: "Sensor permissions for apps, GPS toggles, calibration state.", ref: "devices/oneplus7pro/docs/pathway.md" },
            { n: "Panel — Accessibility", s: "no",
              note: "Text size, contrast, screen reader hooks. Cheap to design in early, expensive to bolt on "
                  + "later.", ref: "docs/mobile-roadmap.md" },
            { n: "Panel — About, updates & recovery", s: "partial",
              note: "Settings About shows hostname, OS, kernel, slot and latest installer backup, with a copyable "
                  + "report. Updates and rollback UI are not started.",
              ref: "devices/oneplus7pro/docs/settings-panels-20260919.md" }
          ]
        },
        {
          title: "Preferences plumbing",
          items: [
            { n: "Unified persistent preferences surface", s: "partial",
              note: "dnd and fontFamily share prefs.json; weather, wallpaper and audio autostart are still "
                  + "separate files.", ref: "devices/oneplus7pro/docs/settings-clipboard-20260919.md" },
            { n: "Themed font picker", s: "ok",
              note: "Settings → Appearance lists installed families and applies them through the theme helper.",
              ref: "devices/oneplus7pro/docs/settings-clipboard-20260919.md" },
            { n: "Full Omarchy theme-repository install", s: "no",
              note: "The bundled theme set works; installing upstream themes as a set is still future work.",
              ref: "docs/mobile-architecture.md" },
            { n: "Status-bar indicator preferences", s: "no",
              note: "Show/hide battery percentage and indicator choice are hardcoded today.",
              ref: "docs/mobile-roadmap.md" }
          ]
        }
      ]
    },

    {
      id: "apps",
      title: "Apps",
      blurb: "Open by design. Desktop Omarchy apps are keyboard-and-mouse shaped, so the phone needs "
           + "touch-first equivalents — several of which do not exist as free software at all and will have to "
           + "be ours. Desktop apps still have a place when docked.",
      groups: [
        {
          title: "Running today",
          items: [
            { n: "Kitty terminal", s: "ok",
              on: { pixel7pro: { s: "ok", note: "Themed Kitty on the GPU desktop; the user saw typed input arrive in it.",
                                 ref: "devices/pixel7pro/docs/hyprland-mobile-20260925.md" } },
              note: "Native Wayland, opt-in touch scroll and long-press word/drag selection, copy-on-select, "
                  + "Nerd Font, Omarchy-branded Fastfetch. Rebuild is v0.48.2-matched; a package upgrade replaces it.",
              ref: "overlay/mobile/kitty-touch/README.md" },
            { n: "Chromium (stock, native Wayland)", s: "ok",
              note: "Sandbox enabled, running in its own restricted mobile-browser account.",
              ref: "devices/oneplus7pro/docs/keyboard-browser-20260918.md" },
            { n: "Grok webapp", s: "ok", note: "App-mode window through the webapp launcher.",
              ref: "devices/oneplus7pro/docs/keyboard-browser-20260918.md" },
            { n: "Webapp launcher", s: "ok", note: "Desktop-entry driven app-mode windows." },
            { n: "pacman package management", s: "ok",
              on: { pixel7pro: { s: "partial", note: "Pacman was verified in the staging root through a temporary host package tunnel. That Arch root is now installed on ext4 userdata; native networking remains unfinished.",
                                 ref: "devices/pixel7pro/docs/hyprland-mobile-20260925.md" } },
              note: "Ordinary sync/install works; the earlier Landlock incompatibility is fixed.",
              ref: "devices/oneplus7pro/docs/status.md" }
          ]
        },
        {
          title: "Phone apps we build for touch",
          note: "The stock set a phone is expected to have. Free-software options that fit a touch phone are "
              + "thin (GNOME Calls and Chatty would pull in about 490 MB and cannot follow the theme), so we "
              + "build these on the shared framework. Phone and Messages pick their telephony backend from the "
              + "device adapter: ModemManager on the OnePlus, the Pixel's own modem service there.",
          items: [
            { n: "Phone / dialer", s: "ok",
              on: { pixel7pro: { s: "ok", note: "Drives the Pixel's modem service: outgoing and incoming calls with two-way audio and mute, user-confirmed, also after reboot. Speaker is unavailable on this backend.",
                                 ref: "devices/pixel7pro/modem/README.md" } },
              note: "Keypad, recents with missed calls, a contacts tab and the call screen (mute, touch tones, "
                  + "speaker, end). VoLTE calls with audio both ways, user-confirmed. Call volume on the "
                  + "volume keys is still to do.",
              ref: "devices/oneplus7pro/docs/calling-20260927.md" },
            { n: "Incoming-call screen", s: "ok",
              on: { pixel7pro: { s: "ok", note: "The user confirmed ringing with the screen off, the display waking, and answering from the shell's screen.",
                                 ref: "devices/pixel7pro/docs/status.md" } },
              note: "A full-screen answer/decline layer over everything: wakes the display, rings through the ring "
                  + "volume group and vibrates, following the alert slider and Do Not Disturb; a green chip returns "
                  + "to the call. Answered from it, user-confirmed.",
              ref: "devices/oneplus7pro/docs/calling-20260927.md" },
            { n: "In-call proximity screen cover", s: "ok",
              on: { pixel7pro: { s: "partial", note: "The shell helper is the same, and the Pixel's proximity chip converts again since September 30; the cover has not been tried in a call yet.",
                                 ref: "devices/pixel7pro/sensors/README.md" } },
              note: "During an earpiece call the shell blanks the screen and swallows touches while proximity reads "
                  + "near, so a cheek cannot end the call; user-confirmed at the ear.",
              ref: "devices/oneplus7pro/docs/calling-20260927.md" },
            { n: "Messaging (SMS/MMS)", s: "partial",
              on: { pixel7pro: { s: "partial", note: "Sending and receiving texts user-confirmed on the Pixel's modem service; an incoming text woke the phone and appeared in Messages. No MMS.",
                                 ref: "devices/pixel7pro/modem/README.md" } },
              note: "Messages: conversations, search, delivery state, part counts, notifications and an unread count "
                  + "even with the app closed; texts both ways, user-confirmed. MMS (pictures, group texts) is not "
                  + "supported.",
              ref: "devices/oneplus7pro/docs/calling-20260927.md" },
            { n: "Contacts", s: "partial",
              note: "One vCard file per contact, with photos, favourites, search, vCard and Google/Outlook CSV "
                  + "import and .vcf export; Phone and Messages read it. Covered by tests, but first real use on the "
                  + "phone is still to come.",
              ref: "devices/oneplus7pro/docs/calling-20260927.md" },
            { n: "Clock (alarm, timer, stopwatch)", s: "no",
              note: "No app yet, and nothing lets a user set an alarm. Alarms have to wake the phone from "
                  + "suspend; RTC alarm wake is verified on the Pixel.",
              ref: "devices/oneplus7pro/docs/background-wake-plan.md" },
            { n: "Gallery / photo viewer", s: "no",
              note: "Blocked behind the camera pipeline for capture, but a viewer over existing files is "
                  + "independent work.", ref: "devices/oneplus7pro/docs/pathway.md" },
            { n: "File manager", s: "partial",
              note: "Files browses, opens, creates and deletes inside the home folder after the user grants "
                  + "files.home. It does not see the rest of the system, and there is no share sheet.",
              ref: "docs/mobile-architecture.md" },
            { n: "Calendar", s: "no",
              note: "The shade already renders calendar data; no app, no account sync.",
              ref: "devices/oneplus7pro/docs/notification-shade-20260918.md" },
            { n: "Weather", s: "partial",
              note: "The shade has current conditions, a 24-hour strip and a five-day forecast, and Settings → "
                  + "Weather picks the place; a real app with several locations does not exist.",
              ref: "devices/oneplus7pro/docs/notification-shade-20260918.md" },
            { n: "Camera", s: "partial", note: "Omarchy Camera, our own app on libcamera's public API so it "
                + "carries to other phones: GPU preview with no CPU copies, tap to focus, 12 MP stills in "
                + "about 1 s, and a swipeable gallery. Exposure keeps up to two stops of highlight headroom, so a "
                + "bright lamp keeps its texture. In dim light a still merges up to eight raw frames, leaving "
                + "out whatever moved, on the sharpest frame; our own renderer takes off lens glare, tone-maps "
                + "locally and keeps night looking like night, close to a Pixel 9 Pro's. Themed with Omarchy; "
                + "the test camera apps are gone. 0.6×, 1× and 3× buttons switch between the ultra-wide, the "
                + "main camera and the telephoto. No video yet.",
              ref: "devices/oneplus7pro/docs/camera-20260922.md" },
            { n: "Settings application", s: "partial",
              on: { pixel7pro: { s: "partial", note: "Opens and renders on the Pixel; its panels have not been checked one by one there.",
                                 ref: "devices/pixel7pro/docs/status.md" } },
              note: "See the Settings & configuration section.",
              ref: "devices/oneplus7pro/docs/settings-clipboard-20260919.md" },
            { n: "Further stock apps (TBD)", s: "no",
              note: "Calculator, notes, tasks, email, music… deliberately unlisted until we decide. Each one "
                  + "should be a framework exercise, not a one-off.", ref: "docs/mobile-roadmap.md" }
          ]
        },
        {
          title: "Shared app framework",
          note: "The thing that turns “we wrote eleven apps” into “anyone can write the twelfth”. Omarchy "
              + "Mobile apps inherit theme, colour, font, density and touch behaviour from the base OS instead "
              + "of re-implementing them.",
          items: [
            { n: "Framework core (shared runtime for mobile apps)", s: "partial",
              note: "OmarchyMobile supplies theme and widgets. A new app is a manifest plus an AppWindow. "
                  + "There is no sandbox or SDK package yet.",
              ref: "docs/mobile-architecture.md" },
            { n: "Theme inheritance from the OS", s: "partial",
              note: "The kit exposes MobileTheme from Omarchy palettes. Settings and Files inherit it through AppWindow.",
              ref: "overlay/mobile/kit/qmldir" },
            { n: "Common touch widgets & layout kit", s: "partial",
              note: "TouchButton, text field, page header, settings row and appearance page are shared. Lists, "
                  + "sheets and selection handles are still ahead.",
              ref: "devices/oneplus7pro/docs/settings-clipboard-20260919.md" },
            { n: "App manifest, launcher & lifecycle integration", s: "partial",
              note: "omarchy-mobile-app reads a manifest, launches one window, and replaces that window if it is "
                  + "opened again. Suspend and resume rules are not defined.",
              ref: "docs/mobile-architecture.md" },
            { n: "Permissions & sandbox model", s: "partial",
              note: "An app declares permissions from a fixed list and the user grants them on first launch. "
                  + "The file helper enforces files.home. There is no process sandbox, and contacts, location, "
                  + "camera and notifications are not in the list yet.",
              ref: "docs/mobile-architecture.md" },
            { n: "Public SDK, docs and examples", s: "no",
              note: "The point of the framework: other people building Omarchy Mobile apps without asking us.",
              ref: "docs/mobile-roadmap.md" },
            { n: "App distribution & update path", s: "no",
              note: "pacman exists, but there is no app-facing story for signing, channels or updates.",
              ref: "devices/oneplus7pro/docs/status.md" }
          ]
        },
        {
          title: "Browser (touch-centred)",
          items: [
            { n: "Touch-centred Chromium fork", s: "no",
              note: "Stock Chromium assumes a mouse and a tab strip. A fork (or a deeply themed wrapper) is "
                  + "likely: touch input handling, chrome/UI, gestures, theming from the OS, media policies.",
              ref: "devices/oneplus7pro/docs/keyboard-browser-20260918.md" },
            { n: "In-browser touch text selection & paste", s: "no",
              note: "Depends on the same selection work as the shell.", ref: "docs/mobile-roadmap.md" },
            { n: "Browser audio", s: "ok",
              note: "YouTube in Chromium plays clean and audible through the media volume group. Loudness stays "
                  + "below the speaker safety cap.", ref: "devices/oneplus7pro/docs/speakers-20260922.md" },
            { n: "Smooth web video playback", s: "no",
              note: "Streaming reported choppy; decode path (Venus vs CPU vs GPU) diagnosed separately.",
              ref: "devices/oneplus7pro/docs/next-session.md" }
          ]
        },
        {
          title: "Desktop apps (docked)",
          note: "Only valuable with a keyboard, a mouse and a screen. Kept available for the docked case, "
              + "never a substitute for the touch apps.",
          items: [
            { n: "Standard Omarchy desktop app set", s: "no",
              note: "Files, editor, media, office… keyboard-and-mouse driven. Decide per app whether it ships "
                  + "on the phone at all.", ref: "docs/mobile-roadmap.md" },
            { n: "Docked session (external display + keyboard + mouse)", s: "no",
              note: "Gated on USB-C DisplayPort and USB host support.", ref: "docs/mobile-roadmap.md" },
            { n: "Desktop-mode shell layout when docked", s: "no",
              note: "Different density, bar, window rules and input handling than the phone layout.",
              ref: "docs/mobile-architecture.md" }
          ]
        },
        {
          title: "Android apps (Waydroid)",
          note: "For the apps that only exist on Android, such as banking, car apps and WhatsApp as the "
              + "primary device. Waydroid runs a full Android system in a container that shares the "
              + "phone's kernel, with each Android app as a window in the shell. Native apps stay the "
              + "default; this is the escape hatch.",
          items: [
            { n: "Kernel support (binder, binderfs)", s: "no",
              note: "Waydroid needs Android's binder IPC in the kernel; neither phone's kernel enables it "
                  + "yet.", ref: "docs/mobile-roadmap.md" },
            { n: "Graphics and display", s: "no",
              note: "Android draws through the host's Mesa stack (GBM). Software rendering works as a "
                  + "fallback, but scrolling needs the GPU path.", ref: "docs/mobile-roadmap.md" },
            { n: "Shell integration", s: "no",
              note: "Android apps in the launcher and the switcher, notifications in the shade, the "
                  + "keyboard and rotation passed through, and the container started on demand rather "
                  + "than kept running.", ref: "docs/mobile-architecture.md" },
            { n: "Hardware passthrough", s: "no",
              note: "Network and audio pass through; the camera, Bluetooth, GPS and telephony do not by "
                  + "default. That rules out apps whose core feature is Bluetooth, such as a car's phone "
                  + "key; those need native clients.", ref: "docs/mobile-roadmap.md" },
            { n: "Google Play services", s: "no",
              note: "Optional: some apps need them for sign-in or push notifications. A Play-free image "
                  + "comes first, with Play services installed only by the user's choice.",
              ref: "docs/mobile-roadmap.md" }
          ]
        }
      ]
    },

    {
      id: "advanced",
      title: "Advanced features",
      blurb: "Platform capability rather than surfaces: rendering, sessions, tooling, power behaviour.",
      items: [
        { n: "GPU Wayland desktop (Hyprland + Quickshell)", s: "ok",
          on: { pixel7pro: { s: "ok", note: "Starts by itself on every normal boot from boot_a and renders on the Mali-G710 at 120 Hz; shell animations average 111–115 fps, which the user reports smooth.",
                             ref: "devices/pixel7pro/docs/native-boot-20260927.md" } },
          note: "Starts automatically on boot with FD640 rendering and native scanout.",
          ref: "devices/oneplus7pro/docs/native-display-work-20260917.md" },
        { n: "Automated UI / backend / hardware-policy tests", s: "ok",
          note: "Gesture, backend and policy checks under tests/, run against the live session.", ref: "tests/" },
        { n: "Guarded build, flash and rollback tooling", s: "ok",
          on: { pixel7pro: { s: "ok", note: "A new image is RAM-tested, then written to boot_a only while that exact kernel runs, with direct readback; pixel-reboot.py moves between Linux and fastboot without buttons, and earlier images are kept for rollback.",
                             ref: "devices/pixel7pro/docs/native-boot-20260927.md" } },
          note: "Explicit target identity, image/hash and slot checks, frozen checkpoints and rollback images.",
          ref: "devices/oneplus7pro/README.md" },
        { n: "Reusable shell across devices", s: "partial",
          on: { pixel7pro: { s: "partial", note: "The current shared shell installs with overlay/mobile/install.sh and the Pixel adapter (display profile, telephony backend, restart and suspend delegates); still installed by hand.",
                             ref: "devices/pixel7pro/adapter/README.md" } },
          note: "Shared UI in overlay/mobile/, one adapter per device. The Pixel 7 Pro runs it unchanged as a second "
              + "device; both now live in one repository, but the shell is still installed by hand, not assembled "
              + "per device.",
          ref: "docs/shared-os-20260925.md" },
        { n: "Standard user session + sudo", s: "no",
          note: "The bring-up desktop runs as root; the mobile-browser account is a temporary Chromium bridge, "
              + "not the intended session design.", ref: "docs/mobile-roadmap.md" },
        { n: "Power-key CRT screen close / open", s: "ok",
          on: { pixel7pro: { s: "ok", note: "The power key, now on a wake-up interrupt, plays the shared CRT close and open, and wakes the phone from s2idle.",
                             ref: "devices/pixel7pro/kernel/keys/README.md" } },
          note: "Shared CrtPower.qml animation runs before panel-off and during wake; hardware suspend remains device-specific.",
          ref: "overlay/mobile/README.md" },
        { n: "Power menu (hold the power key)", s: "partial",
          on: { oneplus7pro: { s: "no", note: "The OnePlus adapter has no restart script yet, so the menu offers no actions here." },
                pixel7pro: { s: "partial", note: "Restart and Restart to bootloader run pixel-reboot, which stops the modem and lets PID 1 sync storage first; a hand test is not written up.",
                             ref: "devices/pixel7pro/adapter/README.md" } },
          note: "Holding the power key for two seconds opens a themed menu; a tap still switches the display. Its "
              + "restart actions come from a device adapter. There is no Power off entry yet.",
          ref: "devices/pixel7pro/adapter/README.md" },
        { n: "Automatic idle / sleep policy", s: "ok",
          on: { pixel7pro: { s: "partial", note: "The same policy delegates to a guarded s2idle helper that refuses on the charger, in calls or without the modem supervisor; five automatic unplugged sleeps were recorded, and a 15-minute RTC fallback stays on pending longer tests.",
                             ref: "devices/pixel7pro/docs/suspend-20260929.md" } },
          note: "A screen timeout, then s2idle 10 s after the screen goes dark, never during a call, audio or on "
              + "the charger; each wake is logged with its cause. The user saw it sleep and wake on the power key.",
          ref: "devices/oneplus7pro/docs/sleep-20260927.md" },
        { n: "Background wake for delayed delivery", s: "partial",
          on: { pixel7pro: { s: "partial", note: "Incoming calls and texts wake the AP from s2idle, and an RTC alarm bounds each sleep; nothing else schedules wakes.",
                             ref: "devices/pixel7pro/docs/suspend-20260929.md" } },
          note: "Calls and texts wake the phone, and a background check wakes it every 15 minutes by default. "
              + "Scheduled wakes for apps, alarms and agent tasks are still design only.",
          ref: "devices/oneplus7pro/docs/sleep-20260927.md" },
        { n: "Measured, repeatable battery life", s: "no",
          on: { pixel7pro: { s: "no", note: "Only USB-input and partial-rail figures (1.47 W screen-off idle on Wi-Fi; metered rails 0.81–0.85 W across sleeps); no unplugged drain measured.",
                             ref: "devices/pixel7pro/docs/suspend-20260929.md" } },
          note: "Only short informal samples exist (131 mA screen-off awake vs 83 mA suspended; 85%→78% over "
              + "~3h45m). No claim is defensible yet.", ref: "devices/oneplus7pro/docs/idle-measurement-20260917.md" },
        { n: "Secure / verified boot path", s: "no",
          note: "Verification is disabled for bring-up; a production boot story is still open." }
      ]
    },

    {
      id: "ai",
      title: "AI & agents",
      blurb: "The phone is meant to be agentic and malleable, like desktop Omarchy — a default agent, room to "
           + "install the coding harnesses we already use, and a small model that keeps working with no network "
           + "at all.",
      groups: [
        {
          title: "Agent platform",
          items: [
            { n: "Default system agent", s: "no",
              note: "The phone's own agent: knows the shell, the settings, the apps and the user's files. Nothing "
                  + "exists yet.", ref: "docs/mobile-roadmap.md" },
            { n: "Phone-control API for agents", s: "no",
              note: "A stable, discoverable interface for reading state and making changes — brightness, theme, "
                  + "network, Do Not Disturb, launching apps, dismissing notifications. Today these are "
                  + "shell-internal Quickshell calls with no boundary.", ref: "docs/mobile-architecture.md" },
            { n: "Permission & confirmation model for agent actions", s: "no",
              note: "Which changes are silent, which ask, which are never allowed — and how that reads on a "
                  + "touch screen.", ref: "docs/mobile-roadmap.md" },
            { n: "Agent surface in the shell", s: "no",
              note: "Where the agent lives: a gesture, a launcher entry, a panel, on-screen context about the "
                  + "focused window.", ref: "docs/mobile-roadmap.md" }
          ]
        },
        {
          title: "Coding harnesses",
          items: [
            { n: "Install & drive harnesses (pi, Codex, Claude Code, Grok, Hermes)", s: "partial",
              note: "Grok runs as a webapp and ordinary Arch packages install fine, but no harness has been "
                  + "installed and driven end-to-end from the touch session.",
              ref: "devices/oneplus7pro/docs/keyboard-browser-20260918.md" },
            { n: "Terminal ergonomics for real CLI work", s: "partial",
              note: "Kitty with touch scrolling and the on-screen keyboard work; no text selection, copy/paste or "
                  + "swipe typing, which is what working at arm's length actually needs.",
              ref: "devices/oneplus7pro/docs/mobile-work-20260917.md" },
            { n: "Long-running sessions that survive suspend", s: "no",
              note: "A build or an agent run should not die because the screen went to sleep; needs job keeping "
                  + "plus a wake policy.", ref: "devices/oneplus7pro/docs/background-wake-plan.md" }
          ]
        },
        {
          title: "On-device model",
          items: [
            { n: "Local inference runtime", s: "no",
              note: "Nothing installed. CPU-only is the certain baseline; whether GLES/Turnip Vulkan is usable "
                  + "for inference on this GPU is unmeasured, and the NPU has no mainline path.",
              ref: "devices/oneplus7pro/docs/pathway.md" },
            { n: "Small offline chat / general-knowledge model", s: "no",
              note: "What fits in 8 GB alongside a compositor, and at what quality, is unknown until measured.",
              ref: "devices/oneplus7pro/docs/pathway.md" },
            { n: "Thermal & battery budget for inference", s: "no",
              note: "How long we may run a model before the handset is too hot to hold or the battery is gone. "
                  + "No thermal policy exists to enforce a limit.", ref: "devices/oneplus7pro/docs/idle-measurement-20260917.md" },
            { n: "Model management (download, swap, memory limits)", s: "no",
              note: "Choosing a model, fetching it, keeping it off the critical path, and falling back to the "
                  + "cloud when a task is too big." },
            { n: "On-device settings & device control", s: "no",
              note: "“Turn on Do Not Disturb at nine”, “switch to the dark theme”, “find the log from the failed "
                  + "flash” — local agent actions with no network round trip. Depends on the phone-control API.",
              ref: "docs/mobile-architecture.md" },
            { n: "Fully offline agent features", s: "no",
              note: "The reason local matters: everything still works in airplane mode, off-grid, or with no "
                  + "account. Nothing built yet." }
          ]
        }
      ]
    }
  ]
};
