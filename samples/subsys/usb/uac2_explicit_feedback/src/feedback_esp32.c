/*
 * Copyright The Zephyr Project Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Explicit feedback on ESP32-P4: PCNT unit 0 counts rising I2S WS edges
 * (one per sample frame, routed from the WS pad by pinctrl), and the
 * (micro)frame number comes from the DWC2 DSTS register. Both are free
 * running, so the feedback value is the edge count difference over the
 * frame count difference (USB 2.0 5.12.4.2). The SOF callback runs in the
 * USB thread, so its latency only shifts the window ends and does not
 * accumulate.
 */

#include <zephyr/kernel.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/clock_control.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>
#include <zephyr/usb/usbd.h>
#include <hal/pcnt_ll.h>
#include <usb_dwc2_hw.h>
#include "feedback.h"

LOG_MODULE_REGISTER(feedback, LOG_LEVEL_INF);

#define PCNT_NODE DT_NODELABEL(pcnt)
#define PCNT_UNIT 0U
#define PCNT_CHAN 0U
/* The counter returns to 0 when it reaches the high limit */
#define PCNT_MODULUS INT16_MAX

#define UDC_NODE DT_NODELABEL(zephyr_udc0)

#define FEEDBACK_FS_K     10
#define FEEDBACK_FS_SHIFT 4
#define FEEDBACK_HS_K     13
#define FEEDBACK_HS_SHIFT 3

/* Feedback is recomputed every 2^(K - P) (micro)frames */
#define FEEDBACK_P 3

/* DSTS.SOFFN holds the frame number at full speed and the microframe
 * number at high speed.
 */
#define FS_FRAME_MODULUS 2048U
#define HS_FRAME_MODULUS 16384U

static struct feedback_ctx {
	uint32_t fb_value;
	uint32_t frames;
	uint32_t edges;
	uint16_t last_frame;
	int16_t last_count;
	bool high_speed;
	bool running;
} fb_ctx;

static pcnt_dev_t *const pcnt_hw = (pcnt_dev_t *)DT_REG_ADDR(PCNT_NODE);

static void read_counters(uint16_t *frame, int16_t *count)
{
	struct usb_dwc2_reg *const base = (struct usb_dwc2_reg *)DT_REG_ADDR(UDC_NODE);
	unsigned int key = irq_lock();

	*frame = usb_dwc2_get_dsts_soffn(sys_read32((mem_addr_t)&base->dsts));
	*count = pcnt_ll_get_count(pcnt_hw, PCNT_UNIT);
	irq_unlock(key);
}

static uint32_t nominal_feedback_value(struct feedback_ctx *ctx)
{
	if (ctx->high_speed) {
		return (SAMPLE_RATE / 8000) << (FEEDBACK_HS_K + FEEDBACK_HS_SHIFT);
	}

	return (SAMPLE_RATE / 1000) << (FEEDBACK_FS_K + FEEDBACK_FS_SHIFT);
}

static uint32_t feedback_period(struct feedback_ctx *ctx)
{
	return BIT(ctx->high_speed ? FEEDBACK_HS_K - FEEDBACK_P : FEEDBACK_FS_K - FEEDBACK_P);
}

struct feedback_ctx *feedback_init(void)
{
	const struct device *clk = DEVICE_DT_GET(DT_CLOCKS_CTLR(PCNT_NODE));
	int err;

	err = clock_control_on(clk, (clock_control_subsys_t)DT_CLOCKS_CELL(PCNT_NODE, offset));
	if (err != 0 && err != -EALREADY) {
		LOG_ERR("PCNT clock enable failed: %d", err);
	}

	pcnt_ll_stop_count(pcnt_hw, PCNT_UNIT);
	pcnt_ll_disable_all_events(pcnt_hw, PCNT_UNIT);
	pcnt_ll_set_high_limit_value(pcnt_hw, PCNT_UNIT, PCNT_MODULUS);
	pcnt_ll_set_low_limit_value(pcnt_hw, PCNT_UNIT, INT16_MIN);
	/* Count rising WS edges only; no control signal is routed */
	pcnt_ll_set_edge_action(pcnt_hw, PCNT_UNIT, PCNT_CHAN, PCNT_CHANNEL_EDGE_ACTION_INCREASE,
				PCNT_CHANNEL_EDGE_ACTION_HOLD);
	pcnt_ll_set_level_action(pcnt_hw, PCNT_UNIT, PCNT_CHAN, PCNT_CHANNEL_LEVEL_ACTION_KEEP,
				 PCNT_CHANNEL_LEVEL_ACTION_KEEP);
	pcnt_ll_set_edge_action(pcnt_hw, PCNT_UNIT, 1, PCNT_CHANNEL_EDGE_ACTION_HOLD,
				PCNT_CHANNEL_EDGE_ACTION_HOLD);
	pcnt_ll_enable_glitch_filter(pcnt_hw, PCNT_UNIT, false);
	pcnt_ll_clear_count(pcnt_hw, PCNT_UNIT);
	pcnt_ll_start_count(pcnt_hw, PCNT_UNIT);

	feedback_reset_ctx(&fb_ctx, false);

	return &fb_ctx;
}

void feedback_process(struct feedback_ctx *ctx)
{
	uint32_t modulus = ctx->high_speed ? HS_FRAME_MODULUS : FS_FRAME_MODULUS;
	uint32_t shift = ctx->high_speed ? FEEDBACK_HS_K + FEEDBACK_HS_SHIFT
					 : FEEDBACK_FS_K + FEEDBACK_FS_SHIFT;
	uint16_t frame;
	int16_t count;
	int32_t d_count;

	if (!ctx->running) {
		return;
	}

	read_counters(&frame, &count);

	ctx->frames += (frame - ctx->last_frame + modulus) % modulus;
	d_count = count - ctx->last_count;
	if (d_count < 0) {
		d_count += PCNT_MODULUS;
	}
	ctx->edges += d_count;
	ctx->last_frame = frame;
	ctx->last_count = count;

	if (ctx->frames < feedback_period(ctx)) {
		return;
	}

	ctx->fb_value = (uint32_t)(((uint64_t)ctx->edges << shift) / ctx->frames);

	LOG_DBG("fb 0x%08x (%u edges / %u frames)", ctx->fb_value, ctx->edges, ctx->frames);

	ctx->frames = 0;
	ctx->edges = 0;
}

void feedback_reset_ctx(struct feedback_ctx *ctx, bool microframes)
{
	ctx->high_speed = microframes;
	ctx->running = false;
	ctx->frames = 0;
	ctx->edges = 0;
	ctx->fb_value = nominal_feedback_value(ctx);
}

void feedback_start(struct feedback_ctx *ctx, int i2s_blocks_queued, bool microframes)
{
	ARG_UNUSED(i2s_blocks_queued);

	ctx->high_speed = microframes;
	ctx->frames = 0;
	ctx->edges = 0;
	ctx->fb_value = nominal_feedback_value(ctx);
	read_counters(&ctx->last_frame, &ctx->last_count);
	ctx->running = true;
}

uint32_t feedback_value(struct feedback_ctx *ctx)
{
	return ctx->fb_value;
}
