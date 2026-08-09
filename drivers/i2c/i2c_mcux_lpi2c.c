/*
 * Copyright (c) 2016 Freescale Semiconductor, Inc.
 * Copyright 2019-2026, NXP
 * Copyright (c) 2022 Vestas Wind Systems A/S
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT nxp_lpi2c

#include <errno.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/clock_control.h>
#include <zephyr/drivers/reset.h>
#include <zephyr/kernel.h>
#include <zephyr/irq.h>
#include <zephyr/pm/device.h>
#include <zephyr/pm/device_runtime.h>
#include <fsl_lpi2c.h>
#if CONFIG_NXP_LP_FLEXCOMM
#include <zephyr/drivers/mfd/nxp_lp_flexcomm.h>
#endif

#include <zephyr/drivers/pinctrl.h>

#ifdef CONFIG_I2C_MCUX_LPI2C_BUS_RECOVERY
#include "i2c_bitbang.h"
#include <zephyr/drivers/gpio.h>
#endif /* CONFIG_I2C_MCUX_LPI2C_BUS_RECOVERY */

#ifdef CONFIG_I2C_MCUX_LPI2C_EDMA
#include <zephyr/drivers/dma.h>
#include <zephyr/linker/sections.h>
#endif

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(mcux_lpi2c);


#include "i2c-priv.h"
/* Wait for the duration of 12 bits to detect a NAK after a bus
 * address scan.  (10 appears sufficient, 20% safety factor.)
 */
#define SCAN_DELAY_US(baudrate) (12 * USEC_PER_SEC / baudrate)

/* Required by DEVICE_MMIO_NAMED_* macros */
#define DEV_CFG(_dev) \
	((const struct mcux_lpi2c_config *)(_dev)->config)
#define DEV_DATA(_dev) ((struct mcux_lpi2c_data *)(_dev)->data)

#ifdef CONFIG_I2C_MCUX_LPI2C_EDMA
/*
 * eDMA transfer state. LPI2C has ONE dma request line per instance
 * (FSL_FEATURE_LPI2C_HAS_SEPARATE_DMA_RX_TX_REQ == 0 on i.MX RT11xx),
 * so a single channel serves both phases: first MEMORY->MTDR pushing the
 * 16-bit command/data stream (TDDE), then - for reads - MRDR->MEMORY
 * draining the payload (RDDE), reconfigured in the phase-1 completion
 * callback. SCL stretches while the RX FIFO is full, so the handover
 * window is not timing critical.
 *
 * Staging lives in this struct, which is placed __nocache: eDMA moves
 * physical bytes and does no cache maintenance, so coherent-by-placement
 * is the same policy the rest of this board's DMA uses.
 */
#define LPI2C_EDMA_MAX_CMDS 20   /* start + up to 16 data/subaddr + recv + stop */
#define LPI2C_EDMA_MAX_DATA 256  /* one RECV command's maximum payload */

/* ONLY what eDMA itself touches goes __nocache. The nocache output
 * section is NOBITS: it is neither loaded with initializer values nor
 * guaranteed zeroed, so wiring/state MUST NOT live there (a garbage
 * `active` made the ISR hook eat the fallback path's interrupts on the
 * first integrated arm, wedging the bus lock forever). */
struct mcux_lpi2c_edma_bufs {
	uint16_t cmds[LPI2C_EDMA_MAX_CMDS];
	uint8_t rx_stage[LPI2C_EDMA_MAX_DATA];
};

struct mcux_lpi2c_edma {
	const struct device *dma_dev;
	uint32_t channel;
	uint32_t slot;             /* DMAMUX request source */
	struct mcux_lpi2c_edma_bufs *bufs;
	struct k_sem done;
	volatile int result;
	volatile bool active;
	uint32_t rx_len;           /* 0 = write-only transfer */
	uint8_t *user_rx;          /* destination in the caller's buffer */
	uint32_t n_cmds;
};
#endif /* CONFIG_I2C_MCUX_LPI2C_EDMA */

struct mcux_lpi2c_config {
	DEVICE_MMIO_NAMED_ROM(reg_base);
	const struct device *clock_dev;
	clock_control_subsys_t clock_subsys;
	void (*irq_config_func)(const struct device *dev);
	uint32_t bitrate;
	uint32_t bus_idle_timeout_ns;
	const struct pinctrl_dev_config *pincfg;
	struct reset_dt_spec reset;
#ifdef CONFIG_I2C_MCUX_LPI2C_BUS_RECOVERY
	struct gpio_dt_spec scl;
	struct gpio_dt_spec sda;
	bool recover_bus_on_init;
#endif /* CONFIG_I2C_MCUX_LPI2C_BUS_RECOVERY */
#ifdef CONFIG_I2C_MCUX_LPI2C_EDMA
	struct mcux_lpi2c_edma *edma;   /* NULL when the node has no dmas */
#endif
};

struct mcux_lpi2c_data {
	DEVICE_MMIO_NAMED_RAM(reg_base);
	lpi2c_master_handle_t handle;
	struct k_sem lock;
	struct k_sem device_sync_sem;
	status_t callback_status;
#ifdef CONFIG_I2C_TARGET
	lpi2c_slave_handle_t target_handle;
	struct i2c_target_config *target_cfg;
	bool target_attached;
	bool first_tx;
	bool read_active;
	bool send_ack;
#endif
};

