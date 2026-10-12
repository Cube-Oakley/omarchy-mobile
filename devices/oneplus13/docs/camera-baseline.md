# Stock camera baseline

The owner wants the stock camera's readable distant text, including at 120x,
to survive the move to Omarchy Mobile. Evaluate actual recovered detail, focus,
stability, exposure and capture latency, not merely whether the zoom UI says 120x.

[OnePlus specifications](https://www.oneplus.com/us/13/specs) describe a 50 MP
LYT-808 main camera, 50 MP LYT-600 3x optical telephoto, 50 MP ultrawide and
32 MP front camera. The advertised 120x is digital zoom. Matching stock output
requires evaluating its processing as well as bringing up sensors and lenses.

## Preserved references

On October 11, 2026, the six initial JPEGs and seven additional samples were
saved with original metadata, without re-encoding. The owner took the requested
zoom sequence as well as possible. EXIF for the seven additional shots reports
35mm-equivalent focal lengths 23, 74, 148, 247, 730, 1450 and 2912 mm, consistent
with the nominal 1x, 3x, 6x, 10x, 30x, 60x and 120x sequence. EXIF digital zoom
is relative to the selected physical lens; it is not the camera UI's total zoom.
The earlier telephoto sample reports 599 mm equivalent. A close-up ground-truth
image of the distant text has not been confirmed.

The files are in `out/stock-*/photos/DCIM/Camera/`. Manifests include independent
phone SHA256 checksums. The final completed sequence contains 13 originals,
60,496,792 bytes. EXIF may contain private information; never publish originals
or raw diagnostic dumps by default. Keep these long-term reference files out of
build-output cleanup despite their location under `out/`.

The camera-assets snapshot includes `/odm/etc/camera`, `/vendor/lib64/camera`,
`/odm/lib64/camera`, and `/product/app/OplusCamera`. It contains tuning, model
files (including `text_enhance_yuv_v1.tflite`) and packaged EEPROM-named files.
Their presence proves neither Linux compatibility nor that they contain this
unit's live factory calibration. Persist/EEPROM and other vendor dependencies
still need separate collection through a privileged read path.

Stock `dumpsys media.camera` exposes five device IDs, four normal camera devices,
and a logical camera with physical IDs 2, 3 and 4. Its logical public zoom range
is 0.6–20 in this dump, despite the stock app offering 120x. That is a concrete
reason to investigate the vendor app/HAL pipeline rather than assume a standard
camera API reproduces stock high zoom.

## Evaluation order

1. Determine whether the stock ISP/HAL pipeline can be reused in a contained
   runtime, and what kernel interfaces, DSP/firmware, tuning and Android services
   it requires. Copying the APK into Waydroid alone does not establish support.
2. Independently establish native capture, autofocus, OIS and exposure for each
   lens. Use existing libcamera and shared camera-app work where applicable.
3. Compare the same scene at the saved zoom steps. Measure text transcription
   against known ground truth, edge detail, motion artifacts, noise, focus,
   latency, temperature and power. Repeat daylight/low-light and supported/handheld.
4. Evaluate multi-frame alignment and super-resolution where needed. Added
   sharpening or plausible generated glyphs must not count as correctly read text.

Stock-quality high zoom remains unproven. A camera driver and digital crop alone
are insufficient evidence that this target is met.
