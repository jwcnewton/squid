
/*
 * DEBUG: section 63    Low Level Memory Pool Management
 * AUTHOR: Alex Rousskov, Andres Kroonmaa, Robert Collins
 *
 * SQUID Internet Object Cache  http://squid.nlanr.net/Squid/
 * ----------------------------------------------------------
 *
 *  Squid is the result of efforts by numerous individuals from the
 *  Internet community.  Development is led by Duane Wessels of the
 *  National Laboratory for Applied Network Research and funded by the
 *  National Science Foundation.  Squid is Copyrighted (C) 1998 by
 *  the Regents of the University of California.  Please see the
 *  COPYRIGHT file for full details.  Squid incorporates software
 *  developed and/or copyrighted by other sources.  Please see the
 *  CREDITS file for full details.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111, USA.
 *
 */

#include "squid.h"

#include <cassert>

#include "MemPool.h"
#include "MemPoolChunked.h"
#include "MemPoolMalloc.h"

#define FLUSH_LIMIT 1000	/* Flush memPool counters to memMeters after flush limit calls */

#include <cstring>

/*
 * XXX This is a boundary violation between lib and src.. would be good
 * if it could be solved otherwise, but left for now.
 */
extern time_t squid_curtime;

/* local data */
static MemPoolMeter TheMeter;
static MemPoolIterator Iterator;

static int Pool_id_counter = 0;

MemPools &
MemPools::GetInstance()
{
    /* Must use this idiom, as we can be double-initialised
     * if we are called during static initialisations.
     */
    if (!Instance)
        Instance = new MemPools;
    return *Instance;
}

MemPools * MemPools::Instance = NULL;

MemPoolIterator *
memPoolIterate(void)
{
    Iterator.pool = MemPools::GetInstance().pools;
    return &Iterator;
}

void
memPoolIterateDone(MemPoolIterator ** iter)
{
    assert(iter != NULL);
    Iterator.pool = NULL;
    *iter = NULL;
}

MemImplementingAllocator *
memPoolIterateNext(MemPoolIterator * iter)
{
    MemImplementingAllocator *pool;
    assert(iter != NULL);

    pool = iter->pool;
    if (!pool)
        return NULL;

    iter->pool = pool->next;
    return pool;
}

void
MemPools::setIdleLimit(ssize_t new_idle_limit)
{
    mem_idle_limit = new_idle_limit;
}

ssize_t
MemPools::idleLimit() const
{
    return mem_idle_limit;
}

/* Change the default calue of defaultIsChunked to override
 * all pools - including those used before main() starts where
 * MemPools::GetInstance().setDefaultPoolChunking() can be called.
 */
MemPools::MemPools() : pools(NULL), mem_idle_limit(2 << 20 /* 2 MB */),
        poolCount(0), defaultIsChunked(USE_CHUNKEDMEMPOOLS && !RUNNING_ON_VALGRIND)
{
    char *cfg = getenv("MEMPOOLS");
    if (cfg)
        defaultIsChunked = atoi(cfg);
}

MemImplementingAllocator *
MemPools::create(const char *label, size_t obj_size)
{
    ++poolCount;
    if (defaultIsChunked)
        return new MemPoolChunked (label, obj_size);
    else
        return new MemPoolMalloc (label, obj_size);
}

void
MemPools::setDefaultPoolChunking(bool const &aBool)
{
    defaultIsChunked = aBool;
}

char const *
MemAllocator::objectType() const
{
    return label;
}

int
MemAllocator::inUseCount()
{
    return getInUseCount();
}

void
MemImplementingAllocator::flushMeters()
{
    size_t calls;

    calls = free_calls;
    if (calls) {
        meter.gb_freed.count += calls;
        free_calls = 0;
    }
    calls = alloc_calls;
    if (calls) {
        meter.gb_allocated.count += calls;
        alloc_calls = 0;
    }
    calls = saved_calls;
    if (calls) {
        meter.gb_saved.count += calls;
        saved_calls = 0;
    }
}

