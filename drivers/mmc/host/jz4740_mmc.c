/*
 *  Copyright (C) 2009-2010, Lars-Peter Clausen <lars@metafoo.de>
 *  Copyright (c) 2013, Imagination Technologies
 *  JZ4740 SD/MMC controller driver
 *
 *  This program is free software; you can redistribute  it and/or modify it
 *  under  the terms of  the GNU General  Public License as published by the
 *  Free Software Foundation;  either version 2 of the  License, or (at your
 *  option) any later version.
 *
 *  You should have received a copy of the  GNU General Public License along
 *  with this program; if not, write  to the Free Software Foundation, Inc.,
 *  675 Mass Ave, Cambridge, MA 02139, USA.
 *
 */

#include <linux/mmc/host.h>
#include <linux/mmc/slot-gpio.h>
#include <linux/err.h>
#include <linux/io.h>
#include <linux/irq.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/of_device.h>
#include <linux/of_dma.h>
#include <linux/platform_device.h>
#include <linux/delay.h>
#include <linux/scatterlist.h>
#include <linux/clk.h>

#include <linux/bitops.h>
#include <linux/gpio.h>
#include <asm/cacheflush.h>
#include <linux/dma-mapping.h>
#include <linux/dmaengine.h>

#define JZ_REG_MMC_STRPCL	0x00
#define JZ_REG_MMC_STATUS	0x04
#define JZ_REG_MMC_CLKRT	0x08
#define JZ_REG_MMC_CMDAT	0x0C
#define JZ_REG_MMC_RESTO	0x10
#define JZ_REG_MMC_RDTO		0x14
#define JZ_REG_MMC_BLKLEN	0x18
#define JZ_REG_MMC_NOB		0x1C
#define JZ_REG_MMC_SNOB		0x20
#define JZ_REG_MMC_IMASK	0x24
#define JZ_REG_MMC_IREG		0x28
#define JZ_REG_MMC_CMD		0x2C
#define JZ_REG_MMC_ARG		0x30
#define JZ_REG_MMC_RESP_FIFO	0x34
#define JZ_REG_MMC_RXFIFO	0x38
#define JZ_REG_MMC_TXFIFO	0x3C
#define JZ_REG_MMC_DMAC		0x44

#define JZ_MMC_STRPCL_EXIT_MULTIPLE BIT(7)
#define JZ_MMC_STRPCL_EXIT_TRANSFER BIT(6)
#define JZ_MMC_STRPCL_START_READWAIT BIT(5)
#define JZ_MMC_STRPCL_STOP_READWAIT BIT(4)
#define JZ_MMC_STRPCL_RESET BIT(3)
#define JZ_MMC_STRPCL_START_OP BIT(2)
#define JZ_MMC_STRPCL_CLOCK_CONTROL (BIT(1) | BIT(0))
#define JZ_MMC_STRPCL_CLOCK_STOP BIT(0)
#define JZ_MMC_STRPCL_CLOCK_START BIT(1)


#define JZ_MMC_STATUS_IS_RESETTING BIT(15)
#define JZ_MMC_STATUS_SDIO_INT_ACTIVE BIT(14)
#define JZ_MMC_STATUS_PRG_DONE BIT(13)
#define JZ_MMC_STATUS_DATA_TRAN_DONE BIT(12)
#define JZ_MMC_STATUS_END_CMD_RES BIT(11)
#define JZ_MMC_STATUS_DATA_FIFO_AFULL BIT(10)
#define JZ_MMC_STATUS_IS_READWAIT BIT(9)
#define JZ_MMC_STATUS_CLK_EN BIT(8)
#define JZ_MMC_STATUS_DATA_FIFO_FULL BIT(7)
#define JZ_MMC_STATUS_DATA_FIFO_EMPTY BIT(6)
#define JZ_MMC_STATUS_CRC_RES_ERR BIT(5)
#define JZ_MMC_STATUS_CRC_READ_ERROR BIT(4)
#define JZ_MMC_STATUS_TIMEOUT_WRITE BIT(3)
#define JZ_MMC_STATUS_CRC_WRITE_ERROR BIT(2)
#define JZ_MMC_STATUS_TIMEOUT_RES BIT(1)
#define JZ_MMC_STATUS_TIMEOUT_READ BIT(0)

#define JZ_MMC_STATUS_READ_ERROR_MASK (BIT(4) | BIT(0))
#define JZ_MMC_STATUS_WRITE_ERROR_MASK (BIT(3) | BIT(2))


