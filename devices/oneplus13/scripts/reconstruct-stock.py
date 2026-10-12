#!/usr/bin/env python3
"""Reconstruct stock boot partitions on the host; never communicates with a phone.

Uses the existing OTA ZIP/protobuf reader, authenticated payload metadata, and
full partition hashes at every step. AOSP format references:
https://android.googlesource.com/platform/system/update_engine/+/refs/heads/main/update_metadata.proto
https://android.googlesource.com/platform/external/bsdiff/+/refs/heads/main/patch_reader.cc

Requires openssl and brotli. Inputs and outputs belong in ignored out/.
"""
import argparse
import bz2
import hashlib
import importlib.util
import json
import lzma
from pathlib import Path
import struct
import subprocess

ROOT = Path(__file__).resolve().parents[1]
READER = ROOT.parent / "oneplus7pro/scripts/fetch_ota_partitions.py"
spec = importlib.util.spec_from_file_location("ota_reader", READER)
ota = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ota)
PARTITIONS = ("boot", "init_boot", "vendor_boot", "dtbo", "recovery", "vbmeta",
              "vbmeta_system", "vbmeta_vendor")
MAX_IMAGE = 256 * 1024 * 1024


def require(condition, message):
    if not condition:
        raise ValueError(message)


def digest(data):
    return hashlib.sha256(data).digest()


def signed_offset(data):
    require(len(data) == 8, "Truncated bsdiff integer")
    value = int.from_bytes(data, "little")
    return -(value & ((1 << 63) - 1)) if value >> 63 else value


def decompress(data, kind):
    if kind == 1:
        return bz2.decompress(data)
    if kind == 2:
        return subprocess.run(["brotli", "-d", "-c"], input=data,
                              capture_output=True, check=True, timeout=120).stdout
    raise ValueError(f"Unsupported bsdiff compression {kind}")


def bspatch(old, patch, expected_size):
    require(len(patch) >= 32, "Truncated bsdiff header")
    if patch[:8] == b"BSDIFF40":
        kinds = (1, 1, 1)
    else:
        require(patch[:5] == b"BSDF2", "Unknown bsdiff format")
        kinds = patch[5:8]
    control_len, diff_len, size = [signed_offset(patch[n:n+8]) for n in (8, 16, 24)]
    require(0 <= control_len <= len(patch) - 32, "Bad control length")
    require(0 <= diff_len <= len(patch) - 32 - control_len, "Bad diff length")
    require(size == expected_size and size <= MAX_IMAGE, "Unexpected patch output size")
    cut = 32 + control_len
    control, diff, extra = [decompress(data, kind) for data, kind in zip(
        (patch[32:cut], patch[cut:cut+diff_len], patch[cut+diff_len:]), kinds)]
    require(len(control) % 24 == 0, "Truncated control triple")
    result = bytearray()
    old_pos = diff_pos = extra_pos = 0
    for pos in range(0, len(control), 24):
        add, copy, seek = [signed_offset(control[n:n+8]) for n in range(pos, pos+24, 8)]
        require(add >= 0 and copy >= 0 and len(result)+add+copy <= size,
                "Patch exceeds destination")
        require(diff_pos+add <= len(diff) and extra_pos+copy <= len(extra),
                "Truncated patch stream")
        # BSDIFF permits seeks beyond the source: out-of-range old bytes are zero.
        delta = bytearray(diff[diff_pos:diff_pos+add])
        low, high = max(0, -old_pos), min(add, len(old)-old_pos)
        if high > low:
            delta[low:high] = bytes((a+b) & 255 for a, b in
                                   zip(delta[low:high], old[old_pos+low:old_pos+high]))
        result.extend(delta)
        result.extend(extra[extra_pos:extra_pos+copy])
        old_pos += add + seek
        diff_pos += add
        extra_pos += copy
    require(len(result) == size, "Short patch output")
    require(diff_pos == len(diff) and extra_pos == len(extra), "Unused patch stream data")
    return bytes(result)


def extents(raw, block, limit):
    result = []
    for item in raw:
        e = ota.pb(item)
        start, length = e.get(1, [0])[0]*block, e.get(2, [0])[0]*block
        require(start <= limit and length <= limit-start, "Extent outside partition")
        result.append((start, length))
    return result


def check_info(data, raw):
    info = ota.pb(raw)
    require(len(data) == info[1][0] and digest(data) == info[2][0],
            "Partition size/SHA256 mismatch")


