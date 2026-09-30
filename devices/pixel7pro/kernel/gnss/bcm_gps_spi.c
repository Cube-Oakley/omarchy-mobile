// SPDX-License-Identifier: GPL-2.0
/* Copyright 2015 Broadcom Corporation
 *
 * The Broadcom GPS SPI driver
 *
 * From Google's gs201 kernel (drivers/misc/bbdpl/bcm_gps_spi.c), for the
 * BCM4776 GNSS receiver of the Pixel 7 Pro on a mainline kernel. The SSI
 * protocol, the MCU_REQ/MCU_RESP handshake, the rx/tx worker, the ring
 * buffers and /dev/ttyBCM are Google's. The platform plumbing is replaced
 * (changes marked "pixel-gnss:", listed in README.md):
 * - a "pixel-gnss" platform device instead of an spi_device;
 * - the lines, the host_req interrupt and the SPI bus come from
 *   pixel-gnss-hw.c (direct GS201 registers) instead of gpiolib, pinctrl
 *   and the vendor spi-s3c64xx host;
 * - bring-up files in debugfs.
 */

/* TODO: Use dev_*() calls instead */
#define pr_fmt(fmt) "GPSREGS: " fmt

#include <linux/module.h>
#include <linux/device.h>
#include <linux/init.h>
#include <linux/delay.h>
#include <linux/circ_buf.h>
#include <linux/debugfs.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/mm.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/poll.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/suspend.h>
#include <linux/kernel.h>
#include <linux/miscdevice.h>
#include <linux/timekeeping.h>
#include <linux/io.h>

#include "bbd.h"
#include "bcm_gps_spi.h"
#include "pixel-gnss-hw.h"

/* 0 - Half Duplex, 1 - Full Duplex */
#define SSI_MODE 1

/*  1 = 1B, 2 = 2B */
#define SSI_LEN 2

/*
 * TODO: Need to read bitrate from bus driver spi.c.
 * Just for startup info notification.
 */
#define BCM_BITRATE 12000

/*
 * pixel-gnss: hex-dump the first N bytes of every SPI transfer, both ways
 * (0: off). lhd's "SSI:DEBUG=1" on /dev/bbd_control still dumps them whole.
 */
static unsigned int debug;
module_param(debug, uint, 0644);
MODULE_PARM_DESC(debug, "Log the first N bytes of each SPI transfer (0: off)");

/*
 * pixel-gnss: Google's driver called gpiolib with the stock DT's GPIO numbers;
 * the lines are now indices into the GS201 pin table of pixel-gnss-hw.c.
 */
static int bcm_gpio_get(struct bcm_spi_priv *priv, int pin)
{
	return pixel_gnss_gpio_get(priv->hw, pin);
}

static void bcm_gpio_set(struct bcm_spi_priv *priv, int pin, int value)
{
	pixel_gnss_gpio_set(priv->hw, pin, value);
}

static void bcm_on_packet_received(
		void *_priv, unsigned char *data, unsigned int size);


static ssize_t nstandby_show(
		struct device *dev, struct device_attribute *attr, char *buf)
{
	int value = 0;
	struct bcm_spi_priv *priv = dev_get_drvdata(dev);
	value = bcm_gpio_get(priv, priv->nstandby);

	return sysfs_emit(buf, "%d\n", value);
}

static ssize_t nstandby_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	struct bcm_spi_priv *priv = dev_get_drvdata(dev);

#ifdef DEBUG_1HZ_STAT
	dev_dbg(dev, "nstandby, buf is %s\n", buf);
#endif

	if (buf[0] == '0')
		bcm_gpio_set(priv, priv->nstandby, 0);
	else
		bcm_gpio_set(priv, priv->nstandby, 1);

	return count;
}

static DEVICE_ATTR_RW(nstandby);

static ssize_t sspmcureq_show(
		struct device *dev, struct device_attribute *attr, char *buf)
{
	int value = 0;
	struct bcm_spi_priv *priv = dev_get_drvdata(dev);
	value = bcm_gpio_get(priv, priv->mcu_req);

	return sysfs_emit(buf, "%d\n", value);
}

static ssize_t sspmcureq_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	struct bcm_spi_priv *priv = dev_get_drvdata(dev);

	dev_dbg(dev, "sspmcureq, buf is %s\n", buf);

	if (buf[0] == '0')
		bcm_gpio_set(priv, priv->mcu_req, 0);
	else
		bcm_gpio_set(priv, priv->mcu_req, 1);

	return count;
}

static DEVICE_ATTR_RW(sspmcureq);

#ifdef CONFIG_TRANSFER_STAT
void bcm_ssi_clear_trans_stat(struct bcm_spi_priv *priv)
{
	memset(priv->trans_stat, 0, sizeof(priv->trans_stat));
}

void bcm_ssi_print_trans_stat(struct bcm_spi_priv *priv)
{
	struct bcm_spi_transfer_stat *trans = &priv->trans_stat[0];

	dev_info(priv->dev, "DBG SPI @ TX: <255B = %d, <1K = %d, <2K = %d, <4K = %d, <8K = %d, <16K = %d, <32K = %d, <64K = %d, total = %ld, min = %ld, max = %ld",
			trans->len_255, trans->len_1K, trans->len_2K,
			trans->len_4K, trans->len_8K, trans->len_16K,
			trans->len_32K, trans->len_64K, trans->len_total,
			trans->len_min, trans->len_max);

	trans = &priv->trans_stat[1];
	dev_info(priv->dev, "DBG SPI @ RX: <255B = %d, <1K = %d, <2K = %d, <4K = %d, <8K = %d, <16K = %d, <32K = %d, <64K = %d, total = %ld, min = %ld, max = %ld",
			trans->len_255, trans->len_1K, trans->len_2K,
			trans->len_4K, trans->len_8K, trans->len_16K,
			trans->len_32K, trans->len_64K, trans->len_total,
			trans->len_min, trans->len_max);

	dev_info(priv->dev, "DBG SPI @ PZC: retries = %d, delays = %d",
			priv->ssi_tx_pzc_retries, priv->ssi_tx_pzc_retry_delays);
}

static void bcm_ssi_calc_trans_stat(
		struct bcm_spi_transfer_stat *trans, unsigned short length)
{
	if (length <= 255)
		trans->len_255++;
	else if (length <= 1024)
		trans->len_1K++;
	else if (length <= (2 * 1024))
		trans->len_2K++;
	else if (length <= (4 * 1024))
		trans->len_4K++;
	else if (length <= (8 * 1024))
		trans->len_8K++;
	else if (length <= (16 * 1024))
		trans->len_16K++;
	else if (length <= (32 * 1024))
		trans->len_32K++;
	else
		trans->len_64K++;

	if (length > trans->len_max)
		trans->len_max = length;

	if (trans->len_min == 0 || length < trans->len_min)
		trans->len_min = length;

	trans->len_total += length;
}
#endif /* CONFIG_TRANSFER_STAT */

static const unsigned long m_ulRxBufferBlockSize[4] = {32, 256, 1024 * 2, 1024 * 16};

