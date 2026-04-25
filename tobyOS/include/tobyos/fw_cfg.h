/* fw_cfg.h -- minimal QEMU firmware configuration interface (M26G).
 *
 * The fw_cfg device is a tiny PIO interface that QEMU exposes to the
 * guest for passing arbitrary key/value blobs from the host command
 * line into the running kernel. It's the cleanest way for an automated
 * test harness to inject "pretend you have a battery with 75% charge"
 * into a guest that otherwise has no battery hardware emulated.
 *
 * Spec reference: https://www.qemu.org/docs/master/specs/fw_cfg.html
 *
 * Usage from QEMU:
 *
 *   -fw_cfg name=opt/tobyos/battery_mock,string=state=charging,percent=75,...
 *
 * Usage from the kernel:
 *
 *   if (fw_cfg_present()) {
 *       char buf[256];
 *       int  n = fw_cfg_read_file("opt/tobyos/battery_mock",
 *                                 buf, sizeof(buf) - 1);
 *       if (n > 0) { buf[n] = '\0'; ... parse ... }
 *   }
 *
 * We only implement the legacy PIO interface (read-only). The DMA
 * interface is faster but requires a few more registers and a tiny
 * physical-memory shuttle buffer; we don't need the speed for the
 * sub-kilobyte blobs M26G uses.
 *
 * Boot dependency: must run after vmm_init (because we need pmm_alloc
 * for any large reads, and we need to be in long mode with PIO
 * accessible). In practice we call fw_cfg_init() from kmain right
 * before acpi_bat_init().
 */

#ifndef TOBYOS_FW_CFG_H
#define TOBYOS_FW_CFG_H

#include <tobyos/types.h>

/* Detect QEMU's fw_cfg device by reading the 4-byte signature at
 * selector 0. Idempotent and very cheap (4 byte-PIO reads). On a
 * real PC (no QEMU) the read returns garbage and we report absent.
 *
 * Returns true if "QEMU" was seen in the signature, false otherwise.
 * Subsequent calls just return the cached result. */
bool fw_cfg_init(void);

/* True iff fw_cfg_init() saw the signature. Safe to call before init
 * (returns false). Modules use this to gate any fw_cfg lookups so
 * they don't add latency on bare-metal boots. */
bool fw_cfg_present(void);

/* Look up a named fw_cfg file, copy at most cap bytes of its content
 * into out, and return the number of bytes copied. Returns -1 if
 * fw_cfg isn't present or the file isn't found, 0 if the file is
 * empty. The lookup walks the file directory at selector 0x0019,
 * which is bounded by the number of -fw_cfg entries on the QEMU
 * command line (handful at most), so it's O(N) per call but N is
 * tiny in practice.
 *
 * The caller is responsible for any NUL-termination it needs (this
 * is not a string API; some fw_cfg blobs are binary). */
int  fw_cfg_read_file(const char *name, void *out, size_t cap);

#endif /* TOBYOS_FW_CFG_H */
