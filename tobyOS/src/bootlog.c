/* bootlog.c -- RAM ring of kprintf output, flush to tobyfs + FAT USB. */

#include <tobyos/bootlog.h>
#include <tobyos/printk.h>
#include <tobyos/vfs.h>
#include <tobyos/klibc.h>
#include <tobyos/blk.h>
#include <tobyos/fat32.h>
#include <tobyos/net.h>
#include <tobyos/udp.h>
#include <tobyos/pit.h>
#include <tobyos/cpu.h>

/* After a successful DHCP lease, upload to this host on the same LAN.
 * UDP so the collector only needs: ncat -u -l -p 40123 > bootlog.txt */
#define BOOTLOG_NET_DST_DHCP ip4(192, 168, 68, 74)
#define BOOTLOG_NET_UDP_PORT htons(40123)
#define BOOTLOG_NET_SRC_PORT htons(49999)
#define BOOTLOG_NET_CHUNK    1000u
#define BOOTLOG_NET_ARP_MS   500u

/* Large capture for USB FAT (tobyfs /data is capped much smaller). */
#define BOOTLOG_RAM_BYTES   (256u * 1024u)
#define BOOTLOG_TOBYFS_BODY 60000u

static char     g_buf[BOOTLOG_RAM_BYTES];
static uint32_t g_len;
static uint32_t g_overflow;
static bool     g_inited;
static bool     g_in_flush;

void bootlog_init(void) { g_inited = true; }

void bootlog_char(char c) {
    if (!g_inited || g_in_flush) return;
    if (g_len < BOOTLOG_RAM_BYTES) {
        g_buf[g_len++] = c;
    } else {
        g_overflow++;
    }
}

/* Same USB+FAT32 discovery as kernel.c (install stick, MSC names). */
static int bootlog_try_mount_usb_fat(void) {
    struct vfs_stat st;
    if (vfs_stat("/usb", &st) == VFS_OK && st.type == VFS_TYPE_DIR)
        return 0;

    size_t it = 0;
    struct blk_dev *p;
    while ((p = blk_iter_next(&it, BLK_CLASS_PARTITION)) != NULL) {
        if (!p->parent || !p->parent->name) continue;
        const char *pn = p->parent->name;
        if (pn[0] != 'u' || pn[1] != 's' || pn[2] != 'b') continue;
        if (!fat32_probe(p)) continue;
        if (fat32_mount("/usb", p) == VFS_OK) {
            kprintf("[bootlog] remounted /usb for log write (partition '%s')\n",
                    p->name);
            return 0;
        }
    }
    it = 0;
    struct blk_dev *d;
    while ((d = blk_iter_next(&it, BLK_CLASS_DISK)) != NULL) {
        if (!d->name) continue;
        if (d->name[0] != 'u' || d->name[1] != 's' || d->name[2] != 'b')
            continue;
        if (!fat32_probe(d)) continue;
        if (fat32_mount("/usb", d) == VFS_OK) {
            kprintf("[bootlog] remounted /usb for log write (disk '%s')\n",
                    d->name);
            return 0;
        }
    }
    return -1;
}

/* Retry udp_send until ARP fills or deadline (ip_send drops on ARP miss). */
static bool bootlog_udp_send_wait(struct net_dev *nd, uint16_t src_port_be,
                                  uint32_t dst_ip_be, uint16_t dst_port_be,
                                  const void *payload, size_t payload_len,
                                  uint32_t wait_ms) {
    uint32_t hz = pit_hz();
    if (hz == 0) hz = 100;
    uint64_t end =
        pit_ticks() + ((uint64_t)hz * (uint64_t)wait_ms) / 1000u;
    while (pit_ticks() < end) {
        if (udp_send(src_port_be, dst_ip_be, dst_port_be, payload, payload_len))
            return true;
        if (nd && nd->rx_drain) nd->rx_drain(nd);
        sti();
        hlt();
    }
    return false;
}

void bootlog_net_upload(void) {
    if (!net_is_up() || g_my_ip == 0) {
        kprintf("[bootlog] net upload skipped (stack down or no IP)\n");
        return;
    }
#ifdef TOBY_NET_FALLBACK_SLIRP
    /* QEMU user-net: no path to a real LAN collector. */
    if (g_my_ip == ip4(10, 0, 2, 15)) {
        kprintf("[bootlog] net upload skipped (SLIRP)\n");
        return;
    }
#endif

    struct net_dev *nd = net_default();
    if (!nd) return;

    char dstbuf[16];
    net_format_ip(dstbuf, BOOTLOG_NET_DST_DHCP);

    char meta[80];
    int ml = ksnprintf(meta, sizeof meta, "TOBYOS_BOOTLOG v1 len=%u\n",
                       (unsigned)g_len);
    if (ml < 0) ml = 0;
    if ((size_t)ml >= sizeof meta) ml = (int)sizeof meta - 1;

    if (!bootlog_udp_send_wait(nd, BOOTLOG_NET_SRC_PORT, BOOTLOG_NET_DST_DHCP,
                                BOOTLOG_NET_UDP_PORT, meta, (size_t)ml,
                                BOOTLOG_NET_ARP_MS)) {
        kprintf("[bootlog] net upload: failed first packet (ARP? dst=%s)\n",
                dstbuf);
        return;
    }

    size_t sent = 0;
    while (sent < g_len) {
        size_t n = g_len - sent;
        if (n > BOOTLOG_NET_CHUNK) n = BOOTLOG_NET_CHUNK;
        if (!bootlog_udp_send_wait(nd, BOOTLOG_NET_SRC_PORT, BOOTLOG_NET_DST_DHCP,
                                    BOOTLOG_NET_UDP_PORT, g_buf + sent, n,
                                    BOOTLOG_NET_ARP_MS)) {
            kprintf("[bootlog] net upload: stopped at offset %lu/%u\n",
                    (unsigned long)sent, (unsigned)g_len);
            return;
        }
        sent += n;
    }
    kprintf("[bootlog] net upload: sent %lu bytes to %s:40123/udp\n",
            (unsigned long)g_len + (unsigned long)ml, dstbuf);
}

