/*
 * Copyright (c) 2024 Nuvoton Technology Corporation.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
/*132*/
#define DT_DRV_COMPAT nuvoton_npcx_i3c

#include <string.h>

#include <zephyr/device.h>
#include <zephyr/irq.h>
#include <zephyr/sys/__assert.h>
#include <zephyr/sys/sys_io.h>

#include <zephyr/drivers/clock_control.h>
#include <zephyr/drivers/i3c.h>
#include <zephyr/drivers/i3c/target_device.h>
#include <zephyr/drivers/pinctrl.h>
#include <zephyr/drivers/reset.h>

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(npcx_i3c, CONFIG_I3C_LOG_LEVEL);

/* MCONFIG register options */
#define MCONFIG_CTRENA_OFF        0x0
#define MCONFIG_CTRENA_ON         0x1
#define MCONFIG_CTRENA_CAPABLE    0x2
#define MCONFIG_HKEEP_EXT_SDA_SCL 0x3

/* MCTRL register options */
#define MCTRL_REQUEST_NONE          0 /* None */
#define MCTRL_REQUEST_EMITSTARTADDR 1 /* Emit a START */
#define MCTRL_REQUEST_EMITSTOP      2 /* Emit a STOP */
#define MCTRL_REQUEST_IBIACKNACK    3 /* Manually ACK or NACK an IBI */
#define MCTRL_REQUEST_PROCESSDAA    4 /* Starts the DAA process */
#define MCTRL_REQUEST_FORCEEXIT     6 /* Emit HDR Exit Pattern  */
#define MCTRL_REQUEST_TGT_RST       6 /* Emit Target Reset Pattern  */
/* Emits a START with address 7Eh when a slave pulls I3C_SDA low to request an IBI */
#define MCTRL_REQUEST_AUTOIBI       7

/* ACK with mandatory byte determined by IBIRULES or ACK with no mandatory byte */
#define MCTRL_IBIRESP_ACK           0
#define MCTRL_IBIRESP_NACK          1 /* NACK */
#define MCTRL_IBIRESP_ACK_MANDATORY 2 /* ACK with mandatory byte  */
#define MCTRL_IBIRESP_MANUAL        3

/* For REQUEST = EmitStartAddr */
enum npcx_i3c_mctrl_type {
	NPCX_I3C_MCTRL_TYPE_I3C,
	NPCX_I3C_MCTRL_TYPE_I2C,
	NPCX_I3C_MCTRL_TYPE_I3C_HDR_DDR,
};

/* For REQUEST = ForceExit/Target Reset */
#define MCTRL_TYPE_HDR_EXIT    0
#define MCTRL_TYPE_TGT_RESTART 2

/* MSTATUS register options */
#define MSTATUS_STATE_IDLE    0x0
#define MSTATUS_STATE_TGTREQ  0x1
#define MSTATUS_STATE_NORMACT 0x3 /* SDR message mode */
#define MSTATUS_STATE_MSGDDR  0x4
#define MSTATUS_STATE_DAA     0x5
#define MSTATUS_STATE_IBIACK  0x6
#define MSTATUS_STATE_IBIRCV  0x7
#define MSTATUS_IBITYPE_NONE  0x0
#define MSTATUS_IBITYPE_IBI   0x1
#define MSTATUS_IBITYPE_CR    0x2
#define MSTATUS_IBITYPE_HJ    0x3

/* IBIRULES register options */
#define IBIRULES_ADDR_MSK   0x3F
#define IBIRULES_ADDR_SHIFT 0x6

/* MDMACTRL register options */
#define MDMA_DMAFB_DISABLE      0x0
#define MDMA_DMAFB_EN_ONE_FRAME 0x1
#define MDMA_DMAFB_EN_MANUAL    0x2
#define MDMA_DMATB_DISABLE      0x0
#define MDMA_DMATB_EN_ONE_FRAME 0x1
#define MDMA_DMATB_EN_MANUAL    0x2

/* CONFIG register options */
#define CFG_HDRCMD_RD_FROM_FIFIO 0

/* CTRL register options */
#define CTRL_EVENT_NORMAL    0
#define CTRL_EVENT_IBI       1
#define CTRL_EVENT_CNTLR_REQ 2
#define CTRL_EVENT_HJ        3

/* STATUS register options */
#define STATUS_EVDET_NONE            0
#define STATUS_EVDET_REQ_NOT_SENT    1
#define STATUS_EVDET_REQ_SENT_NACKED 2
#define STATUS_EVDET_REQ_SENT_ACKED  3

/* Local Constants Definition */
#define NPCX_I3C_CHK_TIMEOUT_US 10000 /* Timeout for checking register status */
#define I3C_SCL_PP_FREQ_MAX_MHZ 12500000
#define I3C_SCL_OD_FREQ_MAX_MHZ 4170000

#define I3C_BUS_TLOW_PP_MIN_NS  24  /* T_LOW period in push-pull mode */
#define I3C_BUS_THigh_PP_MIN_NS 24  /* T_High period in push-pull mode */
#define I3C_BUS_TLOW_OD_MIN_NS  200 /* T_LOW period in open-drain mode */

#define PPBAUD_DIV_MAX (BIT(GET_FIELD_SZ(NPCX_I3C_MCONFIG_PPBAUD)) - 1) /* PPBAUD divider max */

#define I3C_BUS_I2C_BAUD_RATE_FAST_MODE      0x0D
#define I3C_BUS_I2C_BAUD_RATE_FAST_MODE_PLUS 0x03

#define DAA_TGT_INFO_SZ 0x8 /* 8 bytes = PID(6) + BCR(1) + DCR(1) */
#define BAMATCH_DIV     0x4 /* BAMATCH = APB4_CLK divided by four */

/* Default maximum time we allow for an I3C transfer */
#define I3C_TRANS_TIMEOUT_MS K_MSEC(100)

#define MCLKD_FREQ_MHZ(freq) MHZ(freq)

#define I3C_STATUS_CLR_MASK                                                                        \
	(BIT(NPCX_I3C_MSTATUS_MCTRLDONE) | BIT(NPCX_I3C_MSTATUS_COMPLETE) |                        \
	 BIT(NPCX_I3C_MSTATUS_IBIWON) | BIT(NPCX_I3C_MSTATUS_NOWCNTLR))

#define I3C_TGT_INTSET_MASK                                                                        \
	(BIT(NPCX_I3C_INTSET_START) | BIT(NPCX_I3C_INTSET_MATCHED) | BIT(NPCX_I3C_INTSET_STOP) |   \
	 BIT(NPCX_I3C_INTSET_DACHG) | BIT(NPCX_I3C_INTSET_CCC) | BIT(NPCX_I3C_INTSET_ERRWARN) |    \
	 BIT(NPCX_I3C_INTSET_HDRMATCH) | BIT(NPCX_I3C_INTSET_CHANDLED) |                           \
	 BIT(NPCX_I3C_INTSET_EVENT))

#define HDR_DDR_CMD_AND_CRC_SZ_WORD 0x2 /* 2 words =  Command(1 word) + CRC(1 word) */
#define HDR_RD_CMD                  0x80

/* I3C moudle and port parsing from instance_id */
#define GET_MODULE_ID(inst_id) ((inst_id & 0xf0) >> 4)
#define GET_PORT_ID(inst_id)   (inst_id & 0xf)

/* I3C target PID parsing */
#define GET_PID_VENDOR_ID(pid) (((uint64_t)pid >> 33) & 0x7fff) /* PID[47:33] */
#define GET_PID_ID_TYP(pid)    (((uint64_t)pid >> 32) & 0x1)    /* PID[32] */
#define GET_PID_PARTNO(pid)    (pid & 0xffffffff)               /* PID[31:0] */

#define I3C_TGT_WR_REQ_WAIT_US 10 /* I3C target write request MDMA completion after stop */

/* Supported I3C MCLKD frequency */
enum npcx_i3c_speed {
	NPCX_I3C_BUS_SPEED_40MHZ,
	NPCX_I3C_BUS_SPEED_45MHZ,
	NPCX_I3C_BUS_SPEED_48MHZ,
	NPCX_I3C_BUS_SPEED_50MHZ,
};

/* Operation type */
enum npcx_i3c_oper_state {
	NPCX_I3C_OP_STATE_IDLE,
	NPCX_I3C_OP_STATE_WR,
	NPCX_I3C_OP_STATE_RD,
	NPCX_I3C_OP_STATE_IBI,
	NPCX_I3C_OP_STATE_MAX,
};

/* I3C timing configuration for each i3c speed */
struct npcx_i3c_timing_cfg {
	uint8_t ppbaud; /* Push-Pull high period */
	uint8_t pplow;  /* Push-Pull low period */
	uint8_t odhpp;  /* Open-Drain high period */
	uint8_t odbaud; /* Open-Drain low period */
};

/* Recommended I3C timing values are based on different MCLKD frequency */
static const struct npcx_i3c_timing_cfg npcx_def_speed_cfg[] = {
	/* PP = 12.5 mhz, OD = 4.17 Mhz */
	[NPCX_I3C_BUS_SPEED_40MHZ] = {.ppbaud = 1, .pplow = 0, .odhpp = 1, .odbaud = 3},
	[NPCX_I3C_BUS_SPEED_45MHZ] = {.ppbaud = 1, .pplow = 0, .odhpp = 1, .odbaud = 4},
	[NPCX_I3C_BUS_SPEED_48MHZ] = {.ppbaud = 1, .pplow = 0, .odhpp = 1, .odbaud = 4},
	[NPCX_I3C_BUS_SPEED_50MHZ] = {.ppbaud = 1, .pplow = 0, .odhpp = 1, .odbaud = 4},
};

struct npcx_i3c_config {
	/* Common I3C Driver Config */
	struct i3c_driver_config common;

	/* Pointer to controller registers. */
	struct i3c_reg *base;

	/* Pointer to the clock device. */
	const struct device *clock_dev;

	/* Reset controller */
	struct reset_dt_spec reset;

	/* Clock control subsys related struct. */
	struct npcx_clk_cfg clock_subsys;

	/* Reference clock to determine 1 μs bus available time */
	struct npcx_clk_cfg ref_clk_subsys;

	/* Pointer to pin control device. */
	const struct pinctrl_dev_config *pincfg;

	/* Interrupt configuration function. */
	void (*irq_config_func)(const struct device *dev);

	uint8_t instance_id; /* bit[8:4] module id, bit[3:0] port id */

	/* I3C clock frequency configuration */
	struct {
		uint32_t i3c_pp_scl_hz; /* I3C push pull clock frequency in Hz. */
		uint32_t i3c_od_scl_hz; /* I3C open drain clock frequency in Hz. */
	} clocks;

#ifdef CONFIG_I3C_NPCX_DMA
	struct npcx_clk_cfg mdma_clk_subsys;
	struct mdma_reg *mdma_base;
#endif
};

struct npcx_i3c_data {
	/* Controller data */
	struct i3c_driver_data common; /* Common i3c driver data */
	struct k_mutex lock_mutex;     /* Mutex of i3c controller */
	struct k_sem sync_sem;         /* Semaphore used for synchronization */
	struct k_sem ibi_lock_sem;     /* Semaphore used for ibi */

	/* Target data */
	struct i3c_target_config *target_config;
	/* Configuration parameters for I3C hardware to act as target device */
	struct i3c_config_target config_target;
	struct k_sem target_lock_sem;       /* Semaphore used for i3c target */
	struct k_sem target_event_lock_sem; /* Semaphore used for i3c target ibi_raise() */

	enum npcx_i3c_oper_state oper_state; /* Operation state */

#ifdef CONFIG_I3C_NPCX_DMA
	uint8_t mdma_rx_buf[4096];
#endif /* End of  CONFIG_I3C_NPCX_DMA */

#ifdef CONFIG_I3C_USE_IBI
	struct {
		/* List of addresses used in the MIBIRULES register. */
		uint8_t addr[5];

		/* Number of valid addresses in MIBIRULES. */
		uint8_t num_addr;

		/* True if all addresses have MSB set. */
		bool msb;

		/*
		 * True if all target devices require mandatory byte
		 * for IBI.
		 */
		bool has_mandatory_byte;
	} ibi;
#endif
};

static void npcx_i3c_mutex_lock(const struct device *dev)
{
	struct npcx_i3c_data *const data = dev->data;

	k_mutex_lock(&data->lock_mutex, K_FOREVER);
}

static void npcx_i3c_mutex_unlock(const struct device *dev)
{
	struct npcx_i3c_data *const data = dev->data;

	k_mutex_unlock(&data->lock_mutex);
}

#ifdef CONFIG_I3C_NPCX_DMA
static void i3c_ctrl_notify(const struct device *dev)
{
	struct npcx_i3c_data *const data = dev->data;

	k_sem_give(&data->sync_sem);
}

static int i3c_ctrl_wait_completion(const struct device *dev)
{
	struct npcx_i3c_data *const data = dev->data;

	return k_sem_take(&data->sync_sem, I3C_TRANS_TIMEOUT_MS);
}

static enum npcx_i3c_oper_state get_oper_state(const struct device *dev)
{
	struct npcx_i3c_data *const data = dev->data;

	return data->oper_state;
}
#endif /* CONFIG_I3C_NPCX_DMA */

static void set_oper_state(const struct device *dev, enum npcx_i3c_oper_state state)
{
	struct npcx_i3c_data *const data = dev->data;

	data->oper_state = state;
}

static uint8_t get_bus_available_match_val(uint32_t apb4_freq)
{
	uint8_t bamatch;

	bamatch = DIV_ROUND_UP(apb4_freq, MHZ(1));
	/* The clock of this counter is APB4_CLK divided by four */
	bamatch = DIV_ROUND_UP(bamatch, BAMATCH_DIV);

	return bamatch;
}

/*
 * brief:  Wait for status bit done and clear the status
 *
 * param[in] inst  Pointer to I3C register.
 *
 * return  0, success
 *         -ETIMEDOUT: check status timeout.
 */
static inline int npcx_i3c_status_wait_clear(struct i3c_reg *inst, uint8_t bit_offset)
{
	if (WAIT_FOR(IS_BIT_SET(inst->MSTATUS, bit_offset), NPCX_I3C_CHK_TIMEOUT_US, NULL) ==
	    false) {
		return -ETIMEDOUT;
	}

	inst->MSTATUS = BIT(bit_offset); /* W1C */

	return 0;
}

static inline uint32_t npcx_i3c_state_get(struct i3c_reg *inst)
{
	return GET_FIELD(inst->MSTATUS, NPCX_I3C_MSTATUS_STATE);
}

static inline void npcx_i3c_interrupt_all_disable(struct i3c_reg *inst)
{
	uint32_t intmask = inst->MINTSET;

	inst->MINTCLR = intmask;
}

static inline void npcx_i3c_interrupt_enable(struct i3c_reg *inst, uint32_t mask)
{
	inst->MINTSET = mask;
}

static void npcx_i3c_enable_target_interrupt(const struct device *dev, bool enable)
{
	const struct npcx_i3c_config *config = dev->config;
	struct i3c_reg *inst = config->base;

	/* Disable the target interrupt events */
	inst->INTCLR = inst->INTSET;

	/* Clear the target interrupt status */
	inst->STATUS = inst->STATUS;

	/* Enable the target interrupt events */
	if (enable) {
		inst->INTSET = I3C_TGT_INTSET_MASK;
		inst->MINTSET |= BIT(NPCX_I3C_MINTSET_NOWCNTLR); /* I3C target is now controller */

#ifndef CONFIG_I3C_NPCX_DMA
		/* Receive buffer pending (FIFO mode) */
		inst->INTSET |= BIT(NPCX_I3C_INTSET_RXPEND);
#endif
	}
}