def metadata(entry, dest, keys):
    url = entry["url"]
    require(url.startswith("https://android.googleapis.com/packages/ota-api/package/"),
            "Expected the official OTA host")
    base = ota.payload_offset(url)
    head = ota.fetch(url, base, 24)
    require(head[:4] == b"CrAU" and struct.unpack_from(">Q", head, 4)[0] == 2,
            "Expected payload v2")
    size, = struct.unpack_from(">Q", head, 12)
    sigsize, = struct.unpack_from(">I", head, 20)
    require(0 < size < 32*1024*1024 and 0 < sigsize < 65536, "Invalid metadata length")
    raw = ota.fetch(url, base+24, size)
    signed = dest / "signed-metadata.bin"
    signed.write_bytes(head+raw)
    signatures = ota.pb(ota.fetch(url, base+24+size, sigsize))
    verified = False
    for signature in signatures.get(1, []):
        fields = ota.pb(signature)
        value = fields[2][0]
        length = fields.get(3, [len(value)])[0]
        require(0 < length <= len(value), "Invalid signature length")
        path = dest / "metadata-signature.bin"
        path.write_bytes(value[:length])
        for key in keys:
            run = subprocess.run(["openssl", "dgst", "-sha256", "-verify", str(key),
                                  "-signature", str(path), str(signed)], capture_output=True)
            if run.returncode == 0:
                verified = True
                break
        if verified:
            break
    require(verified, "Payload signature does not match stock trusted keys")
    return ota.pb(raw), base+24+size+sigsize


def reconstruct(entry, dest, previous, keys):
    manifest, base = metadata(entry, dest, keys)
    block = manifest.get(3, [4096])[0]
    require(block == 4096, "Unexpected payload block size")
    parts = {ota.pb(p)[1][0].decode(): ota.pb(p) for p in manifest.get(13, [])}
    require(all(name in parts for name in PARTITIONS), "Required boot partition missing")
    results = {}
    for name in PARTITIONS:
        part = parts[name]
        info = ota.pb(part[7][0])
        size = info[1][0]
        require(0 < size <= MAX_IMAGE, "Unexpected boot partition size")
        target = dest / (name + ".img")
        if target.exists():
            check_info(target.read_bytes(), part[7][0])
            results[name] = info[2][0].hex()
            print(f"{entry['id']} {name}: existing hash verified", flush=True)
            continue
        old = b""
        if 6 in part:
            require(previous is not None, "Delta requires previous images")
            old = (previous / (name + ".img")).read_bytes()
            check_info(old, part[6][0])
        output = bytearray(size)
        for raw in part.get(8, []):
            op = ota.pb(raw)
            kind = op[1][0]
            require(kind in (0, 1, 4, 5, 6, 8, 10), f"Unsupported operation {kind}")
            dst = extents(op.get(6, []), block, size)
            dst_len = sum(length for _, length in dst)
            src = b""
            if kind in (4, 5, 10):
                src = b"".join(old[start:start+length] for start, length in
                               extents(op.get(4, []), block, len(old)))
                if 9 in op:
                    require(digest(src) == op[9][0], "Operation source hash mismatch")
            data = b""
            if kind not in (4, 6):
                length = op.get(3, [0])[0]
                require(0 < length <= MAX_IMAGE, "Unexpected operation data size")
                data = ota.fetch(entry["url"], base+op.get(2, [0])[0], length)
                require(8 in op and digest(data) == op[8][0], "Operation data hash mismatch")
            if kind == 1:
                data = bz2.decompress(data)
            elif kind == 8:
                data = lzma.decompress(data)
            elif kind == 4:
                data = src
            elif kind in (5, 10):
                data = bspatch(src, data, dst_len)
            elif kind == 6:
                data = bytes(dst_len)
            require(len(data) == dst_len, "Operation length differs from destination extents")
            offset = 0
            for start, length in dst:
                output[start:start+length] = data[offset:offset+length]
                offset += length
        check_info(output, part[7][0])
        partial = target.with_suffix(".img.part")
        partial.write_bytes(output)
        partial.replace(target)
        results[name] = info[2][0].hex()
        print(f"{entry['id']} {name}: {size} bytes, SHA256 verified", flush=True)
    (dest / "verified.json").write_text(json.dumps(
        {"source": entry, "metadata_signature_verified": True, "partitions": results},
        indent=2) + "\n")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("chain", type=Path)
    parser.add_argument("--key", action="append", type=Path, required=True,
                        help="Trusted public key extracted from stock otacerts.zip")
    args = parser.parse_args()
    chain = json.loads(args.chain.read_text())
    previous = None
    previous_build = None
    for entry in chain:
        require(str(entry["id"]).isdigit(), "Stage ID must be numeric")
        require(entry["pre_build"] == previous_build, "OTA build chain is discontinuous")
        require(entry["post_build"].startswith("OnePlus/CPH2655/OP5D55L1:"),
                "Unexpected target model")
        dest = ROOT / "out/reconstructed-stock" / str(entry["id"])
        require(subprocess.run(["git", "check-ignore", "-q", str(dest / "probe")],
                               cwd=ROOT).returncode == 0, "Output must be Git-ignored")
        dest.mkdir(parents=True, exist_ok=True)
        reconstruct(entry, dest, previous, args.key)
        previous, previous_build = dest, entry["post_build"]
    print(f"Verified stock images: {previous}")
    print("No phone access or flashing performed. Device readback comparison remains separate.")


if __name__ == "__main__":
    main()