static int mcux_lpi2c_configure(const struct device *dev,
				uint32_t dev_config_raw)
{
	const struct mcux_lpi2c_config *config = dev->config;
	struct mcux_lpi2c_data *data = dev->data;
	LPI2C_Type *base = (LPI2C_Type *)DEVICE_MMIO_NAMED_GET(dev, reg_base);
	uint32_t clock_freq;
	uint32_t baudrate;
	int ret;

	if (!(I2C_MODE_CONTROLLER & dev_config_raw)) {
		return -EINVAL;
	}

	if (I2C_ADDR_10_BITS & dev_config_raw) {
		return -EINVAL;
	}

	switch (I2C_SPEED_GET(dev_config_raw)) {
	case I2C_SPEED_STANDARD:
		baudrate = KHZ(100);
		break;
	case I2C_SPEED_FAST:
		baudrate = KHZ(400);
		break;
	case I2C_SPEED_FAST_PLUS:
		baudrate = MHZ(1);
		break;
	default:
		return -EINVAL;
	}

	if (clock_control_get_rate(config->clock_dev, config->clock_subsys,
				   &clock_freq)) {
		return -EINVAL;
	}

	ret = k_sem_take(&data->lock, K_FOREVER);
	if (ret) {
		return ret;
	}

	LPI2C_MasterSetBaudRate(base, clock_freq, baudrate);
	k_sem_give(&data->lock);

	return 0;
}

static void mcux_lpi2c_master_transfer_callback(LPI2C_Type *base,
						lpi2c_master_handle_t *handle,
						status_t status, void *userData)
{
	struct mcux_lpi2c_data *data = userData;

	ARG_UNUSED(handle);
	ARG_UNUSED(base);

	data->callback_status = status;
	k_sem_give(&data->device_sync_sem);
}

static uint32_t mcux_lpi2c_convert_flags(int msg_flags)
{
	uint32_t flags = 0U;

	if (!(msg_flags & I2C_MSG_STOP)) {
		flags |= kLPI2C_TransferNoStopFlag;
	}

	return flags;
}

#ifdef CONFIG_I2C_MCUX_LPI2C_EDMA
/* MTDR command encodings (LPI2C reference manual, MTDR[10:8]) */
#define LPI2C_CMD_TXDATA(b)  ((uint16_t)(b))
#define LPI2C_CMD_RECV(n)    (uint16_t)((1u << 8) | ((n) - 1u))
#define LPI2C_CMD_STOP       ((uint16_t)(2u << 8))
#define LPI2C_CMD_START(a)   (uint16_t)((4u << 8) | (a))

static void mcux_lpi2c_edma_dma_cb(const struct device *dma_dev, void *user_data,
				   uint32_t channel, int status);

static void mcux_lpi2c_edma_finish(const struct device *dev, int result)
{
	const struct mcux_lpi2c_config *config = dev->config;
	struct mcux_lpi2c_edma *e = config->edma;
	LPI2C_Type *base = (LPI2C_Type *)DEVICE_MMIO_NAMED_GET(dev, reg_base);

	base->MDER = 0;                                   /* TDDE/RDDE off */
	base->MIER = 0;
	if (result != 0) {
		/* the REAL abort the IRQ path never had: kill the channel,
		 * flush both FIFOs, clear sticky flags */
		dma_stop(e->dma_dev, e->channel);
		base->MCR |= LPI2C_MCR_RTF_MASK | LPI2C_MCR_RRF_MASK;
		LPI2C_MasterClearStatusFlags(base, (uint32_t)kLPI2C_MasterClearFlags);
	}
	e->result = result;
	e->active = false;
	k_sem_give(&e->done);
}

static int mcux_lpi2c_edma_start_rx(const struct device *dev)
{
	const struct mcux_lpi2c_config *config = dev->config;
	struct mcux_lpi2c_edma *e = config->edma;
	LPI2C_Type *base = (LPI2C_Type *)DEVICE_MMIO_NAMED_GET(dev, reg_base);
	struct dma_block_config blk = {0};
	struct dma_config cfg = {0};

	blk.source_address = (uint32_t)&base->MRDR;
	blk.dest_address = (uint32_t)e->bufs->rx_stage;
	blk.block_size = e->rx_len;
	blk.source_addr_adj = DMA_ADDR_ADJ_NO_CHANGE;
	blk.dest_addr_adj = DMA_ADDR_ADJ_INCREMENT;

	cfg.channel_direction = PERIPHERAL_TO_MEMORY;
	cfg.dma_slot = e->slot;
	cfg.source_data_size = 1;
	cfg.dest_data_size = 1;
	cfg.source_burst_length = 1;
	cfg.dest_burst_length = 1;
	cfg.block_count = 1;
	cfg.head_block = &blk;
	cfg.dma_callback = mcux_lpi2c_edma_dma_cb;
	cfg.user_data = (void *)dev;

	int ret = dma_config(e->dma_dev, e->channel, &cfg);
	if (ret == 0) {
		base->MDER = LPI2C_MDER_RDDE_MASK;        /* switch request to RX */
		ret = dma_start(e->dma_dev, e->channel);
	}
	return ret;
}

