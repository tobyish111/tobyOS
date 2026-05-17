/* rng.c -- compact kernel CSPRNG built on Monocypher primitives. */

#include <tobyos/rng.h>
#include <tobyos/cpu.h>
#include <tobyos/pit.h>
#include <tobyos/net.h>
#include <tobyos/printk.h>
#include <tobyos/klibc.h>

#include <monocypher.h>

struct rng_seed_material {
    uint64_t tsc_a;
    uint64_t tsc_b;
    uint64_t ticks;
    uint64_t rflags;
    uint64_t cr3;
    uint64_t stack_addr;
    uint64_t fn_addr;
    uint8_t  mac[ETH_ADDR_LEN];
    uint8_t  rdrand[64];
    uint32_t rdrand_words;
};

static uint8_t  g_key[32];
static uint8_t  g_nonce[12];
static uint32_t g_counter;
static bool     g_ready;
static bool     g_have_rdrand;
static bool     g_hardware_seeded;

static void cpuid(uint32_t leaf, uint32_t subleaf,
                  uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d) {
    uint32_t ra, rb, rc, rd;
    __asm__ volatile ("cpuid"
                      : "=a"(ra), "=b"(rb), "=c"(rc), "=d"(rd)
                      : "a"(leaf), "c"(subleaf));
    if (a) *a = ra;
    if (b) *b = rb;
    if (c) *c = rc;
    if (d) *d = rd;
}

static bool rdrand64(uint64_t *out) {
    unsigned char ok;
    uint64_t v;
    __asm__ volatile ("rdrand %0; setc %1"
                      : "=r"(v), "=qm"(ok));
    if (!ok) return false;
    *out = v;
    return true;
}

static bool cpu_has_rdrand(void) {
    uint32_t a, b, c, d;
    cpuid(1, 0, &a, &b, &c, &d);
    (void)a; (void)b; (void)d;
    return (c & (1u << 30)) != 0;
}

static void rng_absorb(const void *buf, size_t n) {
    uint8_t new_key[32];
    crypto_blake2b_keyed(new_key, sizeof(new_key), g_key, sizeof(g_key),
                         (const uint8_t *)buf, n);
    memcpy(g_key, new_key, sizeof(g_key));
    crypto_wipe(new_key, sizeof(new_key));
}

void rng_mix(const void *buf, size_t n) {
    if (!buf || n == 0) return;
    if (!g_ready) rng_init();
    rng_absorb(buf, n);
}

void rng_mix_hardware(const void *buf, size_t n, const char *source) {
    if (!buf || n == 0) return;
    if (!g_ready) rng_init();
    rng_absorb(buf, n);
    g_hardware_seeded = true;
    kprintf("[rng] mixed %u bytes from %s; hardware_seeded=1\n",
            (unsigned)n, source ? source : "hardware");
}

void rng_init(void) {
    if (g_ready) return;

    struct rng_seed_material s;
    memset(&s, 0, sizeof(s));
    s.tsc_a = rdtsc();
    s.ticks = pit_ticks();
    s.rflags = read_rflags();
    s.cr3 = read_cr3();
    s.stack_addr = (uint64_t)(uintptr_t)&s;
    s.fn_addr = (uint64_t)(uintptr_t)&rng_init;
    memcpy(s.mac, g_my_mac, sizeof(s.mac));

    g_have_rdrand = cpu_has_rdrand();
    if (g_have_rdrand) {
        uint64_t *r = (uint64_t *)s.rdrand;
        for (size_t i = 0; i < sizeof(s.rdrand) / sizeof(uint64_t); i++) {
            for (int tries = 0; tries < 10; tries++) {
                if (rdrand64(&r[i])) {
                    s.rdrand_words++;
                    break;
                }
            }
        }
    }
    s.tsc_b = rdtsc();
    uint32_t rdrand_words = s.rdrand_words;
    g_hardware_seeded = rdrand_words > 0;

    crypto_blake2b(g_key, sizeof(g_key), (const uint8_t *)&s, sizeof(s));
    crypto_blake2b(g_nonce, sizeof(g_nonce), g_key, sizeof(g_key));
    g_counter = (uint32_t)(s.tsc_a ^ (s.tsc_b >> 32) ^ pit_ticks());
    g_ready = true;

    crypto_wipe(&s, sizeof(s));
    kprintf("[rng] CSPRNG ready (rdrand=%d words=%u)\n",
            g_have_rdrand ? 1 : 0, (unsigned)rdrand_words);
}

bool rng_ready(void) {
    return g_ready;
}

bool rng_hardware_seeded(void) {
    if (!g_ready) rng_init();
    return g_hardware_seeded;
}

void rng_fill(void *buf, size_t n) {
    if (!buf || n == 0) return;
    if (!g_ready) rng_init();

    uint8_t *out = (uint8_t *)buf;
    while (n > 0) {
        uint8_t block[64];
        crypto_chacha20_ietf(block, 0, sizeof(block), g_key, g_nonce,
                             g_counter++);
        size_t take = n < sizeof(block) ? n : sizeof(block);
        memcpy(out, block, take);
        out += take;
        n -= take;
        crypto_wipe(block, sizeof(block));
    }

    struct {
        uint64_t tsc;
        uint64_t ticks;
        uint8_t  sample[32];
    } m;
    m.tsc = rdtsc();
    m.ticks = pit_ticks();
    memcpy(m.sample, g_key, sizeof(m.sample));
    rng_absorb(&m, sizeof(m));
    crypto_wipe(&m, sizeof(m));
}