static bool npcx_i3c_has_error(struct i3c_reg *inst)
{
	if (IS_BIT_SET(inst->MSTATUS, NPCX_I3C_MSTATUS_ERRWARN)) {
		LOG_ERR("ERROR: MSTATUS 0x%08x MERRWARN 0x%08x", inst->MSTATUS, inst->MERRWARN);

		return true;
	}

	return false;
}

static inline void npcx_i3c_status_clear_all(struct i3c_reg *inst)
{
	uint32_t mask = I3C_STATUS_CLR_MASK;

	inst->MSTATUS = mask;
}

static inline void npcx_i3c_errwarn_clear_all(struct i3c_reg *inst)
{
	inst->MERRWARN = inst->MERRWARN;
}

static inline void npcx_i3c_fifo_flush(struct i3c_reg *inst)
{
	inst->MDATACTRL |= (BIT(NPCX_I3C_MDATACTRL_FLUSHTB) | BIT(NPCX_I3C_MDATACTRL_FLUSHFB));
}

/*
 * brief:  Send request and check the request is valid
 *
 * param[in] inst  Pointer to I3C register.
 *
 * return  0, success
 *         -ETIMEDOUT check MCTRLDONE timeout.
 *         -ENOSYS    invalid use of request.
 */
static inline int npcx_i3c_send_request(struct i3c_reg *inst, uint32_t mctrl_val)
{
	inst->MCTRL = mctrl_val;

	if (npcx_i3c_status_wait_clear(inst, NPCX_I3C_MSTATUS_MCTRLDONE) != 0) {
		return -ETIMEDOUT;
	}

	/* Check invalid use of request */
	if (IS_BIT_SET(inst->MERRWARN, NPCX_I3C_MERRWARN_INVERQ)) {
		LOG_ERR("%s: Invalid request, merrwarn: %#x", __func__, inst->MERRWARN);
		return -ENOSYS;
	}

	return 0;
}

/* Start DAA procedure and continue the DAA with a Repeated START */
static inline int npcx_i3c_request_daa(struct i3c_reg *inst)
{
	uint32_t val = 0;
	int ret;

	/* Set IBI response NACK while processing DAA */
	SET_FIELD(val, NPCX_I3C_MCTRL_IBIRESP, MCTRL_IBIRESP_NACK);

	/* Send DAA request */
	SET_FIELD(val, NPCX_I3C_MCTRL_REQUEST, MCTRL_REQUEST_PROCESSDAA);

	ret = npcx_i3c_send_request(inst, val);
	if (ret != 0) {
		LOG_ERR("Request DAA error, %d", ret);
		return ret;
	}

	return 0;
}

/* Tell controller to start auto IBI */
static inline int npcx_i3c_request_auto_ibi(struct i3c_reg *inst)
{
	uint32_t val = 0;
	int ret;

	SET_FIELD(val, NPCX_I3C_MCTRL_IBIRESP, MCTRL_IBIRESP_ACK);
	SET_FIELD(val, NPCX_I3C_MCTRL_REQUEST, MCTRL_REQUEST_AUTOIBI);

	ret = npcx_i3c_send_request(inst, val);
	if (ret != 0) {
		LOG_ERR("Request auto ibi error, %d", ret);
		return ret;
	}

	return 0;
}

/*
 * brief:  Controller emit start and send address
 *
 * param[in] inst     Pointer to I3C register.
 * param[in] addr     Dyamic address for xfer or 0x7E for CCC command.
 * param[in] op_type  Request type.
 * param[in] is_read  Read(true) or write(false) operation.
 * param[in] read_sz  Read size in bytes.
 *                    If op_tye is HDR-DDR, the read_sz must be the number of words.
 *
 * return  0, success
 *         else, error
 */
static int npcx_i3c_request_emit_start(struct i3c_reg *inst, uint8_t addr,
				       enum npcx_i3c_mctrl_type op_type, bool is_read,
				       size_t read_sz)
{
	uint32_t mctrl = 0;
	int ret;

	/* Set request and target address*/
	SET_FIELD(mctrl, NPCX_I3C_MCTRL_REQUEST, MCTRL_REQUEST_EMITSTARTADDR);

	/* Set operation type */
	SET_FIELD(mctrl, NPCX_I3C_MCTRL_TYPE, op_type);

	/* Set IBI response NACK in emit start */
	SET_FIELD(mctrl, NPCX_I3C_MCTRL_IBIRESP, MCTRL_IBIRESP_NACK);

	/* Set dynamic address */
	SET_FIELD(mctrl, NPCX_I3C_MCTRL_ADDR, addr);

	/* Set read(1) or write(0) */
	if (is_read) {
		mctrl |= BIT(NPCX_I3C_MCTRL_DIR);
		SET_FIELD(mctrl, NPCX_I3C_MCTRL_RDTERM, read_sz); /* Set read length */
	} else {
		mctrl &= ~BIT(NPCX_I3C_MCTRL_DIR);
	}

	ret = npcx_i3c_send_request(inst, mctrl);
	if (ret != 0) {
		LOG_ERR("Request start error, %d", ret);
		return ret;
	}

	/* Check NACK after MCTRLDONE is get */
	if (IS_BIT_SET(inst->MERRWARN, NPCX_I3C_MERRWARN_NACK)) {
		LOG_DBG("Address nacked");
		return -ENODEV;
	}

	return 0;
}

/*
 * brief:  Controller emit STOP.
 *
 * This emits STOP when controller is in NORMACT state.
 *
 * param[in] inst  Pointer to I3C register.
 *
 * return  0 success
 *         -ECANCELED i3c state not as expected.
 *         -ETIMEDOUT check MCTRLDONE timeout.
 *         -ENOSYS    invalid use of request.
 */
static inline int npcx_i3c_request_emit_stop(struct i3c_reg *inst)
{
	uint32_t val = 0;
	int ret;
	uint32_t i3c_state = npcx_i3c_state_get(inst);

	/* Make sure we are in a state where we can emit STOP */
	if (i3c_state == MSTATUS_STATE_IDLE) {
		LOG_WRN("Request stop in idle state, state= %#x", i3c_state);
		return -ECANCELED;
	}

	SET_FIELD(val, NPCX_I3C_MCTRL_REQUEST, MCTRL_REQUEST_EMITSTOP);

	ret = npcx_i3c_send_request(inst, val);
	if (ret != 0) {
		LOG_ERR("Request stop error, %d", ret);
		return ret;
	}

	return 0;
}

static inline int npcx_i3c_request_hdr_exit(struct i3c_reg *inst)
{
	uint32_t val = 0;
	uint32_t state;
	int ret;

	/* Before sending the HDR exit command, check the HDR mode */
	state = npcx_i3c_state_get(inst);
	if (state != MSTATUS_STATE_MSGDDR) {
		LOG_ERR("%s, state error: %#x", __func__, state);
		return -EPERM;
	}

	SET_FIELD(val, NPCX_I3C_MCTRL_TYPE, MCTRL_TYPE_HDR_EXIT);
	SET_FIELD(val, NPCX_I3C_MCTRL_REQUEST, MCTRL_REQUEST_FORCEEXIT);

	ret = npcx_i3c_send_request(inst, val);
	if (ret != 0) {
		LOG_ERR("Request hdr exit error %d", ret);
		return ret;
	}

	return 0;
}

static inline int npcx_i3c_request_tgt_reset(struct i3c_reg *inst)
{
	uint32_t val = 0;
	int ret;

	LOG_WRN("Send target reset pattern");
	SET_FIELD(val, NPCX_I3C_MCTRL_TYPE, MCTRL_TYPE_TGT_RESTART);
	SET_FIELD(val, NPCX_I3C_MCTRL_REQUEST, MCTRL_REQUEST_TGT_RST);

	ret = npcx_i3c_send_request(inst, val);
	if (ret != 0) {
		LOG_ERR("Sending tgt reset pattern error %d", ret);
		return ret;
	}

	return 0;
}

static inline int npcx_i3c_xfer_stop(struct i3c_reg *inst)
{
	uint32_t state;
	int ret;

	state = npcx_i3c_state_get(inst);
	LOG_DBG("Current working state=%d", state);

	switch (state) {
	case MSTATUS_STATE_NORMACT: /* SDR */
		ret = npcx_i3c_request_emit_stop(inst);
		break;
	case MSTATUS_STATE_MSGDDR: /* HDR-DDR */
		ret = npcx_i3c_request_hdr_exit(inst);
		break;
	default:
		/* Not supported */
		ret = -ENOTSUP;
		LOG_WRN("xfer_stop state not supported, state:%d", state);
		break;
	}

	return ret;
}

static inline int npcx_i3c_ibi_respond_nack(struct i3c_reg *inst)
{
	uint32_t val = 0;
	int ret;

	SET_FIELD(val, NPCX_I3C_MCTRL_IBIRESP, MCTRL_IBIRESP_NACK);
	SET_FIELD(val, NPCX_I3C_MCTRL_REQUEST, MCTRL_REQUEST_IBIACKNACK);

	ret = npcx_i3c_send_request(inst, val);
	if (ret != 0) {
		LOG_ERR("Request ibi_rsp nack error, %d", ret);
		return ret;
	}

	return 0;
}

static inline int npcx_i3c_ibi_respond_ack(struct i3c_reg *inst)
{
	uint32_t val = 0;
	int ret;

	SET_FIELD(val, NPCX_I3C_MCTRL_IBIRESP, MCTRL_IBIRESP_ACK);
	SET_FIELD(val, NPCX_I3C_MCTRL_REQUEST, MCTRL_REQUEST_IBIACKNACK);

	ret = npcx_i3c_send_request(inst, val);
	if (ret != 0) {
		LOG_ERR("Request ibi_rsp ack error %d", ret);
		return ret;
	}

	return 0;
}

/*
 * brief:  Find a registered I3C target device.
 *
 * This returns the I3C device descriptor of the I3C device
 * matching the incoming id.
 *
 * param[in] dev  Pointer to controller device driver instance.
 * param[in] id   Pointer to I3C device ID.
 *
 * return  see i3c_device_find.
 */
static inline struct i3c_device_desc *npcx_i3c_device_find(const struct device *dev,
							   const struct i3c_device_id *id)
{
	const struct npcx_i3c_config *config = dev->config;

	return i3c_dev_list_find(&config->common.dev_list, id);
}

/*
 * brief:  Perform bus recovery.
 *
 * param[in] dev  Pointer to controller device driver instance.
 *
 * return 0 success, otherwise error
 */
static int npcx_i3c_recover_bus(const struct device *dev)
{
	const struct npcx_i3c_config *config = dev->config;
	struct i3c_reg *inst = config->base;

	/*
	 * If the controller is in NORMACT state, tells it to emit STOP
	 * so it can return to IDLE, or is ready to clear any pending
	 * target initiated IBIs.
	 */
	if (npcx_i3c_state_get(inst) == MSTATUS_STATE_NORMACT) {
		npcx_i3c_request_emit_stop(inst);
	};

	/* Exhaust all target initiated IBI */
	while (IS_BIT_SET(inst->MSTATUS, NPCX_I3C_MSTATUS_TGTSTART)) {
		inst->MSTATUS = BIT(NPCX_I3C_MSTATUS_TGTSTART); /* W1C */

		/* Tell the controller to perform auto IBI. */
		npcx_i3c_request_auto_ibi(inst);

		if (WAIT_FOR(IS_BIT_SET(inst->MSTATUS, NPCX_I3C_MSTATUS_COMPLETE),
			     NPCX_I3C_CHK_TIMEOUT_US, NULL) == false) {
			break;
		}

		/* Once auto IBI is done, discard bytes in FIFO. */
		while (IS_BIT_SET(inst->MSTATUS, NPCX_I3C_MSTATUS_RXPEND)) {
			/* Flush FIFO as long as RXPEND is set. */
			npcx_i3c_fifo_flush(inst);
		}

		/*
		 * There might be other IBIs waiting.
		 * So pause a bit to let other targets initiates
		 * their IBIs.
		 */
		k_busy_wait(100);
	}

	/* Check IDLE state */
	if (WAIT_FOR((npcx_i3c_state_get(inst) == MSTATUS_STATE_IDLE), NPCX_I3C_CHK_TIMEOUT_US,
		     NULL) == false) {
		return -EBUSY;
	}

	return 0;
}

static inline void npcx_i3c_xfer_reset(struct i3c_reg *inst)
{
	npcx_i3c_status_clear_all(inst);
	npcx_i3c_errwarn_clear_all(inst);
	npcx_i3c_fifo_flush(inst);
}

/*
 * brief:  Perform one write transaction.
 *
 * This writes all data in buf to TX FIFO or time out
 * waiting for FIFO spaces.
 *
 * param[in] inst       Pointer to controller registers.
 * param[in] buf        Buffer containing data to be sent.
 * param[in] buf_sz     Number of bytes in buf to send.
 * param[in] no_ending  True, not including ending byte in message.
 *                       False, including ending byte in message
 *
 * return  Number of bytes written, or negative if error.
 *
 */
static int npcx_i3c_xfer_write_fifo(struct i3c_reg *inst, uint8_t *buf, uint8_t buf_sz,
				    bool no_ending)
{
	int offset = 0;
	int remaining = buf_sz;

	while (remaining > 0) {
		/* Check tx fifo not full */
		if (WAIT_FOR(!IS_BIT_SET(inst->MDATACTRL, NPCX_I3C_MDATACTRL_TXFULL),
			     NPCX_I3C_CHK_TIMEOUT_US, NULL) == false) {
			LOG_DBG("Check tx fifo not full timed out");
			return -ETIMEDOUT;
		}

		if ((remaining > 1) || no_ending) {
			inst->MWDATAB = (uint32_t)buf[offset];
		} else {
			inst->MWDATABE = (uint32_t)buf[offset]; /* Set last byte */
		}

		offset += 1;
		remaining -= 1;
	}

	return offset;
}

/*
 * brief:  Perform read transaction.
 *
 * This reads from RX FIFO until COMPLETE bit is set in MSTATUS
 * or time out.
 *
 * param[in] inst    Pointer to controller registers.
 * param[in] buf     Buffer to store data.
 * param[in] buf_sz  Number of bytes to read.
 *
 * return  Number of bytes read, or negative if error.
 *
 */
static int npcx_i3c_xfer_read_fifo(struct i3c_reg *inst, uint8_t *buf, uint8_t rd_sz)
{
	bool is_done = false;
	int offset = 0;

	while (is_done == false) {
		/* Check message is terminated */
		if (IS_BIT_SET(inst->MSTATUS, NPCX_I3C_MSTATUS_COMPLETE)) {
			is_done = true;
		}

		/* Check I3C bus error */
		if (npcx_i3c_has_error(inst)) {
			/* Check timeout*/
			if (IS_BIT_SET(inst->MERRWARN, NPCX_I3C_MERRWARN_TIMEOUT)) {
				LOG_WRN("%s: ERR: timeout", __func__);
			}

			inst->MERRWARN = inst->MERRWARN;

			return -EIO;
		}

		/* Check rx not empty */
		if (IS_BIT_SET(inst->MSTATUS, NPCX_I3C_MSTATUS_RXPEND)) {

			/* Receive all the data in this round.
			 * Read in a tight loop to reduce chance of losing
			 * FIFO data when the i3c speed is high.
			 */
			while (offset < rd_sz) {
				if (GET_FIELD(inst->MDATACTRL, NPCX_I3C_MDATACTRL_RXCOUNT) == 0) {
					break;
				}

				buf[offset++] = (uint8_t)inst->MRDATAB;
			}
		}
	}

	return offset;
}

