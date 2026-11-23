// ps5-homebrew-ui - Process-lifetime heap for the OpenGL runtime.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Adapted from ps5-opengl native-app/app_heap.c. tools/build.sh links with
// --wrap for the malloc family whenever this file exists, routing allocations
// into a fixed sceLibcMspace that is never unmapped.
//
// The heap is 1 GiB of direct memory when the console gives it (a film's decoder and pictures
// do not fit in 128 MiB, docs/decision-log.md D54; blocks of 1 GiB were allocated and checked
// in the memory probe, D46), otherwise 128 MiB of flexible memory as before (only about 260 MiB
// of that is free, so a bigger mmap would fail).

#include <errno.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>

void *__real_malloc(size_t size);
void *__real_calloc(size_t count, size_t size);
void *__real_realloc(void *address, size_t size);
void __real_free(void *address);
int __real_posix_memalign(void **address, size_t alignment, size_t size);
size_t __real_malloc_usable_size(const void *address);

int64_t sceKernelGetDirectMemorySize(void);
int sceKernelAllocateDirectMemory(int64_t search_start, int64_t search_end, size_t length,
                                  size_t alignment, int type, int64_t *physical_start);
int sceKernelMapDirectMemory(void **address, size_t length, int protection, int flags,
                             int64_t physical_start, size_t alignment);
int sceKernelReleaseDirectMemory(int64_t physical_start, size_t length);

void *sceLibcMspaceCreate(const char *name, void *base, size_t size, unsigned flags);
void *sceLibcMspaceMalloc(void *mspace, size_t size);
void *sceLibcMspaceCalloc(void *mspace, size_t count, size_t size);
void *sceLibcMspaceRealloc(void *mspace, void *address, size_t size);
void sceLibcMspaceFree(void *mspace, void *address);
int sceLibcMspacePosixMemalign(void *mspace, void **address, size_t alignment, size_t size);
size_t sceLibcMspaceMallocUsableSize(const void *address);

/* Fixed, process-lifetime heap: do not unmap underneath late C++ destructors. */
#define HUI_HEAP_DIRECT_SIZE ((size_t)1024u * 1024u * 1024u)
#define HUI_HEAP_FLEXIBLE_SIZE ((size_t)128u * 1024u * 1024u)
/* The type and protection the memory probe used for its 1 GiB block (CPU read and write). */
#define HUI_HEAP_DIRECT_TYPE 12
#define HUI_HEAP_DIRECT_PROTECTION 0x33
#define HUI_HEAP_DIRECT_ALIGNMENT 0x4000

static atomic_int hui_heap_state;
static void *hui_heap_base;
static void *hui_heap_mspace;
static size_t hui_heap_size;
static int hui_heap_direct;
/* Owned-heap usable bytes only; relaxed counters are observations, not fences. */
static atomic_size_t hui_heap_live_bytes, hui_heap_peak_bytes, hui_heap_blocks;
static atomic_size_t hui_heap_failures;

static void hui_heap_resize_stats(size_t before, size_t after)
{
    size_t live = after >= before ? atomic_fetch_add_explicit(&hui_heap_live_bytes, after - before,
                                                              memory_order_relaxed) +
                                        after - before
                                  : atomic_fetch_sub_explicit(&hui_heap_live_bytes, before - after,
                                                              memory_order_relaxed) -
                                        (before - after);
    size_t peak = atomic_load_explicit(&hui_heap_peak_bytes, memory_order_relaxed);
    while (live > peak &&
           !atomic_compare_exchange_weak_explicit(&hui_heap_peak_bytes, &peak, live,
                                                  memory_order_relaxed, memory_order_relaxed))
    {
    }
}

static void *hui_heap_record_allocation(void *address, int nonzero)
{
    if (address)
    {
        hui_heap_resize_stats(0, sceLibcMspaceMallocUsableSize(address));
        atomic_fetch_add_explicit(&hui_heap_blocks, 1, memory_order_relaxed);
    }
    else if (nonzero)
    {
        atomic_fetch_add_explicit(&hui_heap_failures, 1, memory_order_relaxed);
    }
    return address;
}

