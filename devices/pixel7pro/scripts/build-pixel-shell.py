#!/usr/bin/env python3
"""Build a local Pixel recovery/desktop image; never access the phone."""
import argparse
import hashlib
from pathlib import Path
import re
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parent.parent
BUSYBOX_SHA = '999cb969d09093a71716cfc747bb53cdada3f332c05eb5046c56e0f66a4d6d22'
ap = argparse.ArgumentParser(description=__doc__)
ap.add_argument('--output', type=Path, required=True)
ap.add_argument('--busybox', type=Path, default=ROOT.parent / 'oneplus7pro/.work/codex-native-initramfs/bin/busybox')
ap.add_argument('--seconds', type=int, default=600, help='automatic reboot interval; 0 leaves manual reboot')
ap.add_argument('--jobs', type=int, default=12)
ap.add_argument('--usb-network', action='store_true', help='build v10 ECM+ACM instead of v9 serial-only')
ap.add_argument('--drm', action='store_true', help='build v11 retained-display DRM with USB networking')
ap.add_argument('--acpm', action='store_true', help='build GS201 ACPM query bring-up controls, inactive at boot')
ap.add_argument('--gpu', action='store_true', help='build guarded GPU power-domain bring-up, implies --acpm')
ap.add_argument('--display-timing', action='store_true', help='build v16 display timing diagnostics, implies --gpu')
ap.add_argument('--direct-scanout', action='store_true', help='build v17 native DMA scanout, implies --display-timing')
ap.add_argument('--panel120', action='store_true', help='build v19 guarded 60/120 Hz panel modes and DPMS, implies --direct-scanout')
ap.add_argument('--persistent-root', action='store_true', help='include UFS/input modules and mount an already installed Pixel root; requires --seconds 0')
ap.add_argument('--initcall-debug', action='store_true', help='trace kernel initialization in this diagnostic image')
a = ap.parse_args()
if a.persistent_root:
    if a.seconds != 0:
        ap.error('--persistent-root requires --seconds 0')
    a.panel120 = True
if a.panel120:
    a.direct_scanout = True
if a.direct_scanout:
    a.display_timing = True
if a.display_timing:
    a.gpu = True
if a.gpu:
    a.acpm = True
if a.acpm:
    a.drm = True
if a.drm:
    a.usb_network = True
if not 0 <= a.seconds <= 86400:
    ap.error('--seconds must be 0..86400')
if hashlib.sha256(a.busybox.read_bytes()).hexdigest() != BUSYBOX_SHA:
    ap.error('BusyBox checksum does not match the verified OnePlus reference')
kernel = ROOT / 'mainline/linux'
base = subprocess.check_output(['git', '-C', str(kernel), 'rev-parse', 'HEAD'], text=True).strip()
expected_base = (ROOT / 'kernel/kernel-base.txt').read_text().strip()
if base != expected_base:
    ap.error('Kernel base differs from the saved v9 checkpoint')
for enabled, stem in ((a.acpm, 'acpm'), (a.gpu, 'gpu')):
    if not enabled:
        continue
    overlay = subprocess.check_output(['dtc', '-@', '-I', 'dts', '-O', 'dtb',
                                      str(ROOT / f'mainline/pixel-{stem}-overlay.dts')])
    (kernel / f'arch/arm64/kernel/pixel_{stem}_overlay.h').write_text(
        '/* SPDX-License-Identifier: GPL-2.0-only */\n'
        f'/* Generated from mainline/pixel-{stem}-overlay.dts */\n'
        f'static const unsigned char pixel_{stem}_overlay[] __aligned(8) = {{\n' +
        '\n'.join('\t' + ', '.join(f'0x{byte:02x}' for byte in overlay[i:i+12]) + ','
                  for i in range(0, len(overlay), 12)) + '\n};\n')