#ifdef CONFIG_I3C_NPCX_DMA
/*
 * brief:  Perform DMA write transaction.
 *
 * For write end, use the interrupt generated by COMPLETE bit in MSTATUS register.
 *
 * param[in] dev     Pointer to controller device driver instance.
 * param[in] buf     Buffer to store data.
 * param[in] buf_sz  Number of bytes to read.
 *
 * return  Number of bytes read, or negative if error.
 *
 */
static int npcx_i3c_xfer_write_fifo_dma(const struct device *dev, uint8_t *buf, uint32_t buf_sz)
{
	const struct npcx_i3c_config *config = dev->config;
	struct i3c_reg *i3c_inst = config->base;
	struct mdma_reg *mdma_inst = config->mdma_base;
	int ret;

	set_oper_state(dev, NPCX_I3C_OP_STATE_WR);

	/* Enable I3C MDMA write for one frame */
	SET_FIELD(i3c_inst->MDMACTRL, NPCX_I3C_MDMACTRL_DMATB, MDMA_DMATB_EN_ONE_FRAME);
	i3c_inst->MINTSET |= BIT(NPCX_I3C_MINTCLR_COMPLETE); /* Enable I3C complete interrupt */

	/* Write Operation (MDMA CH_1) */
	mdma_inst->MDMA_TCNT1 = buf_sz;                    /* Set MDMA transfer count */
	mdma_inst->MDMA_SRCB1 = (uint32_t)buf;             /* Set source address */
	mdma_inst->MDMA_CTL1 |= BIT(NPCX_MDMA_CTL_MDMAEN); /* Start DMA transfer */

	/* Wait I3C COMPLETE */
	ret = i3c_ctrl_wait_completion(dev);
	if (ret < 0) {
		LOG_DBG("Check complete time out, buf_size:%d", buf_sz);
		goto out_wr_fifo_dma;
	}

	/* Check and clear DMA TC after complete */
	if (!IS_BIT_SET(mdma_inst->MDMA_CTL1, NPCX_MDMA_CTL_TC)) {
		LOG_DBG("DMA busy, TC=%d", IS_BIT_SET(mdma_inst->MDMA_CTL1, NPCX_MDMA_CTL_TC));
		ret = -EBUSY;
		goto out_wr_fifo_dma;
	}

	mdma_inst->MDMA_CTL1 &= ~BIT(NPCX_MDMA_CTL_TC); /* Clear TC, W0C */
	ret = buf_sz - mdma_inst->MDMA_CTCNT1;          /* Set transferred count */
	LOG_DBG("Write cnt=%d", ret);

out_wr_fifo_dma:
	i3c_inst->MINTCLR |= BIT(NPCX_I3C_MINTCLR_COMPLETE); /* Disable I3C complete interrupt */
	npcx_i3c_fifo_flush(i3c_inst);
	set_oper_state(dev, NPCX_I3C_OP_STATE_IDLE);

	return ret;
}

/*
 * brief:  Perform DMA read transaction.
 *         (Data width used for DMA transfers is "byte")
 *
 * For read end, use the MDMA end-of-transfer interrupt(SIEN bit)
 * instead of using the I3CI interrupt generated by COMPLETE bit in MSTATUS register.
 *
 * param[in] dev     Pointer to controller device driver instance.
 * param[in] buf     Buffer to store data.
 * param[in] buf_sz  Number of bytes to read.
 *
 * return  Number of bytes read, or negative if error.
 *
 */
static int npcx_i3c_xfer_read_fifo_dma(const struct device *dev, uint8_t *buf, uint32_t buf_sz)
{
	const struct npcx_i3c_config *config = dev->config;
	struct i3c_reg *i3c_inst = config->base;
	struct mdma_reg *mdma_inst = config->mdma_base;
	int ret;

	set_oper_state(dev, NPCX_I3C_OP_STATE_RD);

	/* Enable DMA until DMA is disabled by setting DMAFB to 00 */
	SET_FIELD(i3c_inst->MDMACTRL, NPCX_I3C_MDMACTRL_DMAFB, MDMA_DMAFB_EN_MANUAL);

	/* Read Operation (MDMA CH_0) */
	mdma_inst->MDMA_TCNT0 = buf_sz;                    /* Set MDMA transfer count */
	mdma_inst->MDMA_DSTB0 = (uint32_t)buf;             /* Set destination address */
	mdma_inst->MDMA_CTL0 |= BIT(NPCX_MDMA_CTL_SIEN);   /* Enable stop interrupt */
	mdma_inst->MDMA_CTL0 |= BIT(NPCX_MDMA_CTL_MDMAEN); /* Start DMA transfer */

	/* Wait MDMA TC */
	ret = i3c_ctrl_wait_completion(dev);
	if (ret < 0) {
		LOG_DBG("Check DMA done time out");
	} else {
		ret = buf_sz - mdma_inst->MDMA_CTCNT0; /* Set transferred count */
		LOG_DBG("Read cnt=%d", ret);
	}

	mdma_inst->MDMA_CTL0 &= ~BIT(NPCX_MDMA_CTL_SIEN); /* Disable stop interrupt */
	/* Disable I3C MDMA read */
	SET_FIELD(i3c_inst->MDMACTRL, NPCX_I3C_MDMACTRL_DMAFB, MDMA_DMAFB_DISABLE);
	npcx_i3c_fifo_flush(i3c_inst);
	set_oper_state(dev, NPCX_I3C_OP_STATE_IDLE);

	return ret;
}

/*
 * brief:  Perform one transfer transaction by DMA.
 *         (Support SDR and HDR-DDR)
 *
 * param[in] inst        Pointer to controller registers.
 * param[in] addr        Target address.
 * param[in] op_type     Request type.
 * param[in] buf         Buffer for data to be sent or received.
 * param[in] buf_sz      Buffer size in bytes.
 * param[in] is_read     True if this is a read transaction, false if write.
 * param[in] emit_start  True if START is needed before read/write.
 * param[in] emit_stop   True if STOP is needed after read/write.
 *
 * return  Number of bytes read/written, or negative if error.
 */
static int npcx_i3c_do_one_xfer_dma(const struct device *dev, uint8_t addr,
				    enum npcx_i3c_mctrl_type op_type, uint8_t *buf, size_t buf_sz,
				    bool is_read, bool emit_start, bool emit_stop, uint8_t hdr_cmd)
{
	const struct npcx_i3c_config *config = dev->config;
	struct i3c_reg *inst = config->base;
	int ret = 0;
	bool is_hdr_ddr = (op_type == NPCX_I3C_MCTRL_TYPE_I3C_HDR_DDR) ? true : false;
	size_t rd_len = buf_sz;

	npcx_i3c_status_clear_all(inst);
	npcx_i3c_errwarn_clear_all(inst);

	/* Check HDR-DDR moves data by words */
	if (is_hdr_ddr && (buf_sz % 2 != 0)) {
		LOG_ERR("%s, HDR-DDR data length should be even, len=%#x", __func__, buf_sz);
		return -EINVAL;
	}

	/* Emit START if needed */
	if (emit_start) {
		/*
		 * For HDR-DDR mode read, RDTERM also includes one word (16 bits) for CRC.
		 * For example, to read 8 bytes, set RDTERM to 6.
		 * (1 word HDR-DDR command + 4 words data + 1 word for CRC)
		 */
		if (is_hdr_ddr) {
			if (is_read) {
				/* The unit of rd_len is "word" in DDR mode */
				rd_len /= sizeof(uint16_t); /* Byte to word */
				rd_len += HDR_DDR_CMD_AND_CRC_SZ_WORD;
				hdr_cmd |= HDR_RD_CMD;
			} else {
				hdr_cmd &= ~HDR_RD_CMD;
			}

			/* Write the command code for the HDR-DDR message */
			inst->MWDATAB = hdr_cmd;
		}

		ret = npcx_i3c_request_emit_start(inst, addr, op_type, is_read, rd_len);
		if (ret != 0) {
			LOG_ERR("%s: emit start fail", __func__);
			goto out_do_one_xfer_dma;
		}
	}

	/* No data to be transferred */
	if ((buf == NULL) || (buf_sz == 0)) {
		goto out_do_one_xfer_dma;
	}

	/* Select read or write operation */
	if (is_read) {
		ret = npcx_i3c_xfer_read_fifo_dma(dev, buf, buf_sz);
	} else {
		ret = npcx_i3c_xfer_write_fifo_dma(dev, buf, buf_sz);
	}

	if (ret < 0) {
		LOG_ERR("%s: %s fifo fail", __func__, is_read ? "read" : "write");
		goto out_do_one_xfer_dma;
	}

	/* Check I3C bus error */
	if (npcx_i3c_has_error(inst)) {
		ret = -EIO;
		LOG_ERR("%s: I3C bus error", __func__);
	}

out_do_one_xfer_dma:
	/* Emit STOP or exit DDR if needed */
	if (emit_stop) {
		npcx_i3c_xfer_stop(inst);
	}

	return ret;
}
#endif /* End of CONFIG_I3C_NPCX_DMA */

/*
 * brief:  Perform one transfer transaction.
 *         (Support SDR only)
 *
 * param[in] inst        Pointer to controller registers.
 * param[in] addr        Target address.
 * param[in] op_type     Request type.
 * param[in] buf         Buffer for data to be sent or received.
 * param[in] buf_sz      Buffer size in bytes.
 * param[in] is_read     True if this is a read transaction, false if write.
 * param[in] emit_start  True if START is needed before read/write.
 * param[in] emit_stop   True if STOP is needed after read/write.
 * param[in] no_ending   True if not to signal end of write message.
 *
 * return  Number of bytes read/written, or negative if error.
 */
static int npcx_i3c_do_one_xfer(struct i3c_reg *inst, uint8_t addr,
				enum npcx_i3c_mctrl_type op_type, uint8_t *buf, size_t buf_sz,
				bool is_read, bool emit_start, bool emit_stop, bool no_ending)
{
	int ret = 0;

	npcx_i3c_status_clear_all(inst);
	npcx_i3c_errwarn_clear_all(inst);

	/* Emit START if needed */
	if (emit_start) {
		ret = npcx_i3c_request_emit_start(inst, addr, op_type, is_read, buf_sz);
		if (ret != 0) {
			LOG_ERR("%s: emit start fail", __func__);
			goto out_do_one_xfer;
		}
	}

	/* No data to be transferred */
	if ((buf == NULL) || (buf_sz == 0)) {
		goto out_do_one_xfer;
	}

	/* Select read or write operation */
	if (is_read) {
		ret = npcx_i3c_xfer_read_fifo(inst, buf, buf_sz);
	} else {
		ret = npcx_i3c_xfer_write_fifo(inst, buf, buf_sz, no_ending);
	}

	if (ret < 0) {
		LOG_ERR("%s: %s fifo fail", __func__, is_read ? "read" : "write");
		goto out_do_one_xfer;
	}

	/* Check message complete if is a read transaction or
	 * ending byte of a write transaction.
	 */
	if (is_read || !no_ending) {
		/* Wait message transfer complete */
		if (WAIT_FOR(IS_BIT_SET(inst->MSTATUS, NPCX_I3C_MSTATUS_COMPLETE),
			     NPCX_I3C_CHK_TIMEOUT_US, NULL) == false) {
			LOG_DBG("Wait COMPLETE timed out, addr 0x%02x, buf_sz %u", addr, buf_sz);

			ret = -ETIMEDOUT;
			emit_stop = true;

			goto out_do_one_xfer;
		}

		inst->MSTATUS = BIT(NPCX_I3C_MSTATUS_COMPLETE); /* W1C */
	}

	/* Check I3C bus error */
	if (npcx_i3c_has_error(inst)) {
		ret = -EIO;
		LOG_ERR("%s: I3C bus error", __func__);
	}

out_do_one_xfer:
	/* Emit STOP if needed */
	if (emit_stop) {
		npcx_i3c_request_emit_stop(inst);
	}

	return ret;
}

/*
 * brief:  Transfer messages in I3C mode.
 *
 * see i3c_transfer
 *
 * param[in] dev       Pointer to device driver instance.
 * param[in] target    Pointer to target device descriptor.
 * param[in] msgs      Pointer to I3C messages.
 * param[in] num_msgs  Number of messages to transfers.
 *
 * return  see i3c_transfer
 */
static int npcx_i3c_transfer(const struct device *dev, struct i3c_device_desc *target,
			     struct i3c_msg *msgs, uint8_t num_msgs)
{
	const struct npcx_i3c_config *config = dev->config;
	struct i3c_reg *inst = config->base;
	struct npcx_i3c_data *data = dev->data;
	uint32_t intmask;
	int xfered_len = 0;
	int ret = 0;
	bool send_broadcast = true;
	bool is_xfer_done = true;
	enum npcx_i3c_mctrl_type op_type;

	if (msgs == NULL) {
		return -EINVAL;
	}

	if (target->dynamic_addr == 0U) {
		return -EINVAL;
	}

	npcx_i3c_mutex_lock(dev);

	/* Check bus in idle state */
	if (WAIT_FOR((npcx_i3c_state_get(inst) == MSTATUS_STATE_IDLE), NPCX_I3C_CHK_TIMEOUT_US,
		     NULL) == false) {
		LOG_ERR("%s: xfer state error: %d", __func__, npcx_i3c_state_get(inst));
		npcx_i3c_mutex_unlock(dev);
		return -ETIMEDOUT;
	}

	/* Disable interrupt */
	intmask = inst->MINTSET;
	npcx_i3c_interrupt_all_disable(inst);

	npcx_i3c_xfer_reset(inst);

