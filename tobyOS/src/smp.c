/* smp.c -- AP boot orchestrator + per-CPU table.
 *
 * Responsibilities:
 *   1. Build g_percpu[] from the ACPI MADT (one entry per usable LAPIC).
 *      Slot 0 is always the BSP (we sort BSP-first).
 *   2. Identity-map AP_TRAMPOLINE_PHYS into the kernel PML4 so the AP
 *      can keep executing across the CR0.PG transition. Reserve the
 *      page in the PMM so nobody else grabs it.
 *   3. Copy the trampoline blob into the reserved page and patch the
 *      param area with: kernel PML4 phys, the AP's stack top, the
 *      address of ap_entry(), the AP's cpu_idx, and pseudo-descriptor
 *      copies of the kernel's GDTR + IDTR.
 *   4. For each AP (cpu_idx >= 1):
 *        - allocate a 16 KiB stack
 *        - patch the params for that AP
 *        - send INIT, wait, send SIPI, wait, send SIPI again
 *        - spin until the AP signals "online"
 *
 * APs themselves run ap_entry(), which prints "CPU N online", flips its
 * online flag, and halts forever. There is no scheduler, so they go to
 * sleep on cli/hlt and never wake again.
 *
 * Console output uses g_console_lock so the BSP and any in-flight AP
 * never interleave kprintf characters.
 */

#include <tobyos/smp.h>
#include <tobyos/acpi.h>
#include <tobyos/apic.h>
#include <tobyos/pmm.h>
#include <tobyos/vmm.h>
#include <tobyos/heap.h>
#include <tobyos/printk.h>
#include <tobyos/panic.h>
#include <tobyos/cpu.h>
#include <tobyos/pit.h>
#include <tobyos/klibc.h>
#include <tobyos/spinlock.h>
#include <tobyos/percpu.h>
#include <tobyos/gdt.h>
#include <tobyos/idt.h>
#include <tobyos/sched.h>

/* ---- imports from ap_trampoline.S ---- */

extern char     ap_trampoline_start[];
extern char     ap_trampoline_end[];
extern uint64_t ap_param_pml4;
extern uint64_t ap_param_stack;
extern uint64_t ap_param_entry;
extern uint32_t ap_param_idx;
extern uint8_t  ap_param_gdtr[10];
extern uint8_t  ap_param_idtr[10];

/* ---- module state ---- */

static struct percpu g_percpu[MAX_CPUS];
static uint32_t      g_cpu_count;
static spinlock_t    g_console_lock = SPINLOCK_INIT;
static volatile uint32_t g_aps_online_count;

/* Stack size per AP. 16 KiB matches the BSP's interrupt stack -- plenty
 * for "print one line and halt", and gives us headroom for any exception
 * dump if something goes wrong. */
#define AP_STACK_SIZE (16u * 1024u)

const struct percpu *smp_cpu(uint32_t idx) {
    return idx < g_cpu_count ? &g_percpu[idx] : 0;
}
struct percpu *smp_cpu_mut(uint32_t idx) {
    /* The BSS slot is always valid even before build_percpu_table()
     * fills it in (BSS is zero-initialised, which means an empty
     * ready queue + unlocked spinlock + current=NULL -- exactly
     * the right "no-op" initial state for the scheduler). Allow
     * any in-bounds slot so that sched_enqueue / sched_yield
     * called between sched_init and smp_start_aps still find
     * &g_percpu[0] instead of crashing on a NULL deref. */
    return idx < MAX_CPUS ? &g_percpu[idx] : 0;
}
uint32_t smp_cpu_count(void)    { return g_cpu_count; }
uint32_t smp_online_count(void) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < g_cpu_count; i++) {
        if (g_percpu[i].online) n++;
    }
    return n;
}

uint32_t smp_current_cpu_idx(void) {
    /* Before the percpu table is built, or before the LAPIC is
     * mapped, we're definitionally on the BSP (cpu 0). The first
     * legitimate caller is the LAPIC timer ISR, which can only fire
     * after apic_init_bsp has succeeded. */
    if (g_cpu_count == 0) return 0;
    if (!apic_is_ready())  return 0;
    uint32_t id = apic_read_id();
    for (uint32_t i = 0; i < g_cpu_count; i++) {
        if (g_percpu[i].apic_id == id) return i;
    }
    /* No match -- fall back to BSP. Should never happen unless an
     * AP is running but its slot was never filled (which would be
     * a build_percpu_table bug). */
    return 0;
}

struct percpu *smp_this_cpu(void) {
    if (g_cpu_count == 0) return &g_percpu[0];
    return &g_percpu[smp_current_cpu_idx()];
}

