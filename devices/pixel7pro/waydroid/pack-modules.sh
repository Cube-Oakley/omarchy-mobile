#!/usr/bin/env bash
# Pack the kernel modules Waydroid needs, built from mainline/linux for the
# phone's running kernel: the container bridge (bridge, veth), overlay, fuse,
# and every netfilter module (the host's NAT through iptables-legacy, and
# Android netd's own iptables and ip6tables rules inside the container). The kernel itself has none of them built
# in and the phone has no /lib/modules, so these are the only modules there.
#
# Each packed module's undefined symbols are checked against the kernel's
# vmlinux.symvers and the other packed modules. Modules outside the set may
# not load (ip_tunnel and sit want icmp_ndo_send, which a kernel built
# without NF_NAT does not export), so modpost errors are only warnings here.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
K="$ROOT/mainline/linux"
OUT="${1:-$ROOT/out/waydroid}"
WANT="bridge veth overlay fuse kernel/net/netfilter/ kernel/net/ipv4/netfilter/
      kernel/net/ipv6/netfilter/"
mk() { nice -n 10 make -C "$K" ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- KBUILD_MODPOST_WARN=1 "$@"; }

release="$(cat "$K/include/config/kernel.release")"
stage="$(mktemp -d)"
trap 'rm -rf "$stage"' EXIT
mk -j8 modules > "$stage/build.log" 2>&1 || { tail -20 "$stage/build.log"; exit 1; }
mk INSTALL_MOD_PATH="$stage" INSTALL_MOD_STRIP=1 modules_install > /dev/null
mods="$stage/lib/modules/$release"

python3 - "$mods" "$K" $WANT > "$stage/closure" <<'EOF'
import os, subprocess, sys
mods, k, want = sys.argv[1], sys.argv[2], sys.argv[3:]
dep = {}
for line in open(os.path.join(mods, "modules.dep")):
    path, deps = line.split(":", 1)
    dep[path] = deps.split()
by_name = {os.path.basename(p)[:-3]: p for p in dep}
closure = set()
def add(p):
    if p not in closure:
        closure.add(p)
        for d in dep[p]:
            add(d)
for name in want:
    if name.endswith("/"):
        for path in dep:
            if path.startswith(name):
                add(path)
    else:
        add(by_name[name])
provided = {l.split("\t")[1] for l in open(os.path.join(k, "vmlinux.symvers"))}
names = {os.path.basename(p)[:-3] for p in closure}
for l in open(os.path.join(k, "Module.symvers")):
    sym, mod = l.split("\t")[1:3]
    if os.path.basename(mod) in names:
        provided.add(sym)
bad = False
for p in sorted(closure):
    nm = subprocess.run(["aarch64-linux-gnu-nm", "-u", os.path.join(mods, p)],
                        capture_output=True, text=True, check=True).stdout
    missing = [s.split()[-1] for s in nm.splitlines() if s.split()[-1] not in provided]
    if missing:
        print(f"{p}: unresolved {missing}", file=sys.stderr)
        bad = True
    print(p)
sys.exit(1 if bad else 0)
EOF

mkdir -p "$OUT"
tarball="$OUT/waydroid-modules-$release.tar.gz"
tar czf "$tarball" -C "$mods" --transform "s#^#$release/#" \
    $(cat "$stage/closure") modules.order modules.builtin modules.builtin.modinfo
echo "$(wc -l < "$stage/closure") modules -> $tarball"
