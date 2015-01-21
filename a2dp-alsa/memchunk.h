#ifndef MEMCHUNK_H
#define MEMCHUNK_H

typedef struct pa_memchunk pa_memchunk;

#include "memblock.h"

/* A memchunk describes a part of a memblock. In contrast to the memblock, a
 * memchunk is not allocated dynamically or reference counted, instead
 * it is usually stored on the stack and copied around */

struct pa_memchunk {
    pa_memblock *memblock;
    size_t index, length;
};

#endif // MEMCHUNK_H
