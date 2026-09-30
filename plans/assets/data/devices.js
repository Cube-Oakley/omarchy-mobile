/* ==========================================================================
   Devices — one entry per supported handset, in display order.
   Each entry needs a matching assets/data/device-<id>.js that sets HW.<id>.
   `short` labels the per-device chips on shared rows.
   `reference: true` marks the device the shared software is developed on:
   software and integration rows without an explicit per-device status count
   as verified there and as "not tried yet" everywhere else.
   ========================================================================== */
DEVICES = [
  {
    id: "oneplus7pro",
    name: "OnePlus 7 Pro",
    short: "OnePlus",
    codename: "guacamole",
    soc: "Snapdragon 855 · Adreno 640",
    reference: true,
    stage: "Daily bring-up",
    summary: "Boots Arch Linux ARM from internal storage into the full touch shell on the GPU. Wi-Fi, "
           + "audio, Bluetooth, sensors, all three rear cameras, LTE data, texts and VoLTE calls work; it "
           + "charges to full and sleeps with calls and texts waking it. The SoC's deepest sleep is unfinished.",
    meta: ["kernel #194 · slot B", "updated 2026-09-27"],
    readme: "devices/oneplus7pro/README.md",
    status: "devices/oneplus7pro/docs/status.md"
  },
  {
    id: "pixel7pro",
    name: "Pixel 7 Pro",
    short: "Pixel",
    codename: "cheetah",
    soc: "Google Tensor G2 (GS201) · Mali-G710",
    stage: "Daily bring-up",
    summary: "Mainline Linux boots from internal storage with no fastboot and runs the shared touch shell on "
           + "the Mali GPU at 120 Hz. Wi-Fi, Bluetooth, speakers and microphones, most sensors, LTE data, "
           + "texts and VoLTE calls work, and it sleeps unplugged with calls and texts waking it. Deeper SoC "
           + "sleep, cameras, GPS and the stalled light/proximity sensor are open.",
    meta: ["mainline 7.3-rc2 · boot_a", "updated 2026-09-30"],
    readme: "devices/pixel7pro/README.md",
    status: "devices/pixel7pro/docs/status.md"
  }
];
