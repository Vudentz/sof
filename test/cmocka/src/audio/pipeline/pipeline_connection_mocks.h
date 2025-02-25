/* SPDX-License-Identifier: BSD-3-Clause
 *
 * Copyright(c) 2018 Intel Corporation. All rights reserved.
 *
 * Author: Jakub Dabek <jakub.dabek@linux.intel.com>
 */

#include <sof/audio/component.h>
#include <sof/audio/pipeline.h>
#include <sof/schedule/edf_schedule.h>
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>
#include <cmocka.h>

#define PIPELINE_ID_SAME 3
#define PIPELINE_ID_DIFFERENT 4

struct pipeline_connect_data {
	struct pipeline p;
	struct comp_dev *first;
	struct comp_dev *second;
	struct comp_buffer *b1;
	struct comp_buffer *b2;
};

struct pipeline_connect_data *get_standard_connect_objects(void);

void cleanup_test_data(struct pipeline_connect_data *data);