out = a.output.resolve()
out.mkdir(parents=True, exist_ok=False)
# Normal ABL boot may omit the Android boot header's command line, even though
# fastboot boot honors it. Persistent images must carry their early parameters
# in CONFIG_CMDLINE_FORCE: runtime bootconfig is too late for keep_bootcon/CMA.
# mem_sleep_default: the kernel would otherwise default to "deep", PSCI
# SYSTEM_SUSPEND, which needs the vendor's PMU and CPU_INFORM preparation that
# nothing here does yet. s2idle is the only safe system sleep.
cmdline = ('keep_bootcon loglevel=7 printk.time=1 fw_devlink=off clk_ignore_unused '
           'pd_ignore_unused panic=45 rdinit=/ourinit pixel_usb=1 mem_sleep_default=s2idle '
           f'pixel_test_seconds={a.seconds}')
if a.usb_network:
    cmdline += ' g_cdc.dev_addr=02:70:07:00:00:01 g_cdc.host_addr=02:70:07:00:00:02'
if a.drm:
    cmdline += ' pixel_drm=1'
if a.direct_scanout:
    cmdline += ' cma=128M@0-4G'
if a.panel120:
    # panel_sleep: sleep in while the screen is off, 0.32 W less than display
    # off alone (1.47 against 1.79 W, screen off and idle).
    cmdline += ' pixel_scanout.panel120=1 pixel_scanout.panel_sleep=1'
if a.initcall_debug:
    cmdline += ' initcall_debug'
(out / 'cmdline.txt').write_text(cmdline + '\n')
root = out / 'root'
for d in ('bin', 'etc', 'dev', 'proc', 'sys', 'tmp', 'run', 'root', 'usr'):
    (root / d).mkdir(parents=True, exist_ok=True)
(root / 'tmp').chmod(0o1777)
(root / 'root').chmod(0o700)
shutil.copy2(a.busybox, root / 'bin/busybox')
applets = subprocess.check_output(['qemu-aarch64', str(a.busybox), '--list'], text=True).splitlines()
for name in applets:
    if name != 'busybox': (root / 'bin' / name).symlink_to('busybox')
if a.gpu:
    fwdir = ROOT / 'out/checkpoints/20260925-gpu-v15/firmware'
    firmware = fwdir / 'mali_csffw.bin'
    if hashlib.sha256(firmware.read_bytes()).hexdigest() != 'a27847ea11f8efb3136340c3ba8aab413ae25145eeb4a7f64ff5edd829a2405b':
        ap.error('Pinned architecture 10.8 firmware checksum mismatch')
    target = root / 'lib/firmware/arm/mali/arch10.8'
    target.mkdir(parents=True)
    shutil.copy2(firmware, target / firmware.name)
    shutil.copy2(fwdir / 'LICENCE.mali_csffw', target / 'LICENCE.mali_csffw')
