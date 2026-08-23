#ifndef TOBYOS_VDSO_H
#define TOBYOS_VDSO_H

#include <stdint.h>
#include <stdbool.h>

struct proc;

/* tobyOS vDSO (2026-08-23). A real Linux shared object (tools/vdso/) is
 * embedded in the kernel image, staged into physical pages once at boot,
 * and mapped read-execute into every ELF process at a fixed address; the
 * page below it is the shared time-data page the kernel keeps current.
 * glibc discovers it via AT_SYSINFO_EHDR and stops syscalling for
 * clock_gettime/gettimeofday/time.
 *
 * The data page layout is mirrored VERBATIM in tools/vdso/vdso.c -- the
 * two definitions must stay identical, and the published constants are
 * exactly perf_now_ns()'s own (boot_tsc, tsc_khz), so a vDSO read and a
 * syscall read compute the same function of the same TSC. */

#define VDSO_VVAR_VA  0x00007ffff7ffd000ULL   /* 1 page, R+NX  */
#define VDSO_BASE_VA  0x00007ffff7ffe000ULL   /* blob, R+X     */

struct vdso_data {
    uint32_t seq;            /* seqlock: odd = writer active */
    uint32_t tsc_khz;
    uint64_t boot_tsc;
    uint64_t epoch_base_ns;  /* CLOCK_REALTIME - MONOTONIC; 0 = no RTC */
};

/* Stage the embedded blob into physical pages + allocate the data page.
 * Call once at boot, after perf_init(). Safe to call when the blob is
 * somehow absent -- vdso_map_into then becomes a no-op and processes
 * simply see no AT_SYSINFO_EHDR (the pre-vDSO world). */
void vdso_init(void);

/* Re-publish the time constants if they changed (seqlock write). Called
 * from the ~10 ms alarm sweep -- cheap compare, rare write -- so TSC
 * recalibration and the first RTC latch reach userspace within a tick. */
void vdso_refresh(void);

/* Map the blob + data page into the address space rooted at `cr3` (the
 * NEW PML4 during exec -- p->cr3 may still name the old image). Returns
 * the AT_SYSINFO_EHDR value (VDSO_BASE_VA), or 0 when the vDSO is
 * unavailable and the auxv entry must be omitted. */
uint64_t vdso_map_root(uint64_t cr3);

#endif