static void mcux_lpi2c_edma_dma_cb(const struct device *dma_dev, void *user_data,
				   uint32_t channel, int status)
{
	const struct device *dev = user_data;
	const struct mcux_lpi2c_config *config = dev->config;
	struct mcux_lpi2c_edma *e = config->edma;

	ARG_UNUSED(dma_dev);
	ARG_UNUSED(channel);

	if (!e->active) {
		return;
	}
	if (status < 0) {
		mcux_lpi2c_edma_finish(dev, -EIO);
		return;
	}
	if (e->rx_len > 0 && e->n_cmds != 0) {
		/* command stream delivered - hand the shared request to RX.
		 * SCL stretches on RX-FIFO-full, so this is not a race. */
		e->n_cmds = 0;                       /* marks RX phase */
		if (mcux_lpi2c_edma_start_rx(dev) != 0) {
			mcux_lpi2c_edma_finish(dev, -EIO);
		}
		return;
	}
	if (e->rx_len > 0) {
		memcpy(e->user_rx, e->bufs->rx_stage, e->rx_len);
	}
	mcux_lpi2c_edma_finish(dev, 0);
}

/*
 * Fast-path DMA transfer. Handles the traffic ArduPilot-class users
 * generate - register writes, register reads with repeated start, plain
 * reads - and returns -EAGAIN for anything else so the caller falls back
 * to the interrupt state machine.
 *
 * Cost per transfer: one DMA-complete interrupt for pure writes (plus one
 * LPI2C stop/error interrupt), two DMA-completes for reads. Compare one
 * interrupt per BYTE on the fsl state machine (MFCR watermarks are 0).
 */
static int mcux_lpi2c_transfer_edma(const struct device *dev, struct i2c_msg *msgs,
				    uint8_t num_msgs, uint16_t addr)
{
	const struct mcux_lpi2c_config *config = dev->config;
	struct mcux_lpi2c_data *data = dev->data;
	struct mcux_lpi2c_edma *e = config->edma;
	LPI2C_Type *base = (LPI2C_Type *)DEVICE_MMIO_NAMED_GET(dev, reg_base);
	uint32_t n = 0;

	/* controller unusable (init found no DMA device): permanent fallback */
	if (e->result == -ENODEV && !e->active) {
		return -EAGAIN;
	}
	/* pattern gate */
	if (num_msgs < 1 || num_msgs > 2) {
		return -EAGAIN;
	}
	for (uint8_t i = 0; i < num_msgs; i++) {
		if (msgs[i].flags & I2C_MSG_ADDR_10_BITS) {
			return -EAGAIN;
		}
		if (msgs[i].len == 0) {
			return -EAGAIN;   /* address-scan probes keep the IRQ path */
		}
	}

	const struct i2c_msg *m0 = &msgs[0];
	const struct i2c_msg *m1 = (num_msgs == 2) ? &msgs[1] : NULL;

	e->rx_len = 0;
	e->user_rx = NULL;

	if (!(m0->flags & I2C_MSG_READ)) {
		if (m0->len > 16) {
			return -EAGAIN;   /* large writes: rare, IRQ path */
		}
		e->bufs->cmds[n++] = LPI2C_CMD_START((uint8_t)(addr << 1));
		for (uint32_t i = 0; i < m0->len; i++) {
			e->bufs->cmds[n++] = LPI2C_CMD_TXDATA(m0->buf[i]);
		}
		if (m1 != NULL) {
			if (!(m1->flags & I2C_MSG_READ) || m1->len > LPI2C_EDMA_MAX_DATA) {
				return -EAGAIN;
			}
			e->bufs->cmds[n++] = LPI2C_CMD_START((uint8_t)((addr << 1) | 1u));
			e->bufs->cmds[n++] = LPI2C_CMD_RECV(m1->len);
			e->rx_len = m1->len;
			e->user_rx = m1->buf;
		}
	} else {
		if (m1 != NULL || m0->len > LPI2C_EDMA_MAX_DATA) {
			return -EAGAIN;
		}
		e->bufs->cmds[n++] = LPI2C_CMD_START((uint8_t)((addr << 1) | 1u));
		e->bufs->cmds[n++] = LPI2C_CMD_RECV(m0->len);
		e->rx_len = m0->len;
		e->user_rx = m0->buf;
	}
	e->bufs->cmds[n++] = LPI2C_CMD_STOP;
	e->n_cmds = n;

	int ret = k_sem_take(&data->lock, K_FOREVER);
	if (ret) {
		return ret;
	}
	(void)pm_device_runtime_get(dev);

	if (LPI2C_CheckForBusyBus(base) != kStatus_Success) {
		ret = -EBUSY;
		goto out;
	}

	LPI2C_MasterClearStatusFlags(base, (uint32_t)kLPI2C_MasterClearFlags);
	k_sem_reset(&e->done);
	e->result = 0;
	e->active = true;

	/* phase 1: command/data stream, 16-bit words into MTDR */
	{
		struct dma_block_config blk = {0};
		struct dma_config cfg = {0};

		blk.source_address = (uint32_t)e->bufs->cmds;
		blk.dest_address = (uint32_t)&base->MTDR;
		blk.block_size = e->n_cmds * sizeof(uint16_t);
		blk.source_addr_adj = DMA_ADDR_ADJ_INCREMENT;
		blk.dest_addr_adj = DMA_ADDR_ADJ_NO_CHANGE;

		cfg.channel_direction = MEMORY_TO_PERIPHERAL;
		cfg.dma_slot = e->slot;
		cfg.source_data_size = 2;
		cfg.dest_data_size = 2;
		cfg.source_burst_length = 2;
		cfg.dest_burst_length = 2;
		cfg.block_count = 1;
		cfg.head_block = &blk;
		cfg.dma_callback = mcux_lpi2c_edma_dma_cb;
		cfg.user_data = (void *)dev;

		ret = dma_config(e->dma_dev, e->channel, &cfg);
		if (ret == 0) {
			/* errors surface through the LPI2C IRQ: NACK, arb
			 * loss, FIFO error, pin-low timeout */
			base->MIER = LPI2C_MIER_NDIE_MASK | LPI2C_MIER_ALIE_MASK |
				     LPI2C_MIER_FEIE_MASK | LPI2C_MIER_PLTIE_MASK;
			base->MDER = LPI2C_MDER_TDDE_MASK;
			ret = dma_start(e->dma_dev, e->channel);
		}
	}
	if (ret != 0) {
		e->active = false;
		base->MDER = 0;
		base->MIER = 0;
		goto out;
	}

	/* bounded wait: a wedged bus reports instead of hanging the caller.
	 * 100 ms >> any legal transfer (256 B at 100 kHz is ~23 ms). */
	if (k_sem_take(&e->done, K_MSEC(100)) != 0) {
		mcux_lpi2c_edma_finish(dev, -ETIMEDOUT);
		(void)k_sem_take(&e->done, K_NO_WAIT);
	}
	ret = e->result;

out:
	(void)pm_device_runtime_put(dev);
	k_sem_give(&data->lock);
	return ret;
}
#endif /* CONFIG_I2C_MCUX_LPI2C_EDMA */

