# Native sensor transport

Tested on CPH2655 with the captured OxygenOS 16.0.10.501 ADSP firmware. This
does not replace Android sensor firmware or write calibration to persist.

## Sources and build

- [HexagonRPC](https://github.com/linux-msm/hexagonrpc), commit
  `da7a3742ab8d5273a281b70ee11f16fb2cb6a690` (0.5.0), GPL-3.0-or-later.
  Apply `0001-map-oneplus-odm-sensor-config.patch`. It adds the vendor's ODM
  sensor configuration path to the existing read-only virtual filesystem.
- [libssc](https://codeberg.org/DylanVanAssche/libssc), commit
  `54dd13e36d65c21263cf5b0f4a70c230a77b26bc` (0.4.4), GPL-3.0-or-later.
  The tested library is unmodified. It was built natively using GCC,
  protobuf-c generated sources, GLib and libqmi, then installed privately in
  `/opt/oneplus13-sensors`. The normal upstream Meson build is also available.

The bring-up static HexagonRPC build, from its source directory:

```sh
aarch64-linux-gnu-gcc -O2 -g -static -DHEXAGONRPC_VERBOSE \
  -Iinclude -Ihexagonrpcd \
  libhexagonrpc/*.c libhexagonrpc/interface/*.c \
  hexagonrpcd/*.c hexagonrpcd/interface/*.c -o hexagonrpcd
```

The tested Arch dependencies are libqmi 1.38.0-1, libqrtr-glib 1.4.0-1,
libmbim 1.34.0-1, protobuf 36.1-1 and protobuf-c 1.5.2-15. Package signatures
were checked against the installed Arch Linux ARM keyring. The private build
artifacts and signed packages are under `out/sensors-survey`.

## Private firmware/configuration tree

Populate the `-R` directory from this phone's verified backups, preserving
the stock file contents:

| Relative path | Stock source |
| --- | --- |
| `sensors/sns_reg.conf` | `/vendor/etc/sensors/sns_reg_config` |
| `sensors/config/` | `/vendor/etc/sensors/config/` |
| `sensors/odm-config/` | `/odm/etc/sensor/config/` |
| `sensors/registry/` | `/mnt/vendor/persist/sensors/registry/registry/` |
| `socinfo/` | Captured `/sys/devices/soc0` identity fields |
| `dsp/adsp/` | The `adsp/` directory from the backed-up `dsp_a` filesystem |

`/vendor/dsp` is an empty mountpoint in the vendor image; its files come from
the separate DSP partition. `debugfs rdump` can read that ext4 backup offline.
Raw files, device calibration and generated binaries stay under ignored `out/`.
The read-only service rejects writes, including the firmware's registry `DIR`
write attempt. Missing optional `oemconfig.so` did not prevent the tested
motion/proximity streams. Do not infer that all calibration has been consumed.

## Kernel and runtime

`sm8750-oneplus13-sensors.dts` enables only the stock sensor FastRPC context 5
and its exact two APPSSMMU stream mappings. `prepare-probe-dtb.py --adsp
--fastrpc` validates the 12 MiB dynamic remote heap and stock ADSP reservations.
The SM8750 stock driver uses IOMMU-backed metadata for this context; do not
apply unrelated physical-buffer workarounds from older SoCs.

After the verified ADSP is running, load the matching `fastrpc.ko` from the
outer recovery initramfs. From the prepared native Arch root:

```sh
timeout 300 /opt/oneplus13-sensors/hexagonrpcd \
  -f /dev/fastrpc-adsp-secure -d adsp -s \
  -R /opt/oneplus13-sensors/hexagon-files
/opt/oneplus13-sensors/ssccli --sensor=accelerometer --timeout=5
/opt/oneplus13-sensors/ssccli --sensor=gyroscope --timeout=3
/opt/oneplus13-sensors/ssccli --sensor=compass --timeout=3
/opt/oneplus13-sensors/ssccli --sensor=proximity --timeout=3
```

Probe 18 returned 130 accelerometer samples, 68 gyro samples, 16 compass
samples and NEAR/FAR proximity states, with no observed ADSP or SMMU fault.
The gyro CLI's printed unit is not a validated angular-rate unit. The firmware
returns a zero mount matrix and libssc substitutes identity. `ambient_light`
discovery returns no sensor. Brightness, call-proximity integration, calibration
accuracy and resume support remain unvalidated.

The successful sampling log also contains `Unsupported method: 36
(1f050100)`, after which the file service exits. Qualcomm's maintained
FastRPC at `a56e9d4de3614a8d8f4b0e15ff561350c4b686e3` identifies extended
method 36 as `apps_std_fopen_with_env_fd` in
[`apps_std_skel.c`](https://github.com/qualcomm/fastrpc/blob/a56e9d4de3614a8d8f4b0e15ff561350c4b686e3/src/apps_std_skel.c).
This is a remaining protocol gap, not proof of the ambient-light failure's
cause. Its file-descriptor/shared-buffer lifecycle needs implementation and
validation before adding it; do not return a normal HexagonFS handle as an FD.

## Persistent rotation integration

Fresh probe 20 native boot reproduced accelerometer streaming. The first
immediate SSC query can fail after the file service exits; wait for real samples
before starting sensor-proxy. A shortened 20-second file-service timeout also
interrupted an earlier initialization; retry from a clean reboot instead of
repeatedly attaching to a partially initialized sensor domain.

The owner held the phone in two controlled poses, each sampled for three
seconds:

| Pose | Samples | Mean X, Y, Z (m/s²) |
| --- | --- | --- |
| Upright portrait, USB down | 89 | -0.434, 9.853, 0.511 |
| Upright landscape, USB right | 72 | 9.758, 0.208, -0.892 |

These establish Android's device axes. The separate sensor-proxy mount matrix
is `-1, 0, 0; 0, -1, 0; 0, 0, 1`, converting those axes to its orientation
convention. `adapter/81-oneplus13-sensors.rules` exposes only the accelerometer
on `fastrpc-adsp-secure`. The owner confirmed the shared rotate suggestion
button rotates the desktop upright and that touch remains aligned.

Install signed Arch Linux ARM packages `iio-sensor-proxy` 3.9-1 and `libssc`
0.4.4-1 with their dependencies. Their signatures were verified against the
phone's trusted Arch Linux ARM keyring. The private libssc/ssccli used for
readiness checks stays under `/opt/oneplus13-sensors`.

Copy the matching initramfs `fastrpc.ko` to that directory, record its hash in
`modules.sha256` and kernel release in `KERNELRELEASE`. Install the udev rule
in `/etc/udev/rules.d/` and `scripts/start-native-sensors.sh` executable in
`/usr/local/lib/omarchy-mobile/oneplus13/`. The updated persistent session starts
it in the background after the desktop, logging to `/run/oneplus13-sensors.log`.
It checks board/root identity, module hash/release and running ADSP, permits one
initialization attempt per boot, serves the existing read-only firmware tree,
waits for accelerometer samples, then starts the system D-Bus and sensor-proxy.
Sensor failure does not prevent the desktop from starting.

Automatic startup passed a normal reboot. A screen-off/wake test confirmed
the shared watcher terminates its `monitor-sensor --accel` client while dark
and reconnects on wake. The terminated child currently remains a zombie until
the watcher next creates a subprocess; it no longer holds the sensor. Raw evidence and package artifacts
remain private in `out/native-persistent20-usb` and `out/sensors-survey`.
