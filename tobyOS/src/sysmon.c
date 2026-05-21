/* sysmon.c -- live desktop/system-monitor metrics.
 *
 * The desktop wants "real enough to trust" numbers without adding a
 * scheduler plugin or a userspace daemon yet. We therefore sample:
 *   - RAM from the physical page allocator;
 *   - CPU from per-process cpu_ns deltas, excluding pid 0 idle time;
 *   - GUI/VFS/net activity from perf zones already instrumented in
 *     those subsystems;
 *   - network state from net_status()/g_my_ip.
 *
 * The percentages are intentionally activity percentages over the last
 * sample interval, not capacity promises. For example "Disk" is VFS
 * read/write activity, because tobyOS does not yet have a generic disk
 * utilisation model exposed across every filesystem/backend. */

#include <tobyos/sysmon.h>
#include <tobyos/proc.h>
#include <tobyos/pmm.h>
#include <tobyos/perf.h>
#include <tobyos/net.h>
#include <tobyos/klibc.h>

struct sysmon_state {
    bool     have_prev;
    uint64_t prev_ns;
    uint64_t prev_cpu_user_ns;
    uint64_t prev_syscalls;
    uint64_t prev_gui_frames;
    uint64_t prev_vfs_ops;
    uint64_t prev_vfs_ns;
    uint64_t prev_gui_ns;
    uint64_t prev_net_packets;
    uint64_t prev_net_ns;

    uint32_t cpu_pct;
    uint32_t gui_pct;
    uint32_t disk_pct;
    uint32_t net_pct;
    uint32_t syscalls_per_s;
    uint32_t gui_fps;
    uint32_t vfs_ops_per_s;
    uint32_t net_packets_per_s;

    struct abi_system_metrics last;
};

static struct sysmon_state g_sysmon;

static uint32_t pct_from_delta(uint64_t work_ns, uint64_t wall_ns) {
    if (wall_ns == 0) return 0;
    uint64_t pct = (work_ns * 100ull) / wall_ns;
    if (pct > 100ull) pct = 100ull;
    return (uint32_t)pct;
}

static uint32_t rate_per_s(uint64_t delta, uint64_t wall_ns) {
    if (wall_ns == 0) return 0;
    uint64_t r = (delta * 1000000000ull) / wall_ns;
    if (r > 0xFFFFFFFFull) r = 0xFFFFFFFFull;
    return (uint32_t)r;
}

static uint64_t zone_count(enum perf_zone z) {
    const struct perf_zone_stats *s = perf_zone_get(z);
    return s ? s->count : 0;
}

static uint64_t zone_ns(enum perf_zone z) {
    const struct perf_zone_stats *s = perf_zone_get(z);
    return s ? s->total_ns : 0;
}

static void collect_absolute(struct abi_system_metrics *out) {
    memset(out, 0, sizeof(*out));

    struct perf_sys sys;
    perf_sys_snapshot(&sys);
    out->uptime_ms        = sys.boot_ns / 1000000ull;
    out->total_syscalls   = sys.total_syscalls;
    out->context_switches = sys.context_switches;
    out->gui_frames       = sys.gui_frames;
    out->proc_spawns      = sys.proc_spawns;
    out->proc_exits       = sys.proc_exits;

    out->total_pages = (uint64_t)pmm_total_pages();
    out->used_pages  = (uint64_t)pmm_used_pages();
    out->free_pages  = (uint64_t)pmm_free_pages();
    if (out->total_pages) {
        out->ram_pct = (uint32_t)((out->used_pages * 100ull) /
                                  out->total_pages);
    }

    uint64_t now_tsc = perf_rdtsc();
    for (int i = 0; i < PROC_MAX; i++) {
        struct proc *p = proc_lookup(i);
        if (!p) continue;

        out->process_count++;
        if (p->state == PROC_READY) out->ready_count++;
        if (p->state == PROC_BLOCKED) out->blocked_count++;

        uint64_t cpu = p->cpu_ns;
        if (p->state == PROC_RUNNING && p->last_switch_tsc) {
            cpu += perf_tsc_to_ns(now_tsc - p->last_switch_tsc);
        }
        out->cpu_total_ns += cpu;
        if (p->pid != 0) out->cpu_user_ns += cpu;
    }

    uint64_t vfs_ops = zone_count(PERF_Z_VFS_READ) +
                       zone_count(PERF_Z_VFS_WRITE);
    uint64_t vfs_ns  = zone_ns(PERF_Z_VFS_READ) +
                       zone_ns(PERF_Z_VFS_WRITE);
    uint64_t gui_ns  = zone_ns(PERF_Z_GUI_COMPOSITE) +
                       zone_ns(PERF_Z_GUI_FLIP);
    uint64_t net_pk  = zone_count(PERF_Z_NET_RX) +
                       zone_count(PERF_Z_NET_TX);
    uint64_t net_ns  = zone_ns(PERF_Z_NET_RX) +
                       zone_ns(PERF_Z_NET_TX);

    out->vfs_ops     = vfs_ops;
    out->vfs_ns      = vfs_ns;
    out->gui_ns      = gui_ns;
    out->net_packets = net_pk;
    out->net_ns      = net_ns;

    out->net_up    = net_is_up() ? 1u : 0u;
    out->net_ip_be = g_my_ip;
    out->tsc_mhz   = perf_tsc_khz() / 1000u;
}