static int mcux_lpi2c_transfer(const struct device *dev, struct i2c_msg *msgs,
				   uint8_t num_msgs, uint16_t addr)
{
	const struct mcux_lpi2c_config *config = dev->config;
	struct mcux_lpi2c_data *data = dev->data;
	LPI2C_Type *base = (LPI2C_Type *)DEVICE_MMIO_NAMED_GET(dev, reg_base);
	lpi2c_master_transfer_t transfer;
	status_t status;
	int ret = 0;

#ifdef CONFIG_I2C_MCUX_LPI2C_EDMA
	if (config->edma != NULL) {
		ret = mcux_lpi2c_transfer_edma(dev, msgs, num_msgs, addr);
		if (ret != -EAGAIN) {
			return ret;
		}
		ret = 0;   /* pattern outside the fast path: interrupt path below */
	}
#endif

	ret = k_sem_take(&data->lock, K_FOREVER);
	if (ret) {
		return ret;
	}

	(void)pm_device_runtime_get(dev);

	/* Iterate over all the messages */
	for (int i = 0; i < num_msgs; i++) {
		if (I2C_MSG_ADDR_10_BITS & msgs->flags) {
			ret = -ENOTSUP;
			break;
		}

		/* Initialize the transfer descriptor */
		transfer.flags = mcux_lpi2c_convert_flags(msgs->flags);

		/* Prevent the controller to send a start condition between
		 * messages, except if explicitly requested.
		 */
		if (i != 0 && !(msgs->flags & I2C_MSG_RESTART)) {
			transfer.flags |= kLPI2C_TransferNoStartFlag;
		}

		transfer.slaveAddress = addr;
		transfer.direction = (msgs->flags & I2C_MSG_READ)
			? kLPI2C_Read : kLPI2C_Write;
		transfer.subaddress = 0;
		transfer.subaddressSize = 0;
		transfer.data = msgs->buf;
		transfer.dataSize = msgs->len;

		/* Start the transfer */
		status = LPI2C_MasterTransferNonBlocking(base,
				&data->handle, &transfer);

		/* Return an error if the transfer didn't start successfully
		 * e.g., if the bus was busy
		 */
		if (status != kStatus_Success) {
			LPI2C_MasterTransferAbort(base, &data->handle);
			ret = -EIO;
			break;
		}

		/* Wait for the transfer to complete */
		k_sem_take(&data->device_sync_sem, K_FOREVER);

		/* Return an error if the transfer didn't complete
		 * successfully. e.g., nak, timeout, lost arbitration
		 */
		if (data->callback_status != kStatus_Success) {
			LPI2C_MasterTransferAbort(base, &data->handle);
			ret = -EIO;
			break;
		}
		if (msgs->len == 0) {
			k_busy_wait(SCAN_DELAY_US(config->bitrate));
			if (0 != (base->MSR & LPI2C_MSR_NDF_MASK)) {
				LPI2C_MasterTransferAbort(base, &data->handle);
				ret = -EIO;
				break;
			}
		}
		/* Move to the next message */
		msgs++;
	}

	(void)pm_device_runtime_put(dev);

	k_sem_give(&data->lock);

	return ret;
}

#if CONFIG_I2C_MCUX_LPI2C_BUS_RECOVERY
static void mcux_lpi2c_bitbang_set_scl(void *io_context, int state)
{
	const struct mcux_lpi2c_config *config = io_context;

	gpio_pin_set_dt(&config->scl, state);
}

static void mcux_lpi2c_bitbang_set_sda(void *io_context, int state)
{
	const struct mcux_lpi2c_config *config = io_context;

	gpio_pin_set_dt(&config->sda, state);
}

static int mcux_lpi2c_bitbang_get_sda(void *io_context)
{
	const struct mcux_lpi2c_config *config = io_context;

	return gpio_pin_get_dt(&config->sda) == 0 ? 0 : 1;
}

