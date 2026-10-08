#!/usr/bin/env python3
"""Make Waydroid's Mesa drive the Pixel 7 Pro's Mali-G710 (Panthor).

Upstream Mesa has no model entry for the G710's product ID (GPU_ID 0xa802....),
so the vendor image's libgallium_dri.so refuses the GPU: Android's gralloc
cannot allocate a buffer and surfaceflinger aborts at every start. The host's
/opt/pixel-mesa carries adapter/userspace/mesa/0001-panfrost-add-g710-model.patch,
whose G710 entry copies upstream's G610 entry (same tile buffer and rates).
This makes the vendor library match the G710 with that same entry: in the
G610 row of the model table the match value 0xa8070000 (mask 0xff0f0000)
becomes 0xa8020000. Android then reports "Mali-G610 MC7 (Panfrost)".

    patch-mesa-g710.py VENDOR_IMG OUT_DIR

Writes OUT_DIR/lib64/libgallium_dri.so and OUT_DIR/lib/libgallium_dri.so. On
the phone they go in /var/lib/waydroid/overlay/vendor/{lib64,lib}/ (Waydroid
mounts that over /vendor), so the image itself is never changed. Checked
against Mesa 26.0.8 in lineage-20.0-20260927-MAINLINE-waydroid_arm64-vendor;
a vendor update needs the script run again, and it refuses a library where the
row is not found exactly once.
"""
import hashlib
import subprocess
import sys
from pathlib import Path

G610 = bytes.fromhex('000007a8' '00000fff')  # LE u32 0xa8070000, mask 0xff0f0000
G710 = bytes.fromhex('000002a8' '00000fff')


def main():
    if len(sys.argv) != 3:
        raise SystemExit(__doc__.split('\n\n')[2])
    image, out = Path(sys.argv[1]), Path(sys.argv[2])
    for arch in ('lib64', 'lib'):
        dest = out / arch / 'libgallium_dri.so'
        dest.parent.mkdir(parents=True, exist_ok=True)
        subprocess.run(['debugfs', '-R', f'dump /{arch}/libgallium_dri.so {dest}', str(image)],
                       check=True, capture_output=True)
        data = bytearray(dest.read_bytes())
        hits = []
        at = data.find(G610)
        while at >= 0:
            hits.append(at)
            at = data.find(G610, at + 1)
        if len(hits) != 1 or G710 in data:
            dest.unlink()
            raise SystemExit(f'{arch}: G610 row found {len(hits)} times; not patching')
        data[hits[0]:hits[0] + len(G710)] = G710
        dest.write_bytes(data)
        print(f'{dest}: row at {hits[0]:#x}, sha256 {hashlib.sha256(data).hexdigest()[:16]}')


if __name__ == '__main__':
    main()