#define JZ_MMC_CMDAT_IO_ABORT BIT(11)
#define JZ_MMC_CMDAT_BUS_WIDTH (BIT(9) | BIT(10))
#define JZ_MMC_CMDAT_BUS_WIDTH_1BIT 0
#define JZ_MMC_CMDAT_BUS_WIDTH_4BIT BIT(10)
#define JZ_MMC_CMDAT_BUS_WIDTH_8BIT (BIT(9) | BIT(10))
#define JZ_MMC_CMDAT_DMA_EN BIT(8)
#define JZ_MMC_CMDAT_INIT BIT(7)
#define JZ_MMC_CMDAT_BUSY BIT(6)
#define JZ_MMC_CMDAT_STREAM BIT(5)
#define JZ_MMC_CMDAT_WRITE BIT(4)
#define JZ_MMC_CMDAT_DATA_EN BIT(3)
#define JZ_MMC_CMDAT_RESPONSE_FORMAT (BIT(2) | BIT(1) | BIT(0))
#define JZ_MMC_CMDAT_RSP_R1 1
#define JZ_MMC_CMDAT_RSP_R2 2
#define JZ_MMC_CMDAT_RSP_R3 3

#define JZ_MMC_IRQ_SDIO BIT(7)
#define JZ_MMC_IRQ_TXFIFO_WR_REQ BIT(6)
#define JZ_MMC_IRQ_RXFIFO_RD_REQ BIT(5)
#define JZ_MMC_IRQ_END_CMD_RES BIT(2)
#define JZ_MMC_IRQ_PRG_DONE BIT(1)
#define JZ_MMC_IRQ_DATA_TRAN_DONE BIT(0)

#define JZ_MMC_DMAC_DMA_SEL	BIT(1)
#define JZ_MMC_DMAC_DMA_EN	BIT(0)


#define JZ_MMC_CLK_RATE 24000000

enum jz4740_mmc_version {
	JZ_MMC_JZ4740,
	JZ_MMC_JZ4750,
	JZ_MMC_JZ4780,
};

enum jz4740_mmc_state {
	JZ4740_MMC_STATE_READ_RESPONSE,
	JZ4740_MMC_STATE_TRANSFER_DATA,
	JZ4740_MMC_STATE_SEND_STOP,
	JZ4740_MMC_STATE_DONE,
};

struct jz4740_mmc_host {
	struct mmc_host *mmc;
	struct platform_device *pdev;
	struct clk *clk;

	enum jz4740_mmc_version version;

	int irq;
	int card_detect_irq;

	void __iomem *base;
	struct resource *mem_res;
	struct mmc_request *req;
	struct mmc_command *cmd;

	unsigned long waiting;

	uint32_t cmdat;

	uint32_t irq_mask;

	spinlock_t lock;

	struct timer_list timeout_timer;
	struct sg_mapping_iter miter;
	enum jz4740_mmc_state state;

	/* DMA support */
	struct dma_chan *dma_rx;
	struct dma_chan *dma_tx;
	unsigned int dma_tx_id, dma_rx_id;
	bool use_dma;
	int sg_len;

/* The DMA trigger level is 8 words, that is to say, the DMA read
 * trigger is when data words in MSC_RXFIFO is >= 8 and the DMA write
 * trigger is when data words in MSC_TXFIFO is < 8.
 */
#define JZ4740_MMC_FIFO_HALF_SIZE 8
};

/*----------------------------------------------------------------------------*/
/* DMA infrastructure */

static void jz4740_mmc_release_dma_channels(struct jz4740_mmc_host *host)
{
	if (!host->use_dma)
		return;

	dma_release_channel(host->dma_tx);
	if (host->dma_rx)
		dma_release_channel(host->dma_rx);
}

static int jz4740_mmc_acquire_dma_channels(struct jz4740_mmc_host *host)
{
	struct device *dev = &host->pdev->dev;

	host->dma_rx = NULL;

	of_property_read_u32(dev->of_node, "dma-tx-id", &host->dma_tx_id);
	of_property_read_u32(dev->of_node, "dma-rx-id", &host->dma_rx_id);

	host->dma_tx = of_dma_request_slave_channel(dev->of_node, "rx-tx");

	if (IS_ERR(host->dma_tx)) {
		if (PTR_ERR(host->dma_tx) == -EPROBE_DEFER)
			return -EPROBE_DEFER;

		host->dma_tx = of_dma_request_slave_channel(dev->of_node, "tx");
		if (IS_ERR(host->dma_tx))
			return PTR_ERR(host->dma_tx);

		host->dma_rx = of_dma_request_slave_channel(dev->of_node, "rx");
		if (IS_ERR(host->dma_rx)) {
			dma_release_channel(host->dma_tx);
			return PTR_ERR(host->dma_rx);
		}
	}

	return 0;
}

static inline int jz4740_mmc_get_dma_dir(struct mmc_data *data)
{
	return (data->flags & MMC_DATA_READ) ? DMA_FROM_DEVICE : DMA_TO_DEVICE;
}

static void jz4740_mmc_dma_unmap(struct jz4740_mmc_host *host,
				 struct mmc_data *data)
{
	struct dma_chan *chan;
	enum dma_data_direction dir = jz4740_mmc_get_dma_dir(data);