if a.persistent_root:
    # BCM4389 Wi-Fi: the stock vendor image's firmware, CLM and NVRAM under
    # the names brcmfmac asks for. Proprietary; they stay in the ignored out/.
    wifi = ROOT / 'out/stock-vendor/wifi'
    target = root / 'lib/firmware/brcm'
    target.mkdir(parents=True)
    for source, name, digest in (
        ('fw_bcmdhd.bin', 'brcmfmac4389c1-pcie.bin', '63d1909160af03ac'),
        ('bcmdhd_clm.blob', 'brcmfmac4389c1-pcie.clm_blob', 'adb2b2a28469c25f'),
        ('bcmdhd.cal', 'brcmfmac4389c1-pcie.txt', 'a08c7df9e2ebc152'),
    ):
        data = (wifi / source).read_bytes()
        if not hashlib.sha256(data).hexdigest().startswith(digest):
            ap.error(f'Stock Wi-Fi file {source} checksum mismatch')
        (target / name).write_bytes(data)
    # BCM4389 Bluetooth: the stock vendor image's patch firmware
    # (BCM4389C1, baseline 0391) under the name btbcm asks for.
    data = (ROOT / 'out/stock-vendor/bt/BCM.hcd').read_bytes()
    if not hashlib.sha256(data).hexdigest().startswith('c2c30e9760a94ef3'):
        ap.error('Stock Bluetooth firmware checksum mismatch')
    (target / 'BCM.hcd').write_bytes(data)
    # Audio: the CS35L41 speaker protection firmware with the stock tunings,
    # under the names wm_adsp looks for with the amplifiers' subsystem ID
    # (kernel/spi): the left (top) amplifier's tuning, then the right
    # (bottom) one's with -r. The AoC firmware (21 MB) does not fit under the
    # boot image's AVB boundary; it is on the Pixel root instead
    # (scripts/build-pixel-root-image.py).
    audio = ROOT / 'out/stock-vendor/audio/firmware'
    (root / 'lib/firmware/cirrus').mkdir()
    for source, name, digest in (
        ('cs35l41-dsp1-spk-prot.wmfw', 'cirrus/cs35l41-dsp1-spk-prot-cheetah.wmfw', '9bf65824f8659ab2'),
        ('cs35l41-dsp1-spk-prot.bin', 'cirrus/cs35l41-dsp1-spk-prot-cheetah.bin', '0b0ed1baabc487bf'),
        ('R-cs35l41-dsp1-spk-prot.bin', 'cirrus/cs35l41-dsp1-spk-prot-cheetah-r.bin', '105dd6d0d050c557'),
    ):
        data = (audio / source).read_bytes()
        if not hashlib.sha256(data).hexdigest().startswith(digest):
            ap.error(f'Stock audio firmware {source} checksum mismatch')
        (root / 'lib/firmware' / name).write_bytes(data)
    # cfg80211 requires the signed regulatory database (wireless-regdb).
    for name in ('regulatory.db', 'regulatory.db.p7s'):
        shutil.copy2(Path('/usr/lib/firmware') / name, root / 'lib/firmware' / name)
(root / 'sbin').symlink_to('bin')
(root / 'usr/bin').symlink_to('../bin')
(root / 'usr/sbin').symlink_to('../bin')
(root / 'etc/passwd').write_text('root:x:0:0:Root:/root:/bin/sh\n')
(root / 'etc/group').write_text('root:x:0:\n')
(root / 'etc/os-release').write_text('NAME="Pixel Linux bring-up"\nID=pixel-bringup\nPRETTY_NAME="Pixel Linux RAM-only shell"\n')
if a.usb_network:
    shutil.copy2(ROOT / 'mainline/pixel-usb-network.sh', root / 'etc/pixel-usb-network.sh')
if a.persistent_root:
    shutil.copy2(ROOT / 'scripts/pixel-persistent-start.sh', root / 'etc/pixel-persistent-start.sh')
if a.drm:
    font = bytes(int(x, 16) for x in re.findall(r'^\s*(0x[0-9a-fA-F]{2}),',
                 (kernel / 'lib/fonts/font_8x16.c').read_text(), re.M))
    assert len(font) == 4096
    (out / 'font.h').write_text('/* SPDX-License-Identifier: GPL-2.0; Linux VGA font */\n'
                              'static const unsigned char font[4096] = {' +
                              ','.join(str(x) for x in font) + '};\n')
    subprocess.run(['aarch64-linux-gnu-gcc', '-static', '-O2', '-Wall', '-Wextra',
                    '-I/usr/include/libdrm', '-I' + str(out), str(ROOT / 'mainline/pixel-kms-test.c'),
                    '-o', str(root / 'bin/pixel-kms-test')], check=True)
subprocess.run(['aarch64-linux-gnu-gcc', '-static', '-O2', '-Wall', '-Wextra',
                *(['-DPIXEL_USB_NETWORK'] if a.usb_network else []),
                *(['-DPIXEL_PERSISTENT_ROOT'] if a.persistent_root else []),
                str(ROOT / 'mainline/pixel-shell-init.c'), '-o', str(root / 'ourinit')], check=True)