/* FAT/USB stacks may mis-handle one huge write; keep chunks modest. */
enum { BOOTLOG_VFS_WRITE_CHUNK = 8192u };

static bool bootlog_vfs_write_all(struct vfs_file *f, const void *buf,
                                  size_t total, const char *label) {
    const uint8_t *p = (const uint8_t *)buf;
    size_t          off = 0;
    while (off < total) {
        size_t n = total - off;
        if (n > BOOTLOG_VFS_WRITE_CHUNK) n = BOOTLOG_VFS_WRITE_CHUNK;
        long w = vfs_write(f, p + off, n);
        if (w < 0) {
            kprintf("[bootlog] %s: vfs_write failed at off=%lu: err %ld\n",
                    label, (unsigned long)off, w);
            return false;
        }
        if ((size_t)w != n) {
            kprintf("[bootlog] %s: partial write off=%lu got=%ld want=%lu\n",
                    label, (unsigned long)off, w, (unsigned long)n);
            return false;
        }
        off += (size_t)w;
    }
    return true;
}

static void bootlog_write_one(const char *path, size_t body_max,
                              const char *label) {
    if (!path || body_max == 0) return;

    size_t body = g_len;
    if (body > body_max) body = body_max;

    char head[224];
    int hl = ksnprintf(head, sizeof head,
                       "[bootlog] path=%s captured=%u written=%lu "
                       "overflow_discarded=%u\n",
                       label, (unsigned)g_len, (unsigned long)body,
                       (unsigned)g_overflow);
    if (hl < 0) hl = 0;
    if ((size_t)hl >= sizeof head) hl = (int)sizeof head - 1;

    /* Remove stale file so FAT32 gets a fresh dirent + cluster chain.
     * Re-opening an existing large BOOTLOG.TXT without unlink left
     * wrong on-disk sizes / tails on some flush + replug sequences. */
    int ur = vfs_unlink(path);
    if (ur != VFS_OK && ur != VFS_ERR_NOENT) {
        kprintf("[bootlog] %s: vfs_unlink('%s'): %s (continuing)\n",
                label, path, vfs_strerror(ur));
    }

    int cr = vfs_create(path);
    if (cr != VFS_OK) {
        kprintf("[bootlog] %s: vfs_create: %s\n", label, vfs_strerror(cr));
        return;
    }
    struct vfs_file f;
    int rc = vfs_open(path, &f);
    if (rc != VFS_OK) {
        kprintf("[bootlog] %s: vfs_open: %s\n", label, vfs_strerror(rc));
        return;
    }
    bool ok = bootlog_vfs_write_all(&f, head, (size_t)hl, label) &&
              (body == 0 || bootlog_vfs_write_all(&f, g_buf, body, label));
    vfs_close(&f);
    if (!ok) return;
    kprintf("[bootlog] wrote %s (%lu bytes payload + %d header)\n",
            path, (unsigned long)body, hl);
}

/* FAT32 install/live USB only (no /data). Caller sets g_in_flush. */
static void bootlog_flush_usb_fat_only(const char *phase) {
    struct vfs_stat st;
    if (vfs_stat("/usb", &st) != VFS_OK || st.type != VFS_TYPE_DIR) {
        (void)bootlog_try_mount_usb_fat();
    }
    if (vfs_stat("/usb", &st) == VFS_OK && st.type == VFS_TYPE_DIR) {
        /* Primary name + BOOT.TXT alias (users often look for *.txt). */
        bootlog_write_one("/usb/BOOTLOG.TXT", BOOTLOG_RAM_BYTES, "/usb");
        bootlog_write_one("/usb/BOOT.TXT", BOOTLOG_RAM_BYTES, "/usb");
    } else {
        kprintf("[bootlog] %sno FAT32 USB at /usb — skipping BOOTLOG.TXT\n",
                phase);
    }
}

void bootlog_flush_all(void) {
    if (!g_inited) return;

    g_in_flush = true;

    struct vfs_stat st;
    if (vfs_stat("/data", &st) == VFS_OK && st.type == VFS_TYPE_DIR) {
        bootlog_write_one("/data/boot.log", BOOTLOG_TOBYFS_BODY, "/data");
    } else {
        kprintf("[bootlog] /data not mounted — skipping boot.log\n");
    }

    bootlog_flush_usb_fat_only("");

    g_in_flush = false;
    kprintf("[bootlog] flush complete (ram cap=%u overflow=%u)\n",
            (unsigned)BOOTLOG_RAM_BYTES, (unsigned)g_overflow);
}

void bootlog_flush_usb_retry(void) {
    if (!g_inited) return;
    g_in_flush = true;
    bootlog_flush_usb_fat_only("(deferred) ");
    g_in_flush = false;
    kprintf("[bootlog] deferred USB flush done (ram cap=%u overflow=%u)\n",
            (unsigned)BOOTLOG_RAM_BYTES, (unsigned)g_overflow);
}
