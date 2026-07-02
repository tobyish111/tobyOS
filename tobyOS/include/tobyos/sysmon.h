/* sysmon.h -- live system monitor snapshots for the desktop shell.
 *
 * This is deliberately read-only plumbing: it samples existing kernel
 * counters (PMM, proc CPU accounting, perf zones, net status) and
 * packages them for compositor chrome and userland Settings. It does
 * not drive networking, input, scheduling, or storage. */

#ifndef TOBYOS_SYSMON_H
#define TOBYOS_SYSMON_H

#include <tobyos/types.h>
#include <tobyos/abi/abi.h>

/* Fill *out with the latest absolute counters plus the most recent
 * sampled percentages/rates. Cheap enough for the compositor to call
 * once per second. */
void sysmon_sample(struct abi_system_metrics *out);

/* Copy the last sampled values without advancing the rate window.
 * If no sample has happened yet, this takes the first one. */
void sysmon_snapshot(struct abi_system_metrics *out);

/* Copy the last-computed per-core utilisation (0-100 per core) into `out`,
 * up to `max` entries. Returns the number of cores written. */
uint32_t sysmon_cpu_pcts(uint8_t *out, uint32_t max);

#endif /* TOBYOS_SYSMON_H */
