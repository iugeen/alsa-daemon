#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>

#include "memblock.h"

/* We can allocate 64*1024*1024 bytes at maximum. That's 64MB. Please
 * note that the footprint is usually much smaller, since the data is
 * stored in SHM and our OS does not commit the memory before we use
 * it for the first time. */
#define PA_MEMPOOL_SLOTS_MAX 1024
#define PA_MEMPOOL_SLOT_SIZE (64*1024)

#define PA_MEMEXPORT_SLOTS_MAX 128

#define PA_MEMIMPORT_SLOTS_MAX 160
#define PA_MEMIMPORT_SEGMENTS_MAX 16

//#define PA_REFCNT_DECLARE pa_atomic_t _ref

struct pa_memblock {
//    PA_REFCNT_DECLARE; /* the reference counter */
    pa_mempool *pool;

    pa_memblock_type_t type;

    bool read_only:1;
    bool is_silence:1;

    pa_atomic_ptr_t data;
    size_t length;

    pa_atomic_t n_acquired;
    pa_atomic_t please_signal;

//    union {
//        struct {
//            /* If type == PA_MEMBLOCK_USER this points to a function for freeing this memory block */
//            pa_free_cb_t free_cb;
//        } user;

//        struct {
//            uint32_t id;
//            pa_memimport_segment *segment;
//        } imported;
//    } per_type;
};

//struct pa_memimport_segment {
//    pa_memimport *import;
//    pa_shm memory;
//    pa_memtrap *trap;
//    unsigned n_blocks;
//};

///* A collection of multiple segments */
//struct pa_memimport {
//    pa_mutex *mutex;

//    pa_mempool *pool;
//    pa_hashmap *segments;
//    pa_hashmap *blocks;

//    /* Called whenever an imported memory block is no longer
//     * needed. */
//    pa_memimport_release_cb_t release_cb;
//    void *userdata;

//    PA_LLIST_FIELDS(pa_memimport);
//};

//struct memexport_slot {
//    PA_LLIST_FIELDS(struct memexport_slot);
//    pa_memblock *block;
//};

//struct pa_memexport {
//    pa_mutex *mutex;
//    pa_mempool *pool;

//    struct memexport_slot slots[PA_MEMEXPORT_SLOTS_MAX];

//    PA_LLIST_HEAD(struct memexport_slot, free_slots);
//    PA_LLIST_HEAD(struct memexport_slot, used_slots);
//    unsigned n_init;

//    /* Called whenever a client from which we imported a memory block
//       which we in turn exported to another client dies and we need to
//       revoke the memory block accordingly */
//    pa_memexport_revoke_cb_t revoke_cb;
//    void *userdata;

//    PA_LLIST_FIELDS(pa_memexport);
//};

struct pa_mempool {
    //pa_semaphore *semaphore;
    //pa_mutex *mutex;

    //pa_shm memory;
    size_t block_size;
    unsigned n_blocks;

    pa_atomic_t n_init;

    //PA_LLIST_HEAD(pa_memimport, imports);
    //PA_LLIST_HEAD(pa_memexport, exports);

    /* A list of free slots that may be reused */
    //pa_flist *free_slots;

    pa_mempool_stat stat;
};

pa_mempool* pa_mempool_new(bool shared, size_t size)
{
    pa_mempool *p;
//    char t1[PA_BYTES_SNPRINT_MAX], t2[PA_BYTES_SNPRINT_MAX];

//    p = pa_xnew(pa_mempool, 1);

//    p->block_size = PA_PAGE_ALIGN(PA_MEMPOOL_SLOT_SIZE);
//    if (p->block_size < PA_PAGE_SIZE)
//        p->block_size = PA_PAGE_SIZE;

//    if (size <= 0)
//        p->n_blocks = PA_MEMPOOL_SLOTS_MAX;
//    else {
//        p->n_blocks = (unsigned) (size / p->block_size);

//        if (p->n_blocks < 2)
//            p->n_blocks = 2;
//    }

//    if (pa_shm_create_rw(&p->memory, p->n_blocks * p->block_size, shared, 0700) < 0) {
//        pa_xfree(p);
//        return NULL;
//    }

//    pa_log_debug("Using %s memory pool with %u slots of size %s each, total size is %s, maximum usable slot size is %lu",
//                 p->memory.shared ? "shared" : "private",
//                 p->n_blocks,
//                 pa_bytes_snprint(t1, sizeof(t1), (unsigned) p->block_size),
//                 pa_bytes_snprint(t2, sizeof(t2), (unsigned) (p->n_blocks * p->block_size)),
//                 (unsigned long) pa_mempool_block_size_max(p));

//    memset(&p->stat, 0, sizeof(p->stat));
//    pa_atomic_store(&p->n_init, 0);

//    PA_LLIST_HEAD_INIT(pa_memimport, p->imports);
//    PA_LLIST_HEAD_INIT(pa_memexport, p->exports);

//    p->mutex = pa_mutex_new(true, true);
//    p->semaphore = pa_semaphore_new(0);

//    p->free_slots = pa_flist_new(p->n_blocks);

    return p;
}