shutil.copy2(kernel / '.config', out / 'config-before')
shutil.copy2(ROOT / 'kernel/native-bringup-v9.config', kernel / '.config')
subprocess.run([str(kernel / 'scripts/config'), '--file', str(kernel / '.config'),
                '--set-str', 'INITRAMFS_SOURCE', str(root),
                '--set-str', 'BOOT_CONFIG_EMBED_FILE', str(ROOT / 'mainline/bootconfig'),
                *(['--set-str', 'CMDLINE', cmdline, '--enable', 'CMDLINE_FORCE',
                   '--enable', 'BRCMFMAC_PCIE',
                   # Use only the stock DT's reserved ramoops region.
                   '--enable', 'PSTORE', '--enable', 'PSTORE_RAM',
                   '--enable', 'PSTORE_CONSOLE',
                   # Staged suspend tests (/sys/power/pm_test) and timings
                   '--enable', 'PM_DEBUG', '--enable', 'PM_SLEEP_DEBUG'] if a.persistent_root else []),
                '--set-str', 'LOCALVERSION', '-pixel-panel19' if a.panel120 else '-pixel-scanout17' if a.direct_scanout else ('-pixel-display16' if a.display_timing else ('-pixel-gpu15' if a.gpu else ('-pixel-power14' if a.acpm else ('-pixel-drm11' if a.drm else ('-pixel-net10' if a.usb_network else '-pixel-shell9'))))),
                *(['--enable', 'EXYNOS_ACPM_PROTOCOL', '--enable', 'EXYNOS_MBOX',
                   '--enable', 'EXYNOS_ACPM_CLK', '--enable', 'GS201_ACPM_THERMAL',
                   '--disable', 'CPU_FREQ_DEFAULT_GOV_SCHEDUTIL', '--enable', 'CPU_FREQ_DEFAULT_GOV_USERSPACE',
                   '--enable', 'THERMAL_EMULATION', '--set-val', 'THERMAL_EMERGENCY_POWEROFF_DELAY_MS', '3000'] if a.acpm else []),
                *(['--enable', 'GS201_G3D_PM_DOMAINS', '--enable', 'DRM_PANTHOR'] if a.gpu else []),
                *(['--enable', 'DRM_PIXEL_HANDOFF_TIMING'] if a.display_timing else []),
                *(['--enable', 'DRM_PIXEL_SCANOUT'] if a.direct_scanout else []),
                *(['--enable', 'DRM', '--enable', 'DRM_PIXEL_HANDOFF',
                   '--disable', 'DRM_FBDEV_EMULATION', '--disable', 'DRM_PANIC'] if a.drm else []),
                *(['--disable', 'USB_G_SERIAL', '--enable', 'USB_CDC_COMPOSITE'] if a.usb_network else [])], check=True)