static unsigned long bcm_ssi_chk_pzc(struct bcm_spi_priv *priv,
		unsigned char stat_byte, bool bprint)
{
	unsigned long rx_buffer_blk_bytes =
		m_ulRxBufferBlockSize[(
				stat_byte & HSI_F_MOSI_CTRL_SZE_MASK) >>
				HSI_F_MOSI_CTRL_SZE_SHIFT];
	unsigned long rx_buffer_blk_counts =
		(unsigned long)((stat_byte & HSI_F_MOSI_CTRL_CNT_MASK) >>
			HSI_F_MOSI_CTRL_CNT_SHIFT);

	priv->rx_buffer_avail_bytes = rx_buffer_blk_bytes * rx_buffer_blk_counts;

	if (!bprint)
		return priv->rx_buffer_avail_bytes;

	if (stat_byte & HSI_F_MOSI_CTRL_PE_MASK) {
		dev_dbg(priv->dev, "DBG SPI @ PZC: rx stat 0x%02x%s avail %lu",
			stat_byte,
			stat_byte & HSI_F_MOSI_CTRL_PE_MASK ?  "(PE)" : "   ",
			priv->rx_buffer_avail_bytes);
	}

#ifdef CONFIG_REG_IO
	if (stat_byte & HSI_F_MOSI_CTRL_PE_MASK) {
		u8 regval;

		bcm_dreg_read(priv, "HSI_ERROR_STATUS(R) ",
				HSI_ERROR_STATUS, &regval, 1);

		if (regval & HSI_ERROR_STATUS_STRM_FIFO_OVFL)
			dev_err(priv->dev, "(rx_strm_fifo_ovfl)");

		regval = HSI_ERROR_STATUS_ALL_ERRORS;
		bcm_dreg_write(priv, "HSI_ERROR_STATUS(W) ",
				HSI_ERROR_STATUS, &regval, 1);
	}
#endif /* CONFIG_REG_IO */

	return priv->rx_buffer_avail_bytes;
}


/**********************************
 *
 *	File Operations
 *
 **********************************/
static int bcm_spi_open(struct inode *inode, struct file *filp)
{
	/*
	 * Initially, file->private_data points device itself
	 * and we can get our priv structs from it.
	 */
	struct bcm_spi_priv *priv = container_of(filp->private_data,
			struct bcm_spi_priv, misc);
	struct bcm_spi_strm_protocol *strm;
	unsigned long flags;
	unsigned char fc_mask, len_mask, duplex_mask;

#ifdef CONFIG_REG_IO
	u8 regval8[2];
	u32 regval32[16];
#endif

	if (priv->busy)
		return -EBUSY;

	priv->busy = true;

	/* Reset circ buffer */
	priv->read_buf.head = priv->read_buf.tail = 0;
	priv->write_buf.head = priv->write_buf.tail = 0;

	priv->packet_received = 0;

	/* Enable irq (pixel-gnss: clears the latched EINT first) */
	spin_lock_irqsave(&priv->irq_lock, flags);
	if (!atomic_xchg(&priv->irq_enabled, 1))
		pixel_gnss_irq_enable(priv->hw);

	spin_unlock_irqrestore(&priv->irq_lock, flags);

	priv->irq_wakeup_enabled = (enable_irq_wake(priv->irq) == 0);

	filp->private_data = priv;
#ifdef DEBUG_1HZ_STAT
	bbd_enable_stat(priv->bbd);
#endif

	strm = &priv->tx_strm;
	strm->pckt_len = SSI_LEN == 2 ? 2 : 1;
	len_mask = SSI_LEN == 2 ? SSI_PCKT_2B_LENGTH : SSI_PCKT_1B_LENGTH;
	duplex_mask = SSI_MODE != 0 ? SSI_MODE_FULL_DUPLEX : SSI_MODE_HALF_DUPLEX;

	fc_mask = SSI_FLOW_CONTROL_DISABLED;
	strm->fc_len = 0;
	if (SSI_MODE == 0) {
		/* SSI_MODE_HALF_DUPLEX; */
		strm->pckt_len = 0;
	}
	/* 1 for tx cmd byte */
	strm->ctrl_len = strm->pckt_len + strm->fc_len + 1;

	strm->frame_len = SSI_LEN == 2 ? MAX_SPI_FRAME_LEN : MAX_SPI_DREG_FRAME_LEN;
	strm->ctrl_byte = duplex_mask | SSI_MODE_STREAM |
		len_mask | SSI_WRITE_TRANS | fc_mask;

	/* TX SPI Streaming Protocol in details */
#ifdef DEBUG_1HZ_STAT
	dev_info(priv->dev, "tx ctrl %02X: total %d = len %d + fc %d + cmd 1",
			strm->ctrl_byte, strm->ctrl_len,
			strm->pckt_len, strm->fc_len);
#endif

	strm = &priv->rx_strm;
	strm->pckt_len = SSI_LEN == 2 ? 2 : 1;
	strm->fc_len = 0;
	/* 1 for rx stat byte */
	strm->ctrl_len = strm->pckt_len + strm->fc_len + 1;
	strm->frame_len = SSI_LEN == 2 ? MAX_SPI_FRAME_LEN : MAX_SPI_DREG_FRAME_LEN;
	strm->ctrl_byte = duplex_mask | SSI_MODE_STREAM | len_mask |
		(SSI_MODE == SSI_MODE_FULL_DUPLEX ?
		 SSI_WRITE_TRANS : SSI_READ_TRANS);

	/* RX SPI Streaming Protocol in details */
#ifdef DEBUG_1HZ_STAT
	dev_info(priv->dev, "rx ctrl %02X: total %d = len %d + fc %d + stat 1\n",
			strm->ctrl_byte, strm->ctrl_len,
			strm->pckt_len, strm->fc_len);

	dev_info(priv->dev, "SPI @ %d: %s Duplex Strm mode, %dB Len, w/o FC, Frame Len %u : tx ctrl %02X, rx ctrl %02X\n",
			BCM_BITRATE,
			SSI_MODE != 0 ? "Full" : "Half",
			SSI_LEN == 2 ? 2 : 1,
			strm->frame_len,
			priv->tx_strm.ctrl_byte,
			priv->rx_strm.ctrl_byte);
#endif

#ifdef CONFIG_REG_IO
	bcm_dreg_read(priv, "HSI_STATUS     ",
			HSI_STATUS, regval8, 1);
	bcm_dreg_read(priv, "HSI_ERROR_STATUS(R) ",
			HSI_ERROR_STATUS, regval8, 1);
	bcm_ireg_read(priv, "INTR_MASK/STAT ",
			HSI_INTR_MASK, regval32, 2);
	bcm_ireg_read(priv, "RNGDMA_RX      ",
			HSI_RNGDMA_RX_BASE_ADDR, regval32, 8);
	bcm_ireg_read(priv, "RNGDMA_TX      ",
			HSI_RNGDMA_TX_BASE_ADDR, regval32, 8);
	bcm_ireg_read(priv, "HSI_CTRL       ",
			HSI_CTRL, regval32, 4);
	bcm_ireg_read(priv, "ADL_ABR        ",
			HSI_ADL_ABR_CONTROL, regval32, 4);
	bcm_ireg_read(priv, "RSTN/STBY/EN   ",
			HSI_RESETN, regval32, 4);
	bcm_ireg_read(priv, "STRM/CMND      ",
			HSI_STRM_FIFO_STATUS, regval32, 2);
#endif

#ifdef CONFIG_TRANSFER_STAT
	bcm_ssi_print_trans_stat(priv);
	bcm_ssi_clear_trans_stat(priv);
#endif
	priv->ssi_tx_fail       = 0;
	priv->ssi_tx_pzc_retries = 0;
	priv->ssi_tx_pzc_retry_delays = 0;
	priv->ssi_pm_semaphore = 0;
	priv->rx_buffer_avail_bytes = HSI_PZC_MAX_RX_BUFFER;

	return 0;
}

