/* SPDX-License-Identifier: GPL-2.0-only */
/* pixel-pd-off's interface for drivers that need a domain it powers off. */
#ifndef PIXEL_PD_H
#define PIXEL_PD_H

#include <linux/types.h>

/* Power a domain on or off with the stock sequence, its secure context and
 * its save list. Only "csis" and "pdp" can come back on (-EOPNOTSUPP for the
 * others); -ENODATA if it went off without its save list being read. */
int pixel_pd_power(const char *name, bool on);

#endif