static int mcux_lpi2c_recover_bus(const struct device *dev)
{
	const struct mcux_lpi2c_config *config = dev->config;
	struct mcux_lpi2c_data *data = dev->data;
	struct i2c_bitbang bitbang_ctx;
	struct i2c_bitbang_io bitbang_io = {
		.set_scl = mcux_lpi2c_bitbang_set_scl,
		.set_sda = mcux_lpi2c_bitbang_set_sda,
		.get_sda = mcux_lpi2c_bitbang_get_sda,
	};
	uint32_t bitrate_cfg;
	int error = 0;

	if (!gpio_is_ready_dt(&config->scl)) {
		LOG_ERR("SCL GPIO device not ready");
		return -EIO;
	}

	if (!gpio_is_ready_dt(&config->sda)) {
		LOG_ERR("SDA GPIO device not ready");
		return -EIO;
	}

	k_sem_take(&data->lock, K_FOREVER);

	error = gpio_pin_configure_dt(&config->scl, GPIO_OUTPUT_HIGH);
	if (error != 0) {
		LOG_ERR("failed to configure SCL GPIO (err %d)", error);
		goto restore;
	}

	error = gpio_pin_configure_dt(&config->sda, GPIO_OUTPUT_HIGH);
	if (error != 0) {
		LOG_ERR("failed to configure SDA GPIO (err %d)", error);
		goto restore;
	}

	i2c_bitbang_init(&bitbang_ctx, &bitbang_io, (void *)config);

	bitrate_cfg = i2c_map_dt_bitrate(config->bitrate) | I2C_MODE_CONTROLLER;
	error = i2c_bitbang_configure(&bitbang_ctx, bitrate_cfg);
	if (error != 0) {
		LOG_ERR("failed to configure I2C bitbang (err %d)", error);
		goto restore;
	}

	error = i2c_bitbang_recover_bus(&bitbang_ctx);
	if (error != 0) {
		LOG_ERR("failed to recover bus (err %d)", error);
		goto restore;
	}

restore:
	(void)pinctrl_apply_state(config->pincfg, PINCTRL_STATE_DEFAULT);

	k_sem_give(&data->lock);

	return error;
}
#endif /* CONFIG_I2C_MCUX_LPI2C_BUS_RECOVERY */

#ifdef CONFIG_I2C_TARGET
static void mcux_lpi2c_slave_irq_handler(const struct device *dev)
{
	struct mcux_lpi2c_data *data = dev->data;
	LPI2C_Type *base = (LPI2C_Type *)DEVICE_MMIO_NAMED_GET(dev, reg_base);
	const struct i2c_target_callbacks *target_cb = data->target_cfg->callbacks;
	int ret;
	uint32_t flags;
	uint8_t i2c_data;

	/* Note- the HAL provides a callback-based I2C slave API, but
	 * the API expects the user to provide a transmit buffer of
	 * a fixed length at the first byte received, and will not signal
	 * the user callback until this buffer is exhausted. This does not
	 * work well with the Zephyr API, which requires callbacks for
	 * every byte. For these reason, we handle the LPI2C IRQ
	 * directly.
	 */
	flags = LPI2C_SlaveGetStatusFlags(base);

	if (flags & kLPI2C_SlaveAddressValidFlag) {
		/* Read Slave address to clear flag */
		LPI2C_SlaveGetReceivedAddress(base);
		data->first_tx = true;
		/* Reset to sending ACK, in case we NAK'ed before */
		data->send_ack = true;
	}

	if (flags & kLPI2C_SlaveRxReadyFlag) {
		/* RX data is available, read it and issue callback */
		i2c_data = (uint8_t)base->SRDR;
		if (data->first_tx) {
			data->first_tx = false;
			if (target_cb->write_requested) {
				ret = target_cb->write_requested(data->target_cfg);
				if (ret < 0) {
					/* NAK further bytes */
					data->send_ack = false;
				}
			}
		}
		if (target_cb->write_received) {
			ret = target_cb->write_received(data->target_cfg,
							i2c_data);
			if (ret < 0) {
				/* NAK further bytes */
				data->send_ack = false;
			}
		}
	}

	if (flags & kLPI2C_SlaveTxReadyFlag) {
		/* Space is available in TX fifo, issue callback and write out */
		if (data->first_tx) {
			data->read_active = true;
			data->first_tx = false;
			if (target_cb->read_requested) {
				ret = target_cb->read_requested(data->target_cfg,
								&i2c_data);
				if (ret < 0) {
					/* Disable TX */
					data->read_active = false;
				} else {
					/* Send I2C data */
					base->STDR = i2c_data;
				}
			}
		} else if (data->read_active) {
			if (target_cb->read_processed) {
				ret = target_cb->read_processed(data->target_cfg,
								&i2c_data);
				if (ret < 0) {
					/* Disable TX */
					data->read_active = false;
				} else {
					/* Send I2C data */
					base->STDR = i2c_data;
				}
			}
		}
	}

	if (flags & kLPI2C_SlaveStopDetectFlag) {
		LPI2C_SlaveClearStatusFlags(base, flags);
		if (target_cb->stop) {
			target_cb->stop(data->target_cfg);
		}
	}

	if (flags & kLPI2C_SlaveTransmitAckFlag) {
		LPI2C_SlaveTransmitAck(base, data->send_ack);
	}
}

static int mcux_lpi2c_target_register(const struct device *dev,
					  struct i2c_target_config *target_config)
{
	const struct mcux_lpi2c_config *config = dev->config;
	struct mcux_lpi2c_data *data = dev->data;
	LPI2C_Type *base = (LPI2C_Type *)DEVICE_MMIO_NAMED_GET(dev, reg_base);
	lpi2c_slave_config_t slave_config;
	uint32_t clock_freq;

	LPI2C_MasterDeinit(base);

	/* Get the clock frequency */
	if (clock_control_get_rate(config->clock_dev, config->clock_subsys,
				   &clock_freq)) {
		return -EINVAL;
	}

