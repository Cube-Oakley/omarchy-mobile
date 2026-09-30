/* ==========================================================================
   OnePlus 7 Pro (guacamole) — hardware bring-up
   s:   "ok" working · "partial" running, unproven · "no" not working · "absent" no such hardware
   cap: capability id from capabilities.js; the Compare page rolls rows up by it
   ========================================================================== */
HW.oneplus7pro = {
  blurb: "Everything physically present in the OnePlus 7 Pro, plus the platform pieces the phone depends on "
       + "(boot chain, power management, storage). A row here is a component, not a feature idea.",

  sections: [
    {
      id: "soc",
      title: "SoC & platform",
      items: [
        { n: "Snapdragon 855 CPU (8 × Kryo 485)", s: "ok", cap: "cpu",
          note: "All eight cores online and individually tested.", ref: "devices/oneplus7pro/docs/status.md" },
        { n: "CPU frequency scaling & idle states", s: "partial", cap: "cpufreq",
          note: "schedutil scales all three clusters (up to 1.79 / 2.42 / 2.84 GHz) with energy-aware "
              + "scheduling and CPU thermal cooling; big cores ran 3.5x faster once enabled. Loaded at boot as "
              + "an overlay; deeper idle states remain unfinished.", ref: "devices/oneplus7pro/docs/smoothness-20260923.md" },
        { n: "Adreno 640 GPU", s: "ok", cap: "gpu",
          note: "Freedreno hardware rendering; Hyprland and Quickshell both run on the GPU (renderD128). "
              + "Frequency scaling works (257–585 MHz, simple_ondemand).", ref: "devices/oneplus7pro/docs/smoothness-20260923.md" },
        { n: "RAM (8 GB)", s: "ok", note: "In normal use; memory and swap policy not yet tuned for mobile." },
        { n: "UFS storage (256 GB)", s: "ok", cap: "storage",
          note: "Persistent Arch Linux ARM rootfs on UFS. Full-LUN backup/restore helpers exist for recovery.",
          ref: "devices/oneplus7pro/docs/backup.md" },
        { n: "A/B slots + stock OnePlus ABL boot", s: "ok", cap: "boot",
          note: "Direct Linux boot through the OOS12 ABL on slot B; slot A stays a working Android fallback. "
              + "Each boot marks slot B successful, so ABL's retry counter no longer runs out into the "
              + "\"boot image destroyed\" screen.", ref: "devices/oneplus7pro/docs/microphone-20260922.md" },
        { n: "Hexagon ADSP (audio DSP)", s: "ok",
          note: "ADSP firmware loads and runs; it carries the audio path and, through vendored q6voice modules, "
              + "the VoLTE call vocoder.",
          ref: "devices/oneplus7pro/docs/calling-20260927.md" },
        { n: "Hexagon NPU (cDSP / AI engine)", s: "no",
          note: "Not mainlined for SM8150. Anything we run locally today has to fall back to CPU or GPU.",
          ref: "devices/oneplus7pro/docs/pathway.md" },
        { n: "SLPI / SSC sensor subsystem", s: "partial",
          note: "Runs: kernel #193 moves the firmware carve-outs to the OEM map, a runtime module starts SLPI "
              + "with this phone's OxygenOS 10 SLPI firmware (00083; the OxygenOS 12 image leaves out the motion "
              + "sensors, as on the 7T Pro), and hexagonrpcd serves it the stock configuration and a copy of the "
              + "persist registry. 42 sensor types register. It all starts at boot after the radio, with "
              + "iio-sensor-proxy on top.",
          ref: "devices/oneplus7pro/docs/sensors-20260924.md" },
        { n: "s2idle suspend / resume", s: "partial", cap: "suspend",
          note: "Sleeps 10 s after the screen goes dark, off the charger; calls and texts wake it, and the modem "
              + "and sensor DSP now survive (MSS vote and FastRPC fixes). The SoC never reaches its deepest "
              + "states, and the drain is not measured yet.",
          ref: "devices/oneplus7pro/docs/sleep-20260927.md" },
        { n: "Deepest idle power states", s: "no", cap: "deepsleep",
          note: "CX/MX/MMCX/MSS sleep votes still held; one isolated MSS release wedged modem resume.",
          ref: "devices/oneplus7pro/docs/mss-handoff-test-20260917.md" },
        { n: "Thermal sensing & throttling policy", s: "partial", cap: "thermal",
          note: "September 22: 25 thermal zones and one cooling device exposed; plausible temperatures read. Trip points, cooling action and sustained-load behavior remain unverified.",
          ref: "devices/oneplus7pro/docs/hardware-plan-20260922.md" },
        { n: "RTC and alarms", s: "ok",
          note: "RTC binds under its correct SPMI parent; a five-second alarm fired while awake. RTC wake from "
              + "suspend is still to be proven.", ref: "devices/oneplus7pro/docs/status.md" },
        { n: "Power button", s: "ok", cap: "buttons",
          note: "pm8941 pwrkey; two unplugged physical sleep/wake cycles confirmed by the user.",
          ref: "devices/oneplus7pro/docs/power-button-policy-20260917.md" },
        { n: "Volume up / down keys", s: "partial", cap: "buttons",
          note: "Both keys enumerate cleanly after the SPMI-parent fix; physical press-and-hold behaviour not "
              + "yet confirmed by the user.", ref: "devices/oneplus7pro/docs/audio-bringup-20260918.md" },
        { n: "Three-position alert slider", s: "partial", cap: "buttons",
          note: "gpio-keys on the stock GPIOs (27, 134, 125) reports Vibrate and Ring, which mute and unmute "
              + "the ring group. This unit's Silent contact never closes, a worn switch.",
          ref: "devices/oneplus7pro/docs/controls-20260923.md" },
        { n: "Battery gauge (TI bq27541)", s: "ok", cap: "battery",
          note: "Standard power_supply class: capacity, voltage, current and temperature, live while unplugged.",
          ref: "devices/oneplus7pro/docs/battery-gauge-20260917.md" },
        { n: "Charging (PM8150B charger)", s: "ok", cap: "charging",
          note: "Charges to full on the stock profile (4.39 V float, up to 3 A by temperature band, input by charger "
              + "type) with latching safety stops; Settings can hold it at 80 %, seen holding and resuming. "
              + "Charging stops while asleep, so it never sleeps on the charger.",
          ref: "devices/oneplus7pro/docs/sleep-20260927.md" },
        { n: "Warp Charge fast charging", s: "no", cap: "fastcharge",
          note: "The stock profile takes about 1.1 A from a 2 A wall charger; the Warp MCU is not spoken to. Real "
              + "fast charging needs its negotiation, temperature limits, taper and fault handling.",
          ref: "devices/oneplus7pro/docs/sleep-20260927.md" },
        { n: "True power-off / shutdown", s: "no", cap: "poweroff",
          note: "Power-off attempts currently reboot the phone instead of staying off.",
          ref: "devices/oneplus7pro/docs/shutdown-20260917.md" },
        { n: "Secure element / hardware keystore", s: "no",
          note: "Keymaster/keystore class of hardware. Needed eventually for screen-lock credentials, "
              + "pairing keys and any app that wants hardware-backed secrets.", ref: "devices/oneplus7pro/docs/pathway.md" },
        { n: "Bring-up recovery path (Sahara/crashdump, guarded flash)", s: "ok", cap: "recovery",
          note: "Guarded flashing helpers with serial, hash and slot checks. Since kernel #192 a crash reboots "
              + "the phone by itself instead of stopping in crash-dump mode; lockup detectors panic on a stuck "
              + "CPU. The crash log does not survive the reset yet.",
          ref: "devices/oneplus7pro/docs/kernel192-20260923.md" }
      ]
    },

    {
      id: "display",
      title: "Display & touch",
      items: [
        { n: "AMOLED panel, 1440 × 3120 DSC (DSI)", s: "ok", cap: "display",
          note: "Native DPU/DSI scanout at 60 Hz, correct colours, stable output. No ABL leftover framebuffer.",
          ref: "devices/oneplus7pro/docs/native-display-work-20260917.md" },
        { n: "90 Hz refresh mode", s: "partial", cap: "refresh",
          note: "Kernel #191 offers 60 and 90 Hz and the shell runs at 90 Hz: 89.8 Hz measured, no missed "
              + "refreshes. Stock's per-rate gamma is not applied yet.", ref: "devices/oneplus7pro/docs/smoothness-20260923.md" },
        { n: "Panel power off / wake", s: "ok", cap: "display",
          note: "Blank and restore driven by the power key, charging preserved.",
          ref: "devices/oneplus7pro/docs/power-button-policy-20260917.md" },
        { n: "Backlight / brightness control", s: "ok", cap: "brightness",
          note: "User-confirmed changes; a shade slider sets and remembers the level, and automatic brightness "
              + "fades with the room. Kernel #194 keeps panel commands out of frame transfers, which ended the "
              + "flicker while dragging.",
          ref: "devices/oneplus7pro/docs/kernel194-20260924.md" },
        { n: "Always-on / ambient display (panel doze)", s: "ok",
          note: "A dim clock, date, battery and notification icons in place of switching off, after the CRT "
              + "close; the power button or a double tap wakes it. The panel has no low-power mode (stock's AOD "
              + "commands are empty), so it runs in its normal mode, redrawing once a minute. Face down or in a "
              + "pocket it goes fully off.", ref: "devices/oneplus7pro/docs/always-on-display-20260924.md" },
        { n: "Multitouch (Samsung S6SY761)", s: "ok", cap: "touch",
          note: "User-tested including five-finger input; survives suspend/resume.",
          ref: "devices/oneplus7pro/docs/touchscreen-work-20260916.md" },
        { n: "Touchscreen gesture surface / edge rejection", s: "partial",
          note: "Bottom-edge and top-edge zones work as shell gestures, but palm and accidental-touch rejection "
              + "is untested.", ref: "devices/oneplus7pro/docs/mobile-gestures-20260917.md" },
        { n: "Haptic / vibration motor", s: "ok", cap: "haptics",
          note: "AW8697 (stock 170 Hz profile) through the standard force-feedback interface, user-confirmed. "
              + "Notifications and the alert slider buzz; key feedback still to come.",
          ref: "devices/oneplus7pro/docs/controls-20260923.md" }
      ]
    },

    {
      id: "audio",
      title: "Audio",
      items: [
        { n: "WCD9340 codec + SLIMbus", s: "partial",
          note: "Codec enumerates and ALSA exposes playback and capture (hw:0,3). The capture route has never "
              + "been exercised, so only the playback side has any evidence.",
          ref: "devices/oneplus7pro/docs/audio-bringup-20260918.md" },
        { n: "Main loudspeaker (TFA9874, lower amp)", s: "partial", cap: "speaker",
          note: "Works after the user reseated the bottom board. S16 output is capped at -18 dBFS, where a "
              + "1 kHz tone reaches about 5% distortion, behind a leveler and limiter. No speaker protection "
              + "yet.", ref: "devices/oneplus7pro/docs/speakers-20260922.md" },
        { n: "Earpiece receiver (TFA9874, upper amp)", s: "partial", cap: "earpiece",
          note: "Carries VoLTE calls in its stock receiver profile, with audio both ways user-confirmed. Media "
              + "still reaches it capped at -42 dBFS, where it distorts, and call volume is not on the keys yet.",
          ref: "devices/oneplus7pro/docs/calling-20260927.md" },
        { n: "Stereo playback (speaker + receiver together)", s: "partial",
          note: "Both play a mono mix, verified at the mic; the earpiece is about 17 dB quieter at its cap, "
              + "so true stereo is not used.", ref: "devices/oneplus7pro/docs/speakers-20260922.md" },
        { n: "Application playback (PipeWire)", s: "ok",
          note: "YouTube plays clean and audible through the volume groups and a processing sink (400 Hz "
              + "high-pass, leveler, limiter). An 8-period buffer fixed silent and ticking playback.",
          ref: "devices/oneplus7pro/docs/speakers-20260922.md" },
        { n: "DSP speaker protection / calibration", s: "no",
          note: "No OTP/MTP programming or calibration run; the per-speaker output cap stays until this exists.",
          ref: "devices/oneplus7pro/docs/audio-bringup-20260918.md" },
        { n: "Primary microphone", s: "partial", cap: "mic",
          note: "AMIC4, the stock handset mic, records through PipeWire as Internal microphone, confirmed clean "
              + "by ear, and carries the call uplink (TX topology NONE: the uncalibrated noise suppression was "
              + "silent). Gain is a fixed conservative default.",
          ref: "devices/oneplus7pro/docs/calling-20260927.md" },
        { n: "Secondary / noise-cancelling microphones", s: "partial",
          note: "AMIC1 and AMIC3 also respond to room sound (+27 and +36 dB over quiet) and sound clean. AMIC3 is "
              + "the top mic; AMIC1 is probably near the rear cameras. Not exposed to applications yet.",
          ref: "devices/oneplus7pro/docs/microphone-20260922.md" },
        { n: "Speakerphone audio path", s: "ok",
          note: "Speaker in a call switches the lower amp on live (a new Loudspeaker Switch), user-confirmed in "
              + "a VoLTE call; the earpiece path keeps it off. No wired or Bluetooth headset route in calls.",
          ref: "devices/oneplus7pro/docs/calling-20260927.md" },
        { n: "Audio during suspend", s: "no", note: "Audio suspend has not been tested.",
          ref: "devices/oneplus7pro/docs/audio-bringup-20260918.md" },
        { n: "USB-C headset / adapter audio", s: "no", cap: "headset",
          note: "No analogue jack on this handset, so USB-C is the only wired-audio route; unattempted." }
      ]
    },

    {
      id: "radio",
      title: "Radio & connectivity",
      items: [
        { n: "Wi-Fi (WCN3990 / ath10k_snoc)", s: "ok", cap: "wifi",
          note: "NetworkManager connect, saved-network reconnect, internet access, and reconnection after sleep. "
              + "Wake-on-Wi-Fi is not set up.",
          ref: "devices/oneplus7pro/docs/wifi-20260917.md" },
        { n: "Bluetooth", s: "partial", cap: "bluetooth",
          note: "WCN3990 loads its stock firmware, scans and pairs; OnePlus Bullets headphones play A2DP "
              + "(aptX HD) and Wi-Fi keeps working. Starts after the desktop (just enabled, not yet rebooted). Calls (HFP), PIN "
              + "keyboards and waking from suspend are untested.", ref: "devices/oneplus7pro/docs/bluetooth-20260922.md" },
        { n: "Cellular modem subsystem (QMI / QRTR)", s: "ok",
          note: "Registers on LTE at boot and stays up through sleep: the QLink assertion was the AP holding the "
              + "modem rail at its top corner, which mss_vote.ko releases and re-sends after every resume.",
          ref: "devices/oneplus7pro/docs/cellular-sim-20260926.md" },
        { n: "SIM detection & SIM PIN handling", s: "partial",
          note: "A physical nano-SIM reads (USIM and ISIM) and the modem picks its carrier profile itself. The "
              + "card has no PIN, so PIN entry is untested.",
          ref: "devices/oneplus7pro/docs/cellular-sim-20260926.md" },
        { n: "RF front end, EFS & calibration (IMEI)", s: "partial",
          note: "EFS backed up and never flashed from another device. The network accepts the phone for data, "
              + "SMS and calls; antenna performance under Linux is unmeasured.",
          ref: "devices/oneplus7pro/docs/backup.md" },
        { n: "Cellular data (LTE)", s: "ok", cap: "cellular",
          note: "ModemManager and NetworkManager bring up mobile data at boot over IPA (IPv4 and IPv6 HTTPS 200), "
              + "as a fallback route behind Wi-Fi. Needed IPA before the modem, DPM, QMAPv4 and an ipv6 module.",
          ref: "devices/oneplus7pro/docs/cellular-sim-20260926.md" },
        { n: "SMS / texting", s: "ok", cap: "sms",
          note: "Both ways over IMS in the Messages app, and a text wakes the phone from sleep. MMS is not "
              + "supported.", ref: "devices/oneplus7pro/docs/calling-20260927.md" },
        { n: "Voice calls / IMS / VoLTE", s: "ok", cap: "calls",
          note: "An AP-side IMS DCM server lets the modem register IMS; VoLTE calls carry audio both ways past "
              + "30 s with speaker and mute, user-confirmed, and an incoming call wakes the phone from sleep.",
          ref: "devices/oneplus7pro/docs/calling-20260927.md" },
        { n: "Wi-Fi calling", s: "no", note: "Depends on IMS plus stable call audio; not started." },
        { n: "GPS / GNSS", s: "no", cap: "gps", note: "Not brought up.", ref: "docs/mobile-roadmap.md" },
        { n: "NFC", s: "no", cap: "nfc", note: "Not brought up." }
      ]
    },

    {
      id: "sensors",
      title: "Sensors — physical silicon",
      blurb: "The parts actually wired to the SLPI/SSC sensor subsystem. The SLPI runs; the accelerometer "
           + "and light sensor reach the shell through iio-sensor-proxy, proximity reaches it too but nothing "
           + "uses it yet, the gyroscope and magnetometer stream to a test client, and the Hall sensor and "
           + "fingerprint reader do not work yet.",
      items: [
        { n: "Accelerometer", s: "ok", cap: "motion",
          note: "LSM6DSM on the sensor DSP's SPI bus, with the OxygenOS 10 SLPI firmware, read through libssc "
              + "and iio-sensor-proxy: drives the shell's rotate button, checked by hand in both landscape "
              + "directions.", ref: "devices/oneplus7pro/docs/sensors-20260924.md" },
        { n: "Gyroscope", s: "partial", cap: "motion",
          note: "Same LSM6DSM: streams, about 0 rad/s at rest once its start-up sample passes. Not fed to Linux yet.",
          ref: "devices/oneplus7pro/docs/sensors-20260924.md" },
        { n: "Magnetometer (magnetic field / compass)", s: "partial", cap: "compass",
          note: "MMC5603: streams about 59 µT in the room, a plausible Earth field. Not fed to Linux yet.",
          ref: "devices/oneplus7pro/docs/sensors-20260924.md" },
        { n: "Barometric pressure", s: "no",
          note: "Earlier checklist claimed an Android pressure sensor; recheck the actual stock inventory and physical part before selecting a driver.",
          ref: "devices/oneplus7pro/docs/hardware-plan-20260922.md" },
        { n: "Ambient light sensor", s: "ok", cap: "light",
          note: "STK2232 under the display, through the sensor DSP and iio-sensor-proxy: drives automatic "
              + "brightness, which discounts the panel's own light (about 190 lux at full brightness).",
          ref: "devices/oneplus7pro/docs/sensors-20260924.md" },
        { n: "Proximity sensor (ear-away / call detection)", s: "ok", cap: "proximity",
          note: "The STK2232 under the display, with its near threshold lowered for the panel in front of it: it "
              + "blanked the screen at the ear during a VoLTE call and swallowed touches, user-confirmed.",
          ref: "devices/oneplus7pro/docs/calling-20260927.md" },
        { n: "Hall sensor (pop-up camera endstops)", s: "no",
          note: "Bounds the pop-up selfie mechanism; nothing enabled yet.", ref: "devices/oneplus7pro/docs/pathway.md" },
        { n: "In-display optical fingerprint reader", s: "no", cap: "fingerprint",
          note: "Not brought up; needed for unlock and app authentication.", ref: "docs/mobile-roadmap.md" }
      ]
    },

    {
      id: "sensors-derived",
      title: "Sensors — derived & fused",
      blurb: "Android on this handset reports around seventeen “sensors”. Only a handful are silicon — the "
           + "rest are computed from those, so they are not separate hardware work. They get their own rows so "
           + "a missing derived sensor is never mistaken for a missing chip. Worth re-reading the list off the "
           + "device from Android before we commit to what we reproduce.",
      items: [
        { n: "Gravity", s: "partial",
          note: "Registers on the sensor DSP (SEE); not read yet. Derived estimate of gravitational acceleration. No separate hardware.",
          ref: "devices/oneplus7pro/docs/hardware-plan-20260922.md" },
        { n: "Linear acceleration", s: "no", note: "Derived: accelerometer minus gravity. No separate hardware." },
        { n: "Rotation vector", s: "partial",
          note: "Registers on the sensor DSP (SEE); not read yet. Fused accelerometer + gyroscope + magnetometer; the useful one for stable orientation. Needs "
              + "all three physical sensors and their calibration data." },
        { n: "Geomagnetic rotation vector", s: "partial",
          note: "Registers on the sensor DSP (SEE); not read yet. Orientation derived from accelerometer and magnetometer; depends on magnetic calibration. No separate hardware.",
          ref: "devices/oneplus7pro/docs/hardware-plan-20260922.md" },
        { n: "Game rotation vector", s: "partial",
          note: "Registers on the sensor DSP (SEE); not read yet. Accelerometer and gyroscope without magnetic heading; avoids magnetic interference but can accumulate yaw drift. No separate hardware.",
          ref: "devices/oneplus7pro/docs/hardware-plan-20260922.md" },
        { n: "Orientation (pitch / roll / azimuth)", s: "partial",
          note: "Screen orientation works: iio-sensor-proxy derives it from the accelerometer for the rotate "
              + "button. The DSP's own fused angles (rotation vector, device_orient) register but are not read yet; "
              + "a compass UI would want them." },
        { n: "Uncalibrated accelerometer / gyro / magnetic field", s: "no",
          note: "Same silicon, raw output with the estimated bias reported alongside. Only matters if we do our "
              + "own sensor fusion instead of using the SLPI's." },
        { n: "Step detector", s: "partial",
          note: "Registers on the sensor DSP (SEE); not read yet. Activity recognition derived from the accelerometer; low-power versions want the sensor hub." },
        { n: "Step counter", s: "partial",
          note: "Registers on the sensor DSP (SEE); not read yet. Same source as the step detector, counted. Cheap to keep on the hub, expensive on the CPU." },
        { n: "Significant motion", s: "partial",
          note: "Registers on the sensor DSP (SEE); not read yet. Fires when the handset is actually moved, deliberately ignoring small vibrations. Useful for a "
              + "phone that should know it changed hands or moved — and cheap to do on the sensor hub." },
        { n: "Sensor service for Linux (iio-sensor-proxy or SSI)", s: "partial",
          note: "Arch's iio-sensor-proxy 3.9 with libssc 0.4.4 reads SEE directly and serves the accelerometer, "
              + "light and proximity sensors on D-Bus (net.hadess.SensorProxy); a udev rule adds the accelerometer, "
              + "its mount matrix and proximity. The compass is untried. Its clients wait until it has opened a "
              + "sensor: 3.9 loses a claim that arrives earlier.",
          ref: "devices/oneplus7pro/docs/sensors-20260924.md" }
      ]
    },

    {
      id: "cameras",
      title: "Cameras",
      items: [
        { n: "Rear wide — 48 MP Sony IMX586 (OIS)", s: "partial", cap: "camera-rear",
          note: "Streams 4000x3000 raw over its C-PHY. A patched libcamera (the 7T Pro's IMX586 helper and "
              + "contrast autofocus, plus our statistics, lens-timing and memory fixes) runs it through the GPU "
              + "software ISP: 30 fps previews with up to three stops of highlight headroom, and raw frames "
              + "alongside for merged stills in Omarchy Camera; autofocus lands. Three crashes came with the "
              + "camera stack loaded, two under full CPU load and one on a clean reboot; the cause is open.",
          ref: "devices/oneplus7pro/docs/camera-20260922.md" },
        { n: "Rear ultra-wide — 16 MP", s: "partial", cap: "camera-rear",
          note: "Sony IMX481 on CCI1 / CSIPHY3, from runtime modules: full 4656x3496 frames at 30 fps. Its "
              + "AK7374 focus actuator (found through the OxygenOS module data; the 7T Pro port left it unpowered) "
              + "gives autofocus and tap to focus. All three rear cameras load together and Omarchy Camera "
              + "switches to it (0.6×); its stills merge up to eight frames. Dim in dim rooms; no colour "
              + "calibration yet.",
          ref: "devices/oneplus7pro/docs/camera-ultrawide-20260924.md" },
        { n: "Rear telephoto — 8 MP (OIS)", s: "partial", cap: "camera-rear",
          note: "Samsung S5K3M5 on CCI0 / CSIPHY0 with its LC898217XC focus actuator, from runtime modules. "
              + "With both rear cameras in one overlay, libcamera lists both and Omarchy Camera switches to it "
              + "(3×); autofocus works over the lens's whole range (focus sits above the 7T Pro's 400 limit) "
              + "and stills merge up to eight frames. User-tested: \"not bad\". No colour calibration yet, "
              + "dim rooms push it to its longest exposure and highest gain, and OIS is not driven.",
          ref: "devices/oneplus7pro/docs/camera-telephoto-20260924.md" },
        { n: "Pop-up front camera — 16 MP Sony IMX471", s: "no", cap: "camera-front", note: "Not brought up." },
        { n: "Pop-up camera motor & lifecycle", s: "no",
          note: "Unique to the 7 Pro / 7T Pro. Motor control, endstops and safe retraction (drop detection) "
              + "still to do.", ref: "devices/oneplus7pro/docs/pathway.md" },
        { n: "ISP / image pipeline (IPE)", s: "no",
          note: "Raw capture through CAMSS (CSIPHY, CSID, VFE raw path) works, processed by libcamera's "
              + "software ISP on the GPU (debayer, black level, white balance, exposure, autofocus). The "
              + "hardware ISP is unverified.",
          ref: "devices/oneplus7pro/docs/hardware-plan-20260922.md" },
        { n: "Video codec (Venus: hardware encode / decode)", s: "no",
          note: "Not brought up. Likely part of why web video is choppy, though that is not proven.",
          ref: "devices/oneplus7pro/docs/next-session.md" },
        { n: "LED flash / torch", s: "partial", cap: "flash",
          note: "Both PM8150L flash LEDs light at stock limits, user-confirmed; the shade has a Torch toggle. "
              + "Camera flash synchronization remains.", ref: "devices/oneplus7pro/docs/controls-20260923.md" }
      ]
    },

    {
      id: "usb",
      title: "USB & expansion",
      items: [
        { n: "USB-C peripheral networking (NCM + ACM)", s: "ok", cap: "usbnet",
          note: "Gadget Ethernet plus pinned-key SSH, with reconnect support. After a forced reset USB does not "
              + "attach (cause open), so SSH falls back to Wi-Fi.",
          ref: "devices/oneplus7pro/docs/usb-networking-20260917.md" },
        { n: "USB host mode (keyboard, mouse, Ethernet)", s: "no", cap: "usbhost",
          note: "Role switching, PHY and kernel support unverified. Currently high-speed peripheral only.",
          ref: "docs/mobile-roadmap.md" },
        { n: "USB-C DisplayPort / dock output", s: "no", cap: "dp",
          note: "Monitor output, layout/scaling and simultaneous charging all unverified. This is the gate for "
              + "any docked desktop experience.", ref: "docs/mobile-roadmap.md" },
        { n: "SuperSpeed USB (5 Gbps)", s: "no",
          note: "Deliberately disabled during bring-up — enabling the QMP PHY crashed the kernel. Still open.",
          ref: "devices/oneplus7pro/docs/status.md" }
      ]
    },

    {
      id: "not-present",
      title: "Not present on this handset",
      blurb: "Tracked so they stay off the wish list. Grey means there is no hardware to bring up.",
      items: [
        { n: "3.5 mm headphone jack", s: "absent", note: "Removed on this generation; USB-C only." },
        { n: "microSD card slot", s: "absent", note: "Storage is fixed at factory UFS." },
        { n: "Wireless charging", s: "absent", cap: "wireless", note: "Contact charging only." },
        { n: "eSIM", s: "absent", note: "Physical nano-SIM only on this SKU." },
        { n: "Notification LED", s: "absent", note: "Alerts must come from the screen, haptics or sound." },
        { n: "IR blaster", s: "absent", note: "No infrared transmitter on this model." }
      ]
    }
  ]
};