static int bcm_spi_release(struct inode *inode, struct file *filp)
{
	struct bcm_spi_priv *priv = filp->private_data;
	unsigned long flags;

	priv->busy = false;

#ifdef CONFIG_TRANSFER_STAT
	bcm_ssi_print_trans_stat(priv);
#endif


#ifdef DEBUG_1HZ_STAT
	bbd_disable_stat(priv->bbd);
#endif
	/* Disable irq */
	spin_lock_irqsave(&priv->irq_lock, flags);
	if (atomic_xchg(&priv->irq_enabled, 0))
		disable_irq_nosync(priv->irq);

	spin_unlock_irqrestore(&priv->irq_lock, flags);

	if (priv->irq_wakeup_enabled)
		disable_irq_wake(priv->irq);

	return 0;
}

static ssize_t bcm_spi_read(
		struct file *filp, char __user *buf, size_t size, loff_t *ppos)
{
	struct bcm_spi_priv *priv = filp->private_data;
	struct circ_buf *circ = &priv->read_buf;
	size_t rd_size = 0;

	mutex_lock(&priv->rlock);

	/*
	 * Copy from circ buffer to user
	 * We may require 2 copies from [tail..end] and [end..head]
	 */
	do {
		size_t cnt_to_end = CIRC_CNT_TO_END(
				circ->head, circ->tail, BCM_SPI_READ_BUF_SIZE);
		size_t copied = min(cnt_to_end, size);

		if (copy_to_user(buf + rd_size,
				circ->buf + circ->tail, copied)){
			dev_err(priv->dev, "failed to copy to user.\n");
			mutex_unlock(&priv->rlock);
			return -EFAULT;
		}
		size -= copied;
		rd_size += copied;
		circ->tail = (circ->tail + copied) & (BCM_SPI_READ_BUF_SIZE-1);

	} while (size > 0 && CIRC_CNT(circ->head,
				circ->tail, BCM_SPI_READ_BUF_SIZE));
	mutex_unlock(&priv->rlock);

#ifdef DEBUG_1HZ_STAT
	bbd_update_stat(priv->bbd, STAT_RX_LHD, rd_size);
#endif

	return rd_size;
}

static ssize_t bcm_spi_write(
		struct file *filp, const char __user *buf,
		size_t size, loff_t *ppos)
{
	struct bcm_spi_priv *priv = filp->private_data;
	struct circ_buf *circ = &priv->write_buf;
	size_t wr_size = 0;

	mutex_lock(&priv->wlock);
	/*
	 * Copy from user into circ buffer
	 * We may require 2 copies from [tail..end] and [end..head]
	 */
	do {
		size_t space_to_end = CIRC_SPACE_TO_END(circ->head,
				circ->tail, BCM_SPI_WRITE_BUF_SIZE);
		size_t copied = min(space_to_end, size);

		if (copy_from_user(circ->buf + circ->head,
					buf + wr_size, copied)){
			dev_err(priv->dev, "failed to copy from user.\n");
			mutex_unlock(&priv->wlock);
			return -EFAULT;
		}
		size -= copied;
		wr_size += copied;
		circ->head = (circ->head + copied) &
			(BCM_SPI_WRITE_BUF_SIZE - 1);
	} while (size > 0 && CIRC_SPACE(circ->head, circ->tail,
				BCM_SPI_WRITE_BUF_SIZE));
	mutex_unlock(&priv->wlock);

	/*
	 * kick start rxtx thread
	 * we don't want to queue work in suspending and shutdown
	 */
	if (!atomic_read(&priv->suspending))
		queue_work(priv->serial_wq,
				(struct work_struct *)&priv->rxtx_work);

#ifdef DEBUG_1HZ_STAT
	bbd_update_stat(priv->bbd, STAT_TX_LHD, wr_size);
#endif
	return wr_size;
}

static __poll_t bcm_spi_poll(struct file *filp, poll_table *wait)
{
	struct bcm_spi_priv *priv = filp->private_data;
	struct circ_buf *rd_circ = &priv->read_buf;
	struct circ_buf *wr_circ = &priv->write_buf;
	__poll_t mask = 0;

	poll_wait(filp, &priv->poll_wait, wait);

	if (CIRC_CNT(rd_circ->head, rd_circ->tail, BCM_SPI_READ_BUF_SIZE))
		mask |= EPOLLIN;

	if (CIRC_SPACE(wr_circ->head, wr_circ->tail, BCM_SPI_WRITE_BUF_SIZE))
		mask |= EPOLLOUT;

	return mask;
}


static const struct file_operations bcm_spi_fops = {
	.owner = THIS_MODULE,
	.open = bcm_spi_open,
	.release = bcm_spi_release,
	.read = bcm_spi_read,
	.write = bcm_spi_write,
	.poll = bcm_spi_poll,
};



/* Misc. functions */

static unsigned long bcm_clock_get_ms(void)
{
	struct timespec64 t;
	unsigned long now;
	static unsigned long init_time;

	ktime_get_real_ts64(&t);
	now = t.tv_nsec / 1000000 + t.tv_sec * 1000;
	if (init_time == 0)
		init_time = now;

	return now - init_time;
}

static void wait1secDelay(unsigned int count)
{
	if (count <= 100)
		usleep_range(1000, 2000);
	else
		usleep_range(20000, 30000);
}

#ifdef CONFIG_MCU_WAKEUP
/**
 * bcm4773_hello - wakeup chip by toggling mcu_req
 * while monitoring mcu_resp to check if awake
 */
static bool bcm477x_hello(struct bcm_spi_priv *priv)
{
	int count = 0;
#define MAX_RESP_CHECK_COUNT 100 /* 100 msec */

	unsigned long start_time, delta;

	start_time = bcm_clock_get_ms();
	bcm_gpio_set(priv, priv->mcu_req, 1);
	while (!bcm_gpio_get(priv, priv->mcu_resp)) {
		if (count++ > MAX_RESP_CHECK_COUNT) {
			bcm_gpio_set(priv, priv->mcu_req, 0);
#ifdef DEBUG_1HZ_STAT
			dev_err(priv->dev, " MCU_REQ_RESP timeout. MCU_RESP(gpio%d) not responding to MCU_REQ(gpio%d)\n",
					priv->mcu_resp, priv->mcu_req);
#endif
			return false;
		}

		wait1secDelay(count);

		/*if awake, done */
		if (bcm_gpio_get(priv, priv->mcu_resp))
			break;

		if (count % 20 == 0) {
			bcm_gpio_set(priv, priv->mcu_req, 0);
			usleep_range(1000, 2000);
			bcm_gpio_set(priv, priv->mcu_req, 1);
			usleep_range(1000, 2000);
		}
	}

	delta = bcm_clock_get_ms() - start_time;

	if (count > 100)
		dev_err(priv->dev, "hello consumed %lu = clock_get_ms() - start_time; msec",
				delta);

	return true;
}
#endif

/**
 * bcm4773_bye - set mcu_req low to let chip go to sleep
 */
static void bcm477x_bye(struct bcm_spi_priv *priv)
{
	bcm_gpio_set(priv, priv->mcu_req, 0);
}

