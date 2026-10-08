// SPDX-License-Identifier: GPL-2.0
/*
 * First light for the Pixel 7 Pro's video codec (Samsung MFC v15.6 on GS201):
 * does the firmware start and answer SYS_INIT without the SysMMU? That
 * decides whether a GS201 variant of mainline s5p-mfc can drive it
 * (.work/research/reports/Pixel 7 Pro video codec feasibility.md).
 *
 * Test only, at load, one stage per load (`stage=`):
 *   1  reads: the PMU's MFC status, CMU_MFC's mux, divider and gates, the two
 *      SysMMUs' CTRL, the two S2MPUs' CTRL0, and the MFC's version and
 *      status registers. Only registers defined in Google's GPL sources
 *      (cmucal-sfr.c, mfc_regs.h, samsung-iommu.h; S2MPUs: CTRL0 only).
 *   2  firmware: copies mfc_fw.bin (from the user's vendor partition, never
 *      in git) into a 32-bit CMA buffer, resets the MFC, sets the RISC's
 *      non-secure base, RISC_ON and waits for FW_STATUS_RET; then SYS_INIT
 *      with a 30 KB context and waits for SYS_INIT_RET. The sequence is the
 *      stock driver's (mfc_core_run_init_hw), polled instead of interrupts.
 *      The RISC is stopped again before the buffers are freed at unload.
 *
 * Nothing is touched unless the PMU says the MFC domain is on: the boot
 * leaves it on only with /var/lib/omarchy-mobile/mfc-dev. Any MMIO into an
 * unpowered or unready block hangs GS201, so the modem's wdtkick must run.
 * Each step is logged and followed by a short sleep, so the last line before
 * a hang reaches `dmesg -w`.
 */
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/firmware.h>
#include <linux/io.h>
#include <linux/jiffies.h>
#include <linux/module.h>
#include <linux/platform_device.h>

#define PMU_MFC_STATUS		0x18062384	/* MFC_CONFIGURATION 0x2380 + 4 */
#define CMU_MFC			0x1c800000
#define MFC_CORE		0x1c8d0000
#define SYSMMU_MFC0		0x1c870000
#define SYSMMU_MFC1		0x1c8a0000
#define S2MPU_MFC0		0x1c890000
#define S2MPU_MFC1		0x1c8c0000

/* mfc_regs.h (v15) */
#define RISC_ON			0x0000
#define RISC2HOST_INT		0x003c
#define HOST2RISC_INT		0x0044
#define RISC_NONSECURE_BASE	0x0068
#define MFC_RESET		0x1070
#define HOST2RISC_CMD		0x1100
#define RISC2HOST_CMD		0x1104
#define MFC_BUS_STATUS		0x7018
#define MFC_OFF			0x7120
#define FW_VERSION		0xf000
#define CONTEXT_MEM_ADDR	0xf014
#define CONTEXT_MEM_SIZE	0xf018
#define MFC_VERSION		0xf028
#define ERROR_CODE		0xf074
#define FW_STATUS_INFO		0xf0a4
#define CLEAR_BEGIN		0xf000
#define CLEAR_COUNT		1024
#define H2R_SYS_INIT		1
#define R2H_SYS_INIT_RET	1
#define R2H_FW_STATUS_RET	12
#define R2H_ERR_RET		32

/*
 * One 2 MB block: the firmware's 1 MB region, then the device context. The
 * first try put the context in a CMA block of its own, below the firmware,
 * and SYS_INIT failed with ERR_RET (error 0x33): the RISC seems to address
 * memory upward from its base, as the stock driver keeps its firmware at the
 * lowest address.
 */
#define FW_SIZE			SZ_1M
#define FW_BLOCK		SZ_2M
#define CTX_SIZE		0x8000		/* PAGE_ALIGN(0x7800), stock dev_ctx */

static int stage;
module_param(stage, int, 0400);
MODULE_PARM_DESC(stage, "1: read status registers, 2: boot the firmware (SYS_INIT)");

static struct platform_device *pdev;
static void __iomem *mfc;
static void *fw_va, *ctx_va;
static dma_addr_t fw_pa, ctx_pa;	/* ctx_* point into the firmware block */
static bool risc_started;

