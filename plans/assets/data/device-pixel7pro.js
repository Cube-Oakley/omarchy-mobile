/* ==========================================================================
   Pixel 7 Pro (cheetah) — hardware bring-up
   s:   "ok" working · "partial" running, unproven · "no" not working · "absent" no such hardware
   cap: capability id from capabilities.js; the Compare page rolls rows up by it
   ========================================================================== */
HW.pixel7pro = {
  blurb: "Everything physically present in the Pixel 7 Pro, plus the platform pieces the phone depends on "
       + "(boot chain, firmware interfaces, power management, storage). Every result comes from mainline "
       + "Linux 7.3-rc2, which boots from boot_a into Arch Linux ARM on internal storage and the shared "
       + "mobile shell, with no fastboot involved. Android is gone; the bootloader and saved images are the "
       + "recovery path.",

  sections: [
    {
      id: "soc",
      title: "SoC & platform",
      items: [
        { n: "Tensor G2 CPU (8 cores, three clusters)", s: "ok", cap: "cpu",
          note: "All eight CPUs online under mainline 7.3-rc2, with working GIC and timer interrupts.",
          ref: "devices/pixel7pro/docs/native-shell-20260925.md" },
        { n: "CPU frequency scaling", s: "ok", cap: "cpufreq",
          note: "Three schedutil policies through ACPM at the full stock tables (to 2.85 GHz), with the stock "
              + "cross-cluster floor. A 20 ms thermal thread gives bursts about 0.75 s at the top rate and holds "
              + "sustained load at 2.05 GHz; there is no energy model yet.",
          ref: "devices/pixel7pro/docs/smoothness-20260928.md" },
        { n: "CPU idle (C2 and cluster power-down)", s: "ok",
          note: "C2 works now that the PMU hints the firmware needs are sent, and the MCT is the tick broadcast "
              + "device; mid and big clusters power down when all their CPUs idle. Idle CPUs had spun on "
              + "rejected C2 entries before: screen-on draw fell from 3.01 to 2.09 W with the domain work below.",
          ref: "devices/pixel7pro/kernel/cpupm/README.md" },
        { n: "Mali-G710 MC7 GPU", s: "ok", cap: "gpu",
          note: "Upstream Panthor with CSF firmware; Hyprland and the shell render on it, and shell animations "
              + "average 111–115 fps at 120 Hz, which the user reports as smooth. Needs an isolated Mesa build "
              + "carrying a missing G710 model entry.",
          ref: "devices/pixel7pro/docs/smoothness-20260928.md" },
        { n: "GPU frequency scaling & cooling", s: "ok",
          note: "Scales over the stock 302–885 MHz table with the shader clock paired, a 603 MHz floor, and a "
              + "thermal cap from 60 °C (checked with emulated temperatures). It refuses to reclock a power-gated "
              + "GPU, which reset the phone; the 996 MHz level is not exposed.",
          ref: "devices/pixel7pro/docs/smoothness-20260928.md" },
        { n: "RAM (12 GB)", s: "ok",
          note: "In normal use: the installed desktop runs beside the AoC and the modem's RAM-only runtime. "
              + "Memory and swap policy are not tuned.",
          ref: "devices/pixel7pro/docs/device.md" },
        { n: "UFS storage (256 GB)", s: "ok", cap: "storage",
          note: "Arch runs from ext4 userdata. Google's GS201 calibration gives HS gear 4 rate B on two lanes "
              + "(1.2–1.8 GB/s, up from 0.57 MB/s), so the desktop starts in seconds; a device reset at probe ends "
              + "the old first-probe failure. The bootloader still owns the clocks, and Hibern8 in suspend is "
              + "opt-in, off by default.",
          ref: "devices/pixel7pro/kernel/storage/README.md" },
        { n: "Boot chain (unlocked bootloader, boot_a)", s: "ok", cap: "boot",
          note: "Linux starts from boot_a on a normal power-on or reboot with no fastboot: USB SSH returns in "
              + "about 16–18 s and the desktop starts by itself. New images are RAM-tested first, then written "
              + "with direct readback; slot A is marked successful (a user-approved one-bit repair).",
          ref: "devices/pixel7pro/docs/native-boot-20260927.md" },
        { n: "Bootloader and saved-image recovery", s: "ok", cap: "recovery",
          note: "A warm reset reaches fastboot without buttons (scripts/pixel-reboot.py); Power + Volume Down is "
              + "the fallback, and saved host images provide recovery. Since September 30 the previous boot's "
              + "kernel console survives in ramoops. Slot B is not a fallback.",
          ref: "devices/pixel7pro/kernel/reboot/README.md" },
        { n: "ACPM firmware interface (clocks, temperatures, PMIC)", s: "ok",
          note: "Mailbox and ACPM answer clock, temperature and PMIC requests; CPU scaling, the RTC, the power "
              + "key and the power meters all run through it.",
          ref: "devices/pixel7pro/docs/acpm-bringup-20260925.md" },
        { n: "Thermal sensing & throttling", s: "ok", cap: "thermal",
          note: "Seven thermal zones, a 20 ms CPU thermal thread and a GPU cap. Under real load all eight cores "
              + "hold 75–81 °C by throttling with no thermal reboot. Trips are conservative bring-up values, "
              + "below stock's 100 °C control point.",
          ref: "devices/pixel7pro/docs/smoothness-20260928.md" },
        { n: "Unused power domains", s: "partial",
          note: "TPU, AUR, video codecs, G2D, EH and the camera pipeline are switched off at boot with Google's "
              + "sequences. The camera capture domains (CSIS, PDP) come back on for each stream with their saved "
              + "state, secure context and S2MPUs restored; TPU and codecs still have no on path.",
          ref: "devices/pixel7pro/kernel/pd/README.md" },
        { n: "Power meters (ODPM)", s: "ok",
          note: "Reads 24 PMIC rails through ACPM for measurements; loaded by hand, not at boot. It meters only "
              + "part of the phone, so it cannot stand in for battery drain.",
          ref: "devices/pixel7pro/kernel/odpm/README.md" },
        { n: "Tensor TPU (AI accelerator)", s: "no",
          note: "No driver; its power domain is switched off at boot.", ref: "devices/pixel7pro/kernel/pd/README.md" },
        { n: "Watchdogs", s: "partial",
          note: "The modem manager arms an AP watchdog and feeds it, and pixel-wdt-pm pauses and rearms it across "
              + "suspend (a 44.8 s sleep resumed in the same boot). Without the modem bundle both watchdogs stay "
              + "stopped.", ref: "devices/pixel7pro/kernel/watchdog/README.md" },
        { n: "Suspend / resume", s: "partial", cap: "suspend",
          note: "The shell sleeps the phone 10 s after the screen goes dark while unplugged, now in SYS_SLEEP "
              + "(below); the power key, RTC alarms, calls and texts wake it. Wi-Fi and Bluetooth are unloaded "
              + "while dark and come back for the background check (Wi-Fi) and when the screen lights. It never "
              + "sleeps on the charger. s2idle remains the fallback when deep sleep isn't available.",
          ref: "devices/pixel7pro/docs/suspend-20260930.md" },
        { n: "Deepest idle power states (SYS_SLEEP)", s: "partial", cap: "deepsleep",
          note: "The daily sleep since October 1 (installed boot image): storage, buses, USB, GPU, display, "
              + "cameras, touch and the modem (still IMS-registered) come back after every sleep. Unplugged with the "
              + "modem registered, a 40-minute run averaged 63.6 mA, against about 300 mA in s2idle; the phone "
              + "was asleep 95% of the time, and asleep it draws about 50 mA. Next: that floor, toward ~25 mA.",
          ref: "devices/pixel7pro/docs/suspend-20260930.md" },
        { n: "RTC and alarms", s: "ok",
          note: "The S2MPG12 RTC sets the clock about 2 s into boot, and its alarm wakes the phone from s2idle "
              + "(rtcwake, repeated cycles). Read-only: Linux cannot correct its drift (about 30 s ahead).",
          ref: "devices/pixel7pro/kernel/rtc/README.md" },
        { n: "Power button", s: "ok", cap: "buttons",
          note: "On a wake-up interrupt: clean press/release, the screen toggles through the shell, and a press "
              + "wakes the phone from s2idle. Holding it two seconds opens the power menu.",
          ref: "devices/pixel7pro/kernel/keys/README.md" },
        { n: "Volume up / down keys", s: "partial", cap: "buttons",
          note: "Both report clean press/release pairs on wake-up interrupts and open the volume overlay. That "
              + "they now move the level through the new audio stack is not recorded.",
          ref: "devices/pixel7pro/kernel/keys/README.md" },
        { n: "Battery gauge (MAX77759)", s: "ok", cap: "battery",
          note: "Percentage, voltage, current, temperature and charge status reach the shell. On September 30 the "
              + "stock battery model was restored (5,002 mAh instead of the 3,000 mAh default), which "
              + "recalculated the percentage from 100 to 92 %.",
          ref: "devices/pixel7pro/kernel/battery/README.md" },
        { n: "Charging", s: "ok", cap: "charging",
          note: "1.5 A from USB instead of the bootloader's 500 mA, and the stock step-charging tables up to a "
              + "4.45 V float (the bootloader's 4.35 V had stopped it at 92 %): it now charges to 100 % "
              + "(5,016 mAh at 4.42 V). The charge limit works; it pauses at 45 °C.",
          ref: "devices/pixel7pro/kernel/battery/README.md" },
        { n: "Fast charging", s: "no", cap: "fastcharge",
          note: "5 V only: no USB PD and no BC1.2 port detection, so the 1.5 A limit applies to any port.",
          ref: "devices/pixel7pro/kernel/battery/README.md" },
        { n: "Wireless charging", s: "no", cap: "wireless", note: "Not attempted yet." },
        { n: "True power-off / shutdown", s: "no", cap: "poweroff",
          note: "The power menu offers Restart and Restart to bootloader, both orderly (modem stopped, storage "
              + "synced), but there is no Power off yet; ordinary shutdown still becomes a reboot.",
          ref: "devices/pixel7pro/adapter/README.md" },
        { n: "Security chip / hardware keystore", s: "no", note: "Not attempted yet." }
      ]
    },

    {
      id: "display",
      title: "Display & touch",
      items: [
        { n: "OLED panel, 1440 × 3120 (Samsung S6E3HC4, DSI)", s: "ok", cap: "display",
          note: "Native DMA scanout with real page flips and correct colours, confirmed on the phone. Linux still "
              + "relies on the bootloader's panel, PLL and PHY setup.",
          ref: "devices/pixel7pro/docs/display-direct-20260925.md" },
        { n: "120 Hz refresh mode", s: "ok", cap: "refresh",
          note: "120 Hz on every normal boot, whose memory clock already covers it; the launcher and shade loop "
              + "runs at a steady 120 fps and the user reports it smooth. A shared bandwidth governor is still "
              + "to come.",
          ref: "devices/pixel7pro/docs/smoothness-20260928.md" },
        { n: "Panel power off / wake", s: "ok", cap: "display",
          note: "The power key plays the shared CRT close and open; the panel sleeps while dark (saving 0.32 W) "
              + "and gets its DSC setup again on wake, including after s2idle.",
          ref: "devices/pixel7pro/docs/status.md" },
        { n: "Backlight / brightness control", s: "ok", cap: "brightness",
          note: "The panel's brightness register is a backlight device; the shell's slider works and the level "
              + "is restored at session start. Automatic brightness waits on the light sensor.",
          ref: "devices/pixel7pro/docs/status.md" },
        { n: "Always-on / ambient display", s: "no",
          note: "Not checked on the Pixel. The shared ambient clock would keep the panel in its normal mode and "
              + "the phone awake. Planned: the S6E3HC4's low-power mode, with the SoC in SYS_SLEEP between "
              + "minute updates.",
          ref: "docs/mobile-roadmap.md" },
        { n: "Multitouch (Synaptics S3908)", s: "ok", cap: "touch",
          note: "On the SPI0 controller with its attention interrupt: about 240 Hz for 0.12 ms of CPU per frame, "
              + "user-confirmed smooth, with recovery after controller errors. Still a register-level driver, "
              + "not a Linux SPI controller plus a TouchComm client.",
          ref: "devices/pixel7pro/docs/touch-spi-20260928.md" },
        { n: "Haptic / vibration motor (CS40L26A)", s: "partial", cap: "haptics",
          note: "ROM click and buzz effects through the standard force-feedback device; measured at the USB "
              + "input and heard by the microphones, but not yet confirmed by hand. Strength needs RAM firmware.",
          ref: "devices/pixel7pro/kernel/haptics/README.md" }
      ]
    },

    {
      id: "audio",
      title: "Audio",
      items: [
        { n: "Audio DSP (AoC)", s: "ok",
          note: "Google's Trusty, GSA and AoC drivers, ported without pKVM, load the stock AoC firmware, which "
              + "comes online with 82 services at every boot. Restarting it needs a reboot.",
          ref: "devices/pixel7pro/kernel/aoc/README.md" },
        { n: "Loudspeaker (bottom, CS35L41)", s: "ok", cap: "speaker",
          note: "Mainline cs35l41 on SPI7 with the stock protection firmware, this phone's factory calibration "
              + "and the stock gain: loud and undistorted at full volume by ear.",
          ref: "devices/pixel7pro/kernel/audio/README.md" },
        { n: "Earpiece receiver (top, CS35L41)", s: "ok", cap: "earpiece",
          note: "The top speaker plays its own channel under the same protection. Call audio runs through the "
              + "AoC to the amplifiers, and the user confirmed two-way audio on outgoing and incoming calls.",
          ref: "devices/pixel7pro/modem/README.md" },
        { n: "Speakerphone in calls", s: "no",
          note: "The Speaker control is unavailable on the Pixel's telephony backend, and speakerphone gain is "
              + "untuned.", ref: "devices/pixel7pro/modem/README.md" },
        { n: "Microphones", s: "ok", cap: "mic",
          note: "Their PMIC rails switched on through ACPM; a recorded voice plays back clearly, and calls use a "
              + "+6 dB boost the user confirmed louder and undistorted.",
          ref: "devices/pixel7pro/kernel/audio/README.md" },
        { n: "Application playback (PipeWire)", s: "ok",
          note: "A UCM profile gives PipeWire the speakers and microphones at boot; pw-play reaches pw-record "
              + "end to end.", ref: "devices/pixel7pro/kernel/audio/README.md" },
        { n: "USB-C headset / adapter audio", s: "no", cap: "headset", note: "Not attempted yet." }
      ]
    },

    {
      id: "radio",
      title: "Radio & connectivity",
      items: [
        { n: "Wi-Fi (BCM4389)", s: "ok", cap: "wifi",
          note: "Mainline brcmfmac with the stock firmware, started at boot under NetworkManager: 5 GHz at about "
              + "90 Mbit/s both ways (the network's limit). Firmware deep sleep and L1.2 cut its idle cost to "
              + "about 0.1 W, and it resumes from s2idle. Wake-on-Wi-Fi is not done.",
          ref: "devices/pixel7pro/kernel/sleep-v25/README.md" },
        { n: "Bluetooth (BCM4389)", s: "partial", cap: "bluetooth",
          note: "Google's patch firmware over UART18 at 3 Mbaud, BlueZ and PipeWire, started at boot; an LE scan "
              + "finds nearby devices. SYS_SLEEP cuts the chip's power, so Bluetooth is unloaded while the "
              + "screen is dark and reloads (a few seconds) when it lights. Pairing, audio and calls (HFP) are "
              + "untested by hand, and there is no low-power mode or host wake, so nothing reaches it asleep.",
          ref: "devices/pixel7pro/kernel/bluetooth/README.md" },
        { n: "Cellular modem (Samsung S5300, PCIe)", s: "ok",
          note: "The stock signed firmware boots over PCIe with RAM-only copies of its own NV; the identity and "
              + "modem partitions stay read-only. A supervised manager brings up LTE and IMS at every boot and "
              + "stops the modem cleanly; warm recovery after a modem fault is not done.",
          ref: "devices/pixel7pro/modem/README.md" },
        { n: "SIM detection & SIM PIN handling", s: "partial",
          note: "A physical SIM reads as a ready USIM and registers. No PIN commands are implemented.",
          ref: "devices/pixel7pro/modem/README.md" },
        { n: "eSIM", s: "no",
          note: "Not used. The eSIM sits on the NFC/secure-element chip, which Linux deliberately never writes." },
        { n: "Cellular data (LTE)", s: "ok", cap: "cellular",
          note: "DNS and certificate-verified HTTPS over LTE, also with Wi-Fi off; cellular is the fallback route "
              + "behind Wi-Fi. 5G is switched off because traffic failed on NR (unresolved), and IPv6 internet "
              + "routing is not set up.",
          ref: "devices/pixel7pro/modem/README.md" },
        { n: "SMS / texting", s: "ok", cap: "sms",
          note: "IMS SMS both ways in the Messages app, user-confirmed, also after reboot; an incoming text wakes "
              + "the phone from s2idle. MMS is not supported.",
          ref: "devices/pixel7pro/modem/README.md" },
        { n: "Voice calls / VoLTE", s: "ok", cap: "calls",
          note: "Outgoing and incoming VoLTE calls in the Phone app with two-way audio and mute, user-confirmed, "
              + "including a call that woke the phone from s2idle. Speakerphone, call waiting and emergency "
              + "calling are not implemented.",
          ref: "devices/pixel7pro/modem/README.md" },
        { n: "GPS / GNSS (Broadcom BCM4776)", s: "partial", cap: "gps",
          note: "Its rails and SPI bridge are up and Broadcom's stock daemons run without Android through a HAL "
              + "client we wrote: indoors it tracks GPS satellites and takes UTC time from them. No position fix "
              + "yet (needs a sky view); not started at boot.",
          ref: "devices/pixel7pro/gnss/README.md" },
        { n: "NFC", s: "no", cap: "nfc",
          note: "Not started. The ST54J NFC controller also holds the eSIM, so it is guarded and left untouched.",
          ref: "devices/pixel7pro/kernel/i2c/README.md" },
        { n: "Ultra-wideband (UWB)", s: "no", note: "Not attempted yet." }
      ]
    },

    {
      id: "sensors",
      title: "Sensors",
      blurb: "The sensors hang off the AoC's own buses and run under its sensor framework (USF). A client "
           + "written from the stock library loads the AoC's registry with this phone's calibration, and "
           + "pixel-sensor-proxy serves iio-sensor-proxy's D-Bus API to the shell.",
      items: [
        { n: "Accelerometer (LSM6DSV)", s: "ok", cap: "motion",
          note: "Drives the shell's rotate button, checked by hand both ways. Its UI stream no longer wakes the "
              + "phone, which had blocked suspend.", ref: "devices/pixel7pro/sensors/README.md" },
        { n: "Gyroscope (LSM6DSV)", s: "partial", cap: "motion",
          note: "Streams through the USF client, about 0 at rest; nothing uses it yet.",
          ref: "devices/pixel7pro/sensors/README.md" },
        { n: "Magnetometers (compass, two MMC5633)", s: "partial", cap: "compass",
          note: "The AoC's fused heading is served as the compass; not yet checked against a real heading.",
          ref: "devices/pixel7pro/sensors/README.md" },
        { n: "Barometric pressure (ICP20100)", s: "partial",
          note: "Reads a plausible 1010.5 hPa; nothing uses it yet.", ref: "devices/pixel7pro/sensors/README.md" },
        { n: "Ambient light sensor (TMD3719)", s: "partial", cap: "light",
          note: "Reports the room again (45–67 lux, updating live) since the proxy sends the AoC the display state "
              + "the chip syncs to (a DisplayInfo request, recovered from the stock HAL); that was the September 29 "
              + "stall. Automatic brightness in the shell still needs a check by hand.",
          ref: "devices/pixel7pro/sensors/README.md" },
        { n: "Proximity sensor (TMD3719)", s: "partial", cap: "proximity",
          note: "Converting again with the display state sent, with the screen on or off (reads far, baseline "
              + "calibration runs). Near and far were checked by hand on September 29; the recheck is pending.",
          ref: "devices/pixel7pro/sensors/README.md" },
        { n: "Rear light, flicker and spectral sensor (VD6282)", s: "no",
          note: "On the AoC's buses; not used.", ref: "devices/pixel7pro/sensors/README.md" },
        { n: "Under-display fingerprint reader", s: "no", cap: "fingerprint", note: "Not started." }
      ]
    },

    {
      id: "cameras",
      title: "Cameras",
      items: [
        { n: "Rear main camera (Samsung GN1)", s: "partial", cap: "camera-rear",
          note: "Preview and photos in Omarchy Camera (1×) through our V4L2 driver and libcamera: 2016×1136 at "
              + "120 fps over 3-trio C-PHY, with autofocus. Binned mode only; full-resolution photos are next.",
          ref: "devices/pixel7pro/kernel/camera/README.md" },
        { n: "Rear ultra-wide camera (Sony IMX386)", s: "partial", cap: "camera-rear",
          note: "Preview and photos (0.5×): 2016×1508 at 60 fps over 4-lane D-PHY, with autofocus. Binned mode "
              + "only.",
          ref: "devices/pixel7pro/kernel/camera/README.md" },
        { n: "Rear telephoto camera (Samsung GM5)", s: "partial", cap: "camera-rear",
          note: "Preview and photos (5×): 2016×1512 at 60 fps over 2-trio C-PHY with LRTE packet delimiters. "
              + "Its autofocus is not driven yet, so close subjects are soft.",
          ref: "devices/pixel7pro/kernel/camera/README.md" },
        { n: "Front camera (Samsung 3J1)", s: "partial", cap: "camera-front",
          note: "Preview and photos: 1920×1368 at 60 fps over 4-lane D-PHY. Binned mode only.",
          ref: "devices/pixel7pro/kernel/camera/README.md" },
        { n: "LED flash / torch (LM3644)", s: "partial", cap: "flash",
          note: "The torch lights from the shade's flashlight toggle (measured +0.81 W at the default step). "
              + "Camera flash waits on a camera.", ref: "devices/pixel7pro/kernel/torch/README.md" },
        { n: "Image signal processor", s: "partial",
          note: "libcamera's software ISP debayers on the Mali GPU at the sensors' full rate (cached capture "
              + "buffers keep its statistics at 4 ms a frame). Google's hardware ISP is not used.",
          ref: "devices/pixel7pro/adapter/camera/libcamera/README.md" },
        { n: "Video codec (hardware encode / decode)", s: "no",
          note: "Not started; its power domains are switched off at boot.", ref: "devices/pixel7pro/kernel/pd/README.md" }
      ]
    },

    {
      id: "usb",
      title: "USB & expansion",
      items: [
        { n: "USB-C peripheral networking (CDC-ECM + ACM)", s: "ok", cap: "usbnet",
          note: "Serial console and USB Ethernet with key-only SSH, on normal boots too: the kernel starts the "
              + "USB 2 PHY itself. A helper reconnects the Type-C data switch after unplugging (controlled tests; "
              + "a physical cable test is outstanding).",
          ref: "devices/pixel7pro/kernel/usb/README.md" },
        { n: "USB host mode (keyboard, mouse, Ethernet)", s: "no", cap: "usbhost",
          note: "Peripheral mode only; no Type-C role handling.",
          ref: "devices/pixel7pro/docs/native-boot-20260927.md" },
        { n: "USB-C DisplayPort / dock output", s: "no", cap: "dp",
          note: "Not a requirement for this phone, by the user's decision: Google documents wired projection "
              + "only from the Pixel 8, and native routing is unverified. Docking stays a shared-OS feature "
              + "for capable devices.", ref: "docs/shared-os-20260925.md" },
        { n: "SuperSpeed USB", s: "no",
          note: "USB 2 only; the SuperSpeed combo PHY is left in low power.",
          ref: "devices/pixel7pro/docs/native-boot-20260927.md" }
      ]
    },

    {
      id: "not-present",
      title: "Not present on this handset",
      blurb: "Tracked so they stay off the wish list. Grey means there is no hardware to bring up.",
      items: [
        { n: "3.5 mm headphone jack", s: "absent", note: "USB-C audio only." },
        { n: "microSD card slot", s: "absent", note: "Storage is fixed at factory UFS." },
        { n: "Notification LED", s: "absent", note: "Alerts must come from the screen, haptics or sound." },
        { n: "IR blaster", s: "absent", note: "No infrared transmitter on this model." }
      ]
    }
  ]
};