	if (dir == DMA_TO_DEVICE)
		chan = host->dma_tx;
	else
		chan = host->dma_rx ?: host->dma_tx;

	dma_unmap_sg(chan->device->dev, data->sg, data->sg_len, dir);
}

static int jz4740_mmc_start_dma_transfer(struct jz4740_mmc_host *host,
					 struct mmc_data *data)
{
	struct dma_chan *chan;
	struct dma_async_tx_descriptor *desc;
	struct dma_slave_config conf = {
		.src_addr_width = DMA_SLAVE_BUSWIDTH_4_BYTES,
		.dst_addr_width = DMA_SLAVE_BUSWIDTH_4_BYTES,
		.src_maxburst = JZ4740_MMC_FIFO_HALF_SIZE,
		.dst_maxburst = JZ4740_MMC_FIFO_HALF_SIZE,
	};
	enum dma_data_direction dir = jz4740_mmc_get_dma_dir(data);

	if (dir == DMA_TO_DEVICE) {
		conf.direction = DMA_MEM_TO_DEV;
		conf.dst_addr = host->mem_res->start + JZ_REG_MMC_TXFIFO;
		conf.slave_id = host->dma_tx_id;
		chan = host->dma_tx;
	} else {
		conf.direction = DMA_DEV_TO_MEM;
		conf.src_addr = host->mem_res->start + JZ_REG_MMC_RXFIFO;
		conf.slave_id = host->dma_rx_id;
		chan = host->dma_rx ?: host->dma_tx;
	}

	host->sg_len = dma_map_sg(chan->device->dev,
				  data->sg,
				  data->sg_len,
				  dir);

	if (host->sg_len == 0) {
		dev_err(mmc_dev(host->mmc),
			"Failed to map scatterlist for DMA operation\n");
		return -EINVAL;
	}

	dmaengine_slave_config(chan, &conf);
	desc = dmaengine_prep_slave_sg(chan,
				       data->sg,
				       host->sg_len,
				       conf.direction,
				       DMA_PREP_INTERRUPT | DMA_CTRL_ACK);
	if (!desc) {
		dev_err(mmc_dev(host->mmc),
			"Failed to allocate DMA %s descriptor",
			 conf.direction == DMA_MEM_TO_DEV ? "TX" : "RX");
		goto dma_unmap;
	}

	dmaengine_submit(desc);
	dma_async_issue_pending(chan);

	return 0;

dma_unmap:
	jz4740_mmc_dma_unmap(host, data);
	return -ENOMEM;
}

/*----------------------------------------------------------------------------*/

static uint32_t jz4740_mmc_read_irq_reg(struct jz4740_mmc_host *host)
{
	/* In the 4780 onwards, IREG is expanded to 32 bits. */
	if (host->version >= JZ_MMC_JZ4780)
		return readl(host->base + JZ_REG_MMC_IREG);
	else
		return readw(host->base + JZ_REG_MMC_IREG);
}

static void jz4740_mmc_write_irq_reg(struct jz4740_mmc_host *host, uint32_t val)
{
	if (host->version >= JZ_MMC_JZ4780)
		return writel(val, host->base + JZ_REG_MMC_IREG);
	else
		return writew(val, host->base + JZ_REG_MMC_IREG);
}

static void jz4740_mmc_set_irq_enabled(struct jz4740_mmc_host *host,
	unsigned int irq, bool enabled)
{
	unsigned long flags;

	spin_lock_irqsave(&host->lock, flags);

	if (enabled)
		host->irq_mask &= ~irq;
	else
		host->irq_mask |= irq;

	/* In the 4750 onwards, IMASK is expanded to 32 bits. */
	if (host->version >= JZ_MMC_JZ4750)
		writel(host->irq_mask, host->base + JZ_REG_MMC_IMASK);
	else
		writew(host->irq_mask, host->base + JZ_REG_MMC_IMASK);

	spin_unlock_irqrestore(&host->lock, flags);
}

static void jz4740_mmc_clock_enable(struct jz4740_mmc_host *host,
	bool start_transfer)
{
	uint16_t val = JZ_MMC_STRPCL_CLOCK_START;

	if (start_transfer)
		val |= JZ_MMC_STRPCL_START_OP;

	writew(val, host->base + JZ_REG_MMC_STRPCL);
}

static void jz4740_mmc_clock_disable(struct jz4740_mmc_host *host)
{
	uint32_t status;
	unsigned int timeout = 1000;

	writew(JZ_MMC_STRPCL_CLOCK_STOP, host->base + JZ_REG_MMC_STRPCL);
	do {
		status = readl(host->base + JZ_REG_MMC_STATUS);
	} while (status & JZ_MMC_STATUS_CLK_EN && --timeout);
}

