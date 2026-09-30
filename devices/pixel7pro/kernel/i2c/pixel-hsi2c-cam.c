// SPDX-License-Identifier: GPL-2.0-only
/* The camera buses hsi2c_1-4, configured from scratch: pixel-hsi2c.c built
 * with its camera bus table and the driver name "pixel-hsi2c-cam". A module
 * of its own, so it loads and unloads beside the running pixel-hsi2c, whose
 * buses 15 and 8 serve the torch, the haptics and the camera PMIC.
 */
#define PIXEL_HSI2C_CAM
#include "pixel-hsi2c.c"