/* No lock necessary */
static void stat_add(pa_memblock*b) {
    assert(b);
    assert(b->pool);

    pa_atomic_inc(&b->pool->stat.n_allocated);
    pa_atomic_add(&b->pool->stat.allocated_size, (int) b->length);

    pa_atomic_inc(&b->pool->stat.n_accumulated);
    pa_atomic_add(&b->pool->stat.accumulated_size, (int) b->length);

    if (b->type == PA_MEMBLOCK_IMPORTED) {
        pa_atomic_inc(&b->pool->stat.n_imported);
        pa_atomic_add(&b->pool->stat.imported_size, (int) b->length);
    }

    pa_atomic_inc(&b->pool->stat.n_allocated_by_type[b->type]);
    pa_atomic_inc(&b->pool->stat.n_accumulated_by_type[b->type]);
}

/* Rounds up */
static inline size_t PA_ALIGN(size_t l) {
    return ((l + sizeof(void*) - 1) & ~(sizeof(void*) - 1));
}

/* No lock necessary */
pa_memblock *pa_memblock_new_pool(pa_mempool *p, size_t length) {
    pa_memblock *b = NULL;
    struct mempool_slot *slot;
    static int mempool_disable = 0;

    assert(p);
    assert(length);

    if (mempool_disable == 0)
        mempool_disable = getenv("PULSE_MEMPOOL_DISABLE") ? 1 : -1;

    if (mempool_disable > 0)
        return NULL;

    /* If -1 is passed as length we choose the size for the caller: we
     * take the largest size that fits in one of our slots. */

    if (length == (size_t) -1)
        //length = pa_mempool_block_size_max(p);

    if (p->block_size >= PA_ALIGN(sizeof(pa_memblock)) + length) {

        //if (!(slot = mempool_allocate_slot(p)))
        //    return NULL;

        //b = mempool_slot_data(slot);
        b->type = PA_MEMBLOCK_POOL;
        pa_atomic_ptr_store(&b->data, (uint8_t*) b + PA_ALIGN(sizeof(pa_memblock)));

    } else if (p->block_size >= length) {

//        if (!(slot = mempool_allocate_slot(p)))
//            return NULL;

//        if (!(b = pa_flist_pop(PA_STATIC_FLIST_GET(unused_memblocks))))
//            b = pa_xnew(pa_memblock, 1);

//        b->type = PA_MEMBLOCK_POOL_EXTERNAL;
//        pa_atomic_ptr_store(&b->data, mempool_slot_data(slot));

    } else {
        //pa_log_debug("Memory block too large for pool: %lu > %lu", (unsigned long) length, (unsigned long) p->block_size);
        pa_atomic_inc(&p->stat.n_too_large_for_pool);
        return NULL;
    }

    //PA_REFCNT_INIT(b);
    b->pool = p;
    b->read_only = b->is_silence = false;
    b->length = length;
    pa_atomic_store(&b->n_acquired, 0);
    pa_atomic_store(&b->please_signal, 0);

    stat_add(b);
    return b;
}

/* No lock necessary */
pa_memblock *pa_memblock_new(pa_mempool *p, size_t length)
{
    pa_memblock *b;

    assert(p);
    assert(length);

    if (!(b = pa_memblock_new_pool(p, length)))
        b = memblock_new_appended(p, length);

    return b;
}

/* No lock necessary */
void* pa_memblock_acquire(pa_memblock *b)
{
    assert(b);
    //assert(PA_REFCNT_VALUE(b) > 0);

    pa_atomic_inc(&b->n_acquired);

    return pa_atomic_ptr_load(&b->data);
}


size_t pa_memblock_get_length(pa_memblock *b)
{
    assert(b);
    //assert(PA_REFCNT_VALUE(b) > 0);

    return b->length;
}

/* No lock necessary, in corner cases locks by its own */
void pa_memblock_release(pa_memblock *b)
{
    int r;
    assert(b);
    //assert(PA_REFCNT_VALUE(b) > 0);

    r = pa_atomic_dec(&b->n_acquired);
    assert(r >= 1);

    /* Signal a waiting thread that this memblock is no longer used */
//    if (r == 1 && pa_atomic_load(&b->please_signal))
//        pa_semaphore_post(b->pool->semaphore);
}