static void jz4740_mmc_reset(struct jz4740_mmc_host *host)
{
	uint32_t status;
	unsigned int timeout = 1000;

	writew(JZ_MMC_STRPCL_RESET, host->base + JZ_REG_MMC_STRPCL);
	udelay(10);
	do {
		status = readl(host->base + JZ_REG_MMC_STATUS);
	} while (status & JZ_MMC_STATUS_IS_RESETTING && --timeout);
}

static void jz4740_mmc_request_done(struct jz4740_mmc_host *host)
{
	struct mmc_request *req;

	req = host->req;
	host->req = NULL;

	mmc_request_done(host->mmc, req);
}

static unsigned int jz4740_mmc_poll_irq(struct jz4740_mmc_host *host,
	unsigned int irq)
{
	unsigned int timeout = 0x800;
	uint32_t status;

	do {
		status = jz4740_mmc_read_irq_reg(host);
	} while (!(status & irq) && --timeout);

	if (timeout == 0) {
		set_bit(0, &host->waiting);
		mod_timer(&host->timeout_timer, jiffies + 5*HZ);
		jz4740_mmc_set_irq_enabled(host, irq, true);
		return true;
	}

	return false;
}

static void jz4740_mmc_transfer_check_state(struct jz4740_mmc_host *host,
	struct mmc_data *data)
{
	int status;

	status = readl(host->base + JZ_REG_MMC_STATUS);
	if (status & JZ_MMC_STATUS_WRITE_ERROR_MASK) {
		if (status & (JZ_MMC_STATUS_TIMEOUT_WRITE)) {
			host->req->cmd->error = -ETIMEDOUT;
			data->error = -ETIMEDOUT;
		} else {
			host->req->cmd->error = -EIO;
			data->error = -EIO;
		}
	} else if (status & JZ_MMC_STATUS_READ_ERROR_MASK) {
		if (status & (JZ_MMC_STATUS_TIMEOUT_READ)) {
			host->req->cmd->error = -ETIMEDOUT;
			data->error = -ETIMEDOUT;
		} else {
			host->req->cmd->error = -EIO;
			data->error = -EIO;
		}
	}
}

static bool jz4740_mmc_write_data(struct jz4740_mmc_host *host,
	struct mmc_data *data)
{
	struct sg_mapping_iter *miter = &host->miter;
	void __iomem *fifo_addr = host->base + JZ_REG_MMC_TXFIFO;
	uint32_t *buf;
	bool timeout;
	size_t i, j;

	while (sg_miter_next(miter)) {
		buf = miter->addr;
		i = miter->length / 4;
		j = i / 8;
		i = i & 0x7;
		while (j) {
			timeout = jz4740_mmc_poll_irq(host, JZ_MMC_IRQ_TXFIFO_WR_REQ);
			if (unlikely(timeout))
				goto poll_timeout;

			writel(buf[0], fifo_addr);
			writel(buf[1], fifo_addr);
			writel(buf[2], fifo_addr);
			writel(buf[3], fifo_addr);
			writel(buf[4], fifo_addr);
			writel(buf[5], fifo_addr);
			writel(buf[6], fifo_addr);
			writel(buf[7], fifo_addr);
			buf += 8;
			--j;
		}
		if (unlikely(i)) {
			timeout = jz4740_mmc_poll_irq(host, JZ_MMC_IRQ_TXFIFO_WR_REQ);
			if (unlikely(timeout))
				goto poll_timeout;

			while (i) {
				writel(*buf, fifo_addr);
				++buf;
				--i;
			}
		}
		data->bytes_xfered += miter->length;
	}
	sg_miter_stop(miter);

	return false;

poll_timeout:
	miter->consumed = (void *)buf - miter->addr;
	data->bytes_xfered += miter->consumed;
	sg_miter_stop(miter);

	return true;
}

static bool jz4740_mmc_read_data(struct jz4740_mmc_host *host,
				struct mmc_data *data)
{
	struct sg_mapping_iter *miter = &host->miter;
	void __iomem *fifo_addr = host->base + JZ_REG_MMC_RXFIFO;
	uint32_t *buf;
	uint32_t d;
	uint32_t status;
	size_t i, j;
	unsigned int timeout;

	while (sg_miter_next(miter)) {
		buf = miter->addr;
		i = miter->length;
		j = i / 32;
		i = i & 0x1f;
		while (j) {
			timeout = jz4740_mmc_poll_irq(host, JZ_MMC_IRQ_RXFIFO_RD_REQ);
			if (unlikely(timeout))
				goto poll_timeout;

			buf[0] = readl(fifo_addr);
			buf[1] = readl(fifo_addr);
			buf[2] = readl(fifo_addr);
			buf[3] = readl(fifo_addr);
			buf[4] = readl(fifo_addr);
			buf[5] = readl(fifo_addr);
			buf[6] = readl(fifo_addr);
			buf[7] = readl(fifo_addr);

			buf += 8;
			--j;
		}

		if (unlikely(i)) {
			timeout = jz4740_mmc_poll_irq(host, JZ_MMC_IRQ_RXFIFO_RD_REQ);
			if (unlikely(timeout))
				goto poll_timeout;

			while (i >= 4) {
				*buf++ = readl(fifo_addr);
				i -= 4;
			}
			if (unlikely(i > 0)) {
				d = readl(fifo_addr);
				memcpy(buf, &d, i);
			}
		}
		data->bytes_xfered += miter->length;

		/* This can go away once MIPS implements
		 * flush_kernel_dcache_page */
		flush_dcache_page(miter->page);
	}
	sg_miter_stop(miter);

