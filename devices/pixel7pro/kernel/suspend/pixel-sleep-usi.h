/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * USI (serial engine) state across SYS_SLEEP, as stock's exynos-usi and
 * i2c/spi drivers restore it on resume.
 *
 * PERIC0 and PERIC1 lose their USI configuration in SYS_SLEEP: the sysreg
 * SW_CONF (protocol select), USI_CON/USI_OPTION and the controller registers
 * come back at reset. This port's drivers adopt or set up their buses once
 * (kernel/battery USI13, kernel/i2c USI8/15 and the camera USI1-4,
 * kernel/spi USI7, the touch driver's SPI0 on USI0), so after the first deep
 * sleep every transfer timed out. Each USI configured at arm (SW_CONF not 0)
 * is saved and, after the CMU restore, put back: SW_CONF, USI_CON (reset
 * released), USI_OPTION, then the protocol's configuration registers.
 *
 * Addresses: the drivers above (bases from the stock DT; SW_CONF at sysreg
 * 0x1000 + 4 * index); register offsets from mainline i2c-exynos5.c and
 * spi-s3c64xx.c as those drivers use them. Status, FIFO and data registers
 * are never touched; SPI 0x04 (CLK_CFG, unused with the clock from the CMU)
 * neither.
 */
#ifndef PIXEL_SLEEP_USI_H
#define PIXEL_SLEEP_USI_H

#define USI_CON_OFF		0xc4
#define USI_OPTION_OFF		0xc8
#define SW_CONF_SPI		0x2
#define SW_CONF_I2C		0x4

struct psu_usi {
	const char *name;
	u32 base, sw_conf;
};

static const struct psu_usi psu_usis[] = {
	{ "USI0", 0x10d10000, 0x10c21000 },	/* SPI0: touch */
	{ "USI13", 0x10d60000, 0x10c21014 },	/* battery, charger, gauge */
	{ "USI15", 0x10da0000, 0x10c21018 },	/* flash, wireless charger, EEPROM */
	{ "USI1", 0x10900000, 0x10821000 },	/* camera buses, when configured */
	{ "USI2", 0x10910000, 0x10821004 },
	{ "USI3", 0x10920000, 0x10821008 },
	{ "USI4", 0x10930000, 0x1082100c },
	{ "USI7", 0x10960000, 0x10821018 },	/* SPI7: speaker amplifiers */
	{ "USI8", 0x10970000, 0x1082101c },	/* haptics, camera PMIC, NFC */
};

/* HSI2C: CTL, FIFO_CTL, TRAILING_CTL, INT_ENABLE, CONF, AUTO_CONF, TIMEOUT,
 * TIMING_HS1-3, TIMING_FS1-3, TIMING_SLA, ADDR.
 */
static const u16 psu_i2c_regs[] = {
	0x00, 0x04, 0x08, 0x20, 0x40, 0x44, 0x48, 0x54, 0x58, 0x5c,
	0x60, 0x64, 0x68, 0x6c, 0x70,
};
/* SPI: CH_CFG, MODE_CFG, CS_REG, INT_EN, PACKET_CNT, SWAP_CFG, FB_CLK. */
static const u16 psu_spi_regs[] = { 0x00, 0x08, 0x0c, 0x10, 0x20, 0x28, 0x2c };

#define PSU_MAX_REGS	ARRAY_SIZE(psu_i2c_regs)

static struct {
	void __iomem *usi[ARRAY_SIZE(psu_usis)];
	void __iomem *sysreg0, *sysreg1;	/* 0x10821000, 0x10c21000 */
	bool saved[ARRAY_SIZE(psu_usis)];
	u32 sw_conf[ARRAY_SIZE(psu_usis)], con[ARRAY_SIZE(psu_usis)];
	u32 option[ARRAY_SIZE(psu_usis)];
	u32 regs[ARRAY_SIZE(psu_usis)][PSU_MAX_REGS];
	unsigned int saved_n, lost, written;
} psu;

static void __iomem *psu_sw_conf(unsigned int i)
{
	u32 pa = psu_usis[i].sw_conf;

	return ((pa & ~0xfff) == 0x10821000 ? psu.sysreg0 : psu.sysreg1) + (pa & 0xfff);
}

static int psu_map(void)
{
	unsigned int i;

	psu.sysreg0 = ioremap(0x10821000, 0x1000);
	psu.sysreg1 = ioremap(0x10c21000, 0x1000);
	if (!psu.sysreg0 || !psu.sysreg1)
		return -ENOMEM;
	for (i = 0; i < ARRAY_SIZE(psu_usis); i++) {
		psu.usi[i] = ioremap(psu_usis[i].base, 0x1000);
		if (!psu.usi[i])
			return -ENOMEM;
	}
	return 0;
}

static void psu_unmap(void)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(psu_usis); i++)
		if (psu.usi[i])
			iounmap(psu.usi[i]);
	if (psu.sysreg0)
		iounmap(psu.sysreg0);
	if (psu.sysreg1)
		iounmap(psu.sysreg1);
}

static const u16 *psu_regs(u32 sw_conf, unsigned int *n)
{
	if (sw_conf == SW_CONF_I2C) {
		*n = ARRAY_SIZE(psu_i2c_regs);
		return psu_i2c_regs;
	}
	if (sw_conf == SW_CONF_SPI) {
		*n = ARRAY_SIZE(psu_spi_regs);
		return psu_spi_regs;
	}
	*n = 0;
	return NULL;
}

/* PERIC0/1 are TOP blocks, powered while any CPU runs. */
static void psu_save(void)
{
	unsigned int i, r, n;
	const u16 *regs;

	psu.saved_n = psu.lost = psu.written = 0;
	for (i = 0; i < ARRAY_SIZE(psu_usis); i++) {
		psu.sw_conf[i] = readl(psu_sw_conf(i));
		regs = psu_regs(psu.sw_conf[i], &n);
		psu.saved[i] = regs != NULL;
		if (!regs)
			continue;
		psu.con[i] = readl(psu.usi[i] + USI_CON_OFF);
		psu.option[i] = readl(psu.usi[i] + USI_OPTION_OFF);
		for (r = 0; r < n; r++)
			psu.regs[i][r] = readl(psu.usi[i] + regs[r]);
		psu.saved_n++;
	}
}

/* After the CMU restore: the USI clocks must run before any access. A USI
 * whose SW_CONF still reads as saved kept its state (an early wakeup).
 */
static void psu_restore(bool write)
{
	unsigned int i, r, n;
	const u16 *regs;

	for (i = 0; i < ARRAY_SIZE(psu_usis); i++) {
		if (!psu.saved[i] || readl(psu_sw_conf(i)) == psu.sw_conf[i])
			continue;
		psu.lost++;
		if (!write)
			continue;
		regs = psu_regs(psu.sw_conf[i], &n);
		writel(psu.sw_conf[i], psu_sw_conf(i));
		writel(psu.con[i], psu.usi[i] + USI_CON_OFF);
		writel(psu.option[i], psu.usi[i] + USI_OPTION_OFF);
		for (r = 0; r < n; r++)
			writel(psu.regs[i][r], psu.usi[i] + regs[r]);
		psu.written++;
	}
}

#endif
