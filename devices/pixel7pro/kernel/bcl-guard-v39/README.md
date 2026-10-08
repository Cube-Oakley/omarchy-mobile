# v39: CPU scaling after a hot reset; Waydroid's kernel options (2026-10-05)

v38 with one code change and new configuration.

**CPU scaling guard** (`0001-pixel-acpm-cpu-scaling-guard-cpu-gpu-sensors.patch`).
`pixel_acpm` starts CPU scaling only with plausible temperatures below 60 C.
It checked all seven firmware sensors. The ISP, TPU and AUR blocks are
powered off at boot (pixel-pd-off), and their sensors keep the reading they
had then: after the October 5 brownout reset the ISP read 68 C all day, CPU
scaling never started, and the CPUs stayed at the bootloader's rates
(1.1-1.6 GHz). The guard now reads only the BIG, MID, LITTLE and G3D sensors.
The boot script's cool-down wait does the same.

**Configuration** (`config-added-since-v38.txt`, set by
`scripts/build-pixel-shell.py --persistent-root`), for Waydroid:
- `PSI`: Android's lmkd, a critical service, needs it;
- `IP_ADVANCED_ROUTER`, `IP_MULTIPLE_TABLES`, `IPV6_MULTIPLE_TABLES`,
  `IPV6_ROUTER_PREF`, `IPV6_ROUTE_INFO`, `IPV6_OPTIMISTIC_DAD`,
  `INET_UDP_DIAG`, `INET_DIAG_DESTROY`: netd's per-network routing;
- legacy iptables and ip6tables with NAT, and Android's netfilter set, as
  modules (`waydroid/pack-modules.sh` packs them). Built in as a result:
  nfnetlink, ICMP's `icmp_ndo_send`, the conntrack mark field, textsearch.

PSI adds a field to `task_struct` after `comm`, `pid` and `mm`, and the
routing options grow `struct net` after its `ipv4` member. The AoC modules
are rebuilt with the kernel. None of the other out-of-tree modules on the
root (kernel/*, the cpif modem bundle) use a field past those points, so
they were not rebuilt.

**Validated by RAM boot**: CPU scaling with schedutil at the full ranges,
`pixel-bcl apply=1` loaded at boot, `/proc/pressure`, the desktop, Wi-Fi,
cellular data, audio, sensors and Bluetooth; Android in Waydroid booted with
GPU rendering and internet.

**Installed in boot_a on October 5** (approved by the user); readback matched
`bec33322…`, the previous image was v38 (`f4fddd17…`). The installed boot
came up with the same checks. Built with
`scripts/build-pixel-shell.py --persistent-root --seconds 0 --cma 256M`.
