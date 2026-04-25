/* cpu.h -- low-level CPU helpers (port I/O, halt, interrupt flag).
 *
 * Everything here is a static inline so it can be used in any TU without
 * the linker complaining about duplicate symbols.
 */

#ifndef TOBYOS_CPU_H
#define TOBYOS_CPU_H

#include <tobyos/types.h>

static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port) : "memory");
}

static inline uint8_t inb(uint16_t port) {
    uint8_t val;
    __asm__ volatile ("inb %1, %0" : "=a"(val) : "Nd"(port) : "memory");
    return val;
}

static inline void outw(uint16_t port, uint16_t val) {
    __asm__ volatile ("outw %0, %1" : : "a"(val), "Nd"(port) : "memory");
}

static inline uint16_t inw(uint16_t port) {
    uint16_t val;
    __asm__ volatile ("inw %1, %0" : "=a"(val) : "Nd"(port) : "memory");
    return val;
}

/* Tiny stall after a port write -- writes a dummy byte to an unused port
 * (0x80 is traditionally used by BIOS POST and is safe to scribble on). */
static inline void io_wait(void) {
    outb(0x80, 0);
}

static inline void cli(void) { __asm__ volatile ("cli" ::: "memory"); }
static inline void sti(void) { __asm__ volatile ("sti" ::: "memory"); }
static inline void hlt(void) { __asm__ volatile ("hlt" ::: "memory"); }

/* Control-register reads -- needed by the page-fault handler (cr2 = the
 * faulting address; cr3 = current page-table root). */

static inline uint64_t read_cr0(void) {
    uint64_t v; __asm__ volatile ("mov %%cr0, %0" : "=r"(v)); return v;
}
static inline uint64_t read_cr2(void) {
    uint64_t v; __asm__ volatile ("mov %%cr2, %0" : "=r"(v)); return v;
}
static inline uint64_t read_cr3(void) {
    uint64_t v; __asm__ volatile ("mov %%cr3, %0" : "=r"(v)); return v;
}
static inline uint64_t read_cr4(void) {
    uint64_t v; __asm__ volatile ("mov %%cr4, %0" : "=r"(v)); return v;
}

static inline uint64_t read_rflags(void) {
    uint64_t v;
    __asm__ volatile ("pushfq; pop %0" : "=r"(v));
    return v;
}

static inline void write_cr3(uint64_t v) {
    __asm__ volatile ("mov %0, %%cr3" : : "r"(v) : "memory");
}

/* MSR access -- needed to flip EFER.NXE on so NX bits in PTEs are
 * honoured. rdmsr returns lo|hi in edx:eax; we splice them in C. */
static inline uint64_t rdmsr(uint32_t msr) {
    uint32_t lo, hi;
    __asm__ volatile ("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}
static inline void wrmsr(uint32_t msr, uint64_t val) {
    uint32_t lo = (uint32_t)val;
    uint32_t hi = (uint32_t)(val >> 32);
    __asm__ volatile ("wrmsr" : : "c"(msr), "a"(lo), "d"(hi));
}

static inline void invlpg(uint64_t va) {
    __asm__ volatile ("invlpg (%0)" : : "r"(va) : "memory");
}

/* Read the 64-bit Time Stamp Counter. On all x86_64 CPUs the TSC
 * increments at a constant "base" rate regardless of power state /
 * throttling (invariant TSC, CPUID.80000007H:EDX[8]). We don't check
 * that bit because QEMU reports it and bare-metal x86_64 has had it
 * since Nehalem -- milestone-19 profiling accepts the small risk of
 * drift on exotic hardware. Used by src/perf.c for the high-resolution
 * wallclock + instrumentation zones. */
static inline uint64_t rdtsc(void) {
    uint32_t lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

/* Disable interrupts and halt forever. Used by panic/idle paths. */
static inline __attribute__((noreturn)) void hlt_forever(void) {
    for (;;) {
        __asm__ volatile ("cli; hlt");
    }
}

#endif /* TOBYOS_CPU_H */