static int hui_heap_ready(void)
{
    int state = atomic_load_explicit(&hui_heap_state, memory_order_acquire);
    if (state == 2)
        return 1;
    if (state != 0)
        return 0;

    int expected = 0;
    if (!atomic_compare_exchange_strong_explicit(&hui_heap_state, &expected, 1,
                                                 memory_order_acq_rel, memory_order_acquire))
        return expected == 2;

    void *base = NULL;
    size_t size = HUI_HEAP_DIRECT_SIZE;
    int64_t physical = -1;
    int direct = 0;
    if (sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(), size,
                                      HUI_HEAP_DIRECT_ALIGNMENT, HUI_HEAP_DIRECT_TYPE,
                                      &physical) == 0)
    {
        if (sceKernelMapDirectMemory(&base, size, HUI_HEAP_DIRECT_PROTECTION, 0, physical,
                                     HUI_HEAP_DIRECT_ALIGNMENT) == 0 &&
            base != NULL)
            direct = 1;
        else
        {
            base = NULL;
            sceKernelReleaseDirectMemory(physical, size);
        }
    }
    if (!direct)
    {
        size = HUI_HEAP_FLEXIBLE_SIZE;
        base = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
        if (base == MAP_FAILED)
        {
            atomic_store_explicit(&hui_heap_state, -1, memory_order_release);
            return 0;
        }
    }

    hui_heap_base = base;
    hui_heap_size = size;
    hui_heap_direct = direct;
    hui_heap_mspace = sceLibcMspaceCreate("ps5-homebrew-ui", base, size, 0);
    if (hui_heap_mspace == NULL)
    {
        /* Direct memory stays mapped (never unmapped, like the heap itself); malloc falls back
           to the system's. */
        hui_heap_base = NULL;
        if (!direct)
            munmap(base, size);
        atomic_store_explicit(&hui_heap_state, -1, memory_order_release);
        return 0;
    }

    atomic_store_explicit(&hui_heap_state, 2, memory_order_release);
    return 1;
}

static int hui_heap_owns(const void *address)
{
    /* Acquire publication before reading non-atomic heap metadata. */
    if (atomic_load_explicit(&hui_heap_state, memory_order_acquire) != 2)
        return 0;
    uintptr_t value = (uintptr_t)address;
    uintptr_t base = (uintptr_t)hui_heap_base;
    return value >= base && value - base < hui_heap_size;
}

void *__wrap_malloc(size_t size)
{
    return hui_heap_ready()
               ? hui_heap_record_allocation(sceLibcMspaceMalloc(hui_heap_mspace, size), size != 0)
               : __real_malloc(size);
}

void *__wrap_calloc(size_t count, size_t size)
{
    return hui_heap_ready()
               ? hui_heap_record_allocation(sceLibcMspaceCalloc(hui_heap_mspace, count, size),
                                            count != 0 && size != 0)
               : __real_calloc(count, size);
}

void *__wrap_realloc(void *address, size_t size)
{
    if (address == NULL)
        return __wrap_malloc(size);
    if (!hui_heap_owns(address))
        return __real_realloc(address, size);
    size_t before = sceLibcMspaceMallocUsableSize(address);
    void *result = sceLibcMspaceRealloc(hui_heap_mspace, address, size);
    if (result)
        hui_heap_resize_stats(before, sceLibcMspaceMallocUsableSize(result));
    else if (size)
        atomic_fetch_add_explicit(&hui_heap_failures, 1, memory_order_relaxed);
    return result;
}

void __wrap_free(void *address)
{
    if (hui_heap_owns(address))
    {
        hui_heap_resize_stats(sceLibcMspaceMallocUsableSize(address), 0);
        atomic_fetch_sub_explicit(&hui_heap_blocks, 1, memory_order_relaxed);
        sceLibcMspaceFree(hui_heap_mspace, address);
    }
    else
    {
        __real_free(address);
    }
}

int __wrap_posix_memalign(void **address, size_t alignment, size_t size)
{
    if (!hui_heap_ready())
        return __real_posix_memalign(address, alignment, size);
    int result = sceLibcMspacePosixMemalign(hui_heap_mspace, address, alignment, size);
    if (result == 0)
        hui_heap_record_allocation(*address, size != 0);
    else if (result == ENOMEM)
        atomic_fetch_add_explicit(&hui_heap_failures, 1, memory_order_relaxed);
    return result;
}

size_t __wrap_malloc_usable_size(const void *address)
{
    return hui_heap_owns(address) ? sceLibcMspaceMallocUsableSize(address)
                                  : __real_malloc_usable_size(address);
}

/* Diagnostics: current heap counters for logs and the diagnostics screen. */
/* How big the heap is, and 1 when it is direct memory; 0 before the first allocation. */
size_t hui_heap_capacity(int *direct)
{
    if (direct)
        *direct = hui_heap_direct;
    return atomic_load_explicit(&hui_heap_state, memory_order_acquire) == 2 ? hui_heap_size : 0;
}

void hui_heap_stats(size_t *live_bytes, size_t *peak_bytes, size_t *blocks, size_t *failures)
{
    if (live_bytes)
        *live_bytes = atomic_load_explicit(&hui_heap_live_bytes, memory_order_relaxed);
    if (peak_bytes)
        *peak_bytes = atomic_load_explicit(&hui_heap_peak_bytes, memory_order_relaxed);
    if (blocks)
        *blocks = atomic_load_explicit(&hui_heap_blocks, memory_order_relaxed);
    if (failures)
        *failures = atomic_load_explicit(&hui_heap_failures, memory_order_relaxed);
}