static u32 peek(phys_addr_t pa, const char *what)
{
	void __iomem *va = ioremap(pa & PAGE_MASK, PAGE_SIZE);
	u32 val;

	if (!va)
		return ~0u;
	pr_info("pixel-mfc-light: reading %s (%pa)\n", what, &pa);
	msleep(20);
	val = readl(va + (pa & ~PAGE_MASK));
	iounmap(va);
	pr_info("pixel-mfc-light: %s = %#010x\n", what, val);
	msleep(20);
	return val;
}

static void stage_reads(void)
{
	static const struct { u32 off; const char *name; } cmu[] = {
		{ 0x0600, "PLL_CON0_MUX_CLKCMU_MFC_MFC_USER" },
		{ 0x1800, "CLK_CON_DIV_DIV_CLK_MFC_NOCP" },
		{ 0x2020, "GAT MFC_IPCLKPORT_ACLK" },
		{ 0x2018, "GAT LH_AXI_SI_D0_MFC" },
		{ 0x201c, "GAT LH_AXI_SI_D1_MFC" },
		{ 0x2054, "GAT SYSMMU_D0_MFC CLK_S1" },
		{ 0x205c, "GAT SYSMMU_D1_MFC CLK_S1" },
		{ 0x2044, "GAT SSMT_D0_MFC ACLK" },
		{ 0x204c, "GAT SSMT_D1_MFC ACLK" },
	};
	int i;

	for (i = 0; i < ARRAY_SIZE(cmu); i++)
		peek(CMU_MFC + cmu[i].off, cmu[i].name);
	peek(SYSMMU_MFC0, "SYSMMU_MFC0 CTRL");
	peek(SYSMMU_MFC1, "SYSMMU_MFC1 CTRL");
	peek(S2MPU_MFC0, "S2MPU_MFC0 CTRL0");
	peek(S2MPU_MFC1, "S2MPU_MFC1 CTRL0");
	peek(MFC_CORE + MFC_VERSION, "MFC_VERSION (expect 0x15060000)");
	peek(MFC_CORE + FW_VERSION, "FW_VERSION");
	peek(MFC_CORE + FW_STATUS_INFO, "FIRMWARE_STATUS_INFO");
	peek(MFC_CORE + RISC2HOST_CMD, "RISC2HOST_CMD");
	peek(MFC_CORE + RISC2HOST_INT, "RISC2HOST_INT");
	peek(MFC_CORE + MFC_BUS_STATUS, "MFC_BUS_STATUS");
}

/* Waits up to ms for the RISC to post a return; 0 if none came. */
static u32 wait_r2h(unsigned int ms)
{
	unsigned long end = jiffies + msecs_to_jiffies(ms);

	do {
		if (readl(mfc + RISC2HOST_INT))
			return readl(mfc + RISC2HOST_CMD);
		usleep_range(1000, 2000);
	} while (time_before(jiffies, end));
	return 0;
}

static void clear_r2h(void)
{
	writel(0, mfc + RISC2HOST_CMD);
	writel(0, mfc + RISC2HOST_INT);
}