	/* Iterate over all the messages */
	for (int i = 0; i < num_msgs; i++) {
		/*
		 * Check message is read or write operaion.
		 * For write operation, check the last data byte of a transmit message.
		 */
		bool is_read = (msgs[i].flags & I3C_MSG_RW_MASK) == I3C_MSG_READ;
		bool no_ending = false;

		/*
		 * Emit start if this is the first message or that
		 * the RESTART flag is set in message.
		 */
#ifdef CONFIG_I3C_NPCX_DMA
		bool emit_start =
			(i == 0) || ((msgs[i].flags & I3C_MSG_RESTART) == I3C_MSG_RESTART);
#endif

		bool emit_stop = (msgs[i].flags & I3C_MSG_STOP) == I3C_MSG_STOP;

		/*
		 * The controller requires special treatment of last byte of
		 * a write message. Since the API permits having a bunch of
		 * write messages without RESTART in between, this is just some
		 * logic to determine whether to treat the last byte of this
		 * message to be the last byte of a series of write mssages.
		 * If not, tell the write function not to treat it that way.
		 */
		if (!is_read && !emit_stop && ((i + 1) != num_msgs)) {
			bool next_is_write = (msgs[i + 1].flags & I3C_MSG_RW_MASK) == I3C_MSG_WRITE;
			bool next_is_restart =
				((msgs[i + 1].flags & I3C_MSG_RESTART) == I3C_MSG_RESTART);

			/* Check next msg is still write operation and not including Sr */
			if (next_is_write && !next_is_restart) {
				no_ending = true;
			}
		}

#ifdef CONFIG_I3C_NPCX_DMA
		/* Current DMA not support multi-message write */
		if (!is_read && no_ending) {
			LOG_ERR("I3C DMA transfer not support multi-message write");
			ret = -EINVAL;
			break;
		}
#endif

		/* Check message SDR or HDR mode */
		bool is_msg_hdr = (msgs[i].flags & I3C_MSG_HDR) == I3C_MSG_HDR;

		/* Set emit start type SDR or HDR-DDR mode */
		if (!is_msg_hdr || msgs[i].hdr_mode == 0) {
			op_type = NPCX_I3C_MCTRL_TYPE_I3C; /* Set operation type SDR */

			/*
			 * SDR, send boradcast header(0x7E)
			 *
			 * Two ways to do read/write transfer (SDR mode).
			 * 1. [S] + [0x7E]    + [address] + [data] + [Sr or P]
			 * 2. [S] + [address] + [data]    + [Sr or P]
			 *
			 * Send broadcast header(0x7E) on first transfer or after a STOP,
			 * unless flag is set not to.
			 */
			if (!(msgs[i].flags & I3C_MSG_NBCH) && send_broadcast) {
				ret = npcx_i3c_request_emit_start(inst, I3C_BROADCAST_ADDR,
								  NPCX_I3C_MCTRL_TYPE_I3C, false,
								  0);
				if (ret < 0) {
					LOG_ERR("%s: emit start of broadcast addr failed, error "
						"(%d)",
						__func__, ret);
					break;
				}
				send_broadcast = false;
			}
		} else if ((data->common.ctrl_config.supported_hdr & I3C_MSG_HDR_DDR) &&
			   (msgs[i].hdr_mode == I3C_MSG_HDR_DDR) && is_msg_hdr) {

			op_type = NPCX_I3C_MCTRL_TYPE_I3C_HDR_DDR; /* Set operation type DDR */

			/* Check HDR-DDR moves data by words */
			if ((msgs[i].len % 2) != 0x0) {
				LOG_ERR("HDR-DDR data length should be number of words , xfer "
					"len=%d",
					msgs[i].num_xfer);
				ret = -EINVAL;
				break;
			}
		} else {
			LOG_ERR("%s: %s controller HDR Mode %#x\r\n"
				"msg HDR mode %#x, msg flag %#x",
				__func__, dev->name, data->common.ctrl_config.supported_hdr,
				msgs[i].hdr_mode, msgs[i].flags);
			ret = -ENOTSUP;
			break;
		}

#ifdef CONFIG_I3C_NPCX_DMA
		/* Do transfer with target device */
		xfered_len = npcx_i3c_do_one_xfer_dma(dev, target->dynamic_addr, op_type,
						      msgs[i].buf, msgs[i].len, is_read, emit_start,
						      emit_stop, msgs[i].hdr_cmd_code);
#endif

		if (xfered_len < 0) {
			LOG_ERR("%s: do xfer fail", __func__);
			ret = xfered_len; /* Set error code to ret */
			break;
		}

		/* Write back the total number of bytes transferred */
		msgs[i].num_xfer = xfered_len;

		if (emit_stop) {
			/* SDR. After a STOP, send broadcast header before next msg */
			send_broadcast = true;
		}

		/* Check emit stop flag including in the final msg */
		if ((i == num_msgs - 1) && (emit_stop == false)) {
			is_xfer_done = false;
		}
	}

	/* Emit stop if error occurs or stop flag not in the msg */
	if ((ret != 0) || (is_xfer_done == false)) {
		npcx_i3c_xfer_stop(inst);
	}

	npcx_i3c_errwarn_clear_all(inst);
	npcx_i3c_status_clear_all(inst);

	npcx_i3c_interrupt_enable(inst, intmask);

	npcx_i3c_mutex_unlock(dev);

	return ret;
}

/*
 * brief:  Perform Dynamic Address Assignment.
 *
 * param[in] dev  Pointer to controller device driver instance.
 *
 * return  0 If successful.
 *         -EBUSY Bus is busy.
 *         -EIO General input / output error.
 *         -ENODEV If a provisioned ID does not match to any target devices
 *                 in the registered device list.
 *         -ENOSPC No more free addresses can be assigned to target.
 *         -ENOSYS Dynamic address assignment is not supported by
 *                 the controller driver.
 */
static int npcx_i3c_do_daa(const struct device *dev)
{
	const struct npcx_i3c_config *config = dev->config;
	struct npcx_i3c_data *data = dev->data;
	struct i3c_reg *inst = config->base;
	int ret = 0;
	uint8_t rx_buf[8];
	size_t rx_count;
	uint32_t intmask;

	npcx_i3c_mutex_lock(dev);

	memset(rx_buf, 0xff, sizeof(rx_buf));

	/* Check bus in idle state */
	if (WAIT_FOR((npcx_i3c_state_get(inst) == MSTATUS_STATE_IDLE), NPCX_I3C_CHK_TIMEOUT_US,
		     NULL) == false) {
		LOG_ERR("%s: DAA state error: %d", __func__, npcx_i3c_state_get(inst));
		npcx_i3c_mutex_unlock(dev);
		return -ETIMEDOUT;
	}

	LOG_DBG("DAA: ENTDAA");

	/* Disable interrupt */
	intmask = inst->MINTSET;
	npcx_i3c_interrupt_all_disable(inst);

	npcx_i3c_xfer_reset(inst);

	/* Emit process DAA */
	if (npcx_i3c_request_daa(inst) != 0) {
		ret = -ETIMEDOUT;
		LOG_ERR("Emit process DAA error");
		goto out_do_daa;
	}

	/* Loop until no more responses from devices */
	do {
		/* Check ERRWARN bit set */
		if (npcx_i3c_has_error(inst)) {
			ret = -EIO;
			LOG_ERR("DAA recv error");
			break;
		}

		/* Receive Provisioned ID, BCR and DCR (total 8 bytes) */
		rx_count = GET_FIELD(inst->MDATACTRL, NPCX_I3C_MDATACTRL_RXCOUNT);

		if (rx_count == DAA_TGT_INFO_SZ) {
			for (int i = 0; i < rx_count; i++) {
				rx_buf[i] = (uint8_t)inst->MRDATAB;
			}
		} else {
			/* Data count not as expected, exit DAA */
			ret = -EBADMSG;
			LOG_DBG("Rx count not as expected %d, abort DAA", rx_count);
			break;
		}

		/* Start assign dynamic address */
		if ((npcx_i3c_state_get(inst) == MSTATUS_STATE_DAA) &&
		    IS_BIT_SET(inst->MSTATUS, NPCX_I3C_MSTATUS_BETWEEN)) {
			struct i3c_device_desc *target;
			uint16_t vendor_id;
			uint32_t part_no;
			uint64_t pid;
			uint8_t dyn_addr = 0;

			/* PID[47:33] = manufacturer ID */
			vendor_id = (((uint16_t)rx_buf[0] << 8U) | (uint16_t)rx_buf[1]) & 0xFFFEU;

			/* PID[31:0] = vendor fixed falue or random value */
			part_no = (uint32_t)rx_buf[2] << 24U | (uint32_t)rx_buf[3] << 16U |
				  (uint32_t)rx_buf[4] << 8U | (uint32_t)rx_buf[5];

			/* Combine into one Provisioned ID */
			pid = (uint64_t)vendor_id << 32U | (uint64_t)part_no;

			LOG_DBG("DAA: Rcvd PID 0x%04x%08x", vendor_id, part_no);

			/* Find a usable address during ENTDAA */
			ret = i3c_dev_list_daa_addr_helper(&data->common.attached_dev.addr_slots,
							   &config->common.dev_list, pid, false,
							   false, &target, &dyn_addr);
			if (ret != 0) {
				LOG_ERR("%s: Assign new DA error", __func__);
				break;
			}

			if (target == NULL) {
				LOG_INF("%s: PID 0x%04x%08x is not in registered device "
					"list, given dynamic address 0x%02x",
					dev->name, vendor_id, part_no, dyn_addr);
			} else {
				/* Update target descriptor */
				target->dynamic_addr = dyn_addr;
				target->bcr = rx_buf[6];
				target->dcr = rx_buf[7];
			}

			/* Mark the address as I3C device */
			i3c_addr_slots_mark_i3c(&data->common.attached_dev.addr_slots, dyn_addr);

			/*
			 * If the device has static address, after address assignment,
			 * the device will not respond to the static address anymore.
			 * So free the static one from address slots if different from
			 * newly assigned one.
			 */
			if ((target != NULL) && (target->static_addr != 0U) &&
			    (dyn_addr != target->static_addr)) {
				i3c_addr_slots_mark_free(&data->common.attached_dev.addr_slots,
							 dyn_addr);
			}

			/* Emit process DAA again to send the address to the device */
			inst->MWDATAB = dyn_addr;
			ret = npcx_i3c_request_daa(inst);
			if (ret != 0) {
				LOG_ERR("%s: Assign DA timeout", __func__);
				break;
			}

			LOG_DBG("PID 0x%04x%08x assigned dynamic address 0x%02x", vendor_id,
				part_no, dyn_addr);

			/* Target did not accept the assigned DA, exit DAA */
			if (IS_BIT_SET(inst->MSTATUS, NPCX_I3C_MSTATUS_NACKED)) {
				ret = -EFAULT;
				LOG_DBG("TGT NACK assigned DA %#x", dyn_addr);

				/* Free the reserved DA */
				i3c_addr_slots_mark_free(&data->common.attached_dev.addr_slots,
							 dyn_addr);

				/* 0 if address has not been assigned */
				if (target != NULL) {
					target->dynamic_addr = 0;
				}

				break;
			}
		}

		/* Check all targets have been assigned DA and DAA complete */
	} while ((!IS_BIT_SET(inst->MSTATUS, NPCX_I3C_MSTATUS_COMPLETE)) &&
		 npcx_i3c_state_get(inst) != MSTATUS_STATE_IDLE);

out_do_daa:
	/* Exit DAA mode when error occurs */
	if (ret != 0) {
		npcx_i3c_request_emit_stop(inst);
	}

	/* Clear all flags. */
	npcx_i3c_errwarn_clear_all(inst);
	npcx_i3c_status_clear_all(inst);

	/* Re-Enable I3C IRQ sources. */
	npcx_i3c_interrupt_enable(inst, intmask);

	npcx_i3c_fifo_flush(inst);
	npcx_i3c_mutex_unlock(dev);

	return ret;
}

/*
 * brief:  Send Common Command Code (CCC).
 *
 * param[in] dev      Pointer to controller device driver instance.
 * param[in] payload  Pointer to CCC payload.
 *
 * return:  The same as i3c_do_ccc()
 *          0 If successful.
 *          -EBUSY Bus is busy.
 *          -EIO General Input / output error.
 *          -EINVAL Invalid valid set in the payload structure.
 *          -ENOSYS Not implemented.
 */
static int npcx_i3c_do_ccc(const struct device *dev, struct i3c_ccc_payload *payload)
{
	const struct npcx_i3c_config *config = dev->config;
	int ret;
	struct i3c_reg *inst = config->base;
	uint32_t intmask;
	int xfered_len;

	if (dev == NULL || payload == NULL) {
		return -EINVAL;
	}

	npcx_i3c_mutex_lock(dev);

	/* Disable interrupt */
	intmask = inst->MINTSET;
	npcx_i3c_interrupt_all_disable(inst);

	/* Clear status and flush fifo */
	npcx_i3c_xfer_reset(inst);

	LOG_DBG("CCC[0x%02x]", payload->ccc.id);

	/* Write emit START and broadcast address (0x7E) */
	ret = npcx_i3c_request_emit_start(inst, I3C_BROADCAST_ADDR, NPCX_I3C_MCTRL_TYPE_I3C, false,
					  0);
	if (ret < 0) {
		LOG_ERR("CCC[0x%02x] %s START error (%d)", payload->ccc.id,
			i3c_ccc_is_payload_broadcast(payload) ? "broadcast" : "direct", ret);

		goto out_do_ccc;
	}

	/* Write CCC command */
	npcx_i3c_status_clear_all(inst);
	npcx_i3c_errwarn_clear_all(inst);
	xfered_len = npcx_i3c_xfer_write_fifo(inst, &payload->ccc.id, 1, payload->ccc.data_len > 0);
	if (xfered_len < 0) {
		ret = xfered_len;
		LOG_ERR("CCC[0x%02x] %s command error (%d)", payload->ccc.id,
			i3c_ccc_is_payload_broadcast(payload) ? "broadcast" : "direct", ret);

		goto out_do_ccc;
	}

	/* Write data (defining byte or data bytes) for CCC if needed */
	if (payload->ccc.data_len > 0) {
		npcx_i3c_status_clear_all(inst);
		npcx_i3c_errwarn_clear_all(inst);
		xfered_len = npcx_i3c_xfer_write_fifo(inst, payload->ccc.data,
						      payload->ccc.data_len, false);
		if (xfered_len < 0) {
			ret = xfered_len;
			LOG_ERR("CCC[0x%02x] %s command payload error (%d)", payload->ccc.id,
				i3c_ccc_is_payload_broadcast(payload) ? "broadcast" : "direct",
				ret);

			goto out_do_ccc;
		}

		/* Write back the transferred bytes */
		payload->ccc.num_xfer = xfered_len;
	}

	/* Wait message transfer complete */
	if (WAIT_FOR(IS_BIT_SET(inst->MSTATUS, NPCX_I3C_MSTATUS_COMPLETE), NPCX_I3C_CHK_TIMEOUT_US,
		     NULL) == false) {
		ret = -ETIMEDOUT;
		LOG_DBG("Check complete timeout");
		goto out_do_ccc;
	}

	inst->MSTATUS = BIT(NPCX_I3C_MSTATUS_COMPLETE); /* W1C */

	/* For direct CCC */
	if (!i3c_ccc_is_payload_broadcast(payload)) {
		/*
		 * If there are payload(s) for each target,
		 * RESTART and then send payload for each target.
		 */
		for (int idx = 0; idx < payload->targets.num_targets; idx++) {
			struct i3c_ccc_target_payload *tgt_payload =
				&payload->targets.payloads[idx];

			bool is_read = (tgt_payload->rnw == 1U);

			xfered_len = npcx_i3c_do_one_xfer(
				inst, tgt_payload->addr, NPCX_I3C_MCTRL_TYPE_I3C, tgt_payload->data,
				tgt_payload->data_len, is_read, true, false, false);
			if (xfered_len < 0) {
				ret = xfered_len;
				LOG_ERR("CCC[0x%02x] target payload error (%d)", payload->ccc.id,
					ret);

				goto out_do_ccc;
			}

			/* Write back the total number of bytes transferred */
			tgt_payload->num_xfer = xfered_len;
		}
	}

	/* Currently handle broadcast RSTACT command only */
	if (payload->ccc.id == I3C_CCC_RSTACT(true)) {
		/* Handle invalid or unsupported defining bytes */
		if (payload->ccc.data[0] > I3C_CCC_RSTACT_VIRTUAL_TARGET_DETECT) {
			LOG_ERR("Invalid or unsupported RSTACT defining byte: %#x",
				payload->ccc.data[0]);
			ret = -EINVAL;
			goto out_do_ccc;
		}

		/* Emit target reset pattern */
		ret = npcx_i3c_request_tgt_reset(inst);
	}

out_do_ccc:
	npcx_i3c_request_emit_stop(inst);

	npcx_i3c_interrupt_enable(inst, intmask);

	npcx_i3c_mutex_unlock(dev);

	return ret;
}

#ifdef CONFIG_I3C_USE_IBI
/*
 * brief  Callback to service target initiated IBIs in workqueue.
 *
 * param[in] work  Pointer to k_work item.
 */
