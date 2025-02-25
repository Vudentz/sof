// SPDX-License-Identifier: BSD-3-Clause
//
// Copyright(c) 2018 Intel Corporation. All rights reserved.
//
// Author: Jakub Dabek <jakub.dabek@linux.intel.com>

#include <sof/audio/component.h>
#include <sof/audio/pipeline.h>
#include <sof/schedule/edf_schedule.h>
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>
#include <cmocka.h>
#include <sof/alloc.h>

void *rzalloc(int zone, uint32_t caps, size_t bytes)
{
	(void)zone;
	(void)caps;
	return calloc(bytes, 1);
}
