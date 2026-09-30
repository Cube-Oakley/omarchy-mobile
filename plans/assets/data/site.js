/* Site-wide copy shared by all pages. Device facts live in devices.js. */
SITE = {
  meta: ["updated 2026-09-30", "shared shell · overlay/mobile"],

  /* Where `ref` paths open when the pages are served from GitHub Pages.
     Opened from a checkout (file://), refs open the local file instead. */
  source: "https://github.com/Cube-Oakley/omarchy-mobile/blob/main/",

  landing: {
    eyebrow: "Project plan · overview",
    title: "Omarchy Mobile",
    blurb: "Arch Linux ARM + Hyprland + Quickshell shaped into a touch phone we can actually reprogram: one "
         + "mobile shell and one OS, brought up handset by handset. Each device has its own hardware page; the "
         + "software you touch and the link to a desktop are shared, and every row says which phones it has "
         + "been verified on.",
    meta: ["updated 2026-09-30"]
  },

  howTo: [
    "<b>Working</b> means verified on the handset — not “the driver probed”. Where a check was only "
    + "automated, or only seen once, the note says so.",
    "<b>Partial</b> means the plumbing is up but the user-visible result is unproven, capped, or only "
    + "one of several paths works. Pixel suspend is the current example: the phone sleeps when unplugged and "
    + "calls, texts and alarms wake it, but the SoC never reaches its deeper sleep states, so the saving is modest.",
    "<b>Not working</b> means nothing usable yet, including “not attempted so far”.",
    "<b>Not present</b> means this handset simply has no such hardware — tracked so nobody spends a "
    + "week looking for a headphone jack.",
    "<b>Not tried yet</b> appears on shared software and integration rows: the shared code exists, but nobody "
    + "has checked it on that device. It is not a failure, only missing evidence.",
    "Hardware is per device. <b>Compare</b> rolls each device's rows up into the same list of capabilities, so "
    + "a partial cell means some of the rows behind it work and some do not.",
    "Plain HTML, no build step, no network dependencies. Open any page directly from disk."
  ],

  open: [
    "<b>Device support contract.</b> What a device directory must provide (manifest, kernel pin, boot recipe, "
    + "capabilities) and how the shared OS picks adapters is described in <code>docs/shared-os-20260925.md</code>; "
    + "the <code>os/</code>, <code>packages/</code> and <code>sources.lock</code> parts are not built yet.",
    "<b>App set.</b> Desktop Omarchy apps are keyboard-and-mouse shaped; on the phone we need touch-first "
    + "equivalents, and several do not exist anywhere yet. Which apps we build, in what order, and which "
    + "desktop apps stay available when docked is still open — see <b>Software → Apps</b>.",
    "<b>App framework.</b> Nothing decided about the stack the Omarchy Mobile apps will be built on "
    + "(Quickshell/QML modules, a thin runtime, an app manifest, how theming is inherited, how third "
    + "parties ship apps). See <b>Software → Apps → Shared app framework</b>.",
    "<b>Browser.</b> Stock Chromium is usable but not touch-centred; a fork is likely. Scope of the fork "
    + "(input, chrome/UI, windowing, media) is undecided.",
    "<b>AI surface.</b> What the default agent is, what the on-device model is for, and exactly what the "
    + "phone-control API exposes to an agent — including how an agent action gets confirmed — is open.",
    "<b>Sensor paths.</b> Settled by the hardware on both phones: the OnePlus 7 Pro's sensors stream through "
    + "the sensor DSP's SEE interface (libssc and iio-sensor-proxy, as on the 7T Pro), and the Pixel 7 Pro's "
    + "through the AoC's USF, served by our own proxy on the same D-Bus API. Open: what the shell and agents "
    + "consume beyond rotation, brightness and in-call proximity.",
    "<b>Integration transport.</b> USB, LAN or relay; which existing protocols we evaluate before writing "
    + "anything; and how pairing, per-feature permissions and end-to-end privacy are framed. See "
    + "<b>Integration → Foundations to decide</b>.",
    "<b>Ship order.</b> Both phones now boot from internal storage, text and make VoLTE calls. On the Pixel "
    + "7 Pro the order is deeper SoC sleep (SICD and a real deep suspend), cameras, proximity and ambient "
    + "light (the TMD3719 stall), GPS, and confirming a charge to 100 %. On the OnePlus 7 Pro it is a "
    + "measured sleep drain, then the CX/MX sleep votes for its deepest state, sleeping on the charger, call "
    + "volume on the keys and MMS. The sequence still moves with each result."
  ]
};