static void npcx_i3c_ibi_work(struct k_work *work)
{
	uint8_t payload[CONFIG_I3C_IBI_MAX_PAYLOAD_SIZE];
	size_t payload_sz = 0;

	struct i3c_ibi_work *i3c_ibi_work = CONTAINER_OF(work, struct i3c_ibi_work, work);
	const struct device *dev = i3c_ibi_work->controller;
	const struct npcx_i3c_config *config = dev->config;
	struct npcx_i3c_data *data = dev->data;
	struct i3c_reg *inst = config->base;
	struct i3c_device_desc *target = NULL;
	uint32_t ibitype, ibiaddr;
	int ret;

	k_sem_take(&data->ibi_lock_sem, K_FOREVER);

	if (npcx_i3c_state_get(inst) != MSTATUS_STATE_TGTREQ) {
		LOG_DBG("IBI work %p running not because of IBI", work);
		LOG_ERR("%s: IBI not in TGTREQ state, state : %#x", __func__,
			npcx_i3c_state_get(inst));
		LOG_ERR("%s: MSTATUS 0x%08x MERRWARN 0x%08x", __func__, inst->MSTATUS,
			inst->MERRWARN);
		npcx_i3c_request_emit_stop(inst);

		goto out_ibi_work;
	};

	/* Use auto IBI to service the IBI */
	npcx_i3c_request_auto_ibi(inst);

	/* Wait for target to win address arbitration (ibitype and ibiaddr) */
	if (WAIT_FOR(IS_BIT_SET(inst->MSTATUS, NPCX_I3C_MSTATUS_IBIWON), NPCX_I3C_CHK_TIMEOUT_US,
		     NULL) == false) {
		LOG_ERR("IBI work, IBIWON timeout");
		LOG_ERR("%s: MSTATUS 0x%08x MERRWARN 0x%08x", __func__, inst->MSTATUS,
			inst->MERRWARN);
		npcx_i3c_request_emit_stop(inst);

		goto out_ibi_work;
	}

	ibitype = GET_FIELD(inst->MSTATUS, NPCX_I3C_MSTATUS_IBITYPE);
	ibiaddr = GET_FIELD(inst->MSTATUS, NPCX_I3C_MSTATUS_IBIADDR);

	switch (ibitype) {
	case MSTATUS_IBITYPE_IBI:
		ret = npcx_i3c_xfer_read_fifo(inst, &payload[0], sizeof(payload));
		if (ret >= 0) {
			payload_sz = (size_t)ret;
		} else {
			LOG_ERR("Error reading IBI payload");
			npcx_i3c_request_emit_stop(inst);

			goto out_ibi_work;
		}
		break;
	case MSTATUS_IBITYPE_HJ:
		npcx_i3c_ibi_respond_ack(inst);
		npcx_i3c_request_emit_stop(inst);
		break;
	case MSTATUS_IBITYPE_CR:
		LOG_DBG("Controller role handoff not supported");
		npcx_i3c_ibi_respond_nack(inst);
		npcx_i3c_request_emit_stop(inst);
		break;
	default:
		break;
	}

	if (npcx_i3c_has_error(inst)) {
		LOG_ERR("%s: unexpected error, ibi type:%d", __func__, ibitype);
		/*
		 * If the controller detects any errors, simply
		 * emit a STOP to abort the IBI. The target will
		 * raise IBI again if so desired.
		 */
		npcx_i3c_request_emit_stop(inst);

		goto out_ibi_work;
	}

	switch (ibitype) {
	case MSTATUS_IBITYPE_IBI:
		target = i3c_dev_list_i3c_addr_find(dev, (uint8_t)ibiaddr);
		if (target != NULL) {
			if (i3c_ibi_work_enqueue_target_irq(target, &payload[0], payload_sz) != 0) {
				LOG_ERR("Error enqueue IBI IRQ work");
			}
		} else {
			LOG_ERR("IBI (MDB) target not in the list");
		}

		/* Finishing the IBI transaction */
		npcx_i3c_request_emit_stop(inst);
		break;
	case MSTATUS_IBITYPE_HJ:
		if (i3c_ibi_work_enqueue_hotjoin(dev) != 0) {
			LOG_ERR("Error enqueue IBI HJ work");
		}
		break;
	case MSTATUS_IBITYPE_CR:
		/* Not supported, for future use. */
		break;
	default:
		break;
	}

out_ibi_work:
	npcx_i3c_xfer_reset(inst);

	k_sem_give(&data->ibi_lock_sem);

	/* Re-enable target initiated IBI interrupt. */
	inst->MINTSET = BIT(NPCX_I3C_MINTSET_TGTSTART);
}

/* Set local IBI information to IBIRULES register */
static void npcx_i3c_ibi_rules_setup(struct npcx_i3c_data *data, struct i3c_reg *inst)
{
	uint32_t ibi_rules;
	int idx;

	ibi_rules = 0;

	for (idx = 0; idx < ARRAY_SIZE(data->ibi.addr); idx++) {
		uint32_t addr_6bit;

		/* Extract the lower 6-bit of target address */
		addr_6bit = (uint32_t)data->ibi.addr[idx] & IBIRULES_ADDR_MSK;

		/* Shift into correct place */
		addr_6bit <<= idx * IBIRULES_ADDR_SHIFT;

		/* Put into the temporary IBI Rules register */
		ibi_rules |= addr_6bit;
	}

	if (!data->ibi.msb) {
		/* The MSB0 field is 1 if MSB is 0 */
		ibi_rules |= BIT(NPCX_I3C_IBIRULES_MSB0);
	}

	if (!data->ibi.has_mandatory_byte) {
		/* The NOBYTE field is 1 if there is no mandatory byte */
		ibi_rules |= BIT(NPCX_I3C_IBIRULES_NOBYTE);
	}

	/* Update the register */
	inst->IBIRULES = ibi_rules;

	LOG_DBG("MIBIRULES 0x%08x", ibi_rules);
}

static int npcx_i3c_ibi_enable(const struct device *dev, struct i3c_device_desc *target)
{
	const struct npcx_i3c_config *config = dev->config;
	struct npcx_i3c_data *data = dev->data;
	struct i3c_reg *inst = config->base;
	struct i3c_ccc_events i3c_events;
	uint8_t idx;
	bool msb, has_mandatory_byte;
	int ret;

	/* Check target IBI request capable */
	if (!i3c_device_is_ibi_capable(target)) {
		LOG_ERR("%s: device is not ibi capable", __func__);
		return -EINVAL;
	}

	if (data->ibi.num_addr >= ARRAY_SIZE(data->ibi.addr)) {
		/* No more free entries in the IBI Rules table */
		LOG_ERR("%s: no more free space in the IBI rules table", __func__);
		return -ENOMEM;
	}

	/* Check whether the selected target is already in the list */
	for (idx = 0; idx < ARRAY_SIZE(data->ibi.addr); idx++) {
		if (data->ibi.addr[idx] == target->dynamic_addr) {
			LOG_ERR("%s: selected target is already in the list", __func__);
			return -EINVAL;
		}
	}

	/* Disable controller interrupt while we configure IBI rules. */
	inst->MINTCLR = BIT(NPCX_I3C_MINTCLR_TGTSTART);

	LOG_DBG("IBI enabling for 0x%02x (BCR 0x%02x)", target->dynamic_addr, target->bcr);

	msb = (target->dynamic_addr & BIT(6)) == BIT(6); /* Check addess(7-bit) MSB enable */
	has_mandatory_byte = i3c_ibi_has_payload(target);

	/*
	 * If there are already addresses in the table, we must
	 * check if the incoming entry is compatible with
	 * the existing ones.
	 *
	 * All targets in the list should follow the same IBI rules.
	 */
	if (data->ibi.num_addr > 0) {
		/*
		 * 1. All devices in the table must all use mandatory
		 *    bytes, or do not.
		 *
		 * 2. Each address in entry only captures the lowest 6-bit.
		 *    The MSB (7th bit) is captured separated in another bit
		 *    in the register. So all addresses must have the same MSB.
		 */
		if ((has_mandatory_byte != data->ibi.has_mandatory_byte) ||
		    (msb != data->ibi.msb)) {
			ret = -EINVAL;
			LOG_ERR("%s: New IBI does not have same mandatory byte or msb"
				" as previous IBI",
				__func__);
			goto out_ibi_enable;
		}

		/* Find an empty address slot */
		for (idx = 0; idx < ARRAY_SIZE(data->ibi.addr); idx++) {
			if (data->ibi.addr[idx] == 0U) {
				break;
			}
		}

		if (idx >= ARRAY_SIZE(data->ibi.addr)) {
			ret = -ENOTSUP;
			LOG_ERR("Cannot support more IBIs");
			goto out_ibi_enable;
		}
	} else {
		/*
		 * If the incoming address is the first in the table,
		 * it dictates future compatibilities.
		 */
		data->ibi.has_mandatory_byte = has_mandatory_byte;
		data->ibi.msb = msb;

		idx = 0;
	}

	data->ibi.addr[idx] = target->dynamic_addr;
	data->ibi.num_addr += 1U;

	npcx_i3c_ibi_rules_setup(data, inst);

	/* Enable target IBI event by ENEC command */
	i3c_events.events = I3C_CCC_EVT_INTR;
	ret = i3c_ccc_do_events_set(target, true, &i3c_events);
	if (ret != 0) {
		LOG_ERR("Error sending IBI ENEC for 0x%02x (%d)", target->dynamic_addr, ret);
	}

out_ibi_enable:
	if (data->ibi.num_addr > 0U) {
		/*
		 * If there is more than 1 target in the list,
		 * enable controller to raise interrupt when a target
		 * initiates IBI.
		 */
		inst->MINTSET = BIT(NPCX_I3C_MINTSET_TGTSTART);
	}

	return ret;
}

static int npcx_i3c_ibi_disable(const struct device *dev, struct i3c_device_desc *target)
{
	const struct npcx_i3c_config *config = dev->config;
	struct npcx_i3c_data *data = dev->data;
	struct i3c_reg *inst = config->base;
	struct i3c_ccc_events i3c_events;
	int ret;
	int idx;

	if (!i3c_device_is_ibi_capable(target)) {
		LOG_ERR("%s: device is not ibi capable", __func__);
		return -EINVAL;
	}

	for (idx = 0; idx < ARRAY_SIZE(data->ibi.addr); idx++) {
		if (target->dynamic_addr == data->ibi.addr[idx]) {
			break;
		}
	}

	if (idx == ARRAY_SIZE(data->ibi.addr)) {
		LOG_ERR("%s: target is not in list of registered addresses", __func__);
		return -ENODEV;
	}

	/* Disable controller interrupt while we configure IBI rules. */
	inst->MINTCLR = BIT(NPCX_I3C_MINTCLR_TGTSTART);

	/* Clear the ibi rule data */
	data->ibi.addr[idx] = 0U;
	data->ibi.num_addr -= 1U;

	/* Disable disable target IBI */
	i3c_events.events = I3C_CCC_EVT_INTR;
	ret = i3c_ccc_do_events_set(target, false, &i3c_events);
	if (ret != 0) {
		LOG_ERR("Error sending IBI DISEC for 0x%02x (%d)", target->dynamic_addr, ret);
	}

	npcx_i3c_ibi_rules_setup(data, inst);

	if (data->ibi.num_addr > 0U) {
		/*
		 * Enable controller to raise interrupt when a target
		 * initiates IBI.
		 */
		inst->MINTSET = BIT(NPCX_I3C_MINTSET_TGTSTART);
	}

	return ret;
}
#endif /* CONFIG_I3C_USE_IBI */

static int npcx_i3c_target_ibi_raise(const struct device *dev, struct i3c_ibi *request)
{
	const struct npcx_i3c_config *config = dev->config;
	struct i3c_reg *inst = config->base;
	struct npcx_i3c_data *data = dev->data;
	int index;

	/* the request or the payload were not specific */
	if ((request == NULL) || ((request->payload_len) && (request->payload == NULL))) {
		return -EINVAL;
	}

	/* the I3C was not in target mode or the bus is in HDR mode now */
	if (!IS_BIT_SET(inst->CONFIG, NPCX_I3C_CONFIG_TGTENA) ||
	    IS_BIT_SET(inst->STATUS, NPCX_I3C_STATUS_STHDR)) {
		return -EINVAL;
	}

	switch (request->ibi_type) {
	case I3C_IBI_TARGET_INTR:
		if (IS_BIT_SET(inst->STATUS, NPCX_I3C_STATUS_IBIDIS)) {
			return -ENOTSUP;
		}

		if (request->payload_len == 0) {
			LOG_ERR("%s: IBI invalid payload_len, len: %#x", __func__,
				request->payload_len);
			return -EINVAL;
		}

		k_sem_take(&data->target_event_lock_sem, K_FOREVER);
		set_oper_state(dev, NPCX_I3C_OP_STATE_IBI);

		/* Mandatory data byte */
		SET_FIELD(inst->CTRL, NPCX_I3C_CTRL_IBIDATA, request->payload[0]);

		/* Extended data */
		if (request->payload_len > 1) {
			if (request->payload_len <= 32) {
				for (index = 1; index < (request->payload_len - 1); index++) {
					inst->WDATAB = request->payload[index];
				}

				inst->WDATABE = request->payload[index];
			} else {
				/* transfer data from MDMA */
			}

			SET_FIELD(inst->IBIEXT1, NPCX_I3C_IBIEXT1_CNT, 0);
			inst->CTRL |= BIT(NPCX_I3C_CTRL_EXTDATA);
		}

		SET_FIELD(inst->CTRL, NPCX_I3C_CTRL_EVENT, CTRL_EVENT_IBI);
		break;

	case I3C_IBI_CONTROLLER_ROLE_REQUEST:
		if (IS_BIT_SET(inst->STATUS, NPCX_I3C_STATUS_MRDIS)) {
			return -ENOTSUP;
		}

		/* The bus controller request was generate only a target with controller mode
		 * capabilities mode
		 */
		if (GET_FIELD(inst->MCONFIG, NPCX_I3C_MCONFIG_CTRENA) != MCONFIG_CTRENA_CAPABLE) {
			return -ENOTSUP;
		}

		k_sem_take(&data->target_event_lock_sem, K_FOREVER);
		set_oper_state(dev, NPCX_I3C_OP_STATE_IBI);

		SET_FIELD(inst->CTRL, NPCX_I3C_CTRL_EVENT, CTRL_EVENT_CNTLR_REQ);
		break;

	case I3C_IBI_HOTJOIN:
		if (IS_BIT_SET(inst->STATUS, NPCX_I3C_STATUS_HJDIS)) {
			return -ENOTSUP;
		}

		k_sem_take(&data->target_event_lock_sem, K_FOREVER);
		set_oper_state(dev, NPCX_I3C_OP_STATE_IBI);

		inst->CONFIG &= ~BIT(NPCX_I3C_CONFIG_TGTENA);
		SET_FIELD(inst->CTRL, NPCX_I3C_CTRL_EVENT, CTRL_EVENT_HJ);
		inst->CONFIG |= BIT(NPCX_I3C_CONFIG_TGTENA);
		break;

	default:
		return -EINVAL;
	}

	return 0;
}

#ifdef CONFIG_I3C_NPCX_DMA
static uint16_t npcx_i3c_target_get_mdmafb_count(const struct device *dev)
{
	const struct npcx_i3c_config *config = dev->config;
	struct mdma_reg *mdma_inst = config->mdma_base;

	if (mdma_inst->MDMA_CTCNT0 < mdma_inst->MDMA_TCNT0) {
		return (mdma_inst->MDMA_TCNT0 - mdma_inst->MDMA_CTCNT0);
	} else {
		return 0;
	}
}

static uint16_t npcx_i3c_target_get_mdmatb_count(const struct device *dev)
{
	const struct npcx_i3c_config *config = dev->config;
	struct mdma_reg *mdma_inst = config->mdma_base;

	if (mdma_inst->MDMA_CTCNT1 < mdma_inst->MDMA_TCNT1) {
		return (mdma_inst->MDMA_TCNT1 - mdma_inst->MDMA_CTCNT1);
	} else {
		return 0;
	}
}

