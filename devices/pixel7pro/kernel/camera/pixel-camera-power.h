/* SPDX-License-Identifier: GPL-2.0-only */
/* pixel-camera-power's interface for the camera driver (pixel-camera.c). */
#ifndef PIXEL_CAMERA_POWER_H
#define PIXEL_CAMERA_POWER_H

#include <linux/types.h>

/* Power one sensor up or down by its sysfs name ("uw", "front", "main",
 * "tele"), with the stock DT's sequence. Repeating a state is a no-op. */
int pixel_camera_power_set(const char *name, bool on);

#endif
