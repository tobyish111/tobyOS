/* wireguard.c -- WireGuard VPN tunnel for tobyOS.
 *
 * Simplified WireGuard implementation using ChaCha20-Poly1305 from
 * the monocypher library (third_party/monocypher-4.0.2/).
 *
 * In the QEMU demo environment this operates as a stub that buffers
 * packets locally; real tunnel I/O requires a UDP socket to the peer
 * endpoint.
 */

#include <tobyos/wireguard.h>
#include <tobyos/klibc.h>
#include <tobyos/printk.h>
#include <tobyos/net.h>
#include "monocypher.h"

#define WG_BUF_SIZE  2048
#define WG_BUF_COUNT 4

static struct wg_interface g_iface;
static int                 g_active;

static uint8_t g_tx_buf[WG_BUF_COUNT][WG_BUF_SIZE];
static size_t  g_tx_len[WG_BUF_COUNT];
static int     g_tx_head, g_tx_tail;

static uint8_t g_rx_buf[WG_BUF_COUNT][WG_BUF_SIZE];
static size_t  g_rx_len[WG_BUF_COUNT];
static int     g_rx_head, g_rx_tail;

void wireguard_init(void)
{
    memset(&g_iface, 0, sizeof(g_iface));
    g_active  = 0;
    g_tx_head = g_tx_tail = 0;
    g_rx_head = g_rx_tail = 0;
    memset(g_tx_buf, 0, sizeof(g_tx_buf));
    memset(g_rx_buf, 0, sizeof(g_rx_buf));
    kprintf("[wireguard] initialised\n");
}

static void derive_public_key(const uint8_t priv[32], uint8_t pub[32])
{
    crypto_x25519_public_key(pub, priv);
}

int wg_create_interface(struct wg_interface *iface)
{
    if (!iface) return -1;
    if (g_active) {
        kprintf("[wireguard] interface already active\n");
        return -1;
    }

    memcpy(&g_iface, iface, sizeof(g_iface));
    derive_public_key(g_iface.private_key, g_iface.public_key);
    memcpy(iface->public_key, g_iface.public_key, 32);

    g_iface.active = 1;
    g_active = 1;

    g_tx_head = g_tx_tail = 0;
    g_rx_head = g_rx_tail = 0;

    kprintf("[wireguard] interface created, listen port %u, %d peers\n",
            g_iface.listen_port, g_iface.peer_count);
    return 0;
}

int wg_destroy_interface(void)
{
    if (!g_active) return -1;

    kprintf("[wireguard] interface destroyed\n");
    memset(&g_iface, 0, sizeof(g_iface));
    g_active = 0;
    g_tx_head = g_tx_tail = 0;
    g_rx_head = g_rx_tail = 0;
    return 0;
}

int wg_add_peer(const struct wg_peer *peer)
{
    if (!peer || !g_active) return -1;
    if (g_iface.peer_count >= 4) {
        kprintf("[wireguard] max peers reached\n");
        return -1;
    }

    memcpy(&g_iface.peers[g_iface.peer_count], peer, sizeof(*peer));
    g_iface.peer_count++;

    kprintf("[wireguard] peer added (endpoint %u.%u.%u.%u:%u)\n",
            peer->endpoint_ip & 0xFF,
            (peer->endpoint_ip >> 8) & 0xFF,
            (peer->endpoint_ip >> 16) & 0xFF,
            (peer->endpoint_ip >> 24) & 0xFF,
            peer->endpoint_port);
    return 0;
}

int wg_send(const void *packet, size_t len)
{
    if (!g_active || !packet || len == 0) return -1;
    if (len > WG_BUF_SIZE - 16 - 24) return -1; /* room for mac + nonce */

    int next = (g_tx_head + 1) % WG_BUF_COUNT;
    if (next == g_tx_tail) return -1; /* buffer full */

    /* Encrypt with ChaCha20-Poly1305.
     * In a real implementation we'd use the handshake-derived session
     * key; here we demonstrate the crypto path using the private key
     * as a stand-in symmetric key and a zero nonce. */
    uint8_t nonce[24];
    memset(nonce, 0, sizeof(nonce));

    uint8_t *dst = g_tx_buf[g_tx_head];
    uint8_t mac[16];
    crypto_aead_lock(dst + 24, mac, g_iface.private_key,
                     nonce, NULL, 0, packet, len);
    memcpy(dst, nonce, 24);
    memcpy(dst + 24 + len, mac, 16);
    g_tx_len[g_tx_head] = 24 + len + 16;
    g_tx_head = next;

    return (int)len;
}

int wg_recv(void *buf, size_t max)
{
    if (!g_active || !buf || max == 0) return -1;
    if (g_rx_head == g_rx_tail) return 0; /* nothing queued */

    size_t pkt_len = g_rx_len[g_rx_tail];
    if (pkt_len < 24 + 16) {
        g_rx_tail = (g_rx_tail + 1) % WG_BUF_COUNT;
        return -1;
    }

    uint8_t *src = g_rx_buf[g_rx_tail];
    uint8_t *nonce_p = src;
    size_t ct_len = pkt_len - 24 - 16;
    uint8_t *mac = src + 24 + ct_len;

    if (ct_len > max) {
        g_rx_tail = (g_rx_tail + 1) % WG_BUF_COUNT;
        return -1;
    }

    int r = crypto_aead_unlock(buf, mac, g_iface.private_key,
                               nonce_p, NULL, 0, src + 24, ct_len);
    g_rx_tail = (g_rx_tail + 1) % WG_BUF_COUNT;

    if (r != 0) {
        kprintf("[wireguard] decrypt failed\n");
        return -1;
    }
    return (int)ct_len;
}