	if (!target_config) {
		return -EINVAL;
	}

	if (data->target_attached) {
		return -EBUSY;
	}

	data->target_attached = true;
	data->target_cfg = target_config;
	data->first_tx = false;

	LPI2C_SlaveGetDefaultConfig(&slave_config);
	slave_config.address0 = target_config->address;
	/* Note- this setting enables clock stretching to allow the
	 * slave to respond to each byte with an ACK/NAK.
	 * this behavior may cause issues with some I2C controllers.
	 */
	slave_config.sclStall.enableAck = true;
	LPI2C_SlaveInit(base, &slave_config, clock_freq);
	/* Clear all flags. */
	LPI2C_SlaveClearStatusFlags(base, (uint32_t)kLPI2C_SlaveClearFlags);
	/* Enable interrupt */
	LPI2C_SlaveEnableInterrupts(base,
					(kLPI2C_SlaveTxReadyFlag |
					kLPI2C_SlaveRxReadyFlag |
					kLPI2C_SlaveStopDetectFlag |
					kLPI2C_SlaveAddressValidFlag |
					kLPI2C_SlaveTransmitAckFlag));
	return 0;
}

static int mcux_lpi2c_target_unregister(const struct device *dev,
					struct i2c_target_config *target_config)
{
	struct mcux_lpi2c_data *data = dev->data;
	LPI2C_Type *base = (LPI2C_Type *)DEVICE_MMIO_NAMED_GET(dev, reg_base);

	if (!data->target_attached) {
		return -EINVAL;
	}

	data->target_cfg = NULL;
	data->target_attached = false;

	LPI2C_SlaveDeinit(base);

	return 0;
}
#endif /* CONFIG_I2C_TARGET */

#if DT_HAS_COMPAT_STATUS_OKAY(nxp_lp_flexcomm)
#define LPI2C_IRQHANDLE_ARG LPI2C_GetInstance(base)
#else
#define LPI2C_IRQHANDLE_ARG base
#endif

static void mcux_lpi2c_isr(const struct device *dev)
{
	struct mcux_lpi2c_data *data = dev->data;
	LPI2C_Type *base = (LPI2C_Type *)DEVICE_MMIO_NAMED_GET(dev, reg_base);

 #ifdef CONFIG_I2C_TARGET
	if (data->target_attached) {
		mcux_lpi2c_slave_irq_handler(dev);
	}
#endif /* CONFIG_I2C_TARGET */

#ifdef CONFIG_I2C_MCUX_LPI2C_EDMA
	{
		const struct mcux_lpi2c_config *config = dev->config;

		if (config->edma != NULL && config->edma->active) {
			/* DMA-mode transfer: this IRQ is an error report
			 * (MIER = NDF|ALF|FEF|PLTF only). Never hand it to
			 * the fsl state machine - its unbounded status loop
			 * is the ISR-livelock found live 2026-08-09. */
			const uint32_t msr = base->MSR;

			if (msr & (LPI2C_MSR_NDF_MASK | LPI2C_MSR_ALF_MASK |
				   LPI2C_MSR_FEF_MASK | LPI2C_MSR_PLTF_MASK)) {
				mcux_lpi2c_edma_finish(dev,
					(msr & LPI2C_MSR_NDF_MASK) ? -EIO : -EFAULT);
			}
			return;
		}
	}
#endif

	LPI2C_MasterTransferHandleIRQ(LPI2C_IRQHANDLE_ARG, &data->handle);
}

static int mcux_lpi2c_suspend(const struct device *dev)
{
	int ret;
	const struct mcux_lpi2c_config *config = dev->config;

	ret = clock_control_off(config->clock_dev, config->clock_subsys);
	if (ret < 0) {
		LOG_ERR("failed clock off lpi2c");
		return ret;
	}

	return 0;
}

static int mcux_lpi2c_resume(const struct device *dev)
{
	int ret;
	const struct mcux_lpi2c_config *config = dev->config;

	ret = clock_control_on(config->clock_dev, config->clock_subsys);
	if (ret < 0) {
		LOG_ERR("failed clock on lpi2c");
		return ret;
	}

	return 0;
}

static int mcux_lpi2c_pm_action(const struct device *dev, enum pm_device_action action)
{
	int ret;

	switch (action) {
	case PM_DEVICE_ACTION_RESUME:
		ret = mcux_lpi2c_resume(dev);
		break;
	case PM_DEVICE_ACTION_SUSPEND:
		ret = mcux_lpi2c_suspend(dev);
		break;
	default:
		return -ENOTSUP;
	}

	return ret;
}

