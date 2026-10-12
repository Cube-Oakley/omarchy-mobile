#!/usr/bin/env python3
"""Prepare a RAM probe tree using a local CPH2655 stock DT capture.

Requires fdtget, fdtput and dtc. Output must remain in ignored out/ because the
memory map is derived from a particular handset and firmware build.
"""
import argparse
import json
from pathlib import Path
import shutil
import subprocess


def get(blob, node, prop=None):
    args = ["fdtget", "-t", "x", str(blob), node, prop] if prop else ["fdtget", "-l", str(blob), node]
    return subprocess.check_output(args, text=True).split()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiled-board", type=Path, required=True)
    parser.add_argument("--stock-dtb", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--gpu", action="store_true", help="Retain and verify the stock GPU ZAP reservation")
    parser.add_argument("--adsp", action="store_true", help="Retain and verify this firmware's ADSP and ADSP DT reservations")
    parser.add_argument("--fastrpc", action="store_true", help="Keep the stock-matched 12 MiB dynamic ADSP RPC DMA pool")
    args = parser.parse_args()
    if args.fastrpc and not args.adsp:
        parser.error("FastRPC requires the verified ADSP profile")
    stock, out = args.stock_dtb, args.output.resolve()
    root = Path(__file__).resolve().parents[3]
    subprocess.run(["git", "check-ignore", "-q", str(out)], cwd=root, check=True)
    if out.exists():
        parser.error("Output already exists; use a new probe artifact")
    model = subprocess.check_output(["fdtget", "-t", "s", str(stock), "/", "model"], text=True)
    if "Sun MTP,dodge" not in model:
        parser.error("Expected a captured dodge stock tree; independently verify CPH2655/project 23893")
    out.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy(args.compiled_board, out)

    def put(node, prop, values):
        subprocess.run(["fdtput", "-t", "x", str(out), node, prop] + values, check=True)

    retained = {0x81C60000: (0x20000, "aop-cmd-db@81c60000"),
                0x81D00000: (0x200000, "smem@81d00000")}
    if args.gpu:
        retained[0x9B09A000] = (0x2000, "gpu-micro-code@9b09a000")
    if args.adsp:
        retained[0x9ED80000] = (0x80000, "q6-adsp-dtb@9ed80000")
        retained[0x9EE00000] = (0x5080000, "adspslpi@9ee00000")
    for address, (size, node) in retained.items():
        cells = [int(x, 16) for x in get(out, "/reserved-memory/" + node, "reg")]
        if cells != [0, address, 0, size]:
            raise RuntimeError("Compiled consumer reservation changed: " + node)
    dynamic = []
    if args.fastrpc:
        dynamic = ["adsp-rpc-remote-heap"]
        props = {"size": ["0", "c00000"], "alignment": ["0", "400000"],
                 "alloc-ranges": ["0", "0", "0", "ffffffff"]}
        for prop, expected in props.items():
            for blob, node in [(out, "/reserved-memory/adsp-rpc-remote-heap"),
                               (stock, "/reserved-memory/adsp_heap_region")]:
                if get(blob, node, prop) != expected:
                    raise RuntimeError("ADSP dynamic DMA pool mismatch: " + prop)
        for blob, node in [(out, "/reserved-memory/adsp-rpc-remote-heap"),
                           (stock, "/reserved-memory/adsp_heap_region")]:
            if subprocess.check_output(["fdtget", "-t", "s", str(blob), node,
                                        "compatible"], text=True).strip() != "shared-dma-pool":
                raise RuntimeError("ADSP pool must be shared-dma-pool")
            get(blob, node, "reusable")
        node = "/soc@0/remoteproc@6800000/glink-edge/fastrpc"
        if get(out, node, "memory-region") != get(out, "/reserved-memory/adsp-rpc-remote-heap", "phandle"):
            raise RuntimeError("FastRPC DMA pool reference mismatch")
        if get(out, node) != ["compute-cb@5"]:
            raise RuntimeError("Sensor-only FastRPC requires only the stock SID 5 bank")
        smmu = get(out, "/soc@0/iommu@15000000", "phandle")
        if get(out, node + "/compute-cb@5", "iommus") != smmu + ["1005", "80"] + smmu + ["1045", "20"]:
            raise RuntimeError("Sensor FastRPC IOMMU streams differ from stock")
        if get(out, node, "qcom,vmids") != ["16", "25"]:
            raise RuntimeError("Sensor FastRPC memory owners differ from stock")
    # Consumers are enabled only with their checked stock reservations,
    # preserving the phandles used by their loaders.
    for node in get(out, "/reserved-memory"):
        if node not in dynamic + [value[1] for value in retained.values()]:
            subprocess.run(["fdtput", "-r", str(out), "/reserved-memory/" + node], check=True)
    regions = []
    found = set()
    for node in get(stock, "/reserved-memory"):
        result = subprocess.run(["fdtget", "-t", "x", str(stock), "/reserved-memory/" + node, "reg"],
                                capture_output=True, text=True)
        if result.returncode:
            continue  # Dynamic Android allocations do not carry fixed addresses.
        cells = result.stdout.split()
        if len(cells) % 4:
            raise RuntimeError("Unexpected reservation cell count")
        for offset in range(0, len(cells), 4):
            hi, lo, shi, slo = [int(x, 16) for x in cells[offset:offset + 4]]
            address, size = (hi << 32) | lo, (shi << 32) | slo
            if address in retained:
                expected = retained[address][0]
                if size != expected:
                    raise RuntimeError("Stock consumer reservation size changed")
                found.add(address)
                continue
            if size:
                regions.append((address, address + size))
    if found != retained.keys():
        raise RuntimeError("Stock consumer reservation missing")
    for begin, end in regions:
        for address, (size, _) in retained.items():
            if begin < address + size and end > address:
                raise RuntimeError("Stock reservation overlaps a retained consumer")
    if args.gpu:
        phandle = get(out, "/reserved-memory/gpu-micro-code@9b09a000", "phandle")
        if get(out, "/soc@0/gpu@3d00000/zap-shader", "memory-region") != phandle:
            raise RuntimeError("GPU ZAP reservation reference mismatch")
    if args.adsp:
        adsp_regions = (get(out, "/reserved-memory/adspslpi@9ee00000", "phandle") +
                        get(out, "/reserved-memory/q6-adsp-dtb@9ed80000", "phandle"))
        if get(out, "/soc@0/remoteproc@6800000", "memory-region") != adsp_regions:
            raise RuntimeError("ADSP reservation reference mismatch")
    merged = []
    for begin, end in sorted(regions):
        if merged and begin <= merged[-1][1]:
            merged[-1] = (merged[-1][0], max(end, merged[-1][1]))
        else:
            merged.append((begin, end))
    for begin, end in merged:
        node = f"/reserved-memory/stock@{begin:x}"
        subprocess.run(["fdtput", "-c", str(out), node], check=True)
        put(node, "reg", [f"{begin >> 32:x}", f"{begin & 0xffffffff:x}",
                          f"{(end-begin) >> 32:x}", f"{(end-begin) & 0xffffffff:x}"])
        subprocess.run(["fdtput", str(out), node, "no-map"], check=True)
    put("/memory@a0000000", "reg", get(stock, "/memory", "reg"))
    subprocess.run(["dtc", "-q", "-I", "dtb", "-O", "dtb", "-p", "65536", "-o", str(out), str(out)], check=True)
    out.with_suffix(".reservations.json").write_text(json.dumps(merged, indent=2) + "\n")


if __name__ == "__main__":
    main()
