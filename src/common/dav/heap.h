/* SPDX-License-Identifier: BSD-3-Clause */
/* Copyright 2015-2023, Intel Corporation */

/*
 * heap.h -- internal definitions for heap
 */

#ifndef __DAOS_COMMON_HEAP_H
#define __DAOS_COMMON_HEAP_H 1

#include <stddef.h>
#include <stdint.h>

#include "memblock.h"
#include "bucket.h"
#include "memops.h"
#include "palloc.h"
#include "os_thread.h"
#include "dav_internal.h"

#define HEAP_OFF_TO_PTR(heap, off) ((void *)((char *)((heap)->base) + (off)))
#define HEAP_PTR_TO_OFF(heap, ptr) ((uintptr_t)(ptr) - (uintptr_t)((heap)->base))

#define BIT_IS_CLR(a, i)           (!((a) & (1ULL << (i))))
#define HEAP_ARENA_PER_THREAD      (0)

int
heap_boot(struct palloc_heap *heap, void *heap_start, uint64_t heap_size, uint64_t *sizep,
	  void *base, struct mo_ops *p_ops, struct stats *stats, struct pool_set *set);
int
heap_init(void *heap_start, uint64_t heap_size, uint64_t *sizep, struct mo_ops *p_ops);
void
heap_cleanup(struct palloc_heap *heap);
int
heap_check(void *heap_start, uint64_t heap_size);
int
heap_check_remote(void *heap_start, uint64_t heap_size, struct remote_ops *ops);
int
heap_create_alloc_class_buckets(struct palloc_heap *heap, struct alloc_class *c);
int
heap_extend(struct palloc_heap *heap, struct bucket *defb, size_t size);

struct alloc_class *
heap_get_best_class(struct palloc_heap *heap, size_t size);

struct bucket *
zoneset_bucket_acquire(struct zone_set *zset, uint8_t class_id);
void
zoneset_bucket_release(struct bucket *b);

int
heap_get_bestfit_block(struct palloc_heap *heap, struct bucket *b, struct memory_block *m);
os_mutex_t *
heap_get_run_lock(struct palloc_heap *heap, uint32_t chunk_id);

void
heap_discard_run(struct palloc_heap *heap, struct memory_block *m);

void
heap_memblock_on_free(struct palloc_heap *heap, const struct memory_block *m);

int
heap_free_chunk_reuse(struct palloc_heap *heap, struct bucket *bucket, struct memory_block *m);

void
heap_foreach_object(struct palloc_heap *heap, object_callback cb, void *arg,
		    struct memory_block start);

struct alloc_class_collection *
heap_alloc_classes(struct palloc_heap *heap);

void *
heap_end(struct palloc_heap *heap);

void
heap_vg_open(struct palloc_heap *heap, object_callback cb, void *arg, int objects);

static inline struct chunk_header *
heap_get_chunk_hdr(struct palloc_heap *heap, const struct memory_block *m)
{
	return GET_CHUNK_HDR(heap->layout, m->zone_id, m->chunk_id);
}

static inline struct chunk *
heap_get_chunk(struct palloc_heap *heap, const struct memory_block *m)
{
	return GET_CHUNK(heap->layout, m->zone_id, m->chunk_id);
}

static inline struct chunk_run *
heap_get_chunk_run(struct palloc_heap *heap, const struct memory_block *m)
{
	return GET_CHUNK_RUN(heap->layout, m->zone_id, m->chunk_id);
}

struct zone_set *
heap_get_zoneset(struct palloc_heap *heap, uint32_t zone_id);

#endif /* __DAOS_COMMON_HEAP_H */