/* ---- shared kprintf wrapper (used by BSP + APs) ---- */

static void smp_logf(const char *fmt, ...) {
    va_list ap;
    uint64_t f = spin_lock_irqsave(&g_console_lock);
    va_start(ap, fmt);
    kvprintf(fmt, ap);
    va_end(ap);
    spin_unlock_irqrestore(&g_console_lock, f);
}

/* ---- AP entry point (runs on every secondary CPU) ---- */

/* Called from the trampoline's long-mode tail. RDI holds cpu_idx. The
 * stack is a per-AP kmalloc'd 16 KiB block, the kernel GDT/IDT are
 * already loaded by the trampoline (lgdt/lidt with the BSP's
 * pseudo-descriptors before the lretq into us). From here we behave
 * like any other kernel-mode C code, except we never return.
 *
 * Steps (Milestone 22 step 5):
 *   1. Software-enable this CPU's local APIC.
 *   2. Print "CPU N online" (locked).
 *   3. Publish online state to the BSP.
 *   4. Program this CPU's LAPIC timer to fire at 100 Hz on
 *      APIC_TIMER_VECTOR. The ISR is already wired in the IDT
 *      (registered once by apic_init_bsp on the BSP). The timer's
 *      `sti; hlt` pump lives inside sched_idle().
 *   5. Hand off to sched_idle() forever -- it loops on `sti; hlt`
 *      and consults this CPU's run queue on every wake-up. APs
 *      currently never run user procs (round-robin enqueue is
 *      pinned to the BSP in v1) but the timer ticks land here so
 *      sched_tick + per-CPU bookkeeping stay live.
 */
static __attribute__((noreturn, used)) void ap_entry(uint32_t cpu_idx) {
    apic_init_local();

    struct percpu *me = &g_percpu[cpu_idx];

    /* Sanity check: did the LAPIC ID survive the trip? Should equal
     * what ACPI told us. If not we still proceed (the message will
     * include both values). */
    uint32_t real_apic_id = apic_read_id();

    smp_logf("CPU %u online\n", (unsigned)cpu_idx);
    smp_logf("  apic_id=%u (acpi said %u)  stack_top=%p\n",
             (unsigned)real_apic_id, (unsigned)me->apic_id,
             (void *)me->stack_top);

    /* Publish in this order: per-CPU first (BSP polls this), then the
     * global counter. Both stores are RELEASE so the BSP's ACQUIRE
     * loads are guaranteed to see the prints/state above. */
    __atomic_store_n(&me->online, true, __ATOMIC_RELEASE);
    __atomic_add_fetch(&g_aps_online_count, 1, __ATOMIC_RELEASE);

    /* Start the per-CPU LAPIC timer. Failure isn't fatal -- the AP
     * just wakes only on external IRQs (which is fine in v1, since
     * sched_idle does nothing useful with the timer wake). */
    if (apic_timer_periodic_init(100)) {
        smp_logf("  cpu%u: LAPIC timer @ 100 Hz live (vec=0x%02x)\n",
                 (unsigned)cpu_idx, (unsigned)0x40);
    } else {
        smp_logf("  cpu%u: LAPIC timer init failed -- idling on extern IRQs\n",
                 (unsigned)cpu_idx);
    }

    /* Off to the scheduler idle loop. Never returns. */
    sched_idle();
}

/* ---- trampoline preparation ---- */

static void *trampoline_dst(void) {
    /* phys 0x8000 -> HHDM */
    return pmm_phys_to_virt(AP_TRAMPOLINE_PHYS);
}

/* Compute a pointer into the *destination* (low-phys) copy of the
 * trampoline using the offset of `field` inside the kernel-image copy.
 * This is how we patch ap_param_* without knowing their exact byte
 * offsets here (the assembler picks them).
 */
static void *tramp_field(void *field) {
    size_t off = (uintptr_t)field - (uintptr_t)ap_trampoline_start;
    return (uint8_t *)trampoline_dst() + off;
}

static void install_trampoline(void) {
    /* Copy the bytes from the kernel image to phys 0x8000 (via HHDM). */
    size_t blob_len = (size_t)(ap_trampoline_end - ap_trampoline_start);
    memcpy(trampoline_dst(), ap_trampoline_start, blob_len);

    /* Fill in the parameters that don't change between APs. */
    *(uint64_t *)tramp_field(&ap_param_pml4)  = vmm_kernel_pml4_phys();
    *(uint64_t *)tramp_field(&ap_param_entry) = (uint64_t)&ap_entry;

    /* Snapshot kernel GDTR + IDTR (10-byte pseudo-descriptors). */
    memcpy(tramp_field(&ap_param_gdtr), gdt_pseudo_descriptor(), 10);
    memcpy(tramp_field(&ap_param_idtr), idt_pseudo_descriptor(), 10);

    kprintf("[smp] trampoline installed: phys=%p virt=%p len=%lu bytes\n",
            (void *)AP_TRAMPOLINE_PHYS, trampoline_dst(),
            (unsigned long)blob_len);
}