with (out / 'build.log').open('w') as log:
    for target in ('olddefconfig', 'Image'):
        subprocess.run(['make', '-C', str(kernel), 'ARCH=arm64',
                        'CROSS_COMPILE=aarch64-linux-gnu-', f'-j{a.jobs}', target],
                       stdout=log, stderr=subprocess.STDOUT, check=True)
    if a.persistent_root:
        modules = out / 'modules'
        modules.mkdir()
        for name, source in (
            ('pixel-ufs.c', ROOT / 'kernel/storage/pixel-ufs.c'),
            ('pixel-battery-model.h', ROOT / 'kernel/battery/pixel-battery-model.h'),
            ('pixel-sicd.h', ROOT / 'kernel/cpupm/pixel-sicd.h'),
            ('pixel-powerkey.c', ROOT / 'kernel/powerkey/pixel-powerkey.c'),
            ('pixel-reboot.c', ROOT / 'kernel/reboot/pixel-reboot.c'),
            ('pixel-rtc.c', ROOT / 'kernel/rtc/pixel-rtc.c'),
            ('pixel-battery.c', ROOT / 'kernel/battery/pixel-battery.c'),
            ('pixel-cpupm.c', ROOT / 'kernel/cpupm/pixel-cpupm.c'),
            ('pixel-mct.c', ROOT / 'kernel/cpupm/pixel-mct.c'),
            ('pixel-pd-off.c', ROOT / 'kernel/pd/pixel-pd-off.c'),
            ('pixel-odpm.c', ROOT / 'kernel/odpm/pixel-odpm.c'),
            ('pixel_touch_input.c', ROOT / 'mainline/pixel-touch-input.c'),
            ('pixel-pcie.c', ROOT / 'kernel/pcie/pixel-pcie.c'),
            ('pixel-keys.c', ROOT / 'kernel/keys/pixel-keys.c'),
            ('pixel-hsi2c.c', ROOT / 'kernel/i2c/pixel-hsi2c.c'),
            ('pixel-usb-switch.c', ROOT / 'kernel/usb/pixel-usb-switch.c'),
            ('pixel-torch.c', ROOT / 'kernel/torch/pixel-torch.c'),
            ('pixel-haptics.c', ROOT / 'kernel/haptics/pixel-haptics.c'),
            ('pixel-bt.c', ROOT / 'kernel/bluetooth/pixel-bt.c'),
            ('pixel-gpio.c', ROOT / 'kernel/gpio/pixel-gpio.c'),
            ('pixel-spi.c', ROOT / 'kernel/spi/pixel-spi.c'),
            ('pixel-aoc-power.c', ROOT / 'kernel/aoc-power/pixel-aoc-power.c'),
        ):
            shutil.copy2(source, modules / name)
        subprocess.run([sys.executable, str(ROOT / 'kernel/bluetooth/gen-overlay-header.py'),
                        str(modules / 'pixel_bt_overlay.h')], check=True)
        (modules / 'Makefile').write_text('obj-m += pixel-ufs.o pixel-powerkey.o pixel-reboot.o pixel-rtc.o pixel-battery.o pixel-cpupm.o pixel-mct.o pixel-pd-off.o pixel-odpm.o pixel_touch_input.o pixel-pcie.o pixel-keys.o pixel-hsi2c.o pixel-usb-switch.o pixel-torch.o pixel-haptics.o pixel-bt.o '
                                          'pixel-gpio.o pixel-spi.o pixel-aoc-power.o\n'
                                          'CFLAGS_pixel-pcie.o += -I$(srctree)/drivers/pci/controller/dwc\n')
        old_symbols = {line.split()[1] for line in (kernel / 'Module.symvers').read_text().splitlines()}
        extra = ''.join(line for line in (kernel / 'vmlinux.symvers').read_text().splitlines(True)
                        if line.split()[1] not in old_symbols)
        (modules / 'extra.symvers').write_text(extra)
        subprocess.run(['make', '-C', str(kernel), 'ARCH=arm64',
                        'CROSS_COMPILE=aarch64-linux-gnu-', f'M={modules}',
                        f'KBUILD_EXTRA_SYMBOLS={modules}/extra.symvers', 'modules'],
                       stdout=log, stderr=subprocess.STDOUT, check=True)
        target = root / 'lib/modules/pixel'
        target.mkdir(parents=True)
        for module in modules.glob('*.ko'):
            shutil.copy2(module, target / module.name)
        # The Wi-Fi stack is in-tree but modular; build it against this kernel
        # and ship it here, stripped, with the other Pixel modules.
        wifi_dirs = ('net/rfkill', 'net/wireless', 'drivers/net/wireless/broadcom/brcm80211')
        for n, directory in enumerate(wifi_dirs):
            symbols = ' '.join([str(modules / 'extra.symvers')] +
                               [str(kernel / d / 'Module.symvers') for d in wifi_dirs[:n]])
            subprocess.run(['make', '-C', str(kernel), 'ARCH=arm64',
                            'CROSS_COMPILE=aarch64-linux-gnu-', f'-j{a.jobs}', f'M={kernel / directory}',
                            f'KBUILD_EXTRA_SYMBOLS={symbols}', 'modules'],
                           stdout=log, stderr=subprocess.STDOUT, check=True)
        for relative in ('net/rfkill/rfkill.ko', 'net/wireless/cfg80211.ko',
                         'drivers/net/wireless/broadcom/brcm80211/brcmutil/brcmutil.ko',
                         'drivers/net/wireless/broadcom/brcm80211/brcmfmac/brcmfmac.ko',
                         'drivers/net/wireless/broadcom/brcm80211/brcmfmac/wcc/brcmfmac-wcc.ko'):
            subprocess.run(['aarch64-linux-gnu-strip', '--strip-debug', '-o',
                            str(target / Path(relative).name), str(kernel / relative)], check=True)
        # Bluetooth, likewise: ECDH for pairing, the core, and the UART
        # driver with btbcm (hci_uart also links btqca). These directories
        # hold modules for other platforms whose symbols are missing here.
        bt_dirs = ('crypto', 'net/bluetooth', 'drivers/bluetooth')
        for n, directory in enumerate(bt_dirs):
            symbols = ' '.join([str(modules / 'extra.symvers'), str(kernel / 'net/rfkill/Module.symvers')] +
                               [str(kernel / d / 'Module.symvers') for d in bt_dirs[:n]])
            subprocess.run(['make', '-C', str(kernel), 'ARCH=arm64',
                            'CROSS_COMPILE=aarch64-linux-gnu-', f'-j{a.jobs}', f'M={kernel / directory}',
                            f'KBUILD_EXTRA_SYMBOLS={symbols}', 'KBUILD_MODPOST_WARN=1', 'modules'],
                           stdout=log, stderr=subprocess.STDOUT, check=True)
        for relative in ('crypto/kpp.ko', 'crypto/ecc.ko', 'crypto/ecdh_generic.ko',
                         'net/bluetooth/bluetooth.ko', 'drivers/bluetooth/btbcm.ko',
                         'drivers/bluetooth/btqca.ko', 'drivers/bluetooth/hci_uart.ko'):
            subprocess.run(['aarch64-linux-gnu-strip', '--strip-debug', '-o',
                            str(target / Path(relative).name), str(kernel / relative)], check=True)
        # Audio. ALSA and ASoC are modular in this kernel. Google's AoC drivers
        # (Trusty, GSA, AoC core and ALSA card) are assembled from pinned
        # sources plus the port's patches (kernel/aoc). The CS35L41 codec and
        # its DSP library come from this tree, built on their own because the
        # kernel configuration does not enable them.
        subprocess.run(['make', '-C', str(kernel), 'ARCH=arm64', 'CROSS_COMPILE=aarch64-linux-gnu-',
                        f'-j{a.jobs}', f'M={kernel / "sound"}',
                        f'KBUILD_EXTRA_SYMBOLS={modules / "extra.symvers"}', 'KBUILD_MODPOST_WARN=1',
                        'modules'], stdout=log, stderr=subprocess.STDOUT, check=True)
        aoc = out / 'aoc'
        subprocess.run([sys.executable, str(ROOT / 'kernel/aoc/assemble.py'), str(aoc)],
                       stdout=log, stderr=subprocess.STDOUT, check=True)
        symbols = [str(modules / 'extra.symvers'), str(kernel / 'sound/Module.symvers')]
        for directory in ('trusty', 'gsa', 'aoc', 'aoc/alsa'):
            subprocess.run(['make', '-C', str(kernel), 'ARCH=arm64', 'CROSS_COMPILE=aarch64-linux-gnu-',
                            f'-j{a.jobs}', f'M={aoc / directory}',
                            f'KBUILD_EXTRA_SYMBOLS={" ".join(symbols)}', 'modules'],
                           stdout=log, stderr=subprocess.STDOUT, check=True)
            symbols.append(str(aoc / directory / 'Module.symvers'))
        codec = out / 'cs35l41'
        codec.mkdir()
        for relative in ('sound/soc/codecs/cs35l41.c', 'sound/soc/codecs/cs35l41.h',
                         'sound/soc/codecs/cs35l41-spi.c', 'sound/soc/codecs/cs35l41-lib.c',
                         'sound/soc/codecs/wm_adsp.c', 'sound/soc/codecs/wm_adsp.h',
                         'drivers/firmware/cirrus/cs_dsp.c', 'drivers/firmware/cirrus/cs_dsp.h'):
            shutil.copy2(kernel / relative, codec / Path(relative).name)
        (codec / 'Makefile').write_text(
            'obj-m += cs_dsp.o snd-soc-wm-adsp.o snd-soc-cs35l41-lib.o snd-soc-cs35l41.o snd-soc-cs35l41-spi.o\n'
            'snd-soc-wm-adsp-y := wm_adsp.o\n'
            'snd-soc-cs35l41-lib-y := cs35l41-lib.o\n'
            'snd-soc-cs35l41-y := cs35l41.o\n'
            'snd-soc-cs35l41-spi-y := cs35l41-spi.o\n')
        subprocess.run(['make', '-C', str(kernel), 'ARCH=arm64', 'CROSS_COMPILE=aarch64-linux-gnu-',
                        f'-j{a.jobs}', f'M={codec}', f'KBUILD_EXTRA_SYMBOLS={" ".join(symbols[:2])}',
                        'modules'], stdout=log, stderr=subprocess.STDOUT, check=True)
        for module in (kernel / 'sound/soundcore.ko', kernel / 'sound/core/snd.ko',
                       kernel / 'sound/core/snd-timer.ko', kernel / 'sound/core/snd-pcm.ko',
                       kernel / 'sound/core/snd-compress.ko', kernel / 'sound/core/snd-pcm-dmaengine.ko',
                       kernel / 'sound/soc/snd-soc-core.ko',
                       *(aoc / 'trusty' / f'{m}.ko' for m in ('trusty-core', 'trusty-log', 'trusty-ipc',
                                                               'trusty-virtio')),
                       aoc / 'gsa/gsa.ko',
                       *(aoc / 'aoc' / f'{m}.ko' for m in ('mailbox-wc', 'aoc_core', 'aoc_char_dev',
                                                            'aoc_channel_dev', 'aoc_control_dev')),
                       aoc / 'aoc/alsa/aoc_alsa_dev_util.ko', aoc / 'aoc/alsa/aoc_alsa_dev.ko',
                       *(codec / f'{m}.ko' for m in ('cs_dsp', 'snd-soc-wm-adsp', 'snd-soc-cs35l41-lib',
                                                      'snd-soc-cs35l41', 'snd-soc-cs35l41-spi'))):
            subprocess.run(['aarch64-linux-gnu-strip', '--strip-debug', '-o',
                            str(target / module.name), str(module)], check=True)
        # brcmfmac loads its firmware-vendor module with request_module(), which
        # runs in this initramfs; it has no module tree, so the boot script
        # points kernel.modprobe here.
        helper = target / 'request-module'
        helper.write_text('#!/bin/sh\n# kernel.modprobe helper: modprobe -q -- NAME\n'
                          'for name; do :; done\n'
                          '[ -f "/lib/modules/pixel/$name.ko" ] || exit 1\n'
                          'exec insmod "/lib/modules/pixel/$name.ko"\n')
        helper.chmod(0o755)
        subprocess.run(['make', '-C', str(kernel), 'ARCH=arm64',
                        'CROSS_COMPILE=aarch64-linux-gnu-', f'-j{a.jobs}', 'Image'],
                       stdout=log, stderr=subprocess.STDOUT, check=True)