	/* For whatever reason there is sometime one word more in the fifo then
	 * requested */
	timeout = 1000;
	status = readl(host->base + JZ_REG_MMC_STATUS);
	while (!(status & JZ_MMC_STATUS_DATA_FIFO_EMPTY) && --timeout) {
		d = readl(fifo_addr);
		status = readl(host->base + JZ_REG_MMC_STATUS);
	}

	return false;

poll_timeout:
	miter->consumed = (void *)buf - miter->addr;
	data->bytes_xfered += miter->consumed;
	sg_miter_stop(miter);

	return true;
}

static void jz4740_mmc_timeout(unsigned long data)
{
	struct jz4740_mmc_host *host = (struct jz4740_mmc_host *)data;

	if (!test_and_clear_bit(0, &host->waiting))
		return;

	jz4740_mmc_set_irq_enabled(host, JZ_MMC_IRQ_END_CMD_RES, false);

	host->req->cmd->error = -ETIMEDOUT;
	jz4740_mmc_request_done(host);
}

static void jz4740_mmc_read_response(struct jz4740_mmc_host *host,
	struct mmc_command *cmd)
{
	int i;
	uint16_t tmp;
	void __iomem *fifo_addr = host->base + JZ_REG_MMC_RESP_FIFO;

	if (cmd->flags & MMC_RSP_136) {
		tmp = readw(fifo_addr);
		for (i = 0; i < 4; ++i) {
			cmd->resp[i] = tmp << 24;
			tmp = readw(fifo_addr);
			cmd->resp[i] |= tmp << 8;
			tmp = readw(fifo_addr);
			cmd->resp[i] |= tmp >> 8;
		}
	} else {
		cmd->resp[0] = readw(fifo_addr) << 24;
		cmd->resp[0] |= readw(fifo_addr) << 8;
		cmd->resp[0] |= readw(fifo_addr) & 0xff;
	}
}

static void jz4740_mmc_send_command(struct jz4740_mmc_host *host,
	struct mmc_command *cmd)
{
	uint32_t cmdat = host->cmdat;

	host->cmdat &= ~JZ_MMC_CMDAT_INIT;
	jz4740_mmc_clock_disable(host);

	host->cmd = cmd;

	if (cmd->flags & MMC_RSP_BUSY)
		cmdat |= JZ_MMC_CMDAT_BUSY;

	switch (mmc_resp_type(cmd)) {
	case MMC_RSP_R1B:
	case MMC_RSP_R1:
		cmdat |= JZ_MMC_CMDAT_RSP_R1;
		break;
	case MMC_RSP_R2:
		cmdat |= JZ_MMC_CMDAT_RSP_R2;
		break;
	case MMC_RSP_R3:
		cmdat |= JZ_MMC_CMDAT_RSP_R3;
		break;
	default:
		break;
	}

	if (cmd->data) {
		cmdat |= JZ_MMC_CMDAT_DATA_EN;
		if (cmd->data->flags & MMC_DATA_WRITE)
			cmdat |= JZ_MMC_CMDAT_WRITE;
		if (cmd->data->flags & MMC_DATA_STREAM)
			cmdat |= JZ_MMC_CMDAT_STREAM;
		if (host->use_dma) {
			/*
			 * The 4780's MMC controller has integrated DMA ability
			 * in addition to being able to use the external DMA
			 * controller. It moves DMA control bits to a separate
			 * register. The DMA_SEL bit chooses the external
			 * controller over the integrated one. Earlier SoCs
			 * can only use the external controller, and have a
			 * single DMA enable bit in CMDAT.
			 */
			if (host->version >= JZ_MMC_JZ4780) {
				writel(JZ_MMC_DMAC_DMA_EN | JZ_MMC_DMAC_DMA_SEL,
				       host->base + JZ_REG_MMC_DMAC);
			} else {
				cmdat |= JZ_MMC_CMDAT_DMA_EN;
			}
		} else if (host->version >= JZ_MMC_JZ4780) {
			writel(0, host->base + JZ_REG_MMC_DMAC);
		}

		writew(cmd->data->blksz, host->base + JZ_REG_MMC_BLKLEN);
		writew(cmd->data->blocks, host->base + JZ_REG_MMC_NOB);
	}

