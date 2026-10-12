/* Stock observations are reference evidence, not Linux support claims. */
HW.oneplus13 = {
  blurb: "Native bring-up on the CPH2655. Stock camera references and recovery backups are saved. "
       + "The installed native kernel boots the Arch/Adreno desktop directly. Battery, CPU retention and several sensor streams work; full hardware parity remains a target.",
  sections: [
    {
      id: "platform", title: "Boot, compute & recovery",
      items: [
        { n: "Native Linux boot", s: "yes", cap: "boot", note: "Native kernel in boot_a starts the installed ext4 Arch root and Adreno mobile desktop. Repeated normal boot and clean fastboot return passed; slot A success independently confirmed. Stock VBMeta policy unchanged; clean shutdown and physical Power-button boot passed." },
        { n: "UFS storage", s: "partial", cap: "storage", note: "All six UFS logical units enumerate. Native root installed on userdata; remounted write/readback, filesystem check and program/configuration hashes passed. Other partitions remain protected." },
        { n: "Oryon CPU cores", s: "yes", cap: "cpu", note: "All eight cores online. Capped schedutil scaling and core retention tested; cluster/APSS collapse remains disabled." },
        { n: "CPU frequency scaling", s: "partial", cap: "cpufreq", note: "SCMI exposes both performance domains; capped schedutil is running and frequency readings change. Controlled load/idle and thermal validation pending." },
        { n: "Adreno 830", s: "partial", cap: "gpu", note: "Hardware GLES shader/readback passed twice; Hyprland and the shared shell use Adreno 830. Owner confirmed smoother interaction. GPU capped at 342 MHz; native panel, Vulkan, sustained load and suspend pending." },
        { n: "Recovery", s: "partial", cap: "recovery", note: "Matching stock images and calibration/GPT/super backups are verified. Stock boot restoration and native RAM recovery tested repeatedly. Native startup failures return to fastboot; installed root must not be formatted in Android recovery. Slot B is not a tested fallback.", ref: "devices/oneplus13/docs/recovery.md" }
      ]
    },
    {
      id: "display", title: "Display & input",
      items: [
        { n: "1440 × 3168 LTPO panel", s: "partial", cap: "display", note: "Native DMA KMS and Adreno desktop work, with 1209 direct DRM flips in 10 seconds. Black-frame screen-off passed. Inherited panel/DSI/DSC/SPR; cold initialization, true panel power-down and optical checks pending." },
        { n: "120 Hz refresh", s: "partial", cap: "refresh", note: "Installed Hyprland now presents at 120.7 fps with consecutive hardware vblank sequences and matching DMA completion cadence. Owner confirms significantly smoother interaction; slight orientation-change flicker remains." },
        { n: "Brightness", s: "no", cap: "brightness", note: "Not implemented under Linux." },
        { n: "Touchscreen", s: "partial", cap: "touch", note: "Native S3910 TouchComm v1 driver reaches Hyprland; owner confirmed taps/swipes. Nine contact starts and releases captured. Polling prototype; multitouch and wake validation pending." },
        { n: "Power, volume and alert slider", s: "partial", cap: "buttons", note: "Owner confirmed power-key screen-off/wake with CRT animation and both volume indicators. Native stock-matched input routing; alert slider and suspend wake remain pending." },
        { n: "Haptics", s: "no", cap: "haptics", note: "Stock qcom-hv-haptics input present; Linux pending." }
      ]
    },
    {
      id: "power", title: "Power & thermals",
      items: [
        { n: "Battery gauge", s: "partial", cap: "battery", note: "Verified stock ADSP runs natively; PMIC GLINK supplies percentage, temperature and health to the shared shell. Inconsistent optional readings are excluded from the read-only profile; full-cycle calibration pending." },
        { n: "Wired charging", s: "partial", cap: "charging", note: "USB online/offline transitions and approximately 5 V reconnect verified. Owner used desktop/wake on battery; ADSP reported Charging after reconnect. Actual charge-cycle and protection validation pending; no charging-control setters exposed." },
        { n: "SUPERVOOC fast charging", s: "no", cap: "fastcharge", note: "Negotiation, thermal protection and taper unvalidated." },
        { n: "Wireless charging", s: "no", cap: "wireless", note: "Not tested under Linux." },
        { n: "Thermal sensing and throttling", s: "partial", cap: "thermal", note: "Native TSENS and PMIC temperature readings work; cooling and sustained thermal behavior unvalidated." },
        { n: "Suspend and resume", s: "no", cap: "suspend", note: "Requires repeated unplugged tests with peripherals restored." },
        { n: "Deep idle", s: "no", cap: "deepsleep", note: "Requires measured drain and wake-source validation." },
        { n: "RTC clock", s: "partial", note: "PMK8550 counter tracks elapsed time across power-off. Native-root software offset restores the correct wall clock before desktop startup; RTC alarm/wake untested." },
        { n: "Power off", s: "yes", cap: "poweroff", note: "Clean outer-initramfs shutdown unmounts the native root. Owner confirmed the phone stayed off and booted back to Omarchy after pressing Power; shell menu integration pending." }
      ]
    },
    {
      id: "connectivity", title: "Connectivity",
      items: [
        { n: "USB networking", s: "partial", cap: "usbnet", note: "Native USB Ethernet transferred and hash-verified a 1.74 GB shared Arch rootfs; key-only SSH into RAM and installed userspace works. Explicit UTMI clock selection fixes normal boot without fastboot-inherited clocks. Physical unplug/replug recovered USB SSH without reboot; host role and suspend remain untested." },
        { n: "USB host", s: "no", cap: "usbhost", note: "Role switching and peripherals unvalidated." },
        { n: "USB-C display output", s: "no", cap: "dp", note: "Not tested under Linux." },
        { n: "Wi-Fi", s: "partial", cap: "wifi", note: "Installed native station support reconnects automatically after normal boot. 2.4/5/6 GHz scanning, 5 GHz WPA2/DHCP/HTTPS, reconnect, screen-off traffic and 32 MiB each-way integrity tests passed. Stock US firmware/regulatory data retained. Factory MAC retrieval, 2.4/6 GHz association, AP mode, power saving and suspend remain pending.", ref: "devices/oneplus13/docs/wireless.md" },
        { n: "Bluetooth", s: "partial", cap: "bluetooth", note: "Native autostart, exact stock Ganges firmware/NVM, 3 Mbaud UART, LE/BlueZ discovery and three mobile-shell power cycles passed with Wi-Fi traffic. Pairing, accessory data/audio, range, suspend and factory identity retrieval remain pending.", ref: "devices/oneplus13/docs/bluetooth.md" },
        { n: "Cellular data", s: "no", cap: "cellular", note: "Modem integration and test SIM pending." },
        { n: "SMS", s: "no", cap: "sms", note: "Incoming/outgoing and sleep wake tests pending." },
        { n: "Voice calls", s: "no", cap: "calls", note: "IMS, routing and incoming/outgoing call tests pending." },
        { n: "GNSS", s: "no", cap: "gps", note: "Cold/warm fixes unvalidated." },
        { n: "NFC", s: "no", cap: "nfc", note: "Not tested under Linux." },
        { n: "IR blaster", s: "no", note: "Not tested under Linux." }
      ]
    },
    {
      id: "audio", title: "Audio",
      items: [
        { n: "Speakers", s: "no", cap: "speaker", note: "Require protected amplifier setup and physical playback checks." },
        { n: "Earpiece", s: "no", cap: "earpiece", note: "Call routing and level unvalidated." },
        { n: "Microphones", s: "no", cap: "mic", note: "Recording and routing unvalidated." },
        { n: "USB-C headset", s: "no", cap: "headset", note: "Not tested under Linux." }
      ]
    },
    {
      id: "camera-sensors", title: "Cameras & sensors",
      items: [
        { n: "Main, ultrawide and 3x telephoto cameras", s: "no", cap: "camera-rear", note: "Thirteen stock originals saved, including zoom references through 120x. Native capture and stock processing reuse unproven.", ref: "devices/oneplus13/docs/camera-baseline.md" },
        { n: "Front camera", s: "no", cap: "camera-front", note: "Stock camera metadata saved; Linux capture pending." },
        { n: "Flash and torch", s: "no", cap: "flash", note: "Not tested under Linux." },
        { n: "BMI26x accelerometer and gyroscope", s: "partial", cap: "motion", note: "Native SSC/FastRPC motion streams work. Portrait and landscape axis measurements establish the rotation mapping; owner confirmed the shared rotate button and transformed touch. Gyro accuracy and suspend/resume remain unvalidated." },
        { n: "Compass", s: "partial", cap: "compass", note: "Native SSC delivered 16 heading samples. Calibration, orientation and physical validation pending." },
        { n: "Front and rear ambient light", s: "no", cap: "light", note: "Stock TCS3720/TCS3449 observed; native SSC ambient_light discovery remains unavailable." },
        { n: "Proximity", s: "partial", cap: "proximity", note: "Native SSC reports NEAR/FAR states. Controlled physical response and call/screen integration pending." },
        { n: "Fingerprint reader", s: "no", cap: "fingerprint", note: "Stock input present; enrollment, matching and secure integration pending." }
      ]
    },
    {
      id: "absent", title: "Not present",
      items: [
        { n: "3.5 mm audio jack", s: "absent", note: "Wired audio uses USB-C." }
      ]
    }
  ]
};