static void pk_log(struct bcm_spi_priv *priv, char *dir,
		unsigned char *data, int len)
{
	const char ic = 'D';

	/* pixel-gnss: the debug module parameter dumps the first bytes */
	if (likely(!priv->bbd->ssi_dbg)) {
		int n = min_t(int, len, min_t(unsigned int, debug, 64));

		if (n > 0)
			dev_info(priv->dev, "%s %5d: %*ph%s\n", dir, len, n,
				 data, len > n ? " ..." : "");
		return;
	}

	/*
	 * TODO: There is print issue. Printing 7 digits instead of 6
	 * when clock is over 1000000. "% 1000000" added
	 * E.g.
	 * #999829D w 0x68,  1: A2
	 * #999829D r 0x68, 34: 8D 00 01 52 5F B0 01 B0 00 8E 00 01 53 8B
	 * B0 01 B0 00 8F 00 01 54 61 B0 01 B0 00 90 00 01 55 B5
	 *          r B0 01
	 * #1000001D w 0x68, 1: A1
	 * #1000001D r 0x68, 1: 00
	 */
	dev_info(priv->dev, "#%06ld%c %2s,\t  %5d: ",
			bcm_clock_get_ms() % 1000000, ic, dir, len);

	print_hex_dump(KERN_INFO, dir[0] == 'r' ? "r " : "w ",
			DUMP_PREFIX_NONE, 32, 1, data, len, false);
}

/* SSI tx/rx functions */

static unsigned short bcm_ssi_get_len(
		unsigned char ctrl_byte, unsigned char *data)
{
	if (ctrl_byte & SSI_PCKT_2B_LENGTH)
		return ((unsigned short)data[0] +
			((unsigned short)data[1] << 8));

	return (unsigned short)data[0];
}

static void bcm_ssi_set_len(
	unsigned char ctrl_byte, unsigned char *data, unsigned short len)
{
	if (ctrl_byte & SSI_PCKT_2B_LENGTH) {
		data[0] = (unsigned char)(len & 0xff);
		data[1] = (unsigned char)((len >> 8)  & 0xff);
	} else {
		data[0] = (unsigned char)len;
	}
}

static void bcm_ssi_clr_len(unsigned char ctrl_byte, unsigned char *data)
{
	bcm_ssi_set_len(ctrl_byte, data, 0);
}


/*
 * pixel-gnss: one chip-select-framed transfer on the polled USI5 host
 * (pixel_gnss_spi_xfer()) in place of spi_sync() on the vendor spi-s3c64xx.
 * bits_per_word is ignored: every transfer uses 8-bit words. Google's host
 * used 32-bit words for transfers of MIN_DMA_SIZE and up, with the DT's
 * swap-mode = <1> swapping the bytes of each word so that the wire order is
 * the memory order, the same byte stream 8-bit words give (see
 * pixel-gnss-hw.c). The callers' 4-byte length alignment is kept as is, so
 * the chip sees the frame lengths it saw on stock.
 */
int bcm_spi_sync(struct bcm_spi_priv *priv, void *tx_buf,
		void *rx_buf, int len, int bits_per_word)
{
	int ret;

	/* Sync */
	pk_log(priv, "w", (unsigned char *)tx_buf, len);
	ret = pixel_gnss_spi_xfer(priv->hw, tx_buf, rx_buf, len);
	pk_log(priv, "r", (unsigned char *)rx_buf, len);

	if (ret)
		dev_err(priv->dev, "spi_sync error for cmd:0x%x, return=%d\n",
			((struct bcm_ssi_tx_frame *)tx_buf)->cmd, ret);

	return ret;
}


static int bcm_ssi_tx(struct bcm_spi_priv *priv, int length)
{
	struct bcm_ssi_tx_frame *tx = priv->tx_buf;
	struct bcm_ssi_rx_frame *rx = priv->rx_buf;
	struct bcm_spi_strm_protocol *strm = &priv->tx_strm;
	int bits_per_word = (length + strm->ctrl_len >= MIN_DMA_SIZE) ?
				CONFIG_SPI_DMA_BITS_PER_WORD : 8;
	int ret;
	unsigned short m_write;
	unsigned short bytes_to_write = (unsigned short)length;
	/* pixel-gnss: __maybe_unused, read only with CONFIG_TRANSFER_STAT */
	unsigned short bytes_written __maybe_unused = 0;
	unsigned short n_read = 0; /* for Full Duplex only */
	unsigned short frame_data_size = strm->frame_len - strm->ctrl_len;

	m_write = bytes_to_write;

	tx->cmd = strm->ctrl_byte; /* SSI_WRITE_HD etc. */

	bytes_to_write = max(m_write, n_read);

	if (strm->pckt_len != 0)
		bcm_ssi_set_len(strm->ctrl_byte, tx->data, m_write);

	/* ctrl_len is for tx len + fc */
	ret = bcm_spi_sync(priv, tx, rx, bytes_to_write +
			strm->ctrl_len, bits_per_word);

	if (ret) {
		priv->ssi_tx_fail++;
		return ret;
		/* TODO: failure, operation should gets 0 to continue */
	}

	if (strm->pckt_len != 0) {
		unsigned short m_write =
			bcm_ssi_get_len(strm->ctrl_byte, tx->data);
		/* Just for understanding SPI Streaming Protocol */
		if (m_write > frame_data_size) {
			/* The h/w malfunctioned ? */
			dev_err(priv->dev, "@ TX m_write %d is h/w overflowed of frame %d...Fail\n",
				m_write, frame_data_size);
		}
	}

	if (strm->ctrl_byte & SSI_MODE_FULL_DUPLEX) {
		unsigned char *data_p = rx->data + strm->pckt_len;

		n_read = bcm_ssi_get_len(strm->ctrl_byte, rx->data);
		if (n_read > frame_data_size) {
			dev_err(priv->dev, "@ FD n_read %d is h/w overflowed of frame %d...Fail\n",
				n_read, frame_data_size);
			n_read = frame_data_size;
		}

		if (m_write < n_read) {
			/* Call BBD */
			bcm_on_packet_received(priv, data_p, m_write);
			/* 1/2 bytes for len */
			n_read -= m_write;
			bytes_to_write -= m_write;
			data_p += (m_write + strm->fc_len);
		} else {
			bytes_to_write = n_read;
			/* No data available next time */
			n_read = 0;
		}

		/* Call BBD */
		if (bytes_to_write != 0) {
			bcm_on_packet_received(
				priv, data_p, bytes_to_write);
		}
	}

	bytes_written += bytes_to_write;

#ifdef CONFIG_TRANSFER_STAT
	bcm_ssi_calc_trans_stat(&priv->trans_stat[0], bytes_written);
#endif

	bcm_ssi_chk_pzc(priv, rx->status, priv->bbd->ssi_dbg_pzc);

	return ret;
}