void
MemImplementingAllocator::flushMetersFull()
{
    flushMeters();
    getMeter().gb_allocated.bytes = getMeter().gb_allocated.count * obj_size;
    getMeter().gb_saved.bytes = getMeter().gb_saved.count * obj_size;
    getMeter().gb_freed.bytes = getMeter().gb_freed.count * obj_size;
}

void
MemPoolMeter::flush()
{
    alloc.level = 0;
    inuse.level = 0;
    idle.level = 0;
    gb_allocated.count = 0;
    gb_allocated.bytes = 0;
    gb_oallocated.count = 0;
    gb_oallocated.bytes = 0;
    gb_saved.count = 0;
    gb_saved.bytes = 0;
    gb_freed.count = 0;
    gb_freed.bytes = 0;
}

MemPoolMeter::MemPoolMeter()
{
    flush();
}

/*
 * Updates all pool counters, and recreates TheMeter totals from all pools
 */
void
MemPools::flushMeters()
{
    MemImplementingAllocator *pool;
    MemPoolIterator *iter;

    TheMeter.flush();

    iter = memPoolIterate();
    while ((pool = memPoolIterateNext(iter))) {
        pool->flushMetersFull();
        memMeterAdd(TheMeter.alloc, pool->getMeter().alloc.level * pool->obj_size);
        memMeterAdd(TheMeter.inuse, pool->getMeter().inuse.level * pool->obj_size);
        memMeterAdd(TheMeter.idle, pool->getMeter().idle.level * pool->obj_size);
        TheMeter.gb_allocated.count += pool->getMeter().gb_allocated.count;
        TheMeter.gb_saved.count += pool->getMeter().gb_saved.count;
        TheMeter.gb_freed.count += pool->getMeter().gb_freed.count;
        TheMeter.gb_allocated.bytes += pool->getMeter().gb_allocated.bytes;
        TheMeter.gb_saved.bytes += pool->getMeter().gb_saved.bytes;
        TheMeter.gb_freed.bytes += pool->getMeter().gb_freed.bytes;
    }
    memPoolIterateDone(&iter);
}

void *
MemImplementingAllocator::alloc()
{
    if (++alloc_calls == FLUSH_LIMIT)
        flushMeters();

    return allocate();
}

void
MemImplementingAllocator::freeOne(void *obj)
{
    assert(obj != NULL);
    (void) VALGRIND_CHECK_MEM_IS_ADDRESSABLE(obj, obj_size);
    deallocate(obj, MemPools::GetInstance().mem_idle_limit == 0);
    ++free_calls;
}

/*
 * Returns all cached frees to their home chunks
 * If chunks unreferenced age is over, destroys Idle chunk
 * Flushes meters for a pool
 * If pool is not specified, iterates through all pools.
 * When used for all pools, if new_idle_limit is above -1, new
 * idle memory limit is set before Cleanup. This allows to shrink
 * memPool memory usage to specified minimum.
 */
void
MemPools::clean(time_t maxage)
{
    flushMeters();
    if (mem_idle_limit < 0) // no limit to enforce
        return;

    int shift = 1;
    if (TheMeter.idle.level > mem_idle_limit)
        maxage = shift = 0;

    MemImplementingAllocator *pool;
    MemPoolIterator *iter;
    iter = memPoolIterate();
    while ((pool = memPoolIterateNext(iter)))
        if (pool->idleTrigger(shift))
            pool->clean(maxage);
    memPoolIterateDone(&iter);
}

/* Persistent Pool stats. for GlobalStats accumulation */
static MemPoolStats pp_stats;

/*
 * Totals statistics is returned
 */