static void per_ap_patch(uint32_t cpu_idx, uint64_t stack_top) {
    *(uint64_t *)tramp_field(&ap_param_stack) = stack_top;
    *(uint32_t *)tramp_field(&ap_param_idx)   = cpu_idx;
}

/* ---- enumerate CPUs from ACPI MADT ---- */

static void build_percpu_table(void) {
    const struct acpi_info *info = acpi_get();
    g_cpu_count = 0;
    memset(g_percpu, 0, sizeof(g_percpu));
    /* memset cleared the spinlocks to "unlocked" -- which is the
     * correct initial state for a SPINLOCK_INIT spinlock. The
     * per-CPU ready_head/ready_tail/current/timer_ticks are also
     * implicitly zeroed here, which is what we want. */

    if (!info || !info->ok || info->cpu_count == 0) {
        kprintf("[smp] no ACPI MADT -- assuming uniprocessor\n");
        g_percpu[0].cpu_idx = 0;
        g_percpu[0].apic_id = apic_read_id();
        g_percpu[0].is_bsp  = true;
        g_percpu[0].online  = true;     /* BSP is, definitionally, online */
        g_cpu_count = 1;
        return;
    }

    /* The BSP is the CPU executing this code. Find its APIC ID and
     * place that ACPI entry first, so cpu_idx 0 is always the BSP. */
    uint32_t bsp_apic_id = apic_read_id();
    int bsp_acpi_idx = -1;
    for (uint32_t i = 0; i < info->cpu_count; i++) {
        if (info->cpus[i].apic_id == bsp_apic_id) { bsp_acpi_idx = (int)i; break; }
    }

    if (bsp_acpi_idx < 0) {
        kprintf("[smp] WARN: BSP apic_id=%u not in MADT, taking entry 0\n",
                bsp_apic_id);
        bsp_acpi_idx = 0;
    }

    g_percpu[0].cpu_idx = 0;
    g_percpu[0].apic_id = info->cpus[bsp_acpi_idx].apic_id;
    g_percpu[0].is_bsp  = true;
    g_percpu[0].online  = true;
    g_cpu_count = 1;

    for (uint32_t i = 0; i < info->cpu_count; i++) {
        if ((int)i == bsp_acpi_idx) continue;
        if (!info->cpus[i].enabled && !info->cpus[i].online_capable) {
            kprintf("[smp] skipping ACPI cpu%u (apic_id=%u): not usable\n",
                    i, info->cpus[i].apic_id);
            continue;
        }
        if (g_cpu_count >= MAX_CPUS) {
            kprintf("[smp] cpu table full (%u), dropping rest\n", MAX_CPUS);
            break;
        }
        g_percpu[g_cpu_count].cpu_idx = g_cpu_count;
        g_percpu[g_cpu_count].apic_id = info->cpus[i].apic_id;
        g_percpu[g_cpu_count].is_bsp  = false;
        g_percpu[g_cpu_count].online  = false;
        g_cpu_count++;
    }

    kprintf("[smp] %u CPU(s) total: BSP apic_id=%u\n",
            g_cpu_count, bsp_apic_id);
    for (uint32_t i = 0; i < g_cpu_count; i++) {
        kprintf("[smp]   cpu%u: apic_id=%u%s\n",
                g_percpu[i].cpu_idx, g_percpu[i].apic_id,
                g_percpu[i].is_bsp ? " (BSP)" : "");
    }
}

/* ---- INIT-SIPI-SIPI for one AP ---- */

