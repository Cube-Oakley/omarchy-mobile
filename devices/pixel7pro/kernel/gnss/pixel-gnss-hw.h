/* SPDX-License-Identifier: GPL-2.0-only */
/* GS201 platform layer for Google's BCM4775x bbd driver: the GNSS supplies,
 * its four control lines, the host_req wake-up interrupt and a polled SPI
 * host on PERIC0 USI5. See pixel-gnss-hw.c.
 */
#ifndef __PIXEL_GNSS_HW_H__
#define __PIXEL_GNSS_HW_H__

#include <linux/types.h>

struct device;
struct pixel_gnss_hw;
struct seq_file;

/* Line indices, in place of the stock driver's integer GPIO numbers. */
enum pixel_gnss_pin {
	GNSS_PIN_HOST_REQ,	/* gpa6-4, chip to host, wake-up EINT */
	GNSS_PIN_MCU_REQ,	/* gpp4-3, host to chip */
	GNSS_PIN_MCU_RESP,	/* gph2-2, chip to host */
	GNSS_PIN_NSTANDBY,	/* gph2-3, host to chip, low = standby */
	GNSS_PIN_CS,		/* gpp10-3, SPI chip select, low = selected */
	GNSS_NR_PINS
};

struct pixel_gnss_hw *pixel_gnss_hw_init(struct device *dev);
void pixel_gnss_hw_shutdown(struct pixel_gnss_hw *hw);

int pixel_gnss_gpio_get(struct pixel_gnss_hw *hw, int pin);
void pixel_gnss_gpio_set(struct pixel_gnss_hw *hw, int pin, int value);

int pixel_gnss_irq(struct pixel_gnss_hw *hw);
void pixel_gnss_irq_ack(struct pixel_gnss_hw *hw);
void pixel_gnss_irq_enable(struct pixel_gnss_hw *hw);

int pixel_gnss_spi_xfer(struct pixel_gnss_hw *hw, const void *tx, void *rx, unsigned int len);

void pixel_gnss_hw_show(struct pixel_gnss_hw *hw, struct seq_file *s);

#endif /* __PIXEL_GNSS_HW_H__ */