shutil.copy2(kernel / 'arch/arm64/boot/Image', out / 'Image')
notes = subprocess.check_output(['aarch64-linux-gnu-readelf', '-n', str(kernel / 'vmlinux')], text=True)
build_id = re.search(r'Build ID:\s+([0-9a-f]+)', notes)
if not build_id:
    raise RuntimeError('Kernel ELF build ID missing')
(out / 'kernel-build-id.txt').write_text(build_id[1] + '\n')
shutil.copy2(kernel / '.config', out / 'kernel.config')
shutil.copy2(ROOT / 'mainline/pixel-shell-init.c', out / 'pixel-shell-init.c')
shutil.copy2(ROOT / 'mainline/bootconfig', out / 'bootconfig')
for name in ('pixel_earlycon.c', 'pixel_usb.c', 'pixel_watchdog.c'):
    shutil.copy2(kernel / 'arch/arm64/kernel' / name, out / name)
if a.drm:
    shutil.copy2(kernel / 'drivers/gpu/drm/sysfb/pixel_handoff.c', out / 'pixel_handoff.c')
    shutil.copy2(kernel / 'include/linux/pixel_handoff.h', out / 'pixel_handoff.h')
    shutil.copy2(ROOT / 'mainline/pixel-kms-test.c', out / 'pixel-kms-test.c')