	writeb(cmd->opcode, host->base + JZ_REG_MMC_CMD);
	writel(cmd->arg, host->base + JZ_REG_MMC_ARG);
	writel(cmdat, host->base + JZ_REG_MMC_CMDAT);

	jz4740_mmc_clock_enable(host, 1);
}

static void jz_mmc_prepare_data_transfer(struct jz4740_mmc_host *host)
{
	struct mmc_command *cmd = host->req->cmd;
	struct mmc_data *data = cmd->data;
	int direction;

	if (data->flags & MMC_DATA_READ)
		direction = SG_MITER_TO_SG;
	else
		direction = SG_MITER_FROM_SG;

	sg_miter_start(&host->miter, data->sg, data->sg_len, direction);
}


static irqreturn_t jz_mmc_irq_worker(int irq, void *devid)
{
	struct jz4740_mmc_host *host = (struct jz4740_mmc_host *)devid;
	struct mmc_command *cmd = host->req->cmd;
	struct mmc_request *req = host->req;
	struct mmc_data *data = cmd->data;
	bool timeout = false;

	if (cmd->error)
		host->state = JZ4740_MMC_STATE_DONE;

	switch (host->state) {
	case JZ4740_MMC_STATE_READ_RESPONSE:
		if (cmd->flags & MMC_RSP_PRESENT)
			jz4740_mmc_read_response(host, cmd);

		if (!data)
			break;

		jz_mmc_prepare_data_transfer(host);

	case JZ4740_MMC_STATE_TRANSFER_DATA:
		if (host->use_dma) {
			/* Use DMA if enabled, data transfer direction was
			 * defined  before in jz_mmc_prepare_data_transfer().
			 */
			timeout = jz4740_mmc_start_dma_transfer(host, data);
			data->bytes_xfered = data->blocks * data->blksz;
		} else if (data->flags & MMC_DATA_READ)
			/* If DMA is not enabled, rely on data flags
			 * to establish data transfer direction.
			 */
			timeout = jz4740_mmc_read_data(host, data);
		else
			timeout = jz4740_mmc_write_data(host, data);

		if (unlikely(timeout)) {
			host->state = JZ4740_MMC_STATE_TRANSFER_DATA;
			break;
		}

		jz4740_mmc_transfer_check_state(host, data);

		timeout = jz4740_mmc_poll_irq(host, JZ_MMC_IRQ_DATA_TRAN_DONE);
		if (unlikely(timeout)) {
			host->state = JZ4740_MMC_STATE_SEND_STOP;
			break;
		}
		jz4740_mmc_write_irq_reg(host, JZ_MMC_IRQ_DATA_TRAN_DONE);

	case JZ4740_MMC_STATE_SEND_STOP:
		if (!req->stop)
			break;

		jz4740_mmc_send_command(host, req->stop);

		if (mmc_resp_type(req->stop) & MMC_RSP_BUSY) {
			timeout = jz4740_mmc_poll_irq(host,
						      JZ_MMC_IRQ_PRG_DONE);
			if (timeout) {
				host->state = JZ4740_MMC_STATE_DONE;
				break;
			}
		}
	case JZ4740_MMC_STATE_DONE:
		break;
	}

	if (!timeout)
		jz4740_mmc_request_done(host);

	return IRQ_HANDLED;
}

static irqreturn_t jz_mmc_irq(int irq, void *devid)
{
	struct jz4740_mmc_host *host = devid;
	struct mmc_command *cmd = host->cmd;
	uint32_t irq_reg, status, tmp;

	status = readl(host->base + JZ_REG_MMC_STATUS);
	irq_reg = jz4740_mmc_read_irq_reg(host);
	tmp = irq_reg;
	irq_reg &= ~host->irq_mask;

	tmp &= ~(JZ_MMC_IRQ_TXFIFO_WR_REQ | JZ_MMC_IRQ_RXFIFO_RD_REQ |
		JZ_MMC_IRQ_PRG_DONE | JZ_MMC_IRQ_DATA_TRAN_DONE);

	if (tmp != irq_reg)
		jz4740_mmc_write_irq_reg(host, tmp & ~irq_reg);

	if (irq_reg & JZ_MMC_IRQ_SDIO) {
		jz4740_mmc_write_irq_reg(host, JZ_MMC_IRQ_SDIO);
		mmc_signal_sdio_irq(host->mmc);
		irq_reg &= ~JZ_MMC_IRQ_SDIO;
	}

	if (host->req && cmd && irq_reg) {
		if (test_and_clear_bit(0, &host->waiting)) {
			del_timer(&host->timeout_timer);

			if (status & JZ_MMC_STATUS_TIMEOUT_RES) {
					cmd->error = -ETIMEDOUT;
			} else if (status & JZ_MMC_STATUS_CRC_RES_ERR) {
					cmd->error = -EIO;
			} else if (status & (JZ_MMC_STATUS_CRC_READ_ERROR |
				    JZ_MMC_STATUS_CRC_WRITE_ERROR)) {
					if (cmd->data)
							cmd->data->error = -EIO;
					cmd->error = -EIO;
			}

			jz4740_mmc_set_irq_enabled(host, irq_reg, false);
			jz4740_mmc_write_irq_reg(host, irq_reg);

			return IRQ_WAKE_THREAD;
		}
	}

	return IRQ_HANDLED;
}

