// SPDX-License-Identifier: BSD-3-Clause
//
// Copyright 2019 NXP
//
// Author: Daniel Baluta <daniel.baluta@nxp.com>

#include <platform/memory.h>
#include <platform/mailbox.h>
#include <sof/edma.h>
#include <platform/dma.h>
#include <platform/dai.h>
#include <platform/clk.h>
#include <platform/timer.h>
#include <platform/mu.h>
#include <ipc/info.h>
#include <sof/mailbox.h>
#include <sof/dai.h>
#include <sof/interrupt.h>
#include <sof/sof.h>
#include <sof/clk.h>
#include <sof/schedule/schedule.h>
#include <sof/ipc.h>
#include <sof/trace.h>
#include <sof/agent.h>
#include <sof/dma-trace.h>
#include <sof/audio/component.h>
#include <sof/cpu.h>
#include <sof/notifier.h>
#include <config.h>
#include <string.h>
#include <version.h>

static const struct sof_ipc_fw_ready ready
	__attribute__((section(".fw_ready"))) = {
	.hdr = {
		.cmd = SOF_IPC_FW_READY,
		.size = sizeof(struct sof_ipc_fw_ready),
	},
	/* dspbox is for DSP initiated IPC, hostbox is for host initiated IPC */
	.version = {
		.hdr.size = sizeof(struct sof_ipc_fw_version),
		.micro = SOF_MICRO,
		.minor = SOF_MINOR,
		.major = SOF_MAJOR,
#ifdef DEBUG_BUILD
		/* only added in debug for reproducability in releases */
		.build = SOF_BUILD,
		.date = __DATE__,
		.time = __TIME__,
#endif
		.tag = SOF_TAG,
		.abi_version = SOF_ABI_VERSION,
	},
	.flags = DEBUG_SET_FW_READY_FLAGS,
};

#define NUM_IMX_WINDOWS		6

static const struct sof_ipc_window sram_window = {
	.ext_hdr	= {
		.hdr.cmd = SOF_IPC_FW_READY,
		.hdr.size = sizeof(struct sof_ipc_window) +
			sizeof(struct sof_ipc_window_elem) * NUM_IMX_WINDOWS,
		.type	= SOF_IPC_EXT_WINDOW,
	},
	.num_windows	= NUM_IMX_WINDOWS,
	.window	= {
		{
			.type	= SOF_IPC_REGION_UPBOX,
			.id	= 0,	/* map to host window 0 */
			.flags	= 0, // TODO: set later
			.size	= MAILBOX_DSPBOX_SIZE,
			.offset	= MAILBOX_DSPBOX_OFFSET,
		},
		{
			.type	= SOF_IPC_REGION_DOWNBOX,
			.id	= 0,	/* map to host window 0 */
			.flags	= 0, // TODO: set later
			.size	= MAILBOX_HOSTBOX_SIZE,
			.offset	= MAILBOX_HOSTBOX_OFFSET,
		},
		{
			.type	= SOF_IPC_REGION_DEBUG,
			.id	= 0,	/* map to host window 0 */
			.flags	= 0, // TODO: set later
			.size	= MAILBOX_DEBUG_SIZE,
			.offset	= MAILBOX_DEBUG_OFFSET,
		},
		{
			.type	= SOF_IPC_REGION_TRACE,
			.id	= 0,	/* map to host window 0 */
			.flags	= 0, // TODO: set later
			.size	= MAILBOX_TRACE_SIZE,
			.offset	= MAILBOX_TRACE_OFFSET,
		},
		{
			.type	= SOF_IPC_REGION_STREAM,
			.id	= 0,	/* map to host window 0 */
			.flags	= 0, // TODO: set later
			.size	= MAILBOX_STREAM_SIZE,
			.offset	= MAILBOX_STREAM_OFFSET,
		},
		{
			.type	= SOF_IPC_REGION_EXCEPTION,
			.id	= 0,	/* map to host window 0 */
			.flags	= 0, // TODO: set later
			.size	= MAILBOX_EXCEPTION_SIZE,
			.offset	= MAILBOX_EXCEPTION_OFFSET,
		},
	},
};


struct timesource_data platform_generic_queue[] = {
{
	.timer = {
		.id = TIMER0,
		.irq = IRQ_NUM_TIMER0,
	},
	.clk = PLATFORM_WORKQ_CLOCK,
	.timer_set = platform_timer_set,
	.timer_clear = platform_timer_clear,
	.timer_get = platform_timer_get,
},
};
struct timer *platform_timer =
	&platform_generic_queue[PLATFORM_MASTER_CORE_ID].timer;


int platform_boot_complete(uint32_t boot_message)
{
	mailbox_dspbox_write(0, &ready, sizeof(ready));
	mailbox_dspbox_write(sizeof(ready), &sram_window,
			     sram_window.ext_hdr.hdr.size);

	/* now interrupt host to tell it we are done booting */
	imx_mu_xcr_rmw(IMX_MU_xCR_GIRn(1), 0);

	/* boot now complete so we can relax the CPU */
	/* For now skip this to gain more processing performance
	 * for SRC component.
	 */
	/* clock_set_freq(CLK_CPU, CLK_DEFAULT_CPU_HZ); */

	return 0;
}

int platform_init(struct sof *sof)
{
	int ret;
	struct dai *esai;

	clock_init();
	scheduler_init();

	platform_timer_start(platform_timer);
	sa_init(sof);

	clock_set_freq(CLK_CPU(cpu_get_id()), CLK_MAX_CPU_HZ);

	/* init DMA */
	ret = edma_init();
	if (ret < 0)
		return -ENODEV;

	/* initialize the host IPC mechanims */
	ipc_init(sof);

	ret = dai_init();
	if (ret < 0)
		return -ENODEV;

	esai = dai_get(SOF_DAI_IMX_ESAI, 0, DAI_CREAT);
	if (!esai)
		return -ENODEV;

	dai_probe(esai);

	return 0;
}