static int mcux_lpi2c_init(const struct device *dev)
{
	const struct mcux_lpi2c_config *config = dev->config;
	struct mcux_lpi2c_data *data = dev->data;
	LPI2C_Type *base;
	uint32_t clock_freq, bitrate_cfg;
	lpi2c_master_config_t master_config;
	int error;

	ARG_UNUSED(data);

	DEVICE_MMIO_NAMED_MAP(dev, reg_base, K_MEM_CACHE_NONE | K_MEM_DIRECT_MAP);

	base = (LPI2C_Type *)DEVICE_MMIO_NAMED_GET(dev, reg_base);

	k_sem_init(&data->lock, 1, 1);
	k_sem_init(&data->device_sync_sem, 0, K_SEM_MAX_LIMIT);

#ifdef CONFIG_I2C_MCUX_LPI2C_EDMA
	if (config->edma != NULL) {
		config->edma->active = false;
		config->edma->result = 0;
		config->edma->rx_len = 0;
		config->edma->n_cmds = 0;
		k_sem_init(&config->edma->done, 0, 1);
		if (!device_is_ready(config->edma->dma_dev)) {
			/* GRACEFUL: a missing/late DMA controller must not
			 * take the whole I2C bus down with -ENODEV (first
			 * integrated arm: every bus vanished and the vehicle
			 * spun on a missing baro before serial init). Flag
			 * the path unusable; transfers use the IRQ engine. */
			LOG_WRN("lpi2c edma controller not ready, using IRQ path");
			config->edma->result = -ENODEV;
		}
	}
#endif

	if (!device_is_ready(config->clock_dev)) {
		LOG_ERR("clock control device not ready");
		return -ENODEV;
	}

	if (config->reset.dev != NULL) {
		if (!device_is_ready(config->reset.dev)) {
			LOG_ERR("reset controller not ready");
			return -ENODEV;
		}

		error = reset_line_deassert_dt(&config->reset);
		if (error != 0) {
			LOG_ERR("Failed to deassert reset line (%d)", error);
			return error;
		}
	}

	error = pinctrl_apply_state(config->pincfg, PINCTRL_STATE_DEFAULT);
	if (error) {
		return error;
	}

#ifdef CONFIG_I2C_MCUX_LPI2C_BUS_RECOVERY
	if (config->recover_bus_on_init) {
		error = mcux_lpi2c_recover_bus(dev);
		if (error != 0) {
			return error;
		}
	}
#endif /* CONFIG_I2C_MCUX_LPI2C_BUS_RECOVERY */

	if (clock_control_get_rate(config->clock_dev, config->clock_subsys,
				   &clock_freq)) {
		return -EINVAL;
	}

	LPI2C_MasterGetDefaultConfig(&master_config);
	master_config.busIdleTimeout_ns = config->bus_idle_timeout_ns;
	LPI2C_MasterInit(base, &master_config, clock_freq);
	LPI2C_MasterTransferCreateHandle(base, &data->handle,
					 mcux_lpi2c_master_transfer_callback,
					 data);

	bitrate_cfg = i2c_map_dt_bitrate(config->bitrate);

	error = mcux_lpi2c_configure(dev, I2C_MODE_CONTROLLER | bitrate_cfg);
	if (error) {
		return error;
	}

	config->irq_config_func(dev);

	return pm_device_driver_init(dev, mcux_lpi2c_pm_action);
}

static DEVICE_API(i2c, mcux_lpi2c_driver_api) = {
	.configure = mcux_lpi2c_configure,
	.transfer = mcux_lpi2c_transfer,
#if CONFIG_I2C_MCUX_LPI2C_BUS_RECOVERY
	.recover_bus = mcux_lpi2c_recover_bus,
#endif /* CONFIG_I2C_MCUX_LPI2C_BUS_RECOVERY */
#if CONFIG_I2C_TARGET
	.target_register = mcux_lpi2c_target_register,
	.target_unregister = mcux_lpi2c_target_unregister,
#endif /* CONFIG_I2C_TARGET */
};

#if CONFIG_I2C_MCUX_LPI2C_BUS_RECOVERY
#define I2C_MCUX_LPI2C_SCL_INIT(n) .scl = GPIO_DT_SPEC_INST_GET_OR(n, scl_gpios, {0}),
#define I2C_MCUX_LPI2C_SDA_INIT(n) .sda = GPIO_DT_SPEC_INST_GET_OR(n, sda_gpios, {0}),
#define I2C_MCUX_LPI2C_RECOVER_BUS_ON_INIT(n) \
	.recover_bus_on_init = DT_INST_PROP(n, recover_bus_on_init),
#define I2C_MCUX_LPI2C_RECOVER_CHECK(n)					\
	BUILD_ASSERT(!DT_INST_PROP(n, recover_bus_on_init) ||		\
		     (DT_INST_NODE_HAS_PROP(n, scl_gpios) &&		\
		      DT_INST_NODE_HAS_PROP(n, sda_gpios)),		\
		     "I2C node " DT_NODE_FULL_NAME(DT_DRV_INST(n))	\
		     " has recover-bus-on-init but is missing scl-gpios or sda-gpios");
#else
#define I2C_MCUX_LPI2C_SCL_INIT(n)
#define I2C_MCUX_LPI2C_SDA_INIT(n)
#define I2C_MCUX_LPI2C_RECOVER_BUS_ON_INIT(n)
#define I2C_MCUX_LPI2C_RECOVER_CHECK(n)
#endif /* CONFIG_I2C_MCUX_LPI2C_BUS_RECOVERY */

#define I2C_MCUX_LPI2C_CONFIGURE_IRQ(idx, inst)	\
	IF_ENABLED(DT_INST_IRQ_HAS_IDX(inst, idx), (	\
		IRQ_CONNECT(DT_INST_IRQ_BY_IDX(inst, idx, irq),	\
			DT_INST_IRQ_BY_IDX(inst, idx, priority),	\
			mcux_lpi2c_isr,	\
			DEVICE_DT_INST_GET(inst), 0);	\
			irq_enable(DT_INST_IRQ_BY_IDX(inst, idx, irq));	\
	))

/* When using LP Flexcomm driver, register the interrupt handler
 * so we receive notification from the LP Flexcomm interrupt handler.
 */
#define I2C_MCUX_LPI2C_LPFLEXCOMM_IRQ_FUNC(n)				\
	nxp_lp_flexcomm_setirqhandler(DEVICE_DT_GET(DT_INST_PARENT(n)), \
					DEVICE_DT_INST_GET(n),		\
					LP_FLEXCOMM_PERIPH_LPI2C,	\
					mcux_lpi2c_isr)