static bool start_one_ap(uint32_t cpu_idx) {
    struct percpu *cpu = &g_percpu[cpu_idx];

    /* Allocate a 16 KiB stack. kmalloc lives in heap-virt, well above
     * the user half, well above HHDM -- safe in our higher-half kernel. */
    void *stack = kmalloc(AP_STACK_SIZE);
    if (!stack) {
        kprintf("[smp] cpu%u: failed to allocate stack\n", cpu_idx);
        return false;
    }
    cpu->stack_top = (uint64_t)stack + AP_STACK_SIZE;
    /* SysV ABI wants RSP 16-byte aligned at function entry. The trampoline
     * does an indirect "lretq" into ap_entry which leaves RSP at the
     * value we loaded -- so we align here. */
    cpu->stack_top &= ~(uint64_t)0xF;

    per_ap_patch(cpu_idx, cpu->stack_top);

    uint8_t apic_id = (uint8_t)cpu->apic_id;
    uint8_t vector  = (uint8_t)(AP_TRAMPOLINE_PHYS >> 12);

    kprintf("[smp] cpu%u: INIT-SIPI-SIPI to apic_id=%u (tramp_vector=0x%02x, stack=%p)\n",
            cpu_idx, apic_id, vector, (void *)cpu->stack_top);

    /* Snapshot the global online count BEFORE we kick the AP. On QEMU
     * the AP can finish printing and bump the counter faster than we
     * fall through these few instructions, so capturing `before` after
     * the SIPIs would miss the transition. We watch this CPU's per-CPU
     * `online` flag too, for robustness against any spurious cross-AP
     * counter bumps in future code. */
    uint32_t before = __atomic_load_n(&g_aps_online_count, __ATOMIC_ACQUIRE);
    cpu->online = false;

    /* INIT IPI. Spec wants ~10 ms of dwell after INIT before the first
     * SIPI; the PIT gives us that easily. */
    apic_send_init(apic_id);
    pit_sleep_ms(10);

    /* First SIPI. Wait ~200us; PIT resolution is 10 ms but a 1-tick
     * wait is fine and matches Linux/SeaBIOS practice (200us is a
     * lower bound, not an upper). */
    apic_send_sipi(apic_id, vector);
    pit_sleep_ms(1);

    /* Second SIPI. The spec mandates two; the AP latches whichever
     * arrives first. */
    apic_send_sipi(apic_id, vector);

    /* Wait for the AP to phone home. Cap the wait at ~1 second so a
     * busted AP doesn't hang boot forever. */
    uint64_t deadline = pit_ticks() + (uint64_t)pit_hz();   /* ~1s */
    while (pit_ticks() < deadline) {
        if (__atomic_load_n(&cpu->online, __ATOMIC_ACQUIRE)) {
            return true;
        }
        if (__atomic_load_n(&g_aps_online_count, __ATOMIC_ACQUIRE) != before) {
            /* Some AP came online (us, almost certainly, since we
             * serialise startup). Re-check our flag once before
             * declaring victory. */
            if (__atomic_load_n(&cpu->online, __ATOMIC_ACQUIRE)) return true;
        }
        __asm__ volatile ("pause" ::: "memory");
    }

    kprintf("[smp] cpu%u: timed out waiting for online flag\n", cpu_idx);
    return false;
}

/* ---- public entry ---- */

uint32_t smp_start_aps(void) {
    if (!apic_is_ready()) {
        kprintf("[smp] LAPIC not ready -- staying single-CPU\n");
        build_percpu_table();
        smp_logf("CPU 0 online\n");
        smp_logf("  apic_id=%u (BSP, no ACPI/APIC)\n", (unsigned)apic_read_id());
        return 1;
    }

    build_percpu_table();

    /* Always announce the BSP first, even if there are no APs to wake.
     * That way the output reads naturally from CPU 0 upward. */
    smp_logf("CPU 0 online\n");
    smp_logf("  apic_id=%u (BSP)  stack=bootloader-provided\n",
             (unsigned)g_percpu[0].apic_id);

    if (g_cpu_count <= 1) {
        kprintf("[smp] uniprocessor -- nothing to wake\n");
        return 1;
    }

    /* The PMM already reserved AP_TRAMPOLINE_PHYS at boot (see kernel.c).
     * What we still owe ourselves: identity-map it into the kernel PML4
     * so the AP can keep executing through the CR0.PG transition (RIP
     * is at low phys at that instant). */
    if (!vmm_map(AP_TRAMPOLINE_PHYS, AP_TRAMPOLINE_PHYS, PAGE_SIZE,
                 VMM_PRESENT | VMM_WRITE)) {
        kprintf("[smp] failed to identity-map trampoline page\n");
        return 1;
    }
    install_trampoline();

    /* Bring up APs one at a time. Serialising means we don't need a
     * lock around ap_param_* (each AP reads its values during the
     * narrow window before the next BSP write). */
    uint32_t brought_up = 0;
    for (uint32_t i = 1; i < g_cpu_count; i++) {
        if (start_one_ap(i)) brought_up++;
    }

    kprintf("[smp] %u/%u APs online (total %u CPUs)\n",
            brought_up, g_cpu_count - 1, smp_online_count());
    return smp_online_count();
}
