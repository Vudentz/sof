/* SPDX-License-Identifier: BSD-3-Clause
 *
 * Copyright(c) 2018 Intel Corporation. All rights reserved.
 *
 * Author: Liam Girdwood <liam.r.girdwood@linux.intel.com>
 */

#ifndef __INCLUDE_ARCH_STRING_SOF__
#define __INCLUDE_ARCH_STRING_SOF__

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#define arch_memcpy(dest, src, size) \
	xthal_memcpy(dest, src, size)

#if __XCC__ && !defined(UNIT_TEST)
#define arch_bzero(ptr, size)	\
	memset_s(ptr, size, 0, size)
#else
#define arch_bzero(ptr, size)	\
	memset(ptr, 0, size)
#endif

void *memcpy(void *dest, const void *src, size_t length);
void *memset(void *dest, int data, size_t count);
void *xthal_memcpy(void *dst, const void *src, size_t len);

int memset_s(void *dest, size_t dest_size,
	     int data, size_t count);
int memcpy_s(void *dest, size_t dest_size,
	     const void *src, size_t src_size);

#if __XCC__ && !CONFIG_LIBRARY
void *__vec_memcpy(void *dst, const void *src, size_t len);
void *__vec_memset(void *dest, int data, size_t src_size);
#endif

static inline int arch_memcpy_s(void *dest, size_t dest_size,
				const void *src, size_t src_size)
{
	if (!dest || !src)
		return -EINVAL;

	if (((char *)dest + dest_size >= (char *)src &&
	     (char *)dest + dest_size <= (char *)src + src_size) ||
		((char *)src + src_size >= (char *)dest &&
		 (char *)src + src_size <= (char *)dest + dest_size))
		return -EINVAL;

	if (src_size > dest_size)
		return -EINVAL;

#if __XCC__ && !CONFIG_LIBRARY
	__vec_memcpy(dest, src, src_size);
#else
	memcpy(dest, src, src_size);
#endif

	return 0;
}

static inline int arch_memset_s(void *dest, size_t dest_size,
				int data, size_t count)
{
	if (!dest)
		return -EINVAL;

	if (count > dest_size)
		return -EINVAL;

#if __XCC__ && !CONFIG_LIBRARY
	if (!__vec_memset(dest, data, count))
		return -ENOMEM;
#else
	memset(dest, data, count);
#endif
	return 0;
}

#endif
