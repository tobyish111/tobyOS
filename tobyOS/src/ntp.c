/* ntp.c -- NTP client for tobyOS.
 *
 * Sends NTP requests to a configured server, parses responses,
 * calculates clock offset, and sets system time.
 * Periodic sync every hour.
 */

#include <tobyos/types.h>
#include <tobyos/net.h>
#include <tobyos/udp.h>
#include <tobyos/dns.h>
#include <tobyos/pit.h>
#include <tobyos/heap.h>
#include <tobyos/printk.h>
#include <tobyos/klibc.h>
#include <tobyos/proc.h>

#define NTP_PORT             123
#define NTP_PACKET_SIZE      48
#define NTP_SYNC_INTERVAL_MS (3600u * 1000u)  /* 1 hour */
#define NTP_TIMEOUT_MS       5000
#define NTP_SERVER           "pool.ntp.org"

/* NTP epoch: Jan 1, 1900. Unix epoch: Jan 1, 1970.
 * Difference in seconds = 70 years worth. */
#define NTP_UNIX_OFFSET      2208988800ULL

/* NTP LI/VN/Mode byte: LI=0, Version=4, Mode=3 (client) */
#define NTP_LI_VN_MODE       0x23

struct __attribute__((packed)) ntp_packet {
    uint8_t  li_vn_mode;
    uint8_t  stratum;
    uint8_t  poll;
    int8_t   precision;
    uint32_t root_delay;
    uint32_t root_dispersion;
    uint32_t reference_id;
    uint32_t ref_timestamp_sec;
    uint32_t ref_timestamp_frac;
    uint32_t orig_timestamp_sec;
    uint32_t orig_timestamp_frac;
    uint32_t rx_timestamp_sec;
    uint32_t rx_timestamp_frac;
    uint32_t tx_timestamp_sec;
    uint32_t tx_timestamp_frac;
};

/* System time state */
static int64_t  g_ntp_offset_ms;
static uint64_t g_ntp_last_sync_ms;
static bool     g_ntp_synced;
static uint32_t g_ntp_server_ip;
static uint8_t  g_ntp_stratum;

/* Response state for the async UDP callback path */
static volatile bool     g_ntp_response_ready;
static struct ntp_packet g_ntp_response;
static uint64_t          g_ntp_send_time_ms;

uint64_t ntp_get_unix_ms(void) {
    uint64_t boot_ms = pit_ticks() * 1000 / pit_hz();
    return (uint64_t)((int64_t)boot_ms + g_ntp_offset_ms);
}

bool ntp_is_synced(void) {
    return g_ntp_synced;
}

uint8_t ntp_stratum(void) {
    return g_ntp_stratum;
}

int64_t ntp_offset_ms(void) {
    return g_ntp_offset_ms;
}

/* Called from UDP receive path when port 123 data arrives */
void ntp_recv_hook(uint32_t src_ip_be, const void *data, size_t len) {
    (void)src_ip_be;
    if (len < NTP_PACKET_SIZE) return;
    memcpy((void *)&g_ntp_response, data, NTP_PACKET_SIZE);
    g_ntp_response_ready = true;
}

static bool ntp_resolve_server(void) {
    if (g_ntp_server_ip != 0) return true;

    struct dns_result res;
    if (!dns_resolve(NTP_SERVER, 5000, &res)) {
        kprintf("[ntp] failed to resolve %s\n", NTP_SERVER);
        return false;
    }
    g_ntp_server_ip = res.ip_be;
    uint32_t ip = res.ip_be;
    kprintf("[ntp] server resolved: %u.%u.%u.%u\n",
           ip & 0xFF, (ip >> 8) & 0xFF,
           (ip >> 16) & 0xFF, (ip >> 24) & 0xFF);
    return true;
}

bool ntp_sync(void) {
    if (!ntp_resolve_server()) return false;

    /* Build NTP request */
    struct ntp_packet req;
    memset(&req, 0, sizeof(req));
    req.li_vn_mode = NTP_LI_VN_MODE;

    /* Record send time */
    g_ntp_send_time_ms = pit_ticks() * 1000 / pit_hz();
    g_ntp_response_ready = false;

    /* Send via UDP */
    uint16_t src_port = htons(50123);
    uint16_t dst_port = htons(NTP_PORT);
    if (!udp_send(src_port, g_ntp_server_ip, dst_port, &req, NTP_PACKET_SIZE)) {
        kprintf("[ntp] send failed\n");
        return false;
    }

    /* Wait for response */
    uint64_t start = pit_ticks() * 1000 / pit_hz();
    while (!g_ntp_response_ready) {
        uint64_t now = pit_ticks() * 1000 / pit_hz();
        if (now - start > NTP_TIMEOUT_MS) {
            kprintf("[ntp] timeout\n");
            return false;
        }
        __asm__ volatile("hlt");
    }

    uint64_t recv_time_ms = pit_ticks() * 1000 / pit_hz();

    /* Parse response */
    struct ntp_packet *resp = &g_ntp_response;
    uint8_t mode = resp->li_vn_mode & 0x07;
    if (mode != 4 && mode != 5) {
        kprintf("[ntp] unexpected mode %u\n", mode);
        return false;
    }

    g_ntp_stratum = resp->stratum;
    if (resp->stratum == 0 || resp->stratum > 15) {
        kprintf("[ntp] invalid stratum %u\n", resp->stratum);
        return false;
    }

    /* Extract transmit timestamp (seconds since 1900-01-01) */
    uint32_t tx_sec = ntohl(resp->tx_timestamp_sec);
    uint32_t tx_frac = ntohl(resp->tx_timestamp_frac);

    /* Convert to Unix timestamp in ms */
    uint64_t server_unix_sec = (uint64_t)tx_sec - NTP_UNIX_OFFSET;
    uint64_t server_unix_ms = server_unix_sec * 1000 +
                              ((uint64_t)tx_frac * 1000 >> 32);

    /* Calculate round-trip delay */
    uint64_t rtt_ms = recv_time_ms - g_ntp_send_time_ms;

    /* Clock offset = server_time - local_time - rtt/2 */
    uint64_t local_time_at_server = g_ntp_send_time_ms + rtt_ms / 2;
    g_ntp_offset_ms = (int64_t)server_unix_ms - (int64_t)local_time_at_server;

    g_ntp_synced = true;
    g_ntp_last_sync_ms = recv_time_ms;

    kprintf("[ntp] synced: offset=%lldms rtt=%llums stratum=%u unix=%llu\n",
           (long long)g_ntp_offset_ms, (unsigned long long)rtt_ms,
           g_ntp_stratum, (unsigned long long)server_unix_ms / 1000);
    return true;
}

void ntp_periodic(void) {
    uint64_t now = pit_ticks() * 1000 / pit_hz();
    if (!g_ntp_synced || (now - g_ntp_last_sync_ms) >= NTP_SYNC_INTERVAL_MS) {
        ntp_sync();
    }
}

void ntp_init(void) {
    g_ntp_offset_ms = 0;
    g_ntp_last_sync_ms = 0;
    g_ntp_synced = false;
    g_ntp_server_ip = 0;
    g_ntp_stratum = 0;
    g_ntp_response_ready = false;
    kprintf("[ntp] initialized\n");
}