static int bcm_ssi_rx(struct bcm_spi_priv *priv, size_t *length)
{
	struct bcm_ssi_tx_frame *tx = priv->tx_buf;
	struct bcm_ssi_rx_frame *rx = priv->rx_buf;
	struct circ_buf *rd_circ = &priv->read_buf;
	struct bcm_spi_strm_protocol *strm = &priv->rx_strm;
	unsigned short ctrl_len = strm->pckt_len + 1;  /* +1 for rx status */
	unsigned short payload_len;
	int bits_per_word = 8;
	size_t sz_to_recv = 0;

#ifdef CONFIG_REG_IO
	if (likely(priv->bbd->ssi_dbg_rng) &&
			(priv->packet_received > CONFIG_PACKET_RECEIVED)) {
		u32 regval32[8];

		bcm_ireg_read(priv, "RNGDMA_TX      ",
				HSI_RNGDMA_TX_SW_ADDR_OFFSET, regval32, 3);
	}
#endif

	/* TODO:: Check 1B or 2B mode */

	bcm_ssi_clr_len(strm->ctrl_byte, tx->data);
	/* tx and rx ctrl_byte(s) are same */
	tx->cmd = strm->ctrl_byte;
	/* SSI_READ_HD etc. */
	rx->status = 0;

	if (bcm_spi_sync(priv, tx, rx, ctrl_len, 8))
		return -1;

	bcm_ssi_chk_pzc(priv, rx->status, priv->bbd->ssi_dbg_pzc);

	payload_len = bcm_ssi_get_len(strm->ctrl_byte, rx->data);

	if (payload_len == 0) {
		/*
		 * TODO:  payload_len = MIN_SPI_FRAME_LEN;
		 * Needn't to use MAX_SPI_FRAME_LEN because don't
		 * know how many bytes is ready to really read
		 */
		dev_err(priv->dev, "@ RX length is still read to 0. Set %d\n", payload_len);
		return -1;
	}

	*length = min((unsigned short)(strm->frame_len - ctrl_len), payload_len);

	/* SWGNSSGLL-24487 : slowing down read speed if buffer is half full */
	if (CIRC_CNT(rd_circ->head, rd_circ->tail, BCM_SPI_READ_BUF_SIZE) >
			BCM_SPI_READ_BUF_SIZE / 2) {
		msleep(DELAY_FOR_SYSTEM_OVERLOADED_MS);
		if (*length >= READ_SIZE_FOR_SYSTEM_OVERLOADED)
			*length = READ_SIZE_FOR_SYSTEM_OVERLOADED;
	}

	sz_to_recv = *length + ctrl_len;
	if (sz_to_recv >= MIN_DMA_SIZE) {
		bits_per_word = CONFIG_SPI_DMA_BITS_PER_WORD;
		if (sz_to_recv & (CONFIG_SPI_DMA_BYTES_PER_WORD - 1))
			*length = (sz_to_recv & ~(CONFIG_SPI_DMA_BYTES_PER_WORD - 1))
					- ctrl_len;
	}
	memset(tx->data, 0, *length + ctrl_len - 1); /* -1 for status byte */

	if (bcm_spi_sync(priv, tx, rx, *length+ctrl_len, bits_per_word))
		return -1;

	payload_len = bcm_ssi_get_len(strm->ctrl_byte, rx->data);
	if (payload_len < *length)
		*length = payload_len;

	return 0;
}

static void bcm_check_overrun(struct bcm_spi_priv *priv, size_t avail)
{
	const long THRESHOLD_MS = 100;
	unsigned long curr_tick = bcm_clock_get_ms();

	if (!avail)
		return;

	if (curr_tick - priv->last_tick < THRESHOLD_MS) {
		priv->skip_count++;
		return;
	}

	if (priv->skip_count)
		dev_err(priv->dev, "%ld messages are skipped!\n", priv->skip_count);

	dev_err(priv->dev, "input overrun error by %zu bytes.\n", avail);
	priv->skip_count = 0;
	priv->last_tick = curr_tick;

}

static void bcm_on_packet_received(void *_priv, unsigned char *data,
		unsigned int size)
{
	struct bcm_spi_priv *priv = (struct bcm_spi_priv *)_priv;
	struct circ_buf *rd_circ = &priv->read_buf;
	size_t written = 0, avail = size;

#ifdef DEBUG_1HZ_STAT
	bbd_update_stat(priv->bbd, STAT_RX_SSI, size);
#endif
#ifdef CONFIG_TRANSFER_STAT
	bcm_ssi_calc_trans_stat(&priv->trans_stat[1], size);
#endif

	/* Copy into circ buffer */
	mutex_lock(&priv->rlock);
	do {
		size_t space_to_end = CIRC_SPACE_TO_END(
			rd_circ->head, rd_circ->tail, BCM_SPI_READ_BUF_SIZE);
		size_t copied = min(space_to_end, avail);

		memcpy(rd_circ->buf + rd_circ->head,
				data + written, copied);
		avail -= copied;
		written += copied;
		rd_circ->head = (rd_circ->head + copied) &
			(BCM_SPI_READ_BUF_SIZE - 1);
	} while (avail > 0 && CIRC_SPACE(rd_circ->head,
				rd_circ->tail, BCM_SPI_READ_BUF_SIZE));

	priv->packet_received += size;
	mutex_unlock(&priv->rlock);
	wake_up(&priv->poll_wait);
	bcm_check_overrun(priv, avail);
}

#ifdef DEBUG_1HZ_STAT
static void bcm477x_debug_info(struct bcm_spi_priv *priv)
{
	int pin_ttyBCM, pin_MCU_REQ, pin_MCU_RESP;
	int irq_enabled;

	if (!priv)
		return;

	pin_ttyBCM = bcm_gpio_get(priv, priv->host_req);
	pin_MCU_REQ = bcm_gpio_get(priv, priv->mcu_req);
	pin_MCU_RESP = bcm_gpio_get(priv, priv->mcu_resp);

	irq_enabled = atomic_read(&priv->irq_enabled);

	dev_info(priv->dev, "pin_ttyBCM:%d, pin_MCU_REQ:%d, pin_MCU_RESP:%d\n",
		pin_ttyBCM, pin_MCU_REQ, pin_MCU_RESP);
	dev_info(priv->dev, "irq_enabled:%d\n", irq_enabled);
}
#endif

