// SPDX-License-Identifier: BSD-3-Clause
//
// Copyright(c) 2016 Intel Corporation. All rights reserved.
//
// Author: Liam Girdwood <liam.r.girdwood@linux.intel.com>
//         Keyon Jie <yang.jie@linux.intel.com>

#include <sof/alloc.h>
#include <sof/sof.h>
#include <sof/debug.h>
#include <sof/panic.h>
#include <sof/trace.h>
#include <sof/lock.h>
#include <sof/cpu.h>
#include <platform/memory.h>
#include <stdint.h>

/* debug to set memory value on every allocation */
#if CONFIG_DEBUG_BLOCK_FREE
#define DEBUG_BLOCK_FREE_VALUE_8BIT ((uint8_t) 0xa5)
#define DEBUG_BLOCK_FREE_VALUE_32BIT ((uint32_t) 0xa5a5a5a5)
#endif

#define trace_mem_error(__e, ...) \
	trace_error(TRACE_CLASS_MEM, __e, ##__VA_ARGS__)
#define trace_mem_init(__e, ...) \
	trace_event(TRACE_CLASS_MEM, __e, ##__VA_ARGS__)

extern struct mm memmap;

/* We have 3 memory pools
 *
 * 1) System memory pool does not have a map and it's size is fixed at build
 *    time. Memory cannot be freed from this pool. Used by device drivers
 *    and any system core. Saved as part of PM context.
 * 2) Runtime memory pool has variable size allocation map and memory is freed
 *    on calls to rfree(). Saved as part of PM context. Global size
 *    set at build time.
 * 3) Buffer memory pool has fixed size allocation map and can be freed on
 *    module removal or calls to rfree(). Saved as part of PM context.
 */

#if CONFIG_DEBUG_BLOCK_FREE
/* Check whole memory region for debug pattern to find if memory was freed
 * second time
 */
static void validate_memory(void *ptr, size_t size)
{
	uint32_t *ptr_32 = ptr;
	int i, not_matching = 0;

	for (i = 0; i < size/4; i++) {
		if (ptr_32[i] != DEBUG_BLOCK_FREE_VALUE_32BIT)
			not_matching = 1;
	}

	if (not_matching) {
		trace_mem_init("validate_memory() pointer:"
			"%p freed pattern not detected",
			(uintptr_t)ptr);
	} else {
		trace_mem_error(
			"validate_memory() freeing pointer:"
			"%p double free detected",
			(uintptr_t)ptr);
	}
}
#endif

/* flush block map from cache to sram */
static inline void flush_block_map(struct block_map *map)
{
	dcache_writeback_invalidate_region(map->block,
					   sizeof(*map->block) * map->count);
	dcache_writeback_invalidate_region(map, sizeof(*map));
}

/* total size of block */
static inline uint32_t block_get_size(struct block_map *map)
{
	return sizeof(*map) + map->count *
		(map->block_size + sizeof(struct block_hdr));
}

/* total size of heap */
static inline uint32_t heap_get_size(struct mm_heap *heap)
{
	uint32_t size = sizeof(struct mm_heap);
	int i;

	for (i = 0; i < heap->blocks; i++)
		size += block_get_size(&heap->map[i]);

	return size;
}

#if CONFIG_DEBUG_BLOCK_FREE
static void write_pattern(struct mm_heap *heap_map, int heap_depth,
						  uint8_t pattern)
{
	struct mm_heap *heap;
	struct block_map *current_map;
	int i, j;

	for (i = 0; i < heap_depth; i++) {
		heap = &heap_map[i];

		for (j = 0; j < heap->blocks; j++) {
			current_map = &heap->map[j];
			memset(
				(void *)current_map->base, pattern,
				current_map->count * current_map->block_size);
		}
	}
}
#endif

static void init_heap_map(struct mm_heap *heap, int count)
{
	struct block_map *next_map;
	struct block_map *current_map;
	int i;
	int j;

	for (i = 0; i < count; i++) {
		/* init the map[0] */
		current_map = &heap[i].map[0];
		current_map->base = heap[i].heap;
		flush_block_map(current_map);

		/* map[j]'s base is calculated based on map[j-1] */
		for (j = 1; j < heap[i].blocks; j++) {
			next_map = &heap[i].map[j];
			next_map->base = current_map->base +
				current_map->block_size *
				current_map->count;
			current_map = &heap[i].map[j];
			flush_block_map(current_map);
		}

		dcache_writeback_invalidate_region(&heap[i], sizeof(heap[i]));
	}
}

/* allocate from system memory pool */
static void *rmalloc_sys(int zone, int caps, int core, size_t bytes)
{
	void *ptr;
	struct mm_heap *cpu_heap;
	size_t alignment = 0;

	/* use the heap dedicated for the selected core */
	cpu_heap = memmap.system + core;
	if ((cpu_heap->caps & caps) != caps)
		panic(SOF_IPC_PANIC_MEM);

	/* align address to dcache line size */
	if (cpu_heap->info.used % PLATFORM_DCACHE_ALIGN)
		alignment = PLATFORM_DCACHE_ALIGN -
			(cpu_heap->info.used % PLATFORM_DCACHE_ALIGN);

	/* always succeeds or panics */
	if (alignment + bytes > cpu_heap->info.free) {
		trace_error(TRACE_CLASS_MEM,
			    "rmalloc_sys() error: "
			    "eM1 zone = %x, core = %d, bytes = %d",
			    zone, core, bytes);
		panic(SOF_IPC_PANIC_MEM);
	}
	cpu_heap->info.used += alignment;

	ptr = (void *)(cpu_heap->heap + cpu_heap->info.used);

	cpu_heap->info.used += bytes;
	cpu_heap->info.free -= alignment + bytes;

	/* other core should have the latest value */
	if (core != cpu_get_id())
		dcache_writeback_invalidate_region(cpu_heap,
						   sizeof(*cpu_heap));

	if ((zone & RZONE_FLAG_MASK) == RZONE_FLAG_UNCACHED)
		ptr = cache_to_uncache(ptr);

	return ptr;
}

/* allocate single block */
static void *alloc_block(struct mm_heap *heap, int level,
	uint32_t caps)
{
	struct block_map *map = &heap->map[level];
	struct block_hdr *hdr = &map->block[map->first_free];
	void *ptr;
	int i;

	map->free_count--;
	ptr = (void *)(map->base + map->first_free * map->block_size);
	hdr->size = 1;
	hdr->used = 1;
	heap->info.used += map->block_size;
	heap->info.free -= map->block_size;

	/* find next free */
	for (i = map->first_free; i < map->count; ++i) {

		hdr = &map->block[i];

		if (hdr->used == 0) {
			map->first_free = i;
			break;
		}
	}

	return ptr;
}

/* allocates continuous blocks */
static void *alloc_cont_blocks(struct mm_heap *heap, int level,
	uint32_t caps, size_t bytes)
{
	struct block_map *map = &heap->map[level];
	struct block_hdr *hdr;
	void *ptr;
	unsigned int start;
	unsigned int current;
	unsigned int count = bytes / map->block_size;
	unsigned int remaining = 0;

	if (bytes % map->block_size)
		count++;

	/* check if we have enough consecutive blocks for requested
	 * allocation size.
	 */
	for (current = map->first_free; current < map->count; current++) {
		hdr = &map->block[current];

		if (!hdr->used)
			remaining++;
	}

	if (count > map->count || remaining < count) {
		trace_mem_error("error: %d blocks needed for allocation "
				"but only %d blocks are remaining",
				count, remaining);
		return NULL;
	}

	/* we found enough space, let's allocate it */
	start = map->first_free;
	map->free_count -= count;
	ptr = (void *)(map->base + start * map->block_size);
	hdr = &map->block[start];
	hdr->size = count;
	heap->info.used += count * map->block_size;
	heap->info.free -= count * map->block_size;
	map->first_free = map->first_free + count;

	/* update each block */
	for (current = start; current < count; current++) {
		hdr = &map->block[current];
		hdr->used = 1;
	}

	return ptr;
}

static struct mm_heap *get_heap_from_ptr(void *ptr)
{
	struct mm_heap *heap;
	int i;

	/* find mm_heap that ptr belongs to */
	heap = memmap.system_runtime + cpu_get_id();
	if ((uint32_t)ptr >= heap->heap &&
	    (uint32_t)ptr < heap->heap + heap->size)
		return heap;

	for (i = 0; i < PLATFORM_HEAP_RUNTIME; i++) {
		heap = &memmap.runtime[i];

		if ((uint32_t)ptr >= heap->heap &&
		    (uint32_t)ptr < heap->heap + heap->size)
			return heap;
	}

	for (i = 0; i < PLATFORM_HEAP_BUFFER; i++) {
		heap = &memmap.buffer[i];

		if ((uint32_t)ptr >= heap->heap &&
		    (uint32_t)ptr < heap->heap + heap->size)
			return heap;
	}

	return NULL;
}

static struct mm_heap *get_heap_from_caps(struct mm_heap *heap, int count,
					  uint32_t caps)
{
	uint32_t mask;
	int i;

	/* find first heap that support type */
	for (i = 0; i < count; i++) {
		mask = heap[i].caps & caps;
		if (mask == caps)
			return &heap[i];
	}

	return NULL;
}

static void *get_ptr_from_heap(struct mm_heap *heap, int zone, uint32_t caps,
			       size_t bytes)
{
	struct block_map *map;
	int i;
	void *ptr = NULL;

	for (i = 0; i < heap->blocks; i++) {
		map = &heap->map[i];

		/* is block big enough */
		if (map->block_size < bytes)
			continue;

		/* does block have free space */
		if (map->free_count == 0)
			continue;

		/* free block space exists */
		ptr = alloc_block(heap, i, caps);

		break;
	}

	if (ptr && (zone & RZONE_FLAG_MASK) == RZONE_FLAG_UNCACHED)
		ptr = cache_to_uncache(ptr);

	return ptr;
}

/* free block(s) */
static void free_block(void *ptr)
{
	struct mm_heap *heap;
	struct block_map *block_map;
	struct block_hdr *hdr;
	int i;
	int block;
	int used_blocks;

	heap = get_heap_from_ptr(ptr);
	if (!heap) {
		trace_error(TRACE_CLASS_MEM, "free_block() error: invalid "
			    "heap = %p, cpu = %d",
			    (uintptr_t)ptr, cpu_get_id());
		return;
	}

	/* find block that ptr belongs to */
	for (i = 0; i < heap->blocks; i++) {
		block_map = &heap->map[i];

		/* is ptr in this block */
		if ((uint32_t)ptr < (block_map->base +
		    (block_map->block_size * block_map->count)))
			break;
	}

	if (i == heap->blocks) {
		/* not found */
		trace_error(TRACE_CLASS_MEM,
			    "free_block() error: invalid ptr = %p cpu = %d",
			    (uintptr_t)ptr, cpu_get_id());
		return;
	}

	/* calculate block header */
	block = ((uint32_t)ptr - block_map->base) / block_map->block_size;
	hdr = &block_map->block[block];

	/* report a error if prt is not aligned to block */
	if (block_map->base + block_map->block_size * block != (uint32_t)ptr)
		panic(SOF_IPC_PANIC_MEM);

	/* free block header and continuous blocks */
	used_blocks = block + hdr->size;

	for (i = block; i < used_blocks; i++) {
		hdr = &block_map->block[i];
		hdr->size = 0;
		hdr->used = 0;
		block_map->free_count++;
		heap->info.used -= block_map->block_size;
		heap->info.free += block_map->block_size;
	}

	/* set first free block */
	if (block < block_map->first_free)
		block_map->first_free = block;

#if CONFIG_DEBUG_BLOCK_FREE
	/* memset the whole block in case of unaligned ptr */
	validate_memory(
		(void *)(block_map->base + block_map->block_size * block),
		block_map->block_size * (i - block));
	memset(
		(void *)(block_map->base + block_map->block_size * block),
		DEBUG_BLOCK_FREE_VALUE_8BIT, block_map->block_size *
		(i - block));
#endif
}

#if CONFIG_DEBUG_HEAP

static void trace_heap_blocks(struct mm_heap *heap)
{
	struct block_map *block_map;
	int i;

	trace_mem_error("heap: 0x%x size %d blocks %d caps 0x%x", heap->heap,
			heap->size, heap->blocks, heap->caps);
	trace_mem_error(" used %d free %d", heap->info.used,
			heap->info.free);

	for (i = 0; i < heap->blocks; i++) {
		block_map = &heap->map[i];

		trace_mem_error(" block %d base 0x%x size %d count %d", i,
				block_map->base, block_map->block_size,
				block_map->count);
		trace_mem_error("  free %d first at %d",
				block_map->free_count, block_map->first_free);
	}
}

void alloc_trace_runtime_heap(int zone, uint32_t caps, size_t bytes)
{
	struct mm_heap *heap;
	struct mm_heap *current = NULL;
	int count = 0;

	/* check runtime heap for capabilities */
	trace_mem_error("heap: using runtime");
	do {
		heap = get_heap_from_caps(memmap.runtime, PLATFORM_HEAP_RUNTIME,
					  caps);
		if (heap) {
			trace_heap_blocks(heap);
			count++;
		}
		current = heap;
	} while (heap);

	if (count == 0)
		trace_mem_error("heap: none found for zone %d caps 0x%x, "
				"bytes 0x%x", zone, caps, bytes);
}

void alloc_trace_buffer_heap(int zone, uint32_t caps, size_t bytes)
{
	struct mm_heap *heap;
	struct mm_heap *current = NULL;
	int count = 0;

	/* check buffer heap for capabilities */
	trace_mem_error("heap: using buffer");
	do {
		heap = get_heap_from_caps(memmap.buffer, PLATFORM_HEAP_BUFFER,
					  caps);
		if (heap) {
			trace_heap_blocks(heap);
			count++;
		}
		current = heap;
	} while (heap);

	if (count == 0)
		trace_mem_error("heap: none found for zone %d caps 0x%x, "
				"bytes 0x%x", zone, caps, bytes);
}

#endif

/* allocate single block for system runtime */
static void *rmalloc_sys_runtime(int zone, int caps, int core, size_t bytes)
{
	struct mm_heap *cpu_heap;
	void *ptr;

	/* use the heap dedicated for the selected core */
	cpu_heap = memmap.system_runtime + core;
	if ((cpu_heap->caps & caps) != caps)
		panic(SOF_IPC_PANIC_MEM);

	ptr = get_ptr_from_heap(cpu_heap, zone, caps, bytes);

	/* other core should have the latest value */
	if (core != cpu_get_id())
		dcache_writeback_invalidate_region(cpu_heap,
						   sizeof(*cpu_heap));

	return ptr;
}

/* allocate single block for runtime */
static void *rmalloc_runtime(int zone, uint32_t caps, size_t bytes)
{
	struct mm_heap *heap;

	/* check runtime heap for capabilities */
	heap = get_heap_from_caps(memmap.runtime, PLATFORM_HEAP_RUNTIME, caps);
	if (!heap) {
		/* next check buffer heap for capabilities */
		heap = get_heap_from_caps(memmap.buffer, PLATFORM_HEAP_BUFFER,
					  caps);
		if (!heap) {
			trace_error(TRACE_CLASS_MEM,
				    "rmalloc_runtime() error: "
				    "eMm zone = %d, caps = %x, bytes = %d",
				    zone, caps, bytes);

			return NULL;
		}
	}

	return get_ptr_from_heap(heap, zone, caps, bytes);
}

static void *_malloc_unlocked(int zone, uint32_t caps, size_t bytes)
{
	void *ptr = NULL;

	switch (zone & RZONE_TYPE_MASK) {
	case RZONE_SYS:
		ptr = rmalloc_sys(zone, caps, cpu_get_id(), bytes);
		break;
	case RZONE_SYS_RUNTIME:
		ptr = rmalloc_sys_runtime(zone, caps, cpu_get_id(), bytes);
		break;
	case RZONE_RUNTIME:
		ptr = rmalloc_runtime(zone, caps, bytes);
		break;
	default:
		trace_mem_error("rmalloc() error: invalid zone");
		panic(SOF_IPC_PANIC_MEM); /* logic non recoverable problem */
		break;
	}

#if CONFIG_DEBUG_BLOCK_FREE
	if (ptr)
		bzero(ptr, bytes);
#endif

	memmap.heap_trace_updated = 1;
	return ptr;
}

/* allocates memory - not for direct use, clients use rmalloc() */
void *_malloc(int zone, uint32_t caps, size_t bytes)
{
	uint32_t flags;
	void *ptr = NULL;

	spin_lock_irq(&memmap.lock, flags);

	ptr = _malloc_unlocked(zone, caps, bytes);

	spin_unlock_irq(&memmap.lock, flags);

	return ptr;
}

/* allocates and clears memory - not for direct use, clients use rzalloc() */
void *_zalloc(int zone, uint32_t caps, size_t bytes)
{
	void *ptr;

	ptr = _malloc(zone, caps, bytes);
	if (ptr)
		bzero(ptr, bytes);

	return ptr;
}

void *rzalloc_core_sys(int core, size_t bytes)
{
	uint32_t flags;
	void *ptr = NULL;

	spin_lock_irq(&memmap.lock, flags);

	ptr = rmalloc_sys(RZONE_SYS, 0, core, bytes);
	if (ptr)
		bzero(ptr, bytes);

	spin_unlock_irq(&memmap.lock, flags);

	return ptr;
}

/* allocates continuous buffers - not for direct use, clients use rballoc() */
static void *alloc_heap_buffer(struct mm_heap *heap, int zone, uint32_t caps,
			       size_t bytes)
{
	struct block_map *map;
	int i;
	void *ptr = NULL;

	/* will request fit in single block */
	for (i = 0; i < heap->blocks; i++) {
		map = &heap->map[i];

		/* Check if blocks are big enough and at least one is free */
		if (map->block_size >= bytes && map->free_count) {
			/* found: grab a block */
			ptr = alloc_block(heap, i, caps);
			break;
		}
	}

	/* request spans > 1 block */
	if (!ptr) {
		/*
		 * Find the best block size for request. We know, that we failed
		 * to find a single large enough block, so, skip those.
		 */
		for (i = heap->blocks - 1; i >= 0; i--) {
			map = &heap->map[i];

			/* allocate if block size is smaller than request */
			if (heap->size >= bytes && map->block_size < bytes) {
				ptr = alloc_cont_blocks(heap, i, caps, bytes);
				if (ptr)
					break;
			}
		}
	}

	if (ptr && ((zone & RZONE_FLAG_MASK) == RZONE_FLAG_UNCACHED))
		ptr = cache_to_uncache(ptr);

#if CONFIG_DEBUG_BLOCK_FREE
	if (ptr)
		bzero(ptr, bytes);
#endif

	return ptr;
}

static void *_balloc_unlocked(int zone, uint32_t caps, size_t bytes)
{
	struct mm_heap *heap;
	unsigned int i, n;
	void *ptr = NULL;

	for (i = 0, n = PLATFORM_HEAP_BUFFER, heap = memmap.buffer;
	     i < PLATFORM_HEAP_BUFFER;
	     i = heap - memmap.buffer + 1, n = PLATFORM_HEAP_BUFFER - i,
	     heap++) {
		heap = get_heap_from_caps(heap, n, caps);
		if (!heap)
			break;

		ptr = alloc_heap_buffer(heap, zone, caps, bytes);
		if (ptr)
			break;

		/* Continue from the next heap */
	}

	return ptr;
}

/* allocates continuous buffers - not for direct use, clients use rballoc() */
void *_balloc(int zone, uint32_t caps, size_t bytes)
{
	void *ptr = NULL;
	uint32_t flags;

	spin_lock_irq(&memmap.lock, flags);

	ptr = _balloc_unlocked(zone, caps, bytes);

	spin_unlock_irq(&memmap.lock, flags);

	return ptr;
}

static void _rfree_unlocked(void *ptr)
{
	struct mm_heap *cpu_heap;

	/* sanity check - NULL ptrs are fine */
	if (!ptr)
		return;

	/* operate only on cached addresses */
	if (is_uncached(ptr))
		ptr = uncache_to_cache(ptr);

	/* use the heap dedicated for the selected core */
	cpu_heap = memmap.system + cpu_get_id();

	/* panic if pointer is from system heap */
	if (ptr >= (void *)cpu_heap->heap &&
	    ptr < (void *)cpu_heap->heap + cpu_heap->size) {
		trace_error(TRACE_CLASS_MEM, "rfree() error: "
			   "attempt to free system heap = %p, cpu = %d",
			    (uintptr_t)ptr, cpu_get_id());
		panic(SOF_IPC_PANIC_MEM);
	}

	/* free the block */
	free_block(ptr);
	memmap.heap_trace_updated = 1;
}

void rfree(void *ptr)
{
	uint32_t flags;

	spin_lock_irq(&memmap.lock, flags);
	_rfree_unlocked(ptr);
	spin_unlock_irq(&memmap.lock, flags);
}

void *_realloc(void *ptr, int zone, uint32_t caps, size_t bytes)
{
	void *new_ptr = NULL;
	uint32_t flags;

	if (!bytes)
		return new_ptr;

	spin_lock_irq(&memmap.lock, flags);

	new_ptr = _malloc_unlocked(zone, caps, bytes);

	if (new_ptr && ptr)
		memcpy_s(new_ptr, bytes, ptr, bytes);

	if (new_ptr)
		_rfree_unlocked(ptr);

	spin_unlock_irq(&memmap.lock, flags);

	return new_ptr;
}

void *_brealloc(void *ptr, int zone, uint32_t caps, size_t bytes)
{
	void *new_ptr = NULL;
	uint32_t flags;

	if (!bytes)
		return new_ptr;

	spin_lock_irq(&memmap.lock, flags);

	new_ptr = _balloc_unlocked(zone, caps, bytes);

	if (new_ptr && ptr)
		memcpy_s(new_ptr, bytes, ptr, bytes);

	if (new_ptr)
		_rfree_unlocked(ptr);

	spin_unlock_irq(&memmap.lock, flags);

	return new_ptr;
}

/* TODO: all mm_pm_...() routines to be implemented for IMR storage */
uint32_t mm_pm_context_size(void)
{
	return 0;
}

/*
 * Save the DSP memories that are in use the system and modules.
 * All pipeline and modules must be disabled before calling this functions.
 * No allocations are permitted after calling this and before calling restore.
 */
int mm_pm_context_save(struct dma_copy *dc, struct dma_sg_config *sg)
{
	return -ENOTSUP;
}

/*
 * Restore the DSP memories to modules and the system.
 * This must be called immediately after booting before any pipeline work.
 */
int mm_pm_context_restore(struct dma_copy *dc, struct dma_sg_config *sg)
{
	return -ENOTSUP;
}

void free_heap(int zone)
{
	struct mm_heap *cpu_heap;

	/* to be called by slave cores only for sys heap,
	 * otherwise this is critical flow issue.
	 */
	if (cpu_get_id() == PLATFORM_MASTER_CORE_ID ||
	    zone != RZONE_SYS) {
		trace_mem_error("free_heap() error: critical flow issue");
		panic(SOF_IPC_PANIC_MEM);
	}

	cpu_heap = memmap.system + cpu_get_id();
	cpu_heap->info.used = 0;
	cpu_heap->info.free = cpu_heap->size;

	dcache_writeback_region(cpu_heap, sizeof(*cpu_heap));
}

#if CONFIG_TRACE
void heap_trace(struct mm_heap *heap, int size)
{
	struct block_map *current_map;
	int i;
	int j;

	for (i = 0; i < size; i++) {

		trace_mem_init(" heap: 0x%x size %d blocks %d caps 0x%x",
			       heap->heap, heap->size, heap->blocks,
			       heap->caps);
		trace_mem_init("  used %d free %d", heap->info.used,
			       heap->info.free);

		/* map[j]'s base is calculated based on map[j-1] */
		for (j = 1; j < heap->blocks; j++) {
			current_map = &heap->map[j];

			trace_mem_init("  block %d base 0x%x size %d",
				       j, current_map->base,
				       current_map->block_size);
			trace_mem_init("   count %d free %d",
				       current_map->count,
				       current_map->free_count);
		}

		heap++;
	}
}

void heap_trace_all(int force)
{
	/* has heap changed since last shown */
	if (memmap.heap_trace_updated || force) {
		trace_mem_init("heap: buffer status");
		heap_trace(memmap.buffer, PLATFORM_HEAP_BUFFER);
		trace_mem_init("heap: runtime status");
		heap_trace(memmap.runtime, PLATFORM_HEAP_RUNTIME);
	}
	memmap.heap_trace_updated = 0;
}
#else
void heap_trace_all(int force) { }
void heap_trace(struct mm_heap *heap, int size) { }
#endif

/* initialise map */
void init_heap(struct sof *sof)
{
	extern uintptr_t _system_heap_start;

	/* sanity check for malformed images or loader issues */
	if (memmap.system[0].heap != (uintptr_t)&_system_heap_start)
		panic(SOF_IPC_PANIC_MEM);

	spinlock_init(&memmap.lock);

	init_heap_map(memmap.system_runtime, PLATFORM_HEAP_SYSTEM_RUNTIME);

	init_heap_map(memmap.runtime, PLATFORM_HEAP_RUNTIME);

	init_heap_map(memmap.buffer, PLATFORM_HEAP_BUFFER);

#if CONFIG_DEBUG_BLOCK_FREE
	write_pattern((struct mm_heap *)&memmap.buffer, PLATFORM_HEAP_BUFFER,
				  DEBUG_BLOCK_FREE_VALUE_8BIT);
	write_pattern((struct mm_heap *)&memmap.runtime, PLATFORM_HEAP_RUNTIME,
				  DEBUG_BLOCK_FREE_VALUE_8BIT);
#endif

	dcache_writeback_invalidate_region(&memmap, sizeof(memmap));
}