void sysmon_sample(struct abi_system_metrics *out) {
    struct abi_system_metrics snap;
    collect_absolute(&snap);

    uint64_t now_ns = perf_now_ns();
    if (g_sysmon.have_prev && now_ns > g_sysmon.prev_ns) {
        uint64_t wall = now_ns - g_sysmon.prev_ns;
        uint64_t dcpu = snap.cpu_user_ns > g_sysmon.prev_cpu_user_ns
                      ? snap.cpu_user_ns - g_sysmon.prev_cpu_user_ns : 0;
        uint64_t dsys = snap.total_syscalls > g_sysmon.prev_syscalls
                      ? snap.total_syscalls - g_sysmon.prev_syscalls : 0;
        uint64_t dfrm = snap.gui_frames > g_sysmon.prev_gui_frames
                      ? snap.gui_frames - g_sysmon.prev_gui_frames : 0;
        uint64_t dvop = snap.vfs_ops > g_sysmon.prev_vfs_ops
                      ? snap.vfs_ops - g_sysmon.prev_vfs_ops : 0;
        uint64_t dvns = snap.vfs_ns > g_sysmon.prev_vfs_ns
                      ? snap.vfs_ns - g_sysmon.prev_vfs_ns : 0;
        uint64_t dgns = snap.gui_ns > g_sysmon.prev_gui_ns
                      ? snap.gui_ns - g_sysmon.prev_gui_ns : 0;
        uint64_t dnpk = snap.net_packets > g_sysmon.prev_net_packets
                      ? snap.net_packets - g_sysmon.prev_net_packets : 0;
        uint64_t dnns = snap.net_ns > g_sysmon.prev_net_ns
                      ? snap.net_ns - g_sysmon.prev_net_ns : 0;

        g_sysmon.cpu_pct = pct_from_delta(dcpu, wall);
        g_sysmon.gui_pct = pct_from_delta(dgns, wall);
        g_sysmon.disk_pct = pct_from_delta(dvns, wall);
        g_sysmon.net_pct = pct_from_delta(dnns, wall);
        g_sysmon.syscalls_per_s = rate_per_s(dsys, wall);
        g_sysmon.gui_fps = rate_per_s(dfrm, wall);
        g_sysmon.vfs_ops_per_s = rate_per_s(dvop, wall);
        g_sysmon.net_packets_per_s = rate_per_s(dnpk, wall);

        /* Keep low-but-real activity visible in the tiny desktop graph. */
        if (dfrm && g_sysmon.gui_pct == 0) g_sysmon.gui_pct = 1;
        if (dvop && g_sysmon.disk_pct == 0) g_sysmon.disk_pct = 1;
        if (dnpk && g_sysmon.net_pct == 0) g_sysmon.net_pct = 1;
    }

    g_sysmon.prev_ns          = now_ns;
    g_sysmon.prev_cpu_user_ns = snap.cpu_user_ns;
    g_sysmon.prev_syscalls    = snap.total_syscalls;
    g_sysmon.prev_gui_frames  = snap.gui_frames;
    g_sysmon.prev_vfs_ops     = snap.vfs_ops;
    g_sysmon.prev_vfs_ns      = snap.vfs_ns;
    g_sysmon.prev_gui_ns      = snap.gui_ns;
    g_sysmon.prev_net_packets = snap.net_packets;
    g_sysmon.prev_net_ns      = snap.net_ns;
    g_sysmon.have_prev        = true;

    snap.cpu_pct           = g_sysmon.cpu_pct;
    snap.gui_pct           = g_sysmon.gui_pct;
    snap.disk_pct          = g_sysmon.disk_pct;
    snap.net_pct           = g_sysmon.net_pct;
    snap.syscalls_per_s    = g_sysmon.syscalls_per_s;
    snap.gui_fps           = g_sysmon.gui_fps;
    snap.vfs_ops_per_s     = g_sysmon.vfs_ops_per_s;
    snap.net_packets_per_s = g_sysmon.net_packets_per_s;

    g_sysmon.last = snap;
    if (out) *out = snap;
}

void sysmon_snapshot(struct abi_system_metrics *out) {
    if (!out) return;
    if (!g_sysmon.have_prev) {
        sysmon_sample(out);
        return;
    }
    *out = g_sysmon.last;
}