static int jz4740_mmc_set_clock_rate(struct jz4740_mmc_host *host, int rate)
{
	int div = 0;
	int real_rate;

	jz4740_mmc_clock_disable(host);
	clk_set_rate(host->clk, host->mmc->f_max);

	real_rate = clk_get_rate(host->clk);

	while (real_rate > rate && div < 7) {
		++div;
		real_rate >>= 1;
	}

	writew(div, host->base + JZ_REG_MMC_CLKRT);
	return real_rate;
}

static void jz4740_mmc_request(struct mmc_host *mmc, struct mmc_request *req)
{
	struct jz4740_mmc_host *host = mmc_priv(mmc);

	host->req = req;

	jz4740_mmc_write_irq_reg(host, 0xffffffff);
	jz4740_mmc_set_irq_enabled(host, JZ_MMC_IRQ_END_CMD_RES, true);

	host->state = JZ4740_MMC_STATE_READ_RESPONSE;
	set_bit(0, &host->waiting);
	mod_timer(&host->timeout_timer, jiffies + 5*HZ);
	jz4740_mmc_send_command(host, req->cmd);
}

static void jz4740_mmc_set_ios(struct mmc_host *mmc, struct mmc_ios *ios)
{
	struct jz4740_mmc_host *host = mmc_priv(mmc);
	if (ios->clock)
		jz4740_mmc_set_clock_rate(host, ios->clock);

	switch (ios->power_mode) {
	case MMC_POWER_UP:
		if (!IS_ERR(mmc->supply.vmmc))
			mmc_regulator_set_ocr(mmc, mmc->supply.vmmc, ios->vdd);

		jz4740_mmc_reset(host);
		host->cmdat |= JZ_MMC_CMDAT_INIT;
		clk_prepare_enable(host->clk);
		break;
	case MMC_POWER_ON:
		break;
	default:
		if (!IS_ERR(mmc->supply.vmmc))
			mmc_regulator_set_ocr(mmc, mmc->supply.vmmc, 0);
		clk_disable_unprepare(host->clk);
		break;
	}

	switch (ios->bus_width) {
	case MMC_BUS_WIDTH_1:
		host->cmdat &= ~JZ_MMC_CMDAT_BUS_WIDTH;
		host->cmdat |= JZ_MMC_CMDAT_BUS_WIDTH_1BIT;
		break;
	case MMC_BUS_WIDTH_4:
		host->cmdat &= ~JZ_MMC_CMDAT_BUS_WIDTH;
		host->cmdat |= JZ_MMC_CMDAT_BUS_WIDTH_4BIT;
		break;
	case MMC_BUS_WIDTH_8:
		host->cmdat &= ~JZ_MMC_CMDAT_BUS_WIDTH;
		host->cmdat |= JZ_MMC_CMDAT_BUS_WIDTH_8BIT;
		break;
	default:
		break;
	}
}

static void jz4740_mmc_enable_sdio_irq(struct mmc_host *mmc, int enable)
{
	struct jz4740_mmc_host *host = mmc_priv(mmc);
	jz4740_mmc_set_irq_enabled(host, JZ_MMC_IRQ_SDIO, enable);
}

static const struct mmc_host_ops jz4740_mmc_ops = {
	.request	= jz4740_mmc_request,
	.set_ios	= jz4740_mmc_set_ios,
	.get_ro		= mmc_gpio_get_ro,
	.get_cd		= mmc_gpio_get_cd,
	.enable_sdio_irq = jz4740_mmc_enable_sdio_irq,
};

static const struct of_device_id jz4740_mmc_of_match[] = {
	{ .compatible = "ingenic,jz4740-mmc", .data = (void *) JZ_MMC_JZ4740 },
	{ .compatible = "ingenic,jz4780-mmc", .data = (void *) JZ_MMC_JZ4780 },
	{},
};
MODULE_DEVICE_TABLE(of, jz4740_mmc_of_match);

