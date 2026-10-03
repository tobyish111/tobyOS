/* host check: two-step conversion == exact 128-bit result, for dtsc far
 * past the old overflow point */
#include <stdio.h>
#include <stdint.h>
static uint64_t two(uint64_t d, uint32_t k) {
    uint64_t ms = d / k, rem = d % k;
    return ms * 1000000ull + rem * 1000000ull / k;
}
static uint64_t old(uint64_t d, uint32_t k) { return (d * 1000000ull) / k; }
int main(void) {
    uint32_t khz[] = { 1000, 2400000, 3392000, 4294967, 4300000, 9999999 };
    uint64_t bad = 0, n = 0, oldbad = 0;
    for (int i = 0; i < 6; i++) {
        uint32_t k = khz[i];
        for (uint64_t d = 1; d < (1ull << 62); d = d * 3 + 7) {
            unsigned __int128 exact = (unsigned __int128)d * 1000000u / k;
            if (exact >> 64) break;
            n++;
            if (two(d, k) != (uint64_t)exact) bad++;
            if (old(d, k) != (uint64_t)exact) oldbad++;
        }
        /* the moment the old form wrapped: 2^64/1e6 cycles */
        uint64_t w = (UINT64_MAX / 1000000ull) + 1;
        printf("khz=%u wrap at %.1f s: old=%llu ns, new=%llu ns\n", k,
               (double)w / k / 1000.0, (unsigned long long)old(w, k),
               (unsigned long long)two(w, k));
    }
    printf("checked %llu points: two-step mismatches=%llu, old-form mismatches=%llu\n",
           (unsigned long long)n, (unsigned long long)bad, (unsigned long long)oldbad);
    return bad != 0;
}
