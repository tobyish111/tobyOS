/* programs/tsh/kstubs.c -- the kernel-only half of src/shell.c, stubbed out.
 *
 * shell.c carries 96 builtins. Roughly half are the LANGUAGE (echo, test,
 * read, shift, export, trap, getopts...) and work anywhere; the rest drive
 * kernel subsystems (blkdump, ifconfig, pkg, gui, perf, install...) and
 * cannot mean anything in a user process. Those reach for ~120 kernel symbols
 * that do not exist here.
 *
 * Rather than carve them out of shell.c with #ifdefs -- 90-odd edit sites in a
 * file that IS the OS's interactive shell, every one of them a chance to break
 * it -- the symbols are satisfied here. shell.c stays byte-identical for the
 * kernel build, which is the whole reason this arrangement is safe.
 *
 * Each stub SAYS SO when called, once per symbol. A stub that silently
 * returned 0 would let `ifconfig` in tsh print a confident, entirely
 * fictional network configuration -- the exact failure mode this project
 * keeps finding elsewhere (fabricated statfs, no-op fsync). Better to be
 * useless and honest than plausible and wrong.
 *
 * GENERATED, then hand-finished: the symbol list came from
 *   nm -u shell_hosted.o  minus  what libtoby and host.c provide.
 * If a builtin ever needs to work in userspace, delete its stub and
 * implement it in host.c -- the link error will point at exactly what is
 * missing.
 */

#include <stdio.h>
#include <stdint.h>

static void tsh_unavailable(const char *name) {
    /* Once per symbol: a builtin in a loop should not flood the terminal. */
    static const char *seen[160];
    static int n;
    for (int i = 0; i < n; i++) if (seen[i] == name) return;
    if (n < (int)(sizeof seen / sizeof seen[0])) seen[n++] = name;
    fprintf(stderr, "tsh: %s: kernel-only, not available in userspace\n", name);
}

#define KSTUB(name)  long name(void) { tsh_unavailable(#name); return 0; }
#define KSTUB_STR(name) \
    const char *name(void) { tsh_unavailable(#name); return "(n/a)"; }

/* ---- variables the kernel defines elsewhere ----------------------------- */
const uint8_t GPT_TYPE_BIOS_BOOT[16];
const uint8_t GPT_TYPE_EFI_SYSTEM[16];
const uint8_t GPT_TYPE_LINUX_FS[16];
const uint8_t GPT_TYPE_LINUX_HOME[16];
const uint8_t GPT_TYPE_MS_BASIC_DATA[16];
const uint8_t GPT_TYPE_TOBYOS_DATA[16];
uint32_t g_my_ip, g_my_netmask, g_gateway_ip, g_my_dns_be;
uint8_t  g_my_mac[6];
/* limine_module_request; sized generously so cmd_modules reads in-bounds. */
volatile unsigned char module_req[256];

/* ---- string-returning stubs (a NULL here would fault inside printf) ----- */
KSTUB_STR(http_strerror)
KSTUB_STR(log_cat_name)
KSTUB_STR(proc_state_name)
KSTUB_STR(slog_level_name)

/* ---- everything else ---------------------------------------------------- */
KSTUB(acpi_reboot)
KSTUB(acpi_shutdown)
KSTUB(apic_read_id)
KSTUB(arp_dump)
KSTUB(blk_count)
KSTUB(blk_dump)
KSTUB(blk_find)
KSTUB(blk_get)
KSTUB(blk_get_first)
/* 2026-08-25: `install` now LISTS candidate disks (blk_iter_next) and
 * reports what each carries (provision_classify) instead of silently
 * taking blk_get_first(). Both are kernel-side; /bin/tsh cannot install
 * anything anyway -- installer_run is stubbed just below. */
KSTUB(blk_iter_next)
KSTUB(provision_classify)
KSTUB(cap_dump_proc)
KSTUB(cap_mask_to_string)
KSTUB(cap_profile_foreach)
KSTUB(cap_profile_lookup)
KSTUB(console_backspace)
KSTUB(console_clear)
KSTUB(console_get_size)
KSTUB(console_set_color)
KSTUB(devtest_dump_kprintf)
KSTUB(devtest_for_each)
KSTUB(devtest_run)
KSTUB(dns_resolve)
KSTUB(drvmatch_disable_pci)
KSTUB(drvmatch_dump_kprintf)
KSTUB(drvmatch_reenable_pci)
KSTUB(fat32_mount)
KSTUB(fat32_probe)
KSTUB(gui_dump_status)
KSTUB(gui_emergency_exit)
KSTUB(gui_in_desktop_mode)
KSTUB(gui_set_desktop_mode)
KSTUB(gui_trace_level)
KSTUB(gui_trace_set)
KSTUB(heap_stats)
KSTUB(heap_virt_base)
KSTUB(heap_virt_brk)
KSTUB(heap_virt_end)
KSTUB(http_free)
KSTUB(hwinfo_dump_kprintf)
KSTUB(hwinfo_persist)
KSTUB(installer_image_available)
KSTUB(installer_image_size)
KSTUB(installer_run)
KSTUB(log_cat_from_name)
KSTUB(log_disable)
KSTUB(log_enable)
KSTUB(log_mask)
KSTUB(lx_dump_gaps)
KSTUB(net_dhcp_renew)
KSTUB(net_format_ip)
KSTUB(net_format_mac)
KSTUB(net_is_up)
KSTUB(partition_guid_cmp)
KSTUB(partition_scan_all)
KSTUB(partition_scan_disk)
KSTUB(perf_dump_sys)
KSTUB(perf_dump_syscalls)
KSTUB(perf_dump_zones)
KSTUB(perf_reset)
KSTUB(perf_set_enabled)
KSTUB(perf_sys_snapshot)
KSTUB(pkg_info)
KSTUB(pkg_install_name)
KSTUB(pkg_install_path)
KSTUB(pkg_install_url)
KSTUB(pkg_list)
KSTUB(pkg_remove)
KSTUB(pkg_repo_dump)
KSTUB(pkg_rollback)
KSTUB(pkg_update)
KSTUB(pkg_upgrade_all)
KSTUB(pkg_upgrade_one)
KSTUB(pmm_free_pages)
KSTUB(pmm_phys_to_virt)
KSTUB(pmm_total_pages)
KSTUB(pmm_used_pages)
KSTUB(proc_dump_table)
KSTUB(proc_lookup)
KSTUB(sectest_run)
/* NOT a complaining stub: the shell calls this for every foreground
 * pipeline, and hosted tsh legitimately cannot own the console foreground
 * -- that is the kernel shell's job. The one-line complaint was harmless
 * noise on captured stderr and a parity-breaking byte stream on a pty,
 * where fd 2 IS the terminal the gate compares. */
long signal_set_foreground(void) { return 0; }
KSTUB(slog_drain)
KSTUB(slog_level_from_name)
KSTUB(slog_stats)
KSTUB(smp_cpu)
KSTUB(smp_cpu_count)
KSTUB(smp_online_count)
KSTUB(sock_dump)
KSTUB(tcp_close)
KSTUB(tcp_connect)
KSTUB(tcp_dump)
KSTUB(tobyfs_mount)
KSTUB(users_add)
KSTUB(users_lookup_by_name)
KSTUB(users_lookup_by_uid)
KSTUB(users_save)
KSTUB(users_visit)
KSTUB(vfs_dump_mounts)
KSTUB(vfs_rmtree)
KSTUB(vmm_dump)
KSTUB(vmm_translate)