static void bcm_rxtx_work_func(struct work_struct *work)
{
	struct bcm_spi_priv *priv = container_of(work,
			struct bcm_spi_priv, rxtx_work);
	struct circ_buf *rd_circ = &priv->read_buf;
	struct circ_buf *wr_circ = &priv->write_buf;
	struct bcm_spi_strm_protocol *strm = &priv->tx_strm;
	unsigned short rx_pckt_len = priv->rx_strm.pckt_len;
	int wait_for_pzc = 0;
	unsigned long flags;

#ifdef DEBUG_1HZ_STAT
	u64 ts_rx_start = 0;
	u64 ts_rx_end = 0;
	struct timespec64 ts;
	struct bbd_device *bbd = priv->bbd;
#endif

#ifdef CONFIG_MCU_WAKEUP
	if (!bcm477x_hello(priv)) {
#ifdef DEBUG_1HZ_STAT
		dev_err(priv->dev, "hello timeout!!\n");
		bcm477x_debug_info(priv);
#endif
		return;
	}
#endif

	do {
		int    ret = 0;
		size_t avail = 0;
		size_t written = 0;
		size_t sz_to_send = 0;

		/* Read first */
		if (priv->host_req < 0) {	/* pixel-gnss: was gpio_is_valid() */
			dev_err(priv->dev, "gpio host_req is invalid, return\n");
			return;
		}
		ret = bcm_gpio_get(priv, priv->host_req);

		if (ret || wait_for_pzc) {
			wait_for_pzc = 0;
#ifdef DEBUG_1HZ_STAT
			if (bbd->stat1hz.ts_irq) {
				ts = ktime_to_timespec64(ktime_get_boottime());
				ts_rx_start = ts.tv_sec * 1000000000ULL
					+ ts.tv_nsec;
			}
#endif

			/* Receive SSI frame */
			if (bcm_ssi_rx(priv, &avail))
				break;

#ifdef DEBUG_1HZ_STAT
			if (ts_rx_start && !bcm_gpio_get(priv, priv->host_req)) {
				ts = ktime_to_timespec64(ktime_get_boottime());
				ts_rx_end = ts.tv_sec * 1000000000ULL
					+ ts.tv_nsec;
			}
#endif
			/* Call BBD */
			bcm_on_packet_received(priv,
				priv->rx_buf->data + rx_pckt_len, avail);
		}

		/* Next, write */
		avail = CIRC_CNT(wr_circ->head, wr_circ->tail,
				BCM_SPI_WRITE_BUF_SIZE);

		if (!avail)
			continue;

		mutex_lock(&priv->wlock);
		/*
		 * For big packet, we should align xfer size to
		 * DMA word size and burst size.
		 * That is, SSI payload + one byte command should be
		 * multiple of (DMA word size * burst size)
		 */

		if (avail > (strm->frame_len - strm->ctrl_len))
			avail = strm->frame_len - strm->ctrl_len;

		ret = 0;

		/*
		 * SWGNSSGLL-15521 : Sometimes LHD does not write data
		 * because the following code blocks sending data to MCU
		 * Code is commented out because
		 * 'rx_buffer_avail_bytes'(PZC) is calculated in
		 * bcm_ssi_tx() inside loop in work queue
		 * (bcm_rxtx_work_func) below this code.
		 * It means 'rx_buffer_avail_bytes' doesn't reflect
		 * real available bytes in RX DMA RING buffer when
		 * work queue will be restarted
		 * because MCU is working independently from host.
		 * The 'rx_buffer_avail_bytes' can be tested inside
		 * bcm_ssi_tx but it may not guarantee correct
		 * condition also.
		 * SWGNSSGLL-16290 : FC detecting was broken when buffer
		 * is overflow Using PZC for a software workaround to
		 * not get into fifo overflow condition.
		 */
		if (avail > priv->rx_buffer_avail_bytes) {
			priv->rx_buffer_avail_bytes ? priv->ssi_tx_pzc_retries++ :
				priv->ssi_tx_pzc_retry_delays++;
			dev_dbg(priv->dev, "%d PZC %s, wr CIRC_CNT %lu, RNGDMA_RX %lu\n",
				priv->rx_buffer_avail_bytes ?
					priv->ssi_tx_pzc_retries : priv->ssi_tx_pzc_retry_delays,
				priv->rx_buffer_avail_bytes ?  "writes":"delays",
				avail,
				priv->rx_buffer_avail_bytes);
			if (priv->rx_buffer_avail_bytes == 0) {
				/*
				 *RNGDMA_RX is full ?
				 * If it's YES keep reading
				 */
				u32 regval32[8];

				bcm_ireg_read(priv, "RNGDMA_RX      ",
					HSI_RNGDMA_RX_SW_ADDR_OFFSET,
					regval32, 3);
			}
			avail = priv->rx_buffer_avail_bytes;
			usleep_range(1000, 2000);
			/*
			 * TODO: increase delay for waiting for
			 * draining RNGDMA_RX on MCU side ?
			 */
			wait_for_pzc = 1;
			/*
			 * This case is for when RNGDMA_RX is
			 * full and HOST_REQ is low
			 */
		}

		/* we should align xfer size to DMA word size. */
		sz_to_send = avail + strm->ctrl_len;
		if (sz_to_send >= MIN_DMA_SIZE &&
			sz_to_send & (CONFIG_SPI_DMA_BYTES_PER_WORD - 1))
			avail = (sz_to_send & ~(CONFIG_SPI_DMA_BYTES_PER_WORD - 1))
				- strm->ctrl_len;

		/* Copy from wr_circ the data */
		while (avail > 0) {
			size_t cnt_to_end = CIRC_CNT_TO_END(
				wr_circ->head, wr_circ->tail,
				BCM_SPI_WRITE_BUF_SIZE);
			size_t copied = min(cnt_to_end, avail);

			memcpy(priv->tx_buf->data + strm->pckt_len +
			written, wr_circ->buf + wr_circ->tail, copied);
			avail -= copied;
			written += copied;
			wr_circ->tail = (wr_circ->tail + copied) &
				(BCM_SPI_WRITE_BUF_SIZE - 1);
		}

		/* Transmit SSI frame */
		if (written)
			ret = bcm_ssi_tx(priv, written);

		mutex_unlock(&priv->wlock);

		if (ret)
			break;

		/*
		 * SWGNSSAND-2159  While looping,
		 * wake up lhd only if rx ring is more than 12.5% full
		 */
		if (CIRC_CNT(rd_circ->head, rd_circ->tail, BCM_SPI_READ_BUF_SIZE) >
				BCM_SPI_READ_BUF_SIZE / 8) {
			wake_up(&priv->poll_wait);
		}
#ifdef DEBUG_1HZ_STAT
		bbd_update_stat(bbd, STAT_TX_SSI, written);
#endif

	} while (!atomic_read(&priv->suspending) &&
		(bcm_gpio_get(priv, priv->host_req) ||
		CIRC_CNT(wr_circ->head, wr_circ->tail, BCM_SPI_WRITE_BUF_SIZE)));

	bcm477x_bye(priv);

	wake_up(&priv->poll_wait);

	/* Enable irq */
	spin_lock_irqsave(&priv->irq_lock, flags);

	/* we dont' want to enable irq when going to suspending */
	if (!atomic_read(&priv->suspending))
		if (!atomic_xchg(&priv->irq_enabled, 1))
			pixel_gnss_irq_enable(priv->hw);	/* pixel-gnss */

	spin_unlock_irqrestore(&priv->irq_lock, flags);

#ifdef DEBUG_1HZ_STAT
	if (bbd->stat1hz.ts_irq && ts_rx_start && ts_rx_end) {
		u64 lat = ts_rx_start - bbd->stat1hz.ts_irq;
		u64 dur = ts_rx_end - ts_rx_start;

		bbd->stat1hz.min_rx_lat = (lat < bbd->stat1hz.min_rx_lat) ?
			lat : bbd->stat1hz.min_rx_lat;
		bbd->stat1hz.max_rx_lat = (lat > bbd->stat1hz.max_rx_lat) ?
			lat : bbd->stat1hz.max_rx_lat;
		bbd->stat1hz.min_rx_dur = (dur < bbd->stat1hz.min_rx_dur) ?
			dur : bbd->stat1hz.min_rx_dur;
		bbd->stat1hz.max_rx_dur = (dur > bbd->stat1hz.max_rx_dur) ?
			dur : bbd->stat1hz.max_rx_dur;
		bbd->stat1hz.ts_irq = 0;
	}
#endif
}


/* IRQ Handler */
static irqreturn_t bcm_irq_handler(int irq, void *pdata)
{
	struct bcm_spi_priv *priv = (struct bcm_spi_priv *) pdata;

	/*
	 * pixel-gnss: this is host_req's own GIC line, which follows the
	 * latched wake-up EINT pend bit; clear it, or the line stays up. A
	 * level still high latches it again and the core masks the line once
	 * it is disabled below.
	 */
	pixel_gnss_irq_ack(priv->hw);

	if (!bcm_gpio_get(priv, priv->host_req))
		return IRQ_HANDLED;
#ifdef DEBUG_1HZ_STAT
	{
		struct bbd_device *bbd = priv->bbd;

		struct timespec64 ts;

		ts = ktime_to_timespec64(ktime_get_boottime());
		bbd->stat1hz.ts_irq = ts.tv_sec * 1000000000ULL + ts.tv_nsec;
	}
#endif
	/* Disable irq */
	spin_lock(&priv->irq_lock);
	if (atomic_xchg(&priv->irq_enabled, 0))
		disable_irq_nosync(priv->irq);

	spin_unlock(&priv->irq_lock);

	/* we don't want to queue work in suspending and shutdown */
	if (!atomic_read(&priv->suspending))
		queue_work(priv->serial_wq,
			(struct work_struct *)&priv->rxtx_work);

	return IRQ_HANDLED;
}

