#ifndef MEMBLOCK_H
#define MEMBLOCK_H

typedef struct pa_memblock pa_memblock;

#include <sys/types.h>
#include <inttypes.h>

//#include <pulse/def.h>
#include "atomic.h"
#include "memchunk.h"

/* A pa_memblock is a reference counted memory block. PulseAudio
 * passes references to pa_memblocks around instead of copying
 * data. See pa_memchunk for a structure that describes parts of
 * memory blocks. */

#define true 1
#define false 0
typedef int bool;

/* The type of memory this block points to */
typedef enum pa_memblock_type {
    PA_MEMBLOCK_POOL,             /* Memory is part of the memory pool */
    PA_MEMBLOCK_POOL_EXTERNAL,    /* Data memory is part of the memory pool but the pa_memblock structure itself is not */
    PA_MEMBLOCK_APPENDED,         /* The data is appended to the memory block */
    PA_MEMBLOCK_USER,             /* User supplied memory, to be freed with free_cb */
    PA_MEMBLOCK_FIXED,            /* Data is a pointer to fixed memory that needs not to be freed */
    PA_MEMBLOCK_IMPORTED,         /* Memory is imported from another process via shm */
    PA_MEMBLOCK_TYPE_MAX
} pa_memblock_type_t;

typedef struct pa_mempool pa_mempool;
typedef struct pa_mempool_stat pa_mempool_stat;
typedef struct pa_memimport_segment pa_memimport_segment;
typedef struct pa_memimport pa_memimport;
typedef struct pa_memexport pa_memexport;

typedef void (*pa_memimport_release_cb_t)(pa_memimport *i, uint32_t block_id, void *userdata);
typedef void (*pa_memexport_revoke_cb_t)(pa_memexport *e, uint32_t block_id, void *userdata);

/* Please note that updates to this structure are not locked,
 * i.e. n_allocated might be updated at a point in time where
 * n_accumulated is not yet. Take these values with a grain of salt,
 * they are here for purely statistical reasons.*/
struct pa_mempool_stat {
    pa_atomic_t n_allocated;
    pa_atomic_t n_accumulated;
    pa_atomic_t n_imported;
    pa_atomic_t n_exported;
    pa_atomic_t allocated_size;
    pa_atomic_t accumulated_size;
    pa_atomic_t imported_size;
    pa_atomic_t exported_size;

    pa_atomic_t n_too_large_for_pool;
    pa_atomic_t n_pool_full;

    pa_atomic_t n_allocated_by_type[PA_MEMBLOCK_TYPE_MAX];
    pa_atomic_t n_accumulated_by_type[PA_MEMBLOCK_TYPE_MAX];
};

/* The memory block manager */
pa_mempool* pa_mempool_new(bool shared, size_t size);

pa_memblock *memblock_new_appended(pa_mempool *p, size_t length);
pa_memblock *pa_memblock_new(pa_mempool *p, size_t length);
void* pa_memblock_acquire(pa_memblock *b);
size_t pa_memblock_get_length(pa_memblock *b);
void pa_memblock_release(pa_memblock *b);

#endif // MEMBLOCK_H
