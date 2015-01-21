#ifndef ATOMIC_H
#define ATOMIC_H

#define true 1
#define false 0

#include <atomic_ops.h>

typedef struct pa_atomic {
    volatile AO_t value;
} pa_atomic_t;

#define PA_ATOMIC_INIT(v) { .value = (AO_t) (v) }

static inline int pa_atomic_load(const pa_atomic_t *a) {
    return (int) AO_load_full((AO_t*) &a->value);
}

static inline void pa_atomic_store(pa_atomic_t *a, int i) {
    AO_store_full(&a->value, (AO_t) i);
}

static inline int pa_atomic_add(pa_atomic_t *a, int i) {
    return (int) AO_fetch_and_add_full(&a->value, (AO_t) i);
}

static inline int pa_atomic_sub(pa_atomic_t *a, int i) {
    return (int) AO_fetch_and_add_full(&a->value, (AO_t) -i);
}

static inline int pa_atomic_inc(pa_atomic_t *a) {
    return (int) AO_fetch_and_add1_full(&a->value);
}

static inline int pa_atomic_dec(pa_atomic_t *a) {
    return (int) AO_fetch_and_sub1_full(&a->value);
}

//static inline bool pa_atomic_cmpxchg(pa_atomic_t *a, int old_i, int new_i) {
//    return AO_compare_and_swap_full(&a->value, (unsigned long) old_i, (unsigned long) new_i);
//}

typedef struct pa_atomic_ptr {
    volatile AO_t value;
} pa_atomic_ptr_t;

#define PA_ATOMIC_PTR_INIT(v) { .value = (AO_t) (v) }

static inline void* pa_atomic_ptr_load(const pa_atomic_ptr_t *a) {
    return (void*) AO_load_full((AO_t*) &a->value);
}

static inline void pa_atomic_ptr_store(pa_atomic_ptr_t *a, void *p) {
    AO_store_full(&a->value, (AO_t) p);
}

//static inline bool pa_atomic_ptr_cmpxchg(pa_atomic_ptr_t *a, void *old_p, void* new_p) {
//    return AO_compare_and_swap_full(&a->value, (AO_t) old_p, (AO_t) new_p);
//}

#endif // ATOMIC_H