int
memPoolGetGlobalStats(MemPoolGlobalStats * stats)
{
    int pools_inuse = 0;
    MemAllocator *pool;
    MemPoolIterator *iter;

    memset(stats, 0, sizeof(MemPoolGlobalStats));
    memset(&pp_stats, 0, sizeof(MemPoolStats));

    MemPools::GetInstance().flushMeters(); /* recreate TheMeter */

    /* gather all stats for Totals */
    iter = memPoolIterate();
    while ((pool = memPoolIterateNext(iter))) {
        if (pool->getStats(&pp_stats, 1) > 0)
            ++pools_inuse;
    }
    memPoolIterateDone(&iter);

    stats->TheMeter = &TheMeter;

    stats->tot_pools_alloc = MemPools::GetInstance().poolCount;
    stats->tot_pools_inuse = pools_inuse;
    stats->tot_pools_mempid = Pool_id_counter;

    stats->tot_chunks_alloc = pp_stats.chunks_alloc;
    stats->tot_chunks_inuse = pp_stats.chunks_inuse;
    stats->tot_chunks_partial = pp_stats.chunks_partial;
    stats->tot_chunks_free = pp_stats.chunks_free;
    stats->tot_items_alloc = pp_stats.items_alloc;
    stats->tot_items_inuse = pp_stats.items_inuse;
    stats->tot_items_idle = pp_stats.items_idle;

    stats->tot_overhead += pp_stats.overhead + MemPools::GetInstance().poolCount * sizeof(MemAllocator *);
    stats->mem_idle_limit = MemPools::GetInstance().mem_idle_limit;

    return pools_inuse;
}

MemAllocator::MemAllocator(char const *aLabel) : doZero(true), label(aLabel)
{
}

size_t MemAllocator::RoundedSize(size_t s)
{
    return ((s + sizeof(void*) - 1) / sizeof(void*)) * sizeof(void*);
}

int
memPoolInUseCount(MemAllocator * pool)
{
    return pool->inUseCount();
}

int
memPoolsTotalAllocated(void)
{
    MemPoolGlobalStats stats;
    memPoolGetGlobalStats(&stats);
    return stats.TheMeter->alloc.level;
}

void *
MemAllocatorProxy::alloc()
{
    return getAllocator()->alloc();
}

void
MemAllocatorProxy::freeOne(void *address)
{
    getAllocator()->freeOne(address);
    /* TODO: check for empty, and if so, if the default type has altered,
     * switch
     */
}

MemAllocator *
MemAllocatorProxy::getAllocator() const
{
    if (!theAllocator)
        theAllocator = MemPools::GetInstance().create(objectType(), size);
    return theAllocator;
}

int
MemAllocatorProxy::inUseCount() const
{
    if (!theAllocator)
        return 0;
    else
        return memPoolInUseCount(theAllocator);
}

size_t
MemAllocatorProxy::objectSize() const
{
    return size;
}

char const *
MemAllocatorProxy::objectType() const
{
    return label;
}

MemPoolMeter const &
MemAllocatorProxy::getMeter() const
{
    return getAllocator()->getMeter();
}

int
MemAllocatorProxy::getStats(MemPoolStats * stats)
{
    return getAllocator()->getStats(stats);
}

MemImplementingAllocator::MemImplementingAllocator(char const *aLabel, size_t aSize) : MemAllocator(aLabel),
        next(NULL),
        alloc_calls(0),
        free_calls(0),
        saved_calls(0),
        obj_size(RoundedSize(aSize))
{
    memPID = ++Pool_id_counter;

    MemImplementingAllocator *last_pool;

    assert(aLabel != NULL && aSize);
    /* Append as Last */
    for (last_pool = MemPools::GetInstance().pools; last_pool && last_pool->next;)
        last_pool = last_pool->next;
    if (last_pool)
        last_pool->next = this;
    else
        MemPools::GetInstance().pools = this;
}

MemImplementingAllocator::~MemImplementingAllocator()
{
    MemImplementingAllocator *find_pool, *prev_pool;

    /* Abort if the associated pool doesn't exist */
    assert(MemPools::GetInstance().pools != NULL );

    /* Pool clean, remove it from List and free */
    for (find_pool = MemPools::GetInstance().pools, prev_pool = NULL; (find_pool && this != find_pool); find_pool = find_pool->next)
        prev_pool = find_pool;

    /* make sure that we found the pool to destroy */
    assert(find_pool != NULL);

    if (prev_pool)
        prev_pool->next = next;
    else
        MemPools::GetInstance().pools = next;
    --MemPools::GetInstance().poolCount;
}

MemPoolMeter const &
MemImplementingAllocator::getMeter() const
{
    return meter;
}

MemPoolMeter &
MemImplementingAllocator::getMeter()
{
    return meter;
}

size_t
MemImplementingAllocator::objectSize() const
{
    return obj_size;
}