#define I2C_MCUX_LPI2C_IRQ_SETUP_FUNC(n)				\
	COND_CODE_1(DT_NODE_HAS_COMPAT(DT_INST_PARENT(n),		\
					nxp_lp_flexcomm),		\
		    (I2C_MCUX_LPI2C_LPFLEXCOMM_IRQ_FUNC(n)),		\
		    (LISTIFY(DT_NUM_IRQS(DT_DRV_INST(n)),		\
			I2C_MCUX_LPI2C_CONFIGURE_IRQ, (), n)))

#ifdef CONFIG_I2C_MCUX_LPI2C_EDMA
#ifdef CONFIG_NOCACHE_MEMORY
#define I2C_MCUX_LPI2C_EDMA_BUF_ATTR __nocache
#else
#define I2C_MCUX_LPI2C_EDMA_BUF_ATTR
#endif
/* bufs: __nocache (DMA-touched, needs no init - written before every use).
 * wiring/state: ORDINARY static with initializers (.data - loaded at boot);
 * the nocache section is NOBITS and must never hold initialized state. */
#define I2C_MCUX_LPI2C_EDMA_DEFINE(n)					\
	IF_ENABLED(DT_INST_DMAS_HAS_NAME(n, transfer),			\
	(static I2C_MCUX_LPI2C_EDMA_BUF_ATTR __aligned(4)		\
		struct mcux_lpi2c_edma_bufs mcux_lpi2c_edma_bufs_##n;	\
	static struct mcux_lpi2c_edma mcux_lpi2c_edma_##n = {		\
		.dma_dev = DEVICE_DT_GET(				\
			DT_INST_DMAS_CTLR_BY_NAME(n, transfer)),	\
		/* nxp,mcux-edma cell names: "mux" is the CHANNEL number,
		 * "source" is the DMAMUX request (binding comment, lines
		 * 104-111 of nxp,mcux-edma.yaml) */		\
		.channel = DT_INST_DMAS_CELL_BY_NAME(n, transfer, mux), \
		.slot = DT_INST_DMAS_CELL_BY_NAME(n, transfer, source),	\
		.bufs = &mcux_lpi2c_edma_bufs_##n,			\
	};))
#define I2C_MCUX_LPI2C_EDMA_INIT(n)					\
	.edma = COND_CODE_1(DT_INST_DMAS_HAS_NAME(n, transfer),		\
			    (&mcux_lpi2c_edma_##n), (NULL)),
#else
#define I2C_MCUX_LPI2C_EDMA_DEFINE(n)
#define I2C_MCUX_LPI2C_EDMA_INIT(n)
#endif /* CONFIG_I2C_MCUX_LPI2C_EDMA */

#define I2C_MCUX_LPI2C_INIT(n)						\
	PINCTRL_DT_INST_DEFINE(n);					\
	I2C_MCUX_LPI2C_RECOVER_CHECK(n)					\
	I2C_MCUX_LPI2C_EDMA_DEFINE(n)					\
									\
	static void mcux_lpi2c_config_func_##n(const struct device *dev)\
	{								\
		I2C_MCUX_LPI2C_IRQ_SETUP_FUNC(n);			\
	}								\
									\
	static const struct mcux_lpi2c_config mcux_lpi2c_config_##n = {	\
		DEVICE_MMIO_NAMED_ROM_INIT(reg_base, DT_DRV_INST(n)),	\
		.clock_dev = DEVICE_DT_GET(DT_INST_CLOCKS_CTLR(n)),	\
		.clock_subsys = (clock_control_subsys_t)COND_CODE_1(	\
			DT_PHA_HAS_CELL(DT_DRV_INST(n), clocks, name),	\
			(DT_INST_CLOCKS_CELL(n, name)), (0U)),		\
		.irq_config_func = mcux_lpi2c_config_func_##n,		\
		.bitrate = DT_INST_PROP(n, clock_frequency),		\
		.pincfg = PINCTRL_DT_INST_DEV_CONFIG_GET(n),		\
		.reset = RESET_DT_SPEC_INST_GET_OR(n, {0}),		\
		I2C_MCUX_LPI2C_SCL_INIT(n)				\
		I2C_MCUX_LPI2C_SDA_INIT(n)				\
		I2C_MCUX_LPI2C_RECOVER_BUS_ON_INIT(n)			\
		I2C_MCUX_LPI2C_EDMA_INIT(n)				\
		.bus_idle_timeout_ns =					\
			UTIL_AND(DT_INST_NODE_HAS_PROP(n, bus_idle_timeout),\
				 DT_INST_PROP(n, bus_idle_timeout)),	\
	};								\
									\
	static struct mcux_lpi2c_data mcux_lpi2c_data_##n;		\
									\
	PM_DEVICE_DT_INST_DEFINE(n, mcux_lpi2c_pm_action);		\
									\
	I2C_DEVICE_DT_INST_DEFINE(n, mcux_lpi2c_init,			\
				PM_DEVICE_DT_INST_GET(n),		\
				&mcux_lpi2c_data_##n,			\
				&mcux_lpi2c_config_##n, POST_KERNEL,	\
				CONFIG_I2C_INIT_PRIORITY,		\
				&mcux_lpi2c_driver_api);

DT_INST_FOREACH_STATUS_OKAY(I2C_MCUX_LPI2C_INIT)
