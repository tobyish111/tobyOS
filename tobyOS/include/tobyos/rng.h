/* rng.h -- small kernel CSPRNG service.
 *
 * The pool is seeded from hardware RDRAND and/or trusted device entropy
 * when available plus boot-time jitter/state. It is suitable for protocol
 * keys once rng_init() has run; callers can mix additional device/network
 * timing later.
 */

#ifndef TOBYOS_RNG_H
#define TOBYOS_RNG_H

#include <tobyos/types.h>

void rng_init(void);
void rng_mix(const void *buf, size_t n);
void rng_mix_hardware(const void *buf, size_t n, const char *source);
void rng_fill(void *buf, size_t n);
bool rng_ready(void);
bool rng_hardware_seeded(void);

void virtio_rng_register(void);

#endif /* TOBYOS_RNG_H */
