/* ip_frag.c -- IP fragmentation and reassembly (RFC 791).
 *
 * Provides a standalone fragmentation / reassembly module with an
 * explicit public API.  ip.c already has inline reassembly + frag-send
 * baked into ip_recv / ip_send; this module is a structured alternative
 * that higher layers can call directly.
 *
 * Reassembly uses a small fixed pool (IP_FRAG_MAX_SLOTS slots, each up
 * to IP_FRAG_MAX_SIZE bytes).  A 32-bit bitmap tracks which 256-byte
 * chunks have been received; the datagram is complete when all chunks
 * up to total_len are accounted for.
 *
 * Stale incomplete fragments are reaped by ip_frag_timeout_check().
 */

#include <tobyos/ip_frag.h>
#include <tobyos/ip.h>
#include <tobyos/net.h>
#include <tobyos/pit.h>
#include <tobyos/printk.h>
#include <tobyos/klibc.h>

/* Fixed-pool reassembly buffers.  Kept static to avoid heap churn. */
static struct ip_fragment g_slots[IP_FRAG_MAX_SLOTS];
static uint8_t g_slot_buf[IP_FRAG_MAX_SLOTS][IP_FRAG_MAX_SIZE];
static uint16_t g_frag_id;

void ip_frag_init(void) {
    memset(g_slots, 0, sizeof(g_slots));
    memset(g_slot_buf, 0, sizeof(g_slot_buf));
    g_frag_id = 1;
}

static uint64_t now_ms(void) {
    uint32_t hz = pit_hz();
    if (hz == 0) hz = 100;
    return (pit_ticks() * 1000ull) / hz;
}

static struct ip_fragment *slot_find(uint16_t id, uint32_t src) {
    for (int i = 0; i < IP_FRAG_MAX_SLOTS; i++) {
        struct ip_fragment *f = &g_slots[i];
        if (f->data && f->id == id && f->src_ip == src && !f->complete)
            return f;
    }
    return NULL;
}

static struct ip_fragment *slot_alloc(void) {
    for (int i = 0; i < IP_FRAG_MAX_SLOTS; i++) {
        if (!g_slots[i].data) {
            memset(&g_slots[i], 0, sizeof(g_slots[i]));
            g_slots[i].data = g_slot_buf[i];
            g_slots[i].timestamp = now_ms();
            return &g_slots[i];
        }
    }
    /* Evict the oldest slot. */
    int oldest = 0;
    uint64_t oldest_ts = g_slots[0].timestamp;
    for (int i = 1; i < IP_FRAG_MAX_SLOTS; i++) {
        if (g_slots[i].timestamp < oldest_ts) {
            oldest = i;
            oldest_ts = g_slots[i].timestamp;
        }
    }
    struct ip_fragment *f = &g_slots[oldest];
    memset(f, 0, sizeof(*f));
    f->data = g_slot_buf[oldest];
    f->timestamp = now_ms();
    return f;
}

static void slot_free(struct ip_fragment *f) {
    f->data = NULL;
    f->complete = 0;
    f->received_mask = 0;
    f->total_len = 0;
    f->filled = 0;
}

/* Mark chunk [off, off+len) as received. */
static void bm_mark(struct ip_fragment *f, size_t off, size_t len) {
    size_t start_chunk = off / 256u;
    size_t end_chunk = (off + len + 255u) / 256u;
    for (size_t c = start_chunk; c < end_chunk && c < 32; c++)
        f->received_mask |= (1u << c);
}

static bool bm_complete(const struct ip_fragment *f) {
    if (f->total_len == 0) return false;
    size_t chunks = (f->total_len + 255u) / 256u;
    if (chunks > 32) return false;
    uint32_t mask = (chunks == 32) ? 0xFFFFFFFFu : ((1u << chunks) - 1u);
    return (f->received_mask & mask) == mask;
}

int ip_reassemble(const void *ip_packet, size_t len,
                  void **complete_out, size_t *complete_len) {
    if (!ip_packet || len < IP_HDR_LEN) return -1;

    const struct ip_hdr *h = (const struct ip_hdr *)ip_packet;
    uint16_t ff = ntohs(h->flags_frag);
    unsigned frag_off = ((unsigned)ff & 0x1FFFu) << 3;
    bool more_frag = (ff & 0x2000u) != 0;

    if (frag_off == 0 && !more_frag) {
        /* Unfragmented packet: return the L4 payload directly. */
        uint16_t total_ip = ntohs(h->total_len);
        if (total_ip < IP_HDR_LEN || total_ip > len) return -1;
        if (complete_out) *complete_out = (void *)((const uint8_t *)ip_packet + IP_HDR_LEN);
        if (complete_len) *complete_len = total_ip - IP_HDR_LEN;
        return 1;
    }

    uint16_t total_ip = ntohs(h->total_len);
    if (total_ip < IP_HDR_LEN) return -1;
    size_t l4len = (size_t)(total_ip - IP_HDR_LEN);
    const uint8_t *l4 = (const uint8_t *)ip_packet + IP_HDR_LEN;

    struct ip_fragment *f = slot_find(ntohs(h->id), h->src_ip);
    if (!f) {
        if (frag_off != 0) return 0;
        f = slot_alloc();
        f->id       = ntohs(h->id);
        f->src_ip   = h->src_ip;
        f->dst_ip   = h->dst_ip;
        f->protocol = h->proto;
    }

    if (frag_off + l4len > IP_FRAG_MAX_SIZE) {
        slot_free(f);
        return -1;
    }

    memcpy(f->data + frag_off, l4, l4len);
    bm_mark(f, frag_off, l4len);
    f->filled += l4len;

    if (!more_frag) {
        f->total_len = frag_off + l4len;
    }

    if (bm_complete(f)) {
        f->complete = 1;
        if (complete_out) *complete_out = f->data;
        if (complete_len) *complete_len = f->total_len;
        return 1;
    }

    return 0;
}

int ip_fragment_send(const void *payload, size_t len,
                     uint32_t dst_ip, uint8_t protocol, uint16_t mtu) {
    if (!payload || len == 0) return -1;

    size_t max_l4 = (size_t)mtu - IP_HDR_LEN;

    if (len <= max_l4) {
        return ip_send(dst_ip, protocol, payload, len) ? 0 : -1;
    }

    /* Round down to 8-byte boundary for fragment payloads. */
    size_t frag_payload = max_l4 & ~7u;
    if (frag_payload == 0) return -1;

    (void)g_frag_id++;

    const uint8_t *p = (const uint8_t *)payload;
    size_t remain = len;

    while (remain > 0) {
        size_t chunk = remain;
        if (chunk > frag_payload) chunk = frag_payload;
        if (remain > chunk) chunk &= ~7u;
        if (chunk == 0) return -1;

        if (!ip_send(dst_ip, protocol, p, chunk))
            return -1;

        p      += chunk;
        remain -= chunk;
    }
    return 0;
}

void ip_frag_timeout_check(void) {
    uint64_t now = now_ms();
    for (int i = 0; i < IP_FRAG_MAX_SLOTS; i++) {
        struct ip_fragment *f = &g_slots[i];
        if (!f->data) continue;
        if (f->complete) {
            slot_free(f);
            continue;
        }
        if (now - f->timestamp > IP_FRAG_TIMEOUT_MS) {
            kprintf("[ip-frag] timeout: id=%u src=0x%08x filled=%lu/%lu\n",
                    (unsigned)f->id, f->src_ip,
                    (unsigned long)f->filled,
                    (unsigned long)f->total_len);
            slot_free(f);
        }
    }
}