if a.direct_scanout:
    shutil.copy2(kernel / 'drivers/gpu/drm/sysfb/pixel_scanout.c', out / 'pixel_scanout.c')
    shutil.copy2(kernel / 'drivers/gpu/drm/sysfb/pixel_panel.h', out / 'pixel_panel.h')
    shutil.copy2(kernel / 'drivers/gpu/drm/sysfb/pixel_bandwidth.h', out / 'pixel_bandwidth.h')
if a.acpm:
    for relative in ('arch/arm64/kernel/pixel_acpm.c',
                     'arch/arm64/kernel/pixel_acpm_overlay.h',
                     'drivers/clk/samsung/clk-gs201.c',
                     'drivers/thermal/samsung/gs201_acpm_thermal.c',
                     'include/dt-bindings/clock/google,gs201.h'):
        shutil.copy2(kernel / relative, out / Path(relative).name)
    shutil.copy2(ROOT / 'mainline/pixel-acpm-overlay.dts', out / 'pixel-acpm-overlay.dts')
if a.gpu:
    for relative in ('arch/arm64/kernel/pixel_gpu.c', 'arch/arm64/kernel/pixel_gpu_overlay.h',
                     'drivers/pmdomain/samsung/gs201-g3d-pd.c'):
        shutil.copy2(kernel / relative, out / Path(relative).name)
    shutil.copy2(ROOT / 'mainline/pixel-gpu-overlay.dts', out / 'pixel-gpu-overlay.dts')
(out / 'kernel-tracked.patch').write_bytes(subprocess.check_output(['git', '-C', str(kernel), 'diff']))
(out / 'kernel-base.txt').write_bytes(subprocess.check_output(['git', '-C', str(kernel), 'rev-parse', 'HEAD']))
subprocess.run(['python', str(ROOT / 'scripts/pack-pixel-ram-boot.py'), '--kernel', str(out / 'Image'),
                '--output', str(out / 'boot-pixel-shell.img'), '--cmdline', cmdline], check=True)
with (out / 'SHA256SUMS').open('w') as sums:
    files = sorted(p for p in out.rglob('*') if p.is_file() and not p.is_symlink() and p.name != 'SHA256SUMS')
    for p in files:
        sums.write(hashlib.sha256(p.read_bytes()).hexdigest() + '  ' + str(p.relative_to(out)) + '\n')
print(f'Built {out}/boot-pixel-shell.img; no device commands issued.')
