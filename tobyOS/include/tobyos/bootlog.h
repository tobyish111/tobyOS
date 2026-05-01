/* bootlog.h -- capture kprintf to RAM, flush to disk after bring-up.
 *
 * Used for bring-up on real hardware: persists a text trace to
 *   /data/boot.log       (tobyfs, truncated ~60 KiB — FS file cap)
 *   /usb/BOOTLOG.TXT and /usb/BOOT.TXT (same payload; FAT32 volume root.
 *   Unplug after boot settles so MSC cache can flush.)
 *
 * If /usb was unmounted during boot self-tests, bootlog_flush_all()
 * tries the same USB+FAT32 discovery path as kernel boot to remount
 * /usb before writing.
 *
 * bootlog_flush_usb_retry() runs a second USB-only write (same
 * BOOTLOG.TXT path) after the desktop is up — MSC/FAT32 on the live
 * USB often appears only after the idle loop has polled xHCI a few
 * times, so the first flush at end-of-kmain can miss while the later
 * retry succeeds.
 *
 * bootlog_net_upload() sends the RAM capture over UDP to 192.168.68.74
 * port 40123 once the GUI desktop is up (~300 ms after desktop_mode),
 * so the collector sees the full boot through kmain plus the first
 * idle ticks. Safe-mode / no-GUI boots call it from the kernel right
 * after bootlog_flush_all(). Collector:
 *   ncat -u -l -p 40123 > bootlog.txt
 * Skipped for QEMU SLIRP (10.0.2.15) when built with TOBY_NET_FALLBACK_SLIRP.
 */

#ifndef TOBYOS_BOOTLOG_H
#define TOBYOS_BOOTLOG_H

void bootlog_init(void);
void bootlog_char(char c);
void bootlog_flush_all(void);
void bootlog_flush_usb_retry(void);
void bootlog_net_upload(void);

#endif /* TOBYOS_BOOTLOG_H */
