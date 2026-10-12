# Experimental Adreno 830 support

Apply these five patches in numeric order to Linux
`5225b8eec4c9bb21aecff6295fab6346a3c3738e` (7.3-rc2).
They are Qualcomm-authored upstream submissions, retained with authorship.

- Clock/IOMMU prerequisite: [v6 submission](https://patchew.org/linux/20260714-gpucc._5Fdt._5Fv6-v6-1-16bf5289572d@oss.qualcomm.com/).
- A830 driver, bindings, SoC description and cooling: [GPU series](https://patchew.org/linux/20260809-pakala-gpu-v1-0-207ad1fac1c7@oss.qualcomm.com/).
- MTP/QRD board patches are intentionally excluded; our board supplies its own
  firmware path. The baseline already has the A8xx CP protection last-span fix.

`../gpu.config` enables render-only MSM DRM and SM8750 GPU clocks.
Set `msm.separate_gpu_kms=1` on the kernel command line so the GPU does not
wait for an MSM display component. Simpledrm continues to use the inherited
bootloader framebuffer. The board limits GPU
OPPs to 160/222/342 MHz for initial testing; this is not performance tuning.

Use `prepare-probe-dtb.py --gpu` to preserve the ZAP reservation and its
phandle. Both the stock and compiled address/size must equal
0x9b09a000/0x2000, and other stock reservations must not overlap it. This flag
does not make remote-processor reservations usable.

Firmware comes from this handset's captured vendor partition, kept only in
ignored output: qcom/gen80000_{sqe,aqe}.fw, qcom/gen80000_gmu.bin and
qcom/sm8750/gen80000_zap.mbn. Each probe records file hashes. The initial GPU
experiment remains a RAM boot, with all UFS disks read-only and a timed return
to Android. Probe 10 passed two Adreno GLES shader/readback tests and ran the shared
Hyprland desktop on the GPU. The owner confirmed substantially smoother
interaction. Native panel/120 Hz and sustained-power validation remain pending.