static void npcx_i3c_target_disable_mdmafb(const struct device *dev)
{
	const struct npcx_i3c_config *config = dev->config;
	struct i3c_reg *i3c_inst = config->base;
	struct mdma_reg *mdma_inst = config->mdma_base;

	mdma_inst->MDMA_CTL0 &= ~BIT(NPCX_MDMA_CTL_MDMAEN);
	mdma_inst->MDMA_CTL0 &= ~BIT(NPCX_MDMA_CTL_TC); /* W0C */
	mdma_inst->MDMA_CTL0 &= ~BIT(NPCX_MDMA_CTL_SIEN);
	SET_FIELD(i3c_inst->DMACTRL, NPCX_I3C_DMACTRL_DMAFB, MDMA_DMAFB_DISABLE);

	/* Ignore DA and detect all START and STOP */
	i3c_inst->CONFIG &= ~BIT(NPCX_I3C_CONFIG_MATCHSS);

	/* Flush the tx and rx FIFO */
	i3c_inst->DATACTRL |= BIT(NPCX_I3C_DATACTRL_FLUSHTB) | BIT(NPCX_I3C_DATACTRL_FLUSHFB);
}

static void npcx_i3c_target_enable_mdmafb(const struct device *dev, uint8_t *buf, uint16_t len)
{
	const struct npcx_i3c_config *config = dev->config;
	struct i3c_reg *i3c_inst = config->base;
	struct mdma_reg *mdma_inst = config->mdma_base;

	/* Check MDMA disable */
	if (IS_BIT_SET(mdma_inst->MDMA_CTL0, NPCX_MDMA_CTL_MDMAEN) != 0) {
		mdma_inst->MDMA_CTL0 &= ~BIT(NPCX_MDMA_CTL_MDMAEN);
		LOG_DBG("MDMAFB_EN=1 before enable");
	}

	/* Detect a START and STOP only if the transaction
	 * address matches the target address (STATUS.MATCHED=1).
	 */
	i3c_inst->CONFIG |= BIT(NPCX_I3C_CONFIG_MATCHSS);
	/* Enable manual DMA control */
	SET_FIELD(i3c_inst->DMACTRL, NPCX_I3C_DMACTRL_DMAFB, MDMA_DMAFB_EN_MANUAL);

	/* Read Operation (MDMA CH_0) */
	mdma_inst->MDMA_TCNT0 = len;                       /* Set MDMA transfer count */
	mdma_inst->MDMA_DSTB0 = (uint32_t)buf;             /* Set destination address */
	mdma_inst->MDMA_CTL0 &= ~BIT(NPCX_MDMA_CTL_TC);    /* W0C */
	mdma_inst->MDMA_CTL0 |= BIT(NPCX_MDMA_CTL_SIEN);   /* Enable stop interrupt */
	mdma_inst->MDMA_CTL0 |= BIT(NPCX_MDMA_CTL_MDMAEN); /* Start DMA transfer */
}

static void npcx_i3c_target_disable_mdmatb(const struct device *dev)
{
	const struct npcx_i3c_config *config = dev->config;
	struct i3c_reg *i3c_inst = config->base;
	struct mdma_reg *mdma_inst = config->mdma_base;

	mdma_inst->MDMA_CTL1 &= ~BIT(NPCX_MDMA_CTL_MDMAEN);
	mdma_inst->MDMA_CTL1 &= ~BIT(NPCX_MDMA_CTL_TC); /* W0C */
	mdma_inst->MDMA_CTL1 &= ~BIT(NPCX_MDMA_CTL_SIEN);
	SET_FIELD(i3c_inst->DMACTRL, NPCX_I3C_DMACTRL_DMATB, MDMA_DMATB_DISABLE);

	/* Ignore DA and detect all START and STOP */
	i3c_inst->CONFIG &= ~BIT(NPCX_I3C_CONFIG_MATCHSS);

	/* Flush the tx and rx FIFO */
	i3c_inst->DATACTRL |= BIT(NPCX_I3C_DATACTRL_FLUSHTB) | BIT(NPCX_I3C_DATACTRL_FLUSHFB);
}

static void npcx_i3c_target_enable_mdmatb(const struct device *dev, uint8_t *buf, uint16_t len)
{
	const struct npcx_i3c_config *config = dev->config;
	struct i3c_reg *i3c_inst = config->base;
	struct mdma_reg *mdma_inst = config->mdma_base;

	/* Check MDMA disable */
	if (IS_BIT_SET(mdma_inst->MDMA_CTL1, NPCX_MDMA_CTL_MDMAEN) != 0) {
		mdma_inst->MDMA_CTL1 &= ~BIT(NPCX_MDMA_CTL_MDMAEN);
		LOG_DBG("MDMATB_EN=1 before enable");
	}

	/* Detect a START and STOP only if the transaction address matches the target address */
	i3c_inst->CONFIG |= BIT(NPCX_I3C_CONFIG_MATCHSS);

	/* Enable DMA only for one frame.
	 * MATCHSS must be set to 1 before selecting '0x1' for DMATB field
	 *
	 * In SDR DMATB is automatically cleared if MATCHED bit is set to 1 and either STOP bit
	 * or START bit is set to 1.
	 *
	 * In HDR-DDR mode, DMATB is not automatically cleared.
	 */
	SET_FIELD(i3c_inst->DMACTRL, NPCX_I3C_DMACTRL_DMATB, MDMA_DMAFB_EN_ONE_FRAME);

	/* Write Operation (MDMA CH_1) */
	mdma_inst->MDMA_TCNT1 = len;                       /* Set MDMA transfer count */
	mdma_inst->MDMA_SRCB1 = (uint32_t)buf;             /* Set source address */
	mdma_inst->MDMA_CTL1 &= ~BIT(NPCX_MDMA_CTL_TC);    /* W0C */
	mdma_inst->MDMA_CTL1 |= BIT(NPCX_MDMA_CTL_MDMAEN); /* Start DMA transfer */
}

static void npcx_i3c_target_rx_read(const struct device *dev)
{
	struct npcx_i3c_data *data = dev->data;
	struct i3c_config_target *config_tgt = &data->config_target;

	/* Enable the DMA from bus */
	npcx_i3c_target_enable_mdmafb(dev, data->mdma_rx_buf, config_tgt->max_read_len);
}

/* brief:  Handle the end of transfer (read request or write request).
 *         The ending signal might be either STOP or Sr.
 * return: -EINVAL:
 *              1. operation not read or write request.
 *              2. start or stop flag is not set.
 *         -EBUSY: in write request, wait for mdma done.
 *          0: success
 */
static int npcx_i3c_target_xfer_end_handle(const struct device *dev)
{
	struct npcx_i3c_data *data = dev->data;
	const struct npcx_i3c_config *config = dev->config;
	struct i3c_reg *inst = config->base;
	struct mdma_reg *mdma_inst = config->mdma_base;
	const struct i3c_target_callbacks *target_cb = data->target_config->callbacks;
	bool is_i3c_start = IS_BIT_SET(inst->INTMASKED, NPCX_I3C_INTMASKED_START);
	bool is_i3c_stop = IS_BIT_SET(inst->INTMASKED, NPCX_I3C_INTMASKED_STOP);
	enum npcx_i3c_oper_state op_state = get_oper_state(dev);
	uint32_t cur_xfer_cnt;
	uint32_t timer = 0;
	int ret = 0;

	if ((op_state != NPCX_I3C_OP_STATE_WR) && (op_state != NPCX_I3C_OP_STATE_RD)) {
		LOG_ERR("%s: op_staste error :%d", __func__, op_state);
		return -EINVAL;
	}

	if ((is_i3c_start | is_i3c_stop) == 0) {
		LOG_ERR("%s: not the end of xfer, is_start: %d, is_stop:%d", __func__, is_i3c_start,
			is_i3c_stop);
		return -EINVAL;
	}

	/* Read request */
	if (get_oper_state(dev) == NPCX_I3C_OP_STATE_RD) {
		npcx_i3c_target_disable_mdmatb(dev);
		goto out_tgt_xfer_end_hdl;
	}

	/* Write request */
	/* Check rx fifo count is 0 */
	if (WAIT_FOR((GET_FIELD(inst->DATACTRL, NPCX_I3C_DATACTRL_RXCOUNT) == 0),
		     I3C_TGT_WR_REQ_WAIT_US, NULL) == false) {
		LOG_ERR("%s: target wr_req rxcnt timeout %d", __func__,
			GET_FIELD(inst->DATACTRL, NPCX_I3C_DATACTRL_RXCOUNT));
		ret = -EIO;
		npcx_i3c_target_disable_mdmafb(dev);
		goto out_tgt_xfer_end_hdl;
	}

	/* Check mdma rx transfer count stability */
	cur_xfer_cnt = mdma_inst->MDMA_CTCNT0;
	while (timer < I3C_TGT_WR_REQ_WAIT_US) {
		/* After the stop or Sr, the rx fifo is empty, and the last byte has been
		 * transferred.
		 */
		if (cur_xfer_cnt != mdma_inst->MDMA_CTCNT0) {
			break;
		}

		/* Keep polling if the transferred count does not change */
		k_busy_wait(1);
		timer++;
		cur_xfer_cnt = mdma_inst->MDMA_CTCNT0;
	}

	npcx_i3c_target_disable_mdmafb(dev); /* Disable mdma and check the final result */

	if (cur_xfer_cnt == mdma_inst->MDMA_CTCNT0) {
#ifdef CONFIG_I3C_TARGET_BUFFER_MODE
		if (target_cb && target_cb->buf_write_received_cb) {
			target_cb->buf_write_received_cb(data->target_config, data->mdma_rx_buf,
							 npcx_i3c_target_get_mdmafb_count(dev));
		}
#endif
	} else {
		LOG_ERR("(%s) MDMA rx abnormal, force mdma stop, xfer cnt=%#x",
			is_i3c_start ? "Sr" : "STOP", cur_xfer_cnt);
		ret = -EBUSY;
	}

out_tgt_xfer_end_hdl:
	/* Clear DA matched status and re-enable interrupt */
	inst->STATUS = BIT(NPCX_I3C_STATUS_MATCHED);
	inst->INTSET = BIT(NPCX_I3C_INTSET_MATCHED);

	if (is_i3c_start) {
		set_oper_state(dev, NPCX_I3C_OP_STATE_IDLE);
	}

	return ret;
}
#endif /* End of CONFIG_I3C_NPCX_DMA */

static int npcx_i3c_target_tx_write(const struct device *dev, uint8_t *buf, uint16_t len,
				    uint8_t hdr_mode)
{
	if ((buf == NULL) || (len == 0)) {
		LOG_ERR("%s: Data buffer configuration failed", __func__);
		return -EINVAL;
	}

	if (hdr_mode != 0) {
		LOG_ERR("%s: HDR not supported", __func__);
		return -ENOSYS;
	}

#ifdef CONFIG_I3C_NPCX_DMA
	npcx_i3c_target_enable_mdmatb(dev, buf, len);

	return npcx_i3c_target_get_mdmatb_count(dev); /* Return total bytes written */
#else
	LOG_ERR("%s: Support dma mode only", __func__);
	return -ENOSYS;
#endif
}

static int npcx_i3c_target_register(const struct device *dev, struct i3c_target_config *cfg)
{
	struct npcx_i3c_data *data = dev->data;

	data->target_config = cfg;

	return 0;
}

static int npcx_i3c_target_unregister(const struct device *dev, struct i3c_target_config *cfg)
{
	struct npcx_i3c_data *data = dev->data;

	data->target_config = NULL;

	return 0;
}

static int npcx_i3c_get_scl_config(struct npcx_i3c_timing_cfg *cfg, uint32_t i3c_src_clk,
				   uint32_t pp_baudrate_hz, uint32_t od_baudrate_hz)
{
	uint32_t i3c_div, freq;
	uint32_t ppbaud, odbaud;
	uint32_t pplow_ns, odlow_ns;

	if (cfg == NULL) {
		LOG_ERR("Freq config NULL");
		return -EINVAL;
	}

	if ((pp_baudrate_hz == 0) || (pp_baudrate_hz > I3C_SCL_PP_FREQ_MAX_MHZ) ||
	    (od_baudrate_hz == 0) || (od_baudrate_hz > I3C_SCL_OD_FREQ_MAX_MHZ)) {
		LOG_ERR("I3C PP_SCL should within 12.5 Mhz, input: %d", pp_baudrate_hz);
		LOG_ERR("I3C OD_SCL should within 4.17 Mhz, input: %d", od_baudrate_hz);
		return -EINVAL;
	}

	/* Fixed PPLOW = 0 to achieve 50% duty cycle */
	/* pp_freq = ((f_mclkd / 2) / (PPBAUD+1)) */
	freq = i3c_src_clk / 2UL;

	i3c_div = freq / pp_baudrate_hz;
	i3c_div = (i3c_div == 0UL) ? 1UL : i3c_div;
	if (freq / i3c_div > pp_baudrate_hz) {
		i3c_div++;
	}

	if (i3c_div > PPBAUD_DIV_MAX) {
		LOG_ERR("PPBAUD out of range");
		return -EINVAL;
	}

	ppbaud = i3c_div - 1UL;
	freq /= i3c_div;

	/* Check PP low period in spec (should be the same as PPHIGH) */
	pplow_ns = (uint32_t)(NSEC_PER_SEC / (2UL * freq));
	if (pplow_ns < I3C_BUS_TLOW_PP_MIN_NS) {
		LOG_ERR("PPLOW ns out of spec");
		return -EINVAL;
	}

	/* Fixed odhpp = 1 configuration */
	/* odFreq = (2*freq) / (ODBAUD + 2), 1 <= ODBAUD <= 255 */
	i3c_div = (2UL * freq) / od_baudrate_hz;
	i3c_div = i3c_div < 2UL ? 2UL : i3c_div;
	if ((2UL * freq / i3c_div) > od_baudrate_hz) {
		i3c_div++;
	}

	odbaud = i3c_div - 2UL;
	freq = (2UL * freq) / i3c_div; /* For I2C usage in the future */

	/* Check OD low period in spec */
	odlow_ns = (odbaud + 1UL) * pplow_ns;
	if (odlow_ns < I3C_BUS_TLOW_OD_MIN_NS) {
		LOG_ERR("ODBAUD ns out of spec");
		return -EINVAL;
	}

	cfg->pplow = 0;
	cfg->odhpp = 1;
	cfg->ppbaud = ppbaud;
	cfg->odbaud = odbaud;

	return 0;
}

