/* helix_memory.h -- freestanding allocator shim (tobyOS): map Helix's
 * allocator to libtoby malloc/free. */
#pragma once
#include <stdlib.h>
static inline void *helix_malloc(int size) { return malloc((size_t)size); }
static inline void  helix_free(void *ptr)   { free(ptr); }
