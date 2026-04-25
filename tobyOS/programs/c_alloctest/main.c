/* programs/c_alloctest/main.c -- malloc / realloc / free torture test.
 *
 * Exercises libtoby's allocator across all the failure modes that
 * matter for porting: many small allocations, mixed-size churn,
 * realloc grow/shrink, calloc zeroing, and a final big-block grab
 * that forces a sbrk(). At the end we print "ALL OK" if every check
 * passed; on any failure we abort with a descriptive message and a
 * non-zero exit code so the boot harness in kernel.c flags it. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define N 256

static int fail(const char *msg) {
    printf("[c_alloctest] FAIL: %s\n", msg);
    return 1;
}

int main(void) {
    /* 1. Many small allocations, each filled with a recognisable
     *    pattern. Then free in interleaved order to stress the free
     *    list. */
    char *p[N];
    for (int i = 0; i < N; i++) {
        p[i] = (char *)malloc(64);
        if (!p[i]) return fail("malloc(64) returned NULL");
        memset(p[i], 'a' + (i & 31), 64);
        p[i][63] = '\0';
    }
    /* Verify all 256 buffers still hold their pattern (no overlap). */
    for (int i = 0; i < N; i++) {
        char want = 'a' + (i & 31);
        for (int j = 0; j < 63; j++) {
            if (p[i][j] != want) return fail("pattern overwrite detected");
        }
    }
    /* Free even slots first, then odd slots. */
    for (int i = 0; i < N; i += 2) free(p[i]);
    for (int i = 1; i < N; i += 2) free(p[i]);

    /* 2. realloc growth from 16 -> 4096, with a sentinel preserved. */
    char *q = (char *)malloc(16);
    if (!q) return fail("malloc(16)");
    memcpy(q, "REALLOC-START", 14);
    q[14] = '\0';
    for (size_t sz = 32; sz <= 4096; sz *= 2) {
        q = (char *)realloc(q, sz);
        if (!q) return fail("realloc grow");
        if (memcmp(q, "REALLOC-START", 14) != 0)
            return fail("realloc lost prefix");
    }
    /* Now realloc back down. */
    q = (char *)realloc(q, 32);
    if (!q) return fail("realloc shrink");
    if (memcmp(q, "REALLOC-START", 14) != 0)
        return fail("shrink lost prefix");
    free(q);

    /* 3. calloc zeroing. */
    int *zeros = (int *)calloc(1024, sizeof(int));
    if (!zeros) return fail("calloc(1024)");
    for (int i = 0; i < 1024; i++) {
        if (zeros[i] != 0) return fail("calloc not zero");
        zeros[i] = i;
    }
    int sum = 0;
    for (int i = 0; i < 1024; i++) sum += zeros[i];
    if (sum != 1023 * 512) return fail("calloc payload corrupted");
    free(zeros);

    /* 4. Big block. Forces the heap to grow several times. */
    char *big = (char *)malloc(64 * 1024);
    if (!big) return fail("malloc(64K)");
    memset(big, 0xCC, 64 * 1024);
    if ((unsigned char)big[0] != 0xCC || (unsigned char)big[64*1024 - 1] != 0xCC)
        return fail("big block memset");
    free(big);

    /* 5. realloc(NULL, n) == malloc(n) and realloc(p, 0) == free(p). */
    void *r = realloc(0, 128);
    if (!r) return fail("realloc(NULL, 128)");
    r = realloc(r, 0);   /* should free, return NULL */
    if (r != 0) return fail("realloc(p, 0) didn't return NULL");

    /* 6. strdup round-trip. */
    char *d = strdup("strdup goes through malloc");
    if (!d) return fail("strdup");
    if (strcmp(d, "strdup goes through malloc") != 0) return fail("strdup contents");
    free(d);

    printf("[c_alloctest] ALL OK\n");
    return 0;
}