static int stage_firmware(void)
{
	const struct firmware *fw;
	u32 ret, ver;
	int i, err;

	err = request_firmware(&fw, "mfc_fw.bin", &pdev->dev);
	if (err) {
		pr_err("pixel-mfc-light: no mfc_fw.bin (%d)\n", err);
		return err;
	}
	if (fw->size > FW_SIZE) {
		release_firmware(fw);
		return -EFBIG;
	}
	fw_va = dma_alloc_coherent(&pdev->dev, FW_BLOCK, &fw_pa, GFP_KERNEL);
	if (!fw_va) {
		release_firmware(fw);
		return -ENOMEM;
	}
	ctx_va = fw_va + FW_SIZE;
	ctx_pa = fw_pa + FW_SIZE;
	memcpy(fw_va, fw->data, fw->size);
	pr_info("pixel-mfc-light: firmware %zu bytes at %pad, context at %pad\n",
		fw->size, &fw_pa, &ctx_pa);
	release_firmware(fw);
	wmb();

	mfc = ioremap(MFC_CORE, SZ_64K);
	if (!mfc)
		return -ENOMEM;
	pr_info("pixel-mfc-light: MFC_VERSION %#x\n", readl(mfc + MFC_VERSION));

	/* mfc_core_reg_clear, mfc_core_reset_mfc */
	writel(0, mfc + RISC2HOST_CMD);
	writel(0, mfc + HOST2RISC_CMD);
	writel(0, mfc + FW_VERSION);
	for (i = 0; i < CLEAR_COUNT; i++)
		writel(0, mfc + CLEAR_BEGIN + i * 4);
	writel(0x1fff, mfc + MFC_RESET);
	writel(0, mfc + MFC_RESET);
	pr_info("pixel-mfc-light: reset done\n");
	msleep(20);

	/* mfc_core_set_risc_base_addr (security_ctrl), mfc_core_risc_on */
	writel(lower_32_bits(fw_pa), mfc + RISC_NONSECURE_BASE);
	clear_r2h();
	writel(0, mfc + MFC_OFF);
	writel(1, mfc + RISC_ON);
	risc_started = true;
	pr_info("pixel-mfc-light: RISC_ON, waiting for FW_STATUS_RET\n");
	ret = wait_r2h(1000);
	pr_info("pixel-mfc-light: R2H %u (FW_STATUS_RET is %d), FIRMWARE_STATUS_INFO %#x, BUS_STATUS %#x\n",
		ret, R2H_FW_STATUS_RET, readl(mfc + FW_STATUS_INFO), readl(mfc + MFC_BUS_STATUS));
	if (ret != R2H_FW_STATUS_RET)
		return 0;	/* stays loaded for reads; unload stops the RISC */
	clear_r2h();

	/* mfc_core_cmd_sys_init, mfc_core_cmd_host2risc */
	writel(lower_32_bits(ctx_pa), mfc + CONTEXT_MEM_ADDR);
	writel(0x7800, mfc + CONTEXT_MEM_SIZE);
	for (i = 0; i < 1000 && !(readl(mfc + FW_STATUS_INFO) & 1); i++)
		usleep_range(1000, 2000);
	writel(0, mfc + RISC2HOST_CMD);
	writel(H2R_SYS_INIT, mfc + HOST2RISC_CMD);
	writel(1, mfc + HOST2RISC_INT);
	pr_info("pixel-mfc-light: SYS_INIT sent\n");
	ret = wait_r2h(1000);
	ver = readl(mfc + FW_VERSION);
	pr_info("pixel-mfc-light: R2H %u (SYS_INIT_RET is %d, ERR_RET %d), ERROR_CODE %#x, FW_VERSION %#x ('%c' 20%02x-%02x-%02x)\n",
		ret, R2H_SYS_INIT_RET, R2H_ERR_RET, readl(mfc + ERROR_CODE), ver,
		(ver >> 24) & 0xff, (ver >> 16) & 0xff, (ver >> 8) & 0xff, ver & 0xff);
	clear_r2h();
	return 0;
}

static int __init light_init(void)
{
	u32 status;
	int err;

	if (stage < 1 || stage > 2)
		return -EINVAL;
	status = peek(PMU_MFC_STATUS, "PMU MFC_STATUS");
	if (!(status & 1)) {
		pr_err("pixel-mfc-light: the MFC domain is off; boot with mfc-dev\n");
		return -ENODEV;
	}
	pdev = platform_device_register_simple("pixel-mfc-light", -1, NULL, 0);
	if (IS_ERR(pdev))
		return PTR_ERR(pdev);
	err = dma_coerce_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(32));
	if (err)
		goto fail;
	if (stage == 1) {
		stage_reads();
		return 0;
	}
	err = stage_firmware();
	if (!err)
		return 0;
fail:
	if (mfc)
		iounmap(mfc);
	if (fw_va)
		dma_free_coherent(&pdev->dev, FW_BLOCK, fw_va, fw_pa);
	platform_device_unregister(pdev);
	return err;
}

static void __exit light_exit(void)
{
	if (mfc && risc_started) {
		int i;

		/* mfc_core_risc_off: no bus access pending, then RISC off, then reset */
		for (i = 0; i < 100 && readl(mfc + MFC_BUS_STATUS); i++)
			usleep_range(1000, 2000);
		writel(0, mfc + RISC_ON);
		writel(0x1fff, mfc + MFC_RESET);
		writel(0, mfc + MFC_RESET);
		pr_info("pixel-mfc-light: RISC stopped\n");
	}
	if (mfc)
		iounmap(mfc);
	if (fw_va)
		dma_free_coherent(&pdev->dev, FW_BLOCK, fw_va, fw_pa);
	if (pdev)
		platform_device_unregister(pdev);
}

module_init(light_init);
module_exit(light_exit);
MODULE_DESCRIPTION("GS201 MFC first-light test (test only)");
MODULE_LICENSE("GPL");