static int jz4740_mmc_probe(struct platform_device* pdev)
{
	int ret;
	struct mmc_host *mmc;
	struct jz4740_mmc_host *host;
	const struct of_device_id *of_id = of_match_device(
			jz4740_mmc_of_match, &pdev->dev);

	mmc = mmc_alloc_host(sizeof(struct jz4740_mmc_host), &pdev->dev);
	if (!mmc) {
		dev_err(&pdev->dev, "Failed to alloc mmc host structure\n");
		return -ENOMEM;
	}

	host = mmc_priv(mmc);

	host->irq = platform_get_irq(pdev, 0);
	if (host->irq < 0) {
		ret = host->irq;
		dev_err(&pdev->dev, "Failed to get platform irq: %d\n", ret);
		goto err_free_host;
	}

	host->clk = devm_clk_get(&pdev->dev, "mmc");
	if (IS_ERR(host->clk)) {
		ret = PTR_ERR(host->clk);
		dev_err(&pdev->dev, "Failed to get mmc clock\n");
		goto err_free_host;
	}

	host->mem_res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	host->base = devm_ioremap_resource(&pdev->dev, host->mem_res);
	if (IS_ERR(host->base)) {
		ret = PTR_ERR(host->base);
		dev_err(&pdev->dev, "Failed to ioremap base memory\n");
		goto err_free_host;
	}

	ret = mmc_of_parse(mmc);
	if (ret)
		goto err_free_host;

	/*
	 * Check that the bus width specified in the device tree is supported.
	 * 8 bit bus width is only supported from the 4750 onward.
	 */
	if (mmc->caps & MMC_CAP_8_BIT_DATA && host->version < JZ_MMC_JZ4750) {
		dev_err(&pdev->dev, "8 bit bus width is unsupported\n");
		ret = -EINVAL;
		goto err_free_host;
	}


	mmc_regulator_get_supply(mmc);

	mmc->ops = &jz4740_mmc_ops;
	if (!mmc->f_max)
		mmc->f_max = JZ_MMC_CLK_RATE;
	mmc->f_min = mmc->f_max / 128;
	mmc->ocr_avail = MMC_VDD_32_33 | MMC_VDD_33_34;

	mmc->max_blk_size = (1 << 10) - 1;
	mmc->max_blk_count = (1 << 15) - 1;
	mmc->max_req_size = mmc->max_blk_size * mmc->max_blk_count;

	mmc->max_segs = 128;
	mmc->max_seg_size = mmc->max_req_size;

	host->mmc = mmc;
	host->pdev = pdev;
	spin_lock_init(&host->lock);
	host->irq_mask = 0xffffffff;
	host->version = (enum jz4740_mmc_version) of_id->data;

	jz4740_mmc_reset(host);

	ret = request_threaded_irq(host->irq, jz_mmc_irq, jz_mmc_irq_worker, 0,
			dev_name(&pdev->dev), host);
	if (ret) {
		dev_err(&pdev->dev, "Failed to request irq: %d\n", ret);
		goto err_free_host;
	}

	jz4740_mmc_clock_disable(host);
	setup_timer(&host->timeout_timer, jz4740_mmc_timeout,
			(unsigned long)host);
	/* It is not important when it times out, it just needs to timeout. */
	set_timer_slack(&host->timeout_timer, HZ);

	ret = jz4740_mmc_acquire_dma_channels(host);
	if (ret == -EPROBE_DEFER)
		goto err_free_irq;

	host->use_dma = !ret;

	platform_set_drvdata(pdev, host);
	ret = mmc_add_host(mmc);

	if (ret) {
		dev_err(&pdev->dev, "Failed to add mmc host: %d\n", ret);
		goto err_release_dma;
	}
	dev_info(&pdev->dev, "JZ SD/MMC card driver registered\n");

	dev_info(&pdev->dev, "Using %s, %d-bit mode\n",
		 host->use_dma ? "DMA" : "PIO",
		 (mmc->caps & MMC_CAP_4_BIT_DATA) ? 4 : 1);

	return 0;

err_release_dma:
	if (host->use_dma)
		jz4740_mmc_release_dma_channels(host);
err_free_irq:
	free_irq(host->irq, host);
err_free_host:
	mmc_free_host(mmc);

	return ret;
}

static int jz4740_mmc_remove(struct platform_device *pdev)
{
	struct jz4740_mmc_host *host = platform_get_drvdata(pdev);

	del_timer_sync(&host->timeout_timer);
	jz4740_mmc_set_irq_enabled(host, 0xff, false);
	jz4740_mmc_reset(host);

	mmc_remove_host(host->mmc);

	free_irq(host->irq, host);

	if (host->use_dma)
		jz4740_mmc_release_dma_channels(host);

	mmc_free_host(host->mmc);

	return 0;
}

static struct platform_driver jz4740_mmc_driver = {
	.probe = jz4740_mmc_probe,
	.remove = jz4740_mmc_remove,
	.driver = {
		.name = "jz4740-mmc",
		.of_match_table = of_match_ptr(jz4740_mmc_of_match),
	},
};

module_platform_driver(jz4740_mmc_driver);

MODULE_DESCRIPTION("JZ4740 SD/MMC controller driver");
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Lars-Peter Clausen <lars@metafoo.de>");