static int npcx_i3c_freq_init(const struct device *dev)
{
	const struct npcx_i3c_config *config = dev->config;
	struct npcx_i3c_data *data = dev->data;
	struct i3c_reg *inst = config->base;
	const struct device *const clk_dev = config->clock_dev;
	struct i3c_config_controller *ctrl_config = &data->common.ctrl_config;
	uint32_t scl_pp = ctrl_config->scl.i3c;
	uint32_t scl_od = config->clocks.i3c_od_scl_hz;
	struct npcx_i3c_timing_cfg timing_cfg;
	uint32_t mclkd;
	int ret;

	ret = clock_control_get_rate(clk_dev, (clock_control_subsys_t)&config->clock_subsys,
				     &mclkd);
	if (ret != 0x0) {
		LOG_ERR("Get I3C source clock fail %d", ret);
		return -EINVAL;
	}

	LOG_DBG("MCLKD: %d", mclkd);
	LOG_DBG("SCL_PP_FEQ MAX: %d", I3C_SCL_PP_FREQ_MAX_MHZ);
	LOG_DBG("SCL_OD_FEQ MAX: %d", I3C_SCL_OD_FREQ_MAX_MHZ);
	LOG_DBG("scl_pp: %d", scl_pp);
	LOG_DBG("scl_od: %d", scl_od);
	LOG_DBG("hdr: %d", ctrl_config->supported_hdr);

	/* MCLKD = MCLK / I3C_DIV(1 or 2)
	 * MCLKD must between 40 mhz to 50 mhz.
	 */
	if (mclkd == MCLKD_FREQ_MHZ(40)) {
		/* Set default I3C_SCL configuration */
		timing_cfg = npcx_def_speed_cfg[NPCX_I3C_BUS_SPEED_40MHZ];
	} else if (mclkd == MCLKD_FREQ_MHZ(45)) {
		/* Set default I3C_SCL configuration */
		timing_cfg = npcx_def_speed_cfg[NPCX_I3C_BUS_SPEED_45MHZ];
	} else if (mclkd == MCLKD_FREQ_MHZ(48)) {
		/* Set default I3C_SCL configuration */
		timing_cfg = npcx_def_speed_cfg[NPCX_I3C_BUS_SPEED_48MHZ];
	} else if (mclkd == MCLKD_FREQ_MHZ(50)) {
		/* Set default I3C_SCL configuration */
		timing_cfg = npcx_def_speed_cfg[NPCX_I3C_BUS_SPEED_50MHZ];
	} else {
		LOG_ERR("Unsupported MCLKD freq for %s.", dev->name);
		return -EINVAL;
	}

	ret = npcx_i3c_get_scl_config(&timing_cfg, mclkd, scl_pp, scl_od);
	if (ret != 0x0) {
		LOG_ERR("Adjust I3C frequency fail");
		return -EINVAL;
	}

	/* Apply SCL_PP and SCL_OD */
	SET_FIELD(inst->MCONFIG, NPCX_I3C_MCONFIG_PPBAUD, timing_cfg.ppbaud);
	SET_FIELD(inst->MCONFIG, NPCX_I3C_MCONFIG_PPLOW, timing_cfg.pplow);
	SET_FIELD(inst->MCONFIG, NPCX_I3C_MCONFIG_ODBAUD, timing_cfg.odbaud);
	if (timing_cfg.odhpp != 0) {
		inst->MCONFIG |= BIT(NPCX_I3C_MCONFIG_ODHPP);
	} else {
		inst->MCONFIG &= ~BIT(NPCX_I3C_MCONFIG_ODHPP);
	}
	SET_FIELD(inst->MCONFIG, NPCX_I3C_MCONFIG_I2CBAUD, I3C_BUS_I2C_BAUD_RATE_FAST_MODE);

	LOG_DBG("ppbaud: %d", GET_FIELD(inst->MCONFIG, NPCX_I3C_MCONFIG_PPBAUD));
	LOG_DBG("odbaud: %d", GET_FIELD(inst->MCONFIG, NPCX_I3C_MCONFIG_ODBAUD));
	LOG_DBG("pplow: %d", GET_FIELD(inst->MCONFIG, NPCX_I3C_MCONFIG_PPLOW));
	LOG_DBG("odhpp: %d", IS_BIT_SET(inst->MCONFIG, NPCX_I3C_MCONFIG_ODHPP));
	LOG_DBG("i2cbaud: %d", GET_FIELD(inst->MCONFIG, NPCX_I3C_MCONFIG_I2CBAUD));

	return 0;
}

static int npcx_i3c_apply_cntlr_config(const struct device *dev)
{
	const struct npcx_i3c_config *config = dev->config;
	struct i3c_reg *inst = config->base;
	const struct device *const clk_dev = config->clock_dev;
	int idx_module = GET_MODULE_ID(config->instance_id);
	uint32_t apb4_rate;
	uint8_t bamatch;
	int ret;

	/* I3C module mdma cotroller or target mode select */
	npcx_i3c_target_sel(idx_module, false);

	/* Disable all interrupts */
	npcx_i3c_interrupt_all_disable(inst);

	/* Initial baudrate. PPLOW=1, PPBAUD, ODHPP=1, ODBAUD */
	if (npcx_i3c_freq_init(dev) != 0x0) {
		return -EINVAL;
	}

	/* Enable external high-keeper */
	SET_FIELD(inst->MCONFIG, NPCX_I3C_MCONFIG_HKEEP, MCONFIG_HKEEP_EXT_SDA_SCL);
	/* Enable open-drain stop */
	inst->MCONFIG |= BIT(NPCX_I3C_MCONFIG_ODSTOP);
	/* Enable timeout */
	inst->MCONFIG &= ~BIT(NPCX_I3C_MCONFIG_DISTO);
	/* Flush tx and tx FIFO buffer */
	npcx_i3c_fifo_flush(inst);

	/* Set bus available match value in target register */
	ret = clock_control_get_rate(clk_dev, (clock_control_subsys_t)&config->ref_clk_subsys,
				     &apb4_rate);
	LOG_DBG("APB4_CLK: %d", apb4_rate);

	if (ret != 0x0) {
		LOG_ERR("%s: Get APB4 source clock fail %d", __func__, ret);
		return -EINVAL;
	}

	bamatch = get_bus_available_match_val(apb4_rate);
	LOG_DBG("BAMATCH: %d", bamatch);

	SET_FIELD(inst->CONFIG, NPCX_I3C_CONFIG_BAMATCH, bamatch);

	return 0;
}

static int npcx_i3c_apply_target_config(const struct device *dev)
{
	const struct npcx_i3c_config *config = dev->config;
	struct npcx_i3c_data *data = dev->data;
	struct i3c_config_target *config_target = &data->config_target;
	struct i3c_reg *inst = config->base;
	const struct device *const clk_dev = config->clock_dev;
	uint32_t apb4_rate;
	uint8_t bamatch;
	int idx_module = GET_MODULE_ID(config->instance_id);
	int ret;
	uint64_t pid;

	/* I3C module mdma cotroller or target mode select */
	npcx_i3c_target_sel(idx_module, true);

	/* Set bus available match value in target register */
	ret = clock_control_get_rate(clk_dev, (clock_control_subsys_t)&config->ref_clk_subsys,
				     &apb4_rate);
	LOG_DBG("APB4_CLK: %d", apb4_rate);

	if (ret != 0x0) {
		LOG_ERR("%s: Get APB4 source clock fail %d", __func__, ret);
		return -EINVAL;
	}

	bamatch = get_bus_available_match_val(apb4_rate);
	LOG_DBG("BAMATCH: %d", bamatch);
	SET_FIELD(inst->CONFIG, NPCX_I3C_CONFIG_BAMATCH, bamatch);

	/* Set Provisional ID */
	pid = config_target->pid;

	/* PID[47:33] MIPI manufacturer ID */
	SET_FIELD(inst->VENDORID, NPCX_I3C_VENDORID_VID, (uint32_t)GET_PID_VENDOR_ID(pid));

	/* PID[32] Vendor fixed value(0) or random value(1) */
	if (config_target->pid_random) {
		inst->CONFIG |= BIT(NPCX_I3C_CONFIG_IDRAND);
	} else {
		inst->CONFIG &= ~BIT(NPCX_I3C_CONFIG_IDRAND);
	}

	/* PID[31:0] vendor fixed value */
	inst->PARTNO = (uint32_t)GET_PID_PARTNO(pid);

	LOG_DBG("pid: %#llx", pid);
	LOG_DBG("vendro id: %#x", (uint32_t)GET_PID_VENDOR_ID(pid));
	LOG_DBG("id type: %d", (uint32_t)GET_PID_ID_TYP(pid));
	LOG_DBG("partno: %#x", (uint32_t)GET_PID_PARTNO(pid));

	SET_FIELD(inst->IDEXT, NPCX_I3C_IDEXT_DCR, config_target->dcr);
	SET_FIELD(inst->IDEXT, NPCX_I3C_IDEXT_BCR, config_target->bcr);
	SET_FIELD(inst->CONFIG, NPCX_I3C_CONFIG_SADDR, config_target->static_addr);
	SET_FIELD(inst->CONFIG, NPCX_I3C_CONFIG_HDRCMD, CFG_HDRCMD_RD_FROM_FIFIO);
	SET_FIELD(inst->MAXLIMITS, NPCX_I3C_MAXLIMITS_MAXRD, (config_target->max_read_len) & 0xfff);
	SET_FIELD(inst->MAXLIMITS, NPCX_I3C_MAXLIMITS_MAXWR,
		  (config_target->max_write_len) & 0xfff);

	/* Ignore DA and detect all START and STOP */
	inst->CONFIG &= ~BIT(NPCX_I3C_CONFIG_MATCHSS);

	/* Enable the target interrupt events */
	npcx_i3c_enable_target_interrupt(dev, true);

	return 0;
}

static void npcx_i3c_dev_init(const struct device *dev)
{
	const struct npcx_i3c_config *config = dev->config;
	struct npcx_i3c_data *data = dev->data;
	struct i3c_reg *inst = config->base;
	struct i3c_config_controller *config_cntlr = &data->common.ctrl_config;
	struct i3c_config_target *config_target = &data->config_target;
	int idx_module = GET_MODULE_ID(config->instance_id);

	/* Reset I3C module */
	reset_line_toggle_dt(&config->reset);

	if (I3C_BCR_DEVICE_ROLE(config_target->bcr) == I3C_BCR_DEVICE_ROLE_I3C_CONTROLLER_CAPABLE) {
		npcx_i3c_apply_cntlr_config(dev);
		npcx_i3c_apply_target_config(dev);

		if (config_cntlr->is_secondary) {
			/* Secondary controller enable, so boot as a target */
			SET_FIELD(inst->MCONFIG, NPCX_I3C_MCONFIG_CTRENA, MCONFIG_CTRENA_CAPABLE);
			inst->CONFIG |= BIT(NPCX_I3C_CONFIG_TGTENA); /* Target mode enable */
		} else {
			npcx_i3c_target_sel(idx_module, false); /* Set mdma as controlelr */
			/* Primary Controller enable */
			SET_FIELD(inst->MCONFIG, NPCX_I3C_MCONFIG_CTRENA, MCONFIG_CTRENA_ON);
		}
	} else {
		npcx_i3c_apply_target_config(dev);
		SET_FIELD(inst->MCONFIG, NPCX_I3C_MCONFIG_CTRENA,
			  MCONFIG_CTRENA_OFF);               /* Controller mode off */
		inst->CONFIG |= BIT(NPCX_I3C_CONFIG_TGTENA); /* Target mode enable */
	}
}

static int npcx_i3c_configure(const struct device *dev, enum i3c_config_type type, void *config)
{
	struct npcx_i3c_data *dev_data = dev->data;
	struct i3c_config_controller *config_cntlr;
	struct i3c_config_target *config_target;

	if (config == NULL) {
		LOG_ERR("%s: config is NULL", __func__);
		return -EINVAL;
	}

	if (type == I3C_CONFIG_CONTROLLER) {
		config_cntlr = config;
		/*
		 * Check for valid configuration parameters.
		 * Currently, must be the primary controller.
		 */
		if (config_cntlr->scl.i3c == 0U) {
			LOG_ERR("%s: configure controller failed", __func__);
			return -EINVAL;
		}

		/* Save requested config to dev */
		(void)memcpy(&dev_data->common.ctrl_config, config_cntlr, sizeof(*config_cntlr));

		return npcx_i3c_apply_cntlr_config(dev);
	} else if (type == I3C_CONFIG_TARGET) {
		config_target = config;

		if (config_target->pid == 0) {
			LOG_ERR("%s: configure target failed", __func__);
			return -EINVAL;
		}

		return npcx_i3c_apply_target_config(dev);
	}

	LOG_ERR("Config type not supported, %d", type);

	return -EINVAL;
}

static int npcx_i3c_config_get(const struct device *dev, enum i3c_config_type type, void *config)
{
	struct npcx_i3c_data *data = dev->data;

	if (config == NULL) {
		return -EINVAL;
	}

	if (type == I3C_CONFIG_CONTROLLER) {
		(void)memcpy(config, &data->common.ctrl_config, sizeof(data->common.ctrl_config));
	} else if (type == I3C_CONFIG_TARGET) {
		(void)memcpy(config, &data->config_target, sizeof(data->config_target));
	} else {
		return -EINVAL;
	}

	return 0;
}

