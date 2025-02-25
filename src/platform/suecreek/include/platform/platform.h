/* SPDX-License-Identifier: BSD-3-Clause
 *
 * Copyright(c) 2017 Intel Corporation. All rights reserved.
 *
 * Author: Liam Girdwood <liam.r.girdwood@linux.intel.com>
 *         Keyon Jie <yang.jie@linux.intel.com>
 *         Rander Wang <rander.wang@intel.com>
 *         Xiuli Pan <xiuli.pan@linux.intel.com>
 */

#ifndef __PLATFORM_PLATFORM_H__
#define __PLATFORM_PLATFORM_H__

#include <cavs/memory.h>

#define PLATFORM_RESET_MHE_AT_BOOT	1

#define PLATFORM_MEM_INIT_AT_BOOT	1

#define PLATFORM_MASTER_CORE_ID		0

#define MAX_CORE_COUNT			4

#if PLATFORM_CORE_COUNT > MAX_CORE_COUNT
#error "Invalid core count - exceeding core limit"
#endif

#if !defined(__ASSEMBLER__) && !defined(LINKER)

#include <sof/platform.h>
#include <platform/clk.h>
#include <platform/shim.h>
#include <platform/interrupt.h>

struct sof;

/*! \def PLATFORM_DEFAULT_CLOCK
 *  \brief clock source for audio pipeline
 *
 *  There are two types of clock: cpu clock which is a internal clock in
 *  xtensa core, and ssp clock which is provided by external HW IP.
 *  The choice depends on HW features on different platform
 */
#define PLATFORM_DEFAULT_CLOCK CLK_SSP

/*! \def PLATFORM_WORKQ_DEFAULT_TIMEOUT
 *  \brief work queue default timeout in microseconds
 */
#define PLATFORM_WORKQ_DEFAULT_TIMEOUT	1000

#define PLATFORM_WAITI_DELAY	1

#define MAX_GPDMA_COUNT 2

/* Host page size */
#define HOST_PAGE_SIZE		4096
#define PLATFORM_PAGE_TABLE_SIZE	256

/* IDC Interrupt */
#define PLATFORM_IDC_INTERRUPT(x)	IRQ_EXT_IDC_LVL2(x)

/* IPC Interrupt */
#define PLATFORM_IPC_INTERRUPT	IRQ_EXT_IPC_LVL2(0)

/* pipeline IRQ */
#define PLATFORM_SCHEDULE_IRQ	IRQ_NUM_SOFTWARE4

#define PLATFORM_IRQ_TASK_HIGH	IRQ_NUM_SOFTWARE3
#define PLATFORM_IRQ_TASK_MED	IRQ_NUM_SOFTWARE2
#define PLATFORM_IRQ_TASK_LOW	IRQ_NUM_SOFTWARE1

#define PLATFORM_SCHEDULE_COST	200

/* maximum preload pipeline depth */
#define MAX_PRELOAD_SIZE	20

/* DMA treats PHY addresses as host address unless within DSP region */
#define PLATFORM_HOST_DMA_MASK	0x00000000

/* Platform stream capabilities */
#define PLATFORM_MAX_CHANNELS	4
#define PLATFORM_MAX_STREAMS	5

/* clock source used by scheduler for deadline calculations */
#define PLATFORM_SCHED_CLOCK	PLATFORM_DEFAULT_CLOCK

/* DMA channel drain timeout in microseconds
 * TODO: calculate based on topology
 */
#define PLATFORM_DMA_TIMEOUT	1333

/* DMA host transfer timeouts in microseconds */
#define PLATFORM_HOST_DMA_TIMEOUT	200

/* DMA link transfer timeouts in microseconds
 * TODO: timeout should be reduced
 * (DMA might needs some further changes to do that)
 */
#define PLATFORM_LINK_DMA_TIMEOUT	1000

/* WorkQ window size in microseconds */
#define PLATFORM_WORKQ_WINDOW	2000

/* platform WorkQ clock */
#define PLATFORM_WORKQ_CLOCK	PLATFORM_DEFAULT_CLOCK

/* Host finish work schedule delay in microseconds */
#define PLATFORM_HOST_FINISH_DELAY	100

/* Host finish work(drain from host to dai) timeout in microseconds */
#define PLATFORM_HOST_FINISH_TIMEOUT	50000

/* local buffer size of DMA tracing */
#define DMA_TRACE_LOCAL_SIZE	(HOST_PAGE_SIZE * 2)

/* trace bytes flushed during panic */
#define DMA_FLUSH_TRACE_SIZE    (MAILBOX_TRACE_SIZE >> 2)

/* the interval of DMA trace copying */
#define DMA_TRACE_PERIOD		500000

/*
 * the interval of reschedule DMA trace copying in special case like half
 * fullness of local DMA trace buffer
 */
#define DMA_TRACE_RESCHEDULE_TIME	500

/* DSP should be idle in this time frame */
#define PLATFORM_IDLE_TIME	750000

/* baud-rate used for uart port trace log */
#define PATFORM_TRACE_UART_BAUDRATE		115200

/* DSP default delay in cycles */
#define PLATFORM_DEFAULT_DELAY	12

/* minimal L1 exit time in cycles */
#define PLATFORM_FORCE_L1_EXIT_TIME	985

/* the SSP port fifo depth */
#define SSP_FIFO_DEPTH		16

/* the watermark for the SSP fifo depth setting */
#define SSP_FIFO_WATERMARK	8

/* minimal SSP port delay in cycles */
#define PLATFORM_SSP_DELAY	1000

/* timer driven scheduling start offset in microseconds */
#define PLATFORM_TIMER_START_OFFSET	100

/* SSI / SPI GPIO bindings */
#define PLATFORM_SPI_GPIO_ID	0
#define PLATFORM_SPI_GPIO_IRQ	14

// TODO: need UART versions
#if 0
/* Platform defined trace code */
static inline void platform_panic(uint32_t p)
{
	mailbox_sw_reg_write(SRAM_REG_FW_STATUS, p & 0x3fffffff);
	ipc_write(IPC_DIPCIDD, MAILBOX_EXCEPTION_OFFSET + 2 * 0x20000);
	ipc_write(IPC_DIPCIDR, 0x80000000 | (p & 0x3fffffff));
}

/* Platform defined trace code */
#define platform_trace_point(__x) \
	mailbox_sw_reg_write(SRAM_REG_FW_TRACEP, (__x))
#else
static inline void platform_panic(uint32_t p)
{
}

/* Platform defined trace code */
#define platform_trace_point(__x)
#endif
extern struct timer *platform_timer;

extern intptr_t _module_init_start;
extern intptr_t _module_init_end;

/*
 * APIs declared here are defined for every platform and IPC mechanism.
 */

int platform_ssp_set_mn(uint32_t ssp_port, uint32_t source, uint32_t rate,
	uint32_t bclk_fs);

void platform_ssp_disable_mn(uint32_t ssp_port);

#endif
#endif