/*
 * pixel-gnss: gps_initialize_pinctrl() and gps_pinctrl_select() are gone.
 * There is no GS201 pin controller driver; pixel_gnss_hw_init() sets the
 * stock DT's "default" state (the only one it defines: Google's driver looked
 * up gps_active and gps_suspend, which the Pixel 7 Pro DT does not have).
 */

/*
 * pixel-gnss bring-up files (not in Google's driver), in
 * /sys/kernel/debug/pixel-gnss/:
 * - pins: the lines, the host_req EINT, USI5 and the supplies;
 * - ssi: with /dev/ttyBCM closed, the MCU_REQ/MCU_RESP handshake
 *   (bcm477x_hello()), one SSI status read (the first transfer of
 *   bcm_ssi_rx()), the HSI_STATUS and HSI_ERROR_STATUS direct registers
 *   (bcm_dreg_read()), then MCU_REQ low again (bcm477x_bye()).
 */
static int bcm_dbg_pins_show(struct seq_file *s, void *unused)
{
	struct bcm_spi_priv *priv = s->private;

	pixel_gnss_hw_show(priv->hw, s);
	seq_printf(s, "ttyBCM %s, irq %d %s, %lu bytes received\n",
		   priv->busy ? "open" : "closed", priv->irq,
		   atomic_read(&priv->irq_enabled) ? "enabled" : "disabled",
		   priv->packet_received);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(bcm_dbg_pins);

static DEFINE_MUTEX(bcm_dbg_lock);

static int bcm_dbg_ssi_show(struct seq_file *s, void *unused)
{
	struct bcm_spi_priv *priv = s->private;
	struct bcm_ssi_tx_frame *tx = priv->tx_buf;
	struct bcm_ssi_rx_frame *rx = priv->rx_buf;
	u8 hsi_status = 0, hsi_error = 0;
	int ret;

	mutex_lock(&bcm_dbg_lock);
	if (priv->busy || !priv->serial_wq) {
		seq_puts(s, "/dev/ttyBCM is open or the driver is shut down; stop lhd first\n");
		goto out;
	}
	priv->busy = true;	/* /dev/ttyBCM does not open meanwhile */
	flush_workqueue(priv->serial_wq);

	seq_printf(s, "nstandby %d, mcu_resp %d, host_req %d\n",
		   bcm_gpio_get(priv, priv->nstandby),
		   bcm_gpio_get(priv, priv->mcu_resp),
		   bcm_gpio_get(priv, priv->host_req));
	if (!bcm477x_hello(priv)) {
		seq_puts(s, "hello: MCU_RESP stayed low\n");
		goto bye;
	}
	seq_puts(s, "hello: MCU_RESP high\n");

	/* The rx control byte bcm_spi_open() builds: 0x70, then 2 length bytes. */
	memset(tx, 0, 3);
	tx->cmd = SSI_MODE_FULL_DUPLEX | SSI_MODE_STREAM | SSI_PCKT_2B_LENGTH |
		SSI_READ_TRANS;
	memset(rx, 0, 3);
	ret = bcm_spi_sync(priv, tx, rx, 3, 8);
	seq_printf(s, "ssi status read: tx %*ph, rx %*ph (%d)\n", 3, tx, 3, rx, ret);
	if (!ret)
		seq_printf(s, "  status %#04x%s: %lu bytes free on the chip, %u bytes to read\n",
			   rx->status,
			   rx->status & HSI_F_MOSI_CTRL_PE_MASK ? " (PE)" : "",
			   bcm_ssi_chk_pzc(priv, rx->status, false),
			   (unsigned int)bcm_ssi_get_len(tx->cmd, rx->data));

	ret = bcm_dreg_read(priv, "HSI_STATUS", HSI_STATUS, &hsi_status, 1);
	if (ret >= 0)
		ret = bcm_dreg_read(priv, "HSI_ERROR_STATUS", HSI_ERROR_STATUS,
				    &hsi_error, 1);
	if (ret < 0)
		seq_puts(s, "direct registers: transfer failed\n");
	else
		seq_printf(s, "HSI_STATUS %#04x, HSI_ERROR_STATUS %#04x\n",
			   hsi_status, hsi_error);
bye:
	bcm477x_bye(priv);
	priv->busy = false;
out:
	mutex_unlock(&bcm_dbg_lock);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(bcm_dbg_ssi);

static void bcm_debugfs_init(struct bcm_spi_priv *priv)
{
	priv->debugfs = debugfs_create_dir("pixel-gnss", NULL);
	debugfs_create_file("pins", 0400, priv->debugfs, priv, &bcm_dbg_pins_fops);
	debugfs_create_file("ssi", 0400, priv->debugfs, priv, &bcm_dbg_ssi_fops);
}


/* SPI driver operations (pixel-gnss: platform driver operations) */

static int bcm_spi_suspend(struct device *dev)
{
	struct bcm_spi_priv *priv = dev_get_drvdata(dev);
	unsigned long flags;

	atomic_set(&priv->suspending, 1);

	/* Disable irq */
	spin_lock_irqsave(&priv->irq_lock, flags);
	if (atomic_xchg(&priv->irq_enabled, 0))
		disable_irq_nosync(priv->irq);

	spin_unlock_irqrestore(&priv->irq_lock, flags);

	if (priv->serial_wq)
		flush_workqueue(priv->serial_wq);

	priv->ssi_pm_semaphore++;
	return 0;
}

static int bcm_spi_resume(struct device *dev)
{
	struct bcm_spi_priv *priv = dev_get_drvdata(dev);
	unsigned long flags;

	atomic_set(&priv->suspending, 0);

	/*
	 * Enable irq
	 * pixel-gnss: only while /dev/ttyBCM is open. Google's enabled it
	 * after any resume, which also armed it with no lhd running.
	 */
	spin_lock_irqsave(&priv->irq_lock, flags);
	if (priv->busy && !atomic_xchg(&priv->irq_enabled, 1))
		pixel_gnss_irq_enable(priv->hw);

	spin_unlock_irqrestore(&priv->irq_lock, flags);

	priv->ssi_pm_semaphore--;
	return 0;
}

static void bcm_spi_shutdown(struct platform_device *pdev)
{
	struct bcm_spi_priv *priv = platform_get_drvdata(pdev);
	unsigned long flags;

#ifdef CONFIG_TRANSFER_STAT
	bcm_ssi_print_trans_stat(priv);
#endif

	atomic_set(&priv->suspending, 1);

	/* Disable irq */
	spin_lock_irqsave(&priv->irq_lock, flags);
	if (atomic_xchg(&priv->irq_enabled, 0))
		disable_irq_nosync(priv->irq);

	spin_unlock_irqrestore(&priv->irq_lock, flags);
	synchronize_irq(priv->irq);	/* pixel-gnss */

	flush_workqueue(priv->serial_wq);
	destroy_workqueue(priv->serial_wq);
	priv->serial_wq = NULL;

	/* pixel-gnss: the chip to standby and its supplies off, as at boot */
	pixel_gnss_hw_shutdown(priv->hw);
}

static void bcm_spi_free_priv(void *priv)
{
	kvfree(priv);
}

static int bcm_spi_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct pixel_gnss_hw *hw;
	struct bcm_spi_priv *priv;
	/* pixel-gnss: the stock DT node sets neither ssp- property */
	bool skip_validity_check = false;
	bool legacy_patch = false;
	int ret;

	/*
	 * pixel-gnss: the lines, the host_req interrupt and the bus come from
	 * pixel-gnss-hw.c instead of of_get_named_gpio(), gpio_request(),
	 * gpio_to_irq() and pinctrl. It checks them against the stock DT
	 * node, powers the chip (in standby) and sets the pins; devm undoes
	 * all of it after bcm_spi_remove().
	 */
	hw = pixel_gnss_hw_init(dev);
	if (IS_ERR(hw))
		return PTR_ERR(hw);

	/* Alloc everything (pixel-gnss: 128 KiB of rings, so vmalloc-backed) */
	priv = kvzalloc(sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;
	ret = devm_add_action_or_reset(dev, bcm_spi_free_priv, priv);
	if (ret)
		return ret;

	priv->skip_validity_check = skip_validity_check;
	priv->dev = dev;
	priv->hw = hw;
	priv->irq = pixel_gnss_irq(hw);
	priv->tx_buf = devm_kmalloc(dev,
			sizeof(struct bcm_ssi_tx_frame), GFP_KERNEL);
	priv->rx_buf = devm_kmalloc(dev,
			sizeof(struct bcm_ssi_rx_frame), GFP_KERNEL);
	if (!priv->tx_buf || !priv->rx_buf)
		return -ENOMEM;

	priv->serial_wq = alloc_workqueue("bcm477x_wq",
			WQ_HIGHPRI|WQ_UNBOUND|WQ_MEM_RECLAIM, 1);
	if (!priv->serial_wq) {
		dev_err(dev, "Failed to allocate workqueue\n");
		return -ENOMEM;
	}

	/* Set driver data */
	platform_set_drvdata(pdev, priv);

	/* Init - miscdev stuff */
	init_waitqueue_head(&priv->poll_wait);
	priv->read_buf.buf = priv->_read_buf;
	priv->write_buf.buf = priv->_write_buf;
	mutex_init(&priv->rlock);
	mutex_init(&priv->wlock);
	priv->busy = false;

	/* Init - work */
	INIT_WORK((struct work_struct *)&priv->rxtx_work, bcm_rxtx_work_func);

	/* Init - irq stuff */
	spin_lock_init(&priv->irq_lock);
	atomic_set(&priv->irq_enabled, 0);
	atomic_set(&priv->suspending, 0);

	/* Init - gpios */
	priv->host_req = GNSS_PIN_HOST_REQ;
	priv->mcu_req  = GNSS_PIN_MCU_REQ;
	priv->mcu_resp = GNSS_PIN_MCU_RESP;
	priv->nstandby = GNSS_PIN_NSTANDBY;

	/* Init BBD & SSP */
	priv->bbd = bbd_init(dev, legacy_patch);
	if (priv->bbd == NULL) {
		ret = -ENODEV;
		goto free_wq;
	}

	if (device_create_file(dev, &dev_attr_nstandby))
		dev_err(dev, "Unable to create sysfs 4775 nstandby entry");

	if (device_create_file(dev, &dev_attr_sspmcureq))
		dev_err(dev, "Unable to create sysfs 4775 sspmcureq entry");

	/* Request IRQ */
	ret = devm_request_irq(dev, priv->irq, bcm_irq_handler,
			IRQF_TRIGGER_HIGH | IRQF_NO_AUTOEN, "ttyBCM", priv);

	if (ret) {
		dev_err(dev, "Failed to register BCM477x SPI TTY IRQ %d.\n",
				priv->irq);
		goto free_bbd;
	}

	/*
	 * Register misc device
	 * pixel-gnss: last, once what its open() uses is ready; Google's
	 * registered it before the buffers, the locks and the interrupt.
	 */
	priv->misc.minor = MISC_DYNAMIC_MINOR;
	priv->misc.name = "ttyBCM";
	priv->misc.fops = &bcm_spi_fops;
	priv->misc.parent = dev;

	ret = misc_register(&priv->misc);
	if (ret) {
		dev_err(dev, "Failed to register bcm_gps_spi's misc dev. err=%d\n", ret);
		goto free_bbd;
	}

	bcm_debugfs_init(priv);

	dev_info(dev, "Probe OK. ssp-host-req=%d, irq=%d, priv=0x%pK\n",
			priv->host_req, priv->irq, priv);

	return 0;

free_bbd:
	device_remove_file(dev, &dev_attr_nstandby);
	device_remove_file(dev, &dev_attr_sspmcureq);
	bbd_exit(priv->bbd);
free_wq:
	destroy_workqueue(priv->serial_wq);
	return ret;
}


static void bcm_spi_remove(struct platform_device *pdev)
{
	struct bcm_spi_priv *priv = platform_get_drvdata(pdev);
	unsigned long flags;

	/* pixel-gnss: Google's remove left /dev/ttyBCM registered */
	debugfs_remove_recursive(priv->debugfs);
	misc_deregister(&priv->misc);

	atomic_set(&priv->suspending, 1);

	/* Disable irq */
	spin_lock_irqsave(&priv->irq_lock, flags);
	if (atomic_xchg(&priv->irq_enabled, 0))
		disable_irq_nosync(priv->irq);

	spin_unlock_irqrestore(&priv->irq_lock, flags);
	/* pixel-gnss: a handler still running could queue work below */
	synchronize_irq(priv->irq);

	/* Flush work (pixel-gnss: unless shutdown destroyed it) */
	if (priv->serial_wq) {
		flush_workqueue(priv->serial_wq);
		destroy_workqueue(priv->serial_wq);
	}

	/* Free everything */
	bbd_exit(priv->bbd);

	device_remove_file(priv->dev, &dev_attr_nstandby);
	device_remove_file(priv->dev, &dev_attr_sspmcureq);
	/* pixel-gnss: devm then frees the interrupt and restores the hardware */
}

static DEFINE_SIMPLE_DEV_PM_OPS(bcm_spi_pm_ops, bcm_spi_suspend, bcm_spi_resume);

/*
 * pixel-gnss: a platform device of its own, "pixel-gnss": its sysfs
 * directory holds nstandby and sspmcureq, and it parents /dev/ttyBCM and the
 * /dev/bbd_* devices. Nothing binds the stock spi@10940000 node.
 */
static struct platform_driver bcm_spi_driver = {
	.probe = bcm_spi_probe,
	.remove = bcm_spi_remove,
	.shutdown = bcm_spi_shutdown,
	.driver = {
		.name = "pixel-gnss",
		.pm = pm_sleep_ptr(&bcm_spi_pm_ops),
		.suppress_bind_attrs = true,
	},
};

static struct platform_device *bcm_pdev;

/* Module init/exit */
static int __init bcm_spi_init(void)
{
	int ret;

	if (!of_machine_is_compatible("google,GS201"))
		return -ENODEV;
	ret = platform_driver_register(&bcm_spi_driver);
	if (ret)
		return ret;
	bcm_pdev = platform_device_register_simple("pixel-gnss",
			PLATFORM_DEVID_NONE, NULL, 0);
	if (IS_ERR(bcm_pdev)) {
		platform_driver_unregister(&bcm_spi_driver);
		return PTR_ERR(bcm_pdev);
	}
	/* Registering probes it synchronously; the log has the reason. */
	if (!bcm_pdev->dev.driver) {
		platform_device_unregister(bcm_pdev);
		platform_driver_unregister(&bcm_spi_driver);
		return -ENODEV;
	}
	return 0;
}

static void __exit bcm_spi_exit(void)
{
	platform_device_unregister(bcm_pdev);
	platform_driver_unregister(&bcm_spi_driver);
}

module_init(bcm_spi_init);
module_exit(bcm_spi_exit);
MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Broadcom");
MODULE_DESCRIPTION("BCM SPI/SSI Driver, on the GS201 Pixel 7 Pro");