static void npcx_i3c_target_isr(const struct device *dev)
{
	struct npcx_i3c_data *data = dev->data;
	const struct npcx_i3c_config *config = dev->config;
	struct i3c_config_target *config_tgt = &data->config_target;
	struct i3c_target_config *target_config = data->target_config;
	struct i3c_reg *inst = config->base;
	const struct i3c_target_callbacks *target_cb = data->target_config->callbacks;

#ifdef CONFIG_I3C_NPCX_DMA
	struct mdma_reg *mdma_inst = config->mdma_base;

	/* Check mdma read end (for write request) */
	if (IS_BIT_SET(mdma_inst->MDMA_CTL0, NPCX_MDMA_CTL_TC)) {
		/* Disable target read operation */
		npcx_i3c_target_disable_mdmafb(dev);

		/* End of mdma read (write request) */
		if (get_oper_state(dev) == NPCX_I3C_OP_STATE_WR) {
#ifdef CONFIG_I3C_TARGET_BUFFER_MODE
			if ((target_cb != NULL) && (target_cb->buf_write_received_cb != NULL)) {
				target_cb->buf_write_received_cb(
					data->target_config, data->mdma_rx_buf,
					npcx_i3c_target_get_mdmafb_count(dev));
			}
#endif
		} else {
			LOG_ERR("%s: write request TC=1, operation state error, %d", __func__,
				data->oper_state);
		}

		set_oper_state(dev, NPCX_I3C_OP_STATE_IDLE);
	}
#endif /* CONFIG_I3C_NPCX_DMA */

	while (inst->INTMASKED) {
		/* Check STOP detected */
		if (IS_BIT_SET(inst->INTMASKED, NPCX_I3C_INTMASKED_STOP)) {

			if (IS_BIT_SET(inst->INTMASKED, NPCX_I3C_INTMASKED_START)) {
				inst->STATUS = BIT(NPCX_I3C_STATUS_START);
			}

#ifdef CONFIG_I3C_NPCX_DMA
			/* The end of xfer is a stop.
			 * For write request: check whether mdma TC is done or still busy.
			 * For read request: disable the mdma operation.
			 */
			if ((get_oper_state(dev) == NPCX_I3C_OP_STATE_WR) ||
			    (get_oper_state(dev) == NPCX_I3C_OP_STATE_RD)) {
				if (npcx_i3c_target_xfer_end_handle(dev) != 0) {
					LOG_ERR("xfer end handle failed after stop, op state=%d",
						get_oper_state(dev));
				}
			}

			inst->STATUS = BIT(NPCX_I3C_STATUS_STOP);
#endif

			/* Notify upper layer a STOP condition received */
			if ((target_cb != NULL) && (target_cb->stop_cb != NULL)) {
				target_cb->stop_cb(data->target_config);
			}

			/* Clear DA matched status and re-enable interrupt */
			inst->STATUS = BIT(NPCX_I3C_STATUS_MATCHED);
			inst->INTSET = BIT(NPCX_I3C_INTSET_MATCHED);
			set_oper_state(dev, NPCX_I3C_OP_STATE_IDLE);
		}

		/* Check START or Sr detected */
		if (IS_BIT_SET(inst->INTMASKED, NPCX_I3C_INTMASKED_START)) {
			/* The end of xfer is a Sr */
			if ((get_oper_state(dev) == NPCX_I3C_OP_STATE_WR) ||
			    (get_oper_state(dev) == NPCX_I3C_OP_STATE_RD)) {
				if (-EBUSY == npcx_i3c_target_xfer_end_handle(dev)) {
					return;
				}
			}

			inst->STATUS = BIT(NPCX_I3C_STATUS_START);
		}

		if (IS_BIT_SET(inst->INTMASKED, NPCX_I3C_INTMASKED_TGTRST)) {
			inst->STATUS = BIT(NPCX_I3C_STATUS_TGTRST);
		}

		/* Check error or warning has occurred */
		if (IS_BIT_SET(inst->INTMASKED, NPCX_I3C_INTMASKED_ERRWARN)) {
			LOG_ERR("%s: Error %#x", __func__, inst->ERRWARN);
			inst->ERRWARN = inst->ERRWARN;
		}

		/* Check incoming header matched target dynamic address */
		if (IS_BIT_SET(inst->INTMASKED, NPCX_I3C_INTMASKED_MATCHED)) {
			if (get_oper_state(dev) != NPCX_I3C_OP_STATE_IBI) {
				/* The current bus request is an SDR mode read or write */
				if (IS_BIT_SET(inst->STATUS, NPCX_I3C_STATUS_STREQRD)) {
					/* SDR read request */
					set_oper_state(dev, NPCX_I3C_OP_STATE_RD);

					/* Emit read request callback */
#if CONFIG_I3C_TARGET_BUFFER_MODE
					/* It will be too late to enable mdma here, use
					 * target_tx_write() to write tx data into fifo before
					 * controller send read request.
					 */
					if ((target_cb != NULL) &&
					    (target_cb->buf_read_requested_cb != NULL)) {
						target_cb->buf_read_requested_cb(
							data->target_config, NULL, NULL, NULL);
					}
#endif
				} else {
					/* SDR write request */
					set_oper_state(dev, NPCX_I3C_OP_STATE_WR);

					/* Emit write request callback */
					if ((target_cb != NULL) &&
					    (target_cb->write_requested_cb != NULL)) {
						target_cb->write_requested_cb(data->target_config);
					}

					npcx_i3c_target_rx_read(dev);
				}
			}

			/* If CONFIG.MATCHSS=1, MATCHED bit must remain 1 to detect next start
			 * or stop.
			 *
			 * Clear the status bit in STOP or START handler.
			 */
			if (IS_BIT_SET(inst->CONFIG, NPCX_I3C_CONFIG_MATCHSS)) {
				inst->INTCLR = BIT(NPCX_I3C_INTCLR_MATCHED);
			} else {
				inst->STATUS = BIT(NPCX_I3C_STATUS_MATCHED);
			}
		}

		/* Check dynamic address changed */
		if (IS_BIT_SET(inst->INTMASKED, NPCX_I3C_INTMASKED_DACHG)) {
			inst->STATUS = BIT(NPCX_I3C_STATUS_DACHG);

			if (IS_BIT_SET(inst->DYNADDR, NPCX_I3C_DYNADDR_DAVALID)) {
				if (target_config != NULL) {
					config_tgt->dynamic_addr =
						GET_FIELD(inst->DYNADDR, NPCX_I3C_DYNADDR_DADDR);
				}
			}
		}

		/* CCC 'not' automatically handled was received */
		if (IS_BIT_SET(inst->INTMASKED, NPCX_I3C_INTMASKED_CCC)) {
			inst->STATUS = BIT(NPCX_I3C_STATUS_CCC);
		}

		/* HDR command, address match */
		if (IS_BIT_SET(inst->INTMASKED, NPCX_I3C_INTMASKED_HDRMATCH)) {
			inst->STATUS = BIT(NPCX_I3C_STATUS_HDRMATCH);
		}

		/* CCC handled (handled by IP) */
		if (IS_BIT_SET(inst->INTMASKED, NPCX_I3C_INTMASKED_CHANDLED)) {
			inst->STATUS = BIT(NPCX_I3C_STATUS_CHANDLED);
		}

		/* Event requested. IBI, hot-join, bus control */
		if (IS_BIT_SET(inst->INTMASKED, NPCX_I3C_INTMASKED_EVENT)) {
			inst->STATUS = BIT(NPCX_I3C_STATUS_EVENT);

			if (GET_FIELD(inst->STATUS, NPCX_I3C_STATUS_EVDET) ==
			    STATUS_EVDET_REQ_SENT_ACKED) {
				k_sem_give(&data->target_event_lock_sem);
			}
		}
	}

	/* Secondary controller (Controller register).
	 * Check I3C now bus controller.
	 * Disable target mode if target switch to controller mode success.
	 */
	if (IS_BIT_SET(inst->MINTMASKED, NPCX_I3C_MINTMASKED_NOWCNTLR)) {
		inst->MSTATUS = BIT(NPCX_I3C_MSTATUS_NOWCNTLR); /* W1C */
		inst->CONFIG &= ~BIT(NPCX_I3C_CONFIG_TGTENA);   /* Disable target mode */
	}
}

static void npcx_i3c_isr(const struct device *dev)
{
	const struct npcx_i3c_config *config = dev->config;
	struct i3c_reg *inst = config->base;

	if (IS_BIT_SET(inst->CONFIG, NPCX_I3C_CONFIG_TGTENA)) {
		npcx_i3c_target_isr(dev);
		return;
	}

#ifdef CONFIG_I3C_NPCX_DMA
	struct mdma_reg *mdma_inst = config->mdma_base;

	/* Controller write end */
	if (IS_BIT_SET(inst->MSTATUS, NPCX_I3C_MSTATUS_COMPLETE)) {
		inst->MSTATUS = BIT(NPCX_I3C_MSTATUS_COMPLETE); /* W1C */

		/* MDMA write */
		if (get_oper_state(dev) == NPCX_I3C_OP_STATE_WR) {
			i3c_ctrl_notify(dev);
			return;
		}
	}

	/* Controller read end */
	if (IS_BIT_SET(mdma_inst->MDMA_CTL0, NPCX_MDMA_CTL_TC)) {
		mdma_inst->MDMA_CTL0 &= ~BIT(NPCX_MDMA_CTL_TC); /* W0C */

		/* MDMA read */
		if (get_oper_state(dev) == NPCX_I3C_OP_STATE_RD) {
			i3c_ctrl_notify(dev);
			return;
		}
	}
#endif /* CONFIG_I3C_NPCX_DMA */

#ifdef CONFIG_I3C_USE_IBI
	int ret;

	/* Target start detected */
	if (IS_BIT_SET(inst->MSTATUS, NPCX_I3C_MSTATUS_TGTSTART)) {
		LOG_DBG("ISR TGTSTART !");

		/* Disable further target initiated IBI interrupt */
		inst->MINTCLR = BIT(NPCX_I3C_MINTCLR_TGTSTART);
		/* Clear TGTSTART interrupt */
		inst->MSTATUS = BIT(NPCX_I3C_MSTATUS_TGTSTART);

		/* Handle IBI in workqueue */
		ret = i3c_ibi_work_enqueue_cb(dev, npcx_i3c_ibi_work);
		if (ret < 0) {
			LOG_ERR("Enqueuing ibi work fail, ret %d", ret);
			inst->MINTSET = BIT(NPCX_I3C_MINTSET_TGTSTART);
		}
	}
#endif /* CONFIG_I3C_USE_IBI */
}

static int npcx_i3c_init(const struct device *dev)
{
	const struct npcx_i3c_config *config = dev->config;
	struct npcx_i3c_data *data = dev->data;
	struct i3c_config_controller *config_cntlr = &data->common.ctrl_config;
	const struct device *const clk_dev = config->clock_dev;
	struct i3c_reg *inst = config->base;
	int ret;

	/* Check clock device ready */
	if (!device_is_ready(clk_dev)) {
		LOG_ERR("%s Clk device not ready", clk_dev->name);
		return -ENODEV;
	}

	/* Set I3C_PD operational */
	ret = clock_control_on(clk_dev, (clock_control_subsys_t)&config->clock_subsys);
	if (ret < 0) {
		LOG_ERR("Turn on I3C clock fail %d", ret);
		return ret;
	}

#ifdef CONFIG_I3C_NPCX_DMA
	ret = clock_control_on(clk_dev, (clock_control_subsys_t)&config->mdma_clk_subsys);
	if (ret < 0) {
		LOG_ERR("Turn on I3C MDMA clock fail %d", ret);
		return ret;
	}
#endif

	/* Apply pin-muxing */
	ret = pinctrl_apply_state(config->pincfg, PINCTRL_STATE_DEFAULT);
	if (ret != 0) {
		LOG_ERR("Apply pinctrl fail %d", ret);
		return ret;
	}

	/* Lock initial */
	k_mutex_init(&data->lock_mutex);
	k_sem_init(&data->sync_sem, 0, 1);
	k_sem_init(&data->ibi_lock_sem, 1, 1);
	k_sem_init(&data->target_lock_sem, 1, 1);
	k_sem_init(&data->target_event_lock_sem, 1, 1);

	ret = i3c_addr_slots_init(dev);
	if (ret != 0) {
		LOG_ERR("Addr slots init fail %d", ret);
		return ret;
	}

	/* Set controller default configuration */
	config_cntlr->supported_hdr = I3C_MSG_HDR_DDR;        /* HDR-DDR mode is supported. */
	config_cntlr->scl.i3c = config->clocks.i3c_pp_scl_hz; /* Set I3C frequency */

	/* Initial I3C device as controller or target */
	npcx_i3c_dev_init(dev);

	if (GET_FIELD(inst->MCONFIG, NPCX_I3C_MCONFIG_CTRENA) == MCONFIG_CTRENA_ON) {
		/* Just in case the bus is not in idle. */
		ret = npcx_i3c_recover_bus(dev);
		if (ret != 0) {
			LOG_ERR("Apply i3c_recover_bus() fail %d", ret);
			return ret;
		}
	}

	/* Configure interrupt */
	config->irq_config_func(dev);

	/* Initialize driver status machine */
	set_oper_state(dev, NPCX_I3C_OP_STATE_IDLE);

	/* Check I3C is controller mode and target device exist in device tree */
	if ((config->common.dev_list.num_i3c > 0) &&
	    GET_FIELD(inst->MCONFIG, NPCX_I3C_MCONFIG_CTRENA) == MCONFIG_CTRENA_ON) {
		/* Perform bus initialization */
		ret = i3c_bus_init(dev, &config->common.dev_list);
		if (ret != 0) {
			LOG_ERR("Apply i3c_bus_init() fail %d", ret);
			return ret;
		}
	}

	return 0;
}

static DEVICE_API(i3c, npcx_i3c_driver_api) = {
	.configure = npcx_i3c_configure,
	.config_get = npcx_i3c_config_get,

	.recover_bus = npcx_i3c_recover_bus,

	.do_daa = npcx_i3c_do_daa,
	.do_ccc = npcx_i3c_do_ccc,

	.i3c_device_find = npcx_i3c_device_find,

	.i3c_xfers = npcx_i3c_transfer,

	.target_tx_write = npcx_i3c_target_tx_write,
	.target_register = npcx_i3c_target_register,
	.target_unregister = npcx_i3c_target_unregister,

#ifdef CONFIG_I3C_USE_IBI
	.ibi_enable = npcx_i3c_ibi_enable,
	.ibi_disable = npcx_i3c_ibi_disable,

	.ibi_raise = npcx_i3c_target_ibi_raise,
#endif

#ifdef CONFIG_I3C_RTIO
	.iodev_submit = i3c_iodev_submit_fallback,
#endif
};

#define DT_INST_TGT_PID_PROP_OR(id, prop, idx)                                                     \
	COND_CODE_1(DT_INST_PROP_HAS_IDX(id, prop, idx), (DT_INST_PROP_BY_IDX(id, prop, idx)), (0))
#define DT_INST_TGT_PID_RAND_PROP_OR(id, prop, idx)                                                \
	COND_CODE_1(DT_INST_PROP_HAS_IDX(id, prop, idx),                                           \
		    IS_BIT_SET(DT_INST_PROP_BY_IDX(id, prop, 0), 0), (0))

#define I3C_NPCX_DEVICE(id)                                                                        \
	PINCTRL_DT_INST_DEFINE(id);                                                                \
	static void npcx_i3c_config_func_##id(const struct device *dev)                            \
	{                                                                                          \
		IRQ_CONNECT(DT_INST_IRQN(id), DT_INST_IRQ(id, priority), npcx_i3c_isr,             \
			    DEVICE_DT_INST_GET(id), 0);                                            \
		irq_enable(DT_INST_IRQN(id));                                                      \
	};                                                                                         \
	static struct i3c_device_desc npcx_i3c_device_array_##id[] = I3C_DEVICE_ARRAY_DT_INST(id); \
	static struct i3c_i2c_device_desc npcx_i3c_i2c_device_array_##id[] =                       \
		I3C_I2C_DEVICE_ARRAY_DT_INST(id);                                                  \
	static const struct npcx_i3c_config npcx_i3c_config_##id = {                               \
		.base = (struct i3c_reg *)DT_INST_REG_ADDR(id),                                    \
		.clock_dev = DEVICE_DT_GET(NPCX_CLK_CTRL_NODE),                                    \
		.reset = RESET_DT_SPEC_INST_GET(id),                                               \
		.clock_subsys = NPCX_DT_CLK_CFG_ITEM_BY_NAME(id, mclkd),                           \
		.ref_clk_subsys = NPCX_DT_CLK_CFG_ITEM_BY_NAME(id, apb4),                          \
		.irq_config_func = npcx_i3c_config_func_##id,                                      \
		.common.dev_list.i3c = npcx_i3c_device_array_##id,                                 \
		.common.dev_list.num_i3c = ARRAY_SIZE(npcx_i3c_device_array_##id),                 \
		.common.dev_list.i2c = npcx_i3c_i2c_device_array_##id,                             \
		.common.dev_list.num_i2c = ARRAY_SIZE(npcx_i3c_i2c_device_array_##id),             \
		.pincfg = PINCTRL_DT_INST_DEV_CONFIG_GET(id),                                      \
		.instance_id = DT_INST_PROP(id, instance_id),                                      \
		.clocks.i3c_pp_scl_hz = DT_INST_PROP_OR(id, i3c_scl_hz, 0),                        \
		.clocks.i3c_od_scl_hz = DT_INST_PROP_OR(id, i3c_od_scl_hz, 0),                     \
		IF_ENABLED(CONFIG_I3C_NPCX_DMA, (                                                  \
			.mdma_clk_subsys = NPCX_DT_CLK_CFG_ITEM_BY_IDX(id, 2),                     \
		))                                                                                 \
		IF_ENABLED(CONFIG_I3C_NPCX_DMA, (                                                  \
			.mdma_base = (struct mdma_reg *)DT_INST_REG_ADDR_BY_IDX(id, 1),            \
		)) };                                                                              \
	static struct npcx_i3c_data npcx_i3c_data_##id = {                                         \
		.common.ctrl_config.is_secondary = DT_INST_PROP_OR(id, secondary, false),          \
		.config_target.static_addr = DT_INST_PROP_OR(id, static_address, 0),               \
		.config_target.pid = ((uint64_t)DT_INST_TGT_PID_PROP_OR(id, tgt_pid, 0) << 32) |   \
				     DT_INST_TGT_PID_PROP_OR(id, tgt_pid, 1),                      \
		.config_target.pid_random = DT_INST_TGT_PID_RAND_PROP_OR(id, tgt_pid, 0),          \
		.config_target.bcr = DT_INST_PROP(id, bcr),                                        \
		.config_target.dcr = DT_INST_PROP_OR(id, dcr, 0),                                  \
		.config_target.max_read_len = DT_INST_PROP_OR(id, maximum_read, 0),                \
		.config_target.max_write_len = DT_INST_PROP_OR(id, maximum_write, 0),              \
		.config_target.supported_hdr = false,                                              \
	};                                                                                         \
	DEVICE_DT_INST_DEFINE(id, npcx_i3c_init, NULL, &npcx_i3c_data_##id, &npcx_i3c_config_##id, \
			      POST_KERNEL, CONFIG_I3C_CONTROLLER_INIT_PRIORITY,                    \
			      &npcx_i3c_driver_api);

DT_INST_FOREACH_STATUS_OKAY(I3C_NPCX_DEVICE)
