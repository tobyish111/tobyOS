/* http2.c -- HTTP/2 client implementation for tobyOS.
 *
 * Implements HPACK header compression (static table), frame parsing,
 * stream multiplexing, and a simple API for HTTP/2 requests.
 */

#include <tobyos/types.h>
#include <tobyos/tcp.h>
#include <tobyos/net.h>
#include <tobyos/heap.h>
#include <tobyos/printk.h>
#include <tobyos/klibc.h>

#define HTTP2_MAX_STREAMS       100
#define HTTP2_MAX_FRAME_SIZE    16384
#define HTTP2_WINDOW_SIZE       65535
#define HTTP2_HEADER_TABLE_SIZE 4096
#define HTTP2_MAX_HEADER_SIZE   8192

/* HTTP/2 frame types */
#define HTTP2_FRAME_DATA          0x0
#define HTTP2_FRAME_HEADERS       0x1
#define HTTP2_FRAME_PRIORITY      0x2
#define HTTP2_FRAME_RST_STREAM    0x3
#define HTTP2_FRAME_SETTINGS      0x4
#define HTTP2_FRAME_PUSH_PROMISE  0x5
#define HTTP2_FRAME_PING          0x6
#define HTTP2_FRAME_GOAWAY        0x7
#define HTTP2_FRAME_WINDOW_UPDATE 0x8
#define HTTP2_FRAME_CONTINUATION  0x9

/* Frame flags */
#define HTTP2_FLAG_END_STREAM     0x01
#define HTTP2_FLAG_END_HEADERS    0x04
#define HTTP2_FLAG_PADDED         0x08
#define HTTP2_FLAG_PRIORITY       0x20
#define HTTP2_FLAG_ACK            0x01

/* Settings identifiers */
#define HTTP2_SETTINGS_HEADER_TABLE_SIZE      0x1
#define HTTP2_SETTINGS_ENABLE_PUSH            0x2
#define HTTP2_SETTINGS_MAX_CONCURRENT_STREAMS 0x3
#define HTTP2_SETTINGS_INITIAL_WINDOW_SIZE    0x4
#define HTTP2_SETTINGS_MAX_FRAME_SIZE         0x5
#define HTTP2_SETTINGS_MAX_HEADER_LIST_SIZE   0x6

/* Error codes */
#define HTTP2_NO_ERROR            0x0
#define HTTP2_PROTOCOL_ERROR      0x1
#define HTTP2_INTERNAL_ERROR      0x2
#define HTTP2_FLOW_CONTROL_ERROR  0x3
#define HTTP2_SETTINGS_TIMEOUT    0x4
#define HTTP2_STREAM_CLOSED       0x5
#define HTTP2_FRAME_SIZE_ERROR    0x6
#define HTTP2_REFUSED_STREAM      0x7
#define HTTP2_CANCEL              0x8
#define HTTP2_COMPRESSION_ERROR   0x9

/* Connection preface */
static const char HTTP2_PREFACE[] = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";
#define HTTP2_PREFACE_LEN 24

/* Stream states */
enum http2_stream_state {
    STREAM_IDLE,
    STREAM_OPEN,
    STREAM_HALF_CLOSED_LOCAL,
    STREAM_HALF_CLOSED_REMOTE,
    STREAM_CLOSED
};

struct http2_stream {
    uint32_t                 id;
    enum http2_stream_state  state;
    int32_t                  window_size;
    uint8_t                 *recv_buf;
    size_t                   recv_len;
    size_t                   recv_cap;
    uint16_t                 status_code;
    bool                     headers_done;
    bool                     data_done;
};

struct http2_conn {
    struct tcp_conn         *tcp;
    uint32_t                 next_stream_id;
    int32_t                  conn_window_size;
    uint32_t                 max_frame_size;
    uint32_t                 max_concurrent;
    uint32_t                 peer_window_size;
    struct http2_stream      streams[HTTP2_MAX_STREAMS];
    int                      stream_count;
    bool                     connected;
    bool                     goaway_received;
    uint32_t                 last_stream_id;
};

struct __attribute__((packed)) http2_frame_header {
    uint8_t  length[3];
    uint8_t  type;
    uint8_t  flags;
    uint32_t stream_id;
};

/* HPACK static table (RFC 7541 Appendix A) - most common entries */
static const struct {
    const char *name;
    const char *value;
} hpack_static_table[] = {
    { NULL,             NULL },
    { ":authority",     "" },
    { ":method",        "GET" },
    { ":method",        "POST" },
    { ":path",          "/" },
    { ":path",          "/index.html" },
    { ":scheme",        "http" },
    { ":scheme",        "https" },
    { ":status",        "200" },
    { ":status",        "204" },
    { ":status",        "206" },
    { ":status",        "304" },
    { ":status",        "400" },
    { ":status",        "404" },
    { ":status",        "500" },
    { "accept-charset", "" },
    { "accept-encoding", "gzip, deflate" },
    { "accept-language", "" },
    { "accept-ranges",  "" },
    { "accept",         "" },
    { "access-control-allow-origin", "" },
    { "age",            "" },
    { "allow",          "" },
    { "authorization",  "" },
    { "cache-control",  "" },
    { "content-disposition", "" },
    { "content-encoding", "" },
    { "content-language", "" },
    { "content-length", "" },
    { "content-location", "" },
    { "content-range",  "" },
    { "content-type",   "" },
    { "cookie",         "" },
    { "date",           "" },
    { "etag",           "" },
    { "expect",         "" },
    { "expires",        "" },
    { "from",           "" },
    { "host",           "" },
    { "if-match",       "" },
    { "if-modified-since", "" },
    { "if-none-match",  "" },
    { "if-range",       "" },
    { "if-unmodified-since", "" },
    { "last-modified",  "" },
    { "link",           "" },
    { "location",       "" },
    { "max-forwards",   "" },
    { "proxy-authenticate", "" },
    { "proxy-authorization", "" },
    { "range",          "" },
    { "referer",        "" },
    { "refresh",        "" },
    { "retry-after",    "" },
    { "server",         "" },
    { "set-cookie",     "" },
    { "strict-transport-security", "" },
    { "transfer-encoding", "" },
    { "user-agent",     "" },
    { "vary",           "" },
    { "via",            "" },
    { "www-authenticate", "" },
};
#define HPACK_STATIC_TABLE_SIZE 61

/* HPACK integer encoding */
static size_t hpack_encode_int(uint8_t *buf, uint32_t value, uint8_t prefix_bits,
                               uint8_t pattern) {
    uint8_t max_prefix = (uint8_t)((1u << prefix_bits) - 1);
    if (value < max_prefix) {
        buf[0] = pattern | (uint8_t)value;
        return 1;
    }
    buf[0] = pattern | max_prefix;
    value -= max_prefix;
    size_t i = 1;
    while (value >= 128) {
        buf[i++] = (uint8_t)(value & 0x7F) | 0x80;
        value >>= 7;
    }
    buf[i++] = (uint8_t)value;
    return i;
}

/* HPACK integer decoding */
static size_t hpack_decode_int(const uint8_t *buf, size_t len, uint8_t prefix_bits,
                               uint32_t *out) {
    if (len == 0) return 0;
    uint8_t max_prefix = (uint8_t)((1u << prefix_bits) - 1);
    *out = buf[0] & max_prefix;
    if (*out < max_prefix) return 1;

    size_t i = 1;
    uint32_t m = 0;
    while (i < len) {
        *out += (uint32_t)(buf[i] & 0x7F) << m;
        if (!(buf[i] & 0x80)) return i + 1;
        m += 7;
        i++;
    }
    return 0;
}

/* Encode a header using HPACK (indexed or literal) */
static size_t hpack_encode_header(uint8_t *buf, size_t cap,
                                  const char *name, const char *value) {
    /* Check static table for indexed representation */
    for (int i = 1; i <= HPACK_STATIC_TABLE_SIZE; i++) {
        if (strcmp(hpack_static_table[i].name, name) == 0 &&
            strcmp(hpack_static_table[i].value, value) == 0) {
            return hpack_encode_int(buf, (uint32_t)i, 7, 0x80);
        }
    }

    /* Check for name match only */
    int name_idx = 0;
    for (int i = 1; i <= HPACK_STATIC_TABLE_SIZE; i++) {
        if (strcmp(hpack_static_table[i].name, name) == 0) {
            name_idx = i;
            break;
        }
    }

    size_t off = 0;
    if (name_idx > 0) {
        /* Literal with name index, never indexed */
        off += hpack_encode_int(buf + off, (uint32_t)name_idx, 4, 0x00);
    } else {
        /* Literal with new name */
        buf[off++] = 0x00;
        size_t nlen = strlen(name);
        if (off + 1 + nlen > cap) return 0;
        off += hpack_encode_int(buf + off, (uint32_t)nlen, 7, 0x00);
        memcpy(buf + off, name, nlen);
        off += nlen;
    }

    size_t vlen = strlen(value);
    if (off + 1 + vlen > cap) return 0;
    off += hpack_encode_int(buf + off, (uint32_t)vlen, 7, 0x00);
    memcpy(buf + off, value, vlen);
    off += vlen;
    return off;
}

/* Frame helpers */
static void frame_set_length(struct http2_frame_header *fh, uint32_t len) {
    fh->length[0] = (uint8_t)((len >> 16) & 0xFF);
    fh->length[1] = (uint8_t)((len >> 8) & 0xFF);
    fh->length[2] = (uint8_t)(len & 0xFF);
}

static uint32_t frame_get_length(const struct http2_frame_header *fh) {
    return ((uint32_t)fh->length[0] << 16) |
           ((uint32_t)fh->length[1] << 8) |
           (uint32_t)fh->length[2];
}

static bool http2_send_frame(struct http2_conn *conn, uint8_t type, uint8_t flags,
                             uint32_t stream_id, const void *payload, size_t len) {
    struct http2_frame_header fh;
    memset(&fh, 0, sizeof(fh));
    frame_set_length(&fh, (uint32_t)len);
    fh.type = type;
    fh.flags = flags;
    fh.stream_id = htonl(stream_id & 0x7FFFFFFF);

    if (tcp_send(conn->tcp, &fh, 9) < 0) return false;
    if (len > 0 && tcp_send(conn->tcp, payload, len) < 0) return false;
    return true;
}

static bool http2_send_settings(struct http2_conn *conn) {
    uint8_t payload[18];
    /* SETTINGS_MAX_CONCURRENT_STREAMS = 100 */
    payload[0] = 0; payload[1] = HTTP2_SETTINGS_MAX_CONCURRENT_STREAMS;
    payload[2] = 0; payload[3] = 0; payload[4] = 0; payload[5] = 100;
    /* SETTINGS_INITIAL_WINDOW_SIZE = 65535 */
    payload[6] = 0; payload[7] = HTTP2_SETTINGS_INITIAL_WINDOW_SIZE;
    payload[8] = 0; payload[9] = 0; payload[10] = 0xFF; payload[11] = 0xFF;
    /* SETTINGS_MAX_FRAME_SIZE = 16384 */
    payload[12] = 0; payload[13] = HTTP2_SETTINGS_MAX_FRAME_SIZE;
    payload[14] = 0; payload[15] = 0; payload[16] = 0x40; payload[17] = 0x00;

    return http2_send_frame(conn, HTTP2_FRAME_SETTINGS, 0, 0, payload, 18);
}

static bool http2_send_settings_ack(struct http2_conn *conn) {
    return http2_send_frame(conn, HTTP2_FRAME_SETTINGS, HTTP2_FLAG_ACK, 0, NULL, 0);
}

static bool http2_send_window_update(struct http2_conn *conn, uint32_t stream_id,
                                     uint32_t increment) {
    uint32_t payload = htonl(increment & 0x7FFFFFFF);
    return http2_send_frame(conn, HTTP2_FRAME_WINDOW_UPDATE, 0, stream_id,
                            &payload, 4);
}

static struct http2_stream *http2_find_stream(struct http2_conn *conn, uint32_t id) {
    for (int i = 0; i < conn->stream_count; i++) {
        if (conn->streams[i].id == id)
            return &conn->streams[i];
    }
    return NULL;
}

static struct http2_stream *http2_alloc_stream(struct http2_conn *conn) {
    if (conn->stream_count >= HTTP2_MAX_STREAMS) return NULL;
    struct http2_stream *s = &conn->streams[conn->stream_count++];
    memset(s, 0, sizeof(*s));
    s->id = conn->next_stream_id;
    conn->next_stream_id += 2;
    s->state = STREAM_OPEN;
    s->window_size = (int32_t)HTTP2_WINDOW_SIZE;
    s->recv_cap = HTTP2_MAX_FRAME_SIZE * 4;
    s->recv_buf = kmalloc(s->recv_cap);
    s->recv_len = 0;
    return s;
}

static bool http2_recv_frame(struct http2_conn *conn, struct http2_frame_header *fh,
                             uint8_t *payload, size_t payload_cap, size_t *payload_len) {
    uint8_t hdr_buf[9];
    long r = tcp_recv(conn->tcp, hdr_buf, 9, 10000);
    if (r < 9) return false;

    fh->length[0] = hdr_buf[0];
    fh->length[1] = hdr_buf[1];
    fh->length[2] = hdr_buf[2];
    fh->type = hdr_buf[3];
    fh->flags = hdr_buf[4];
    fh->stream_id = ntohl(*(uint32_t *)(hdr_buf + 5)) & 0x7FFFFFFF;

    uint32_t flen = frame_get_length(fh);
    *payload_len = 0;
    if (flen > 0) {
        if (flen > payload_cap) return false;
        size_t total = 0;
        while (total < flen) {
            r = tcp_recv(conn->tcp, payload + total, flen - total, 10000);
            if (r <= 0) return false;
            total += (size_t)r;
        }
        *payload_len = flen;
    }
    return true;
}

static void http2_process_data(struct http2_conn *conn, uint32_t stream_id,
                               const uint8_t *data, size_t len, uint8_t flags) {
    struct http2_stream *s = http2_find_stream(conn, stream_id);
    if (!s) return;

    if (s->recv_buf && s->recv_len + len <= s->recv_cap) {
        memcpy(s->recv_buf + s->recv_len, data, len);
        s->recv_len += len;
    }

    if (flags & HTTP2_FLAG_END_STREAM) {
        s->data_done = true;
        s->state = STREAM_HALF_CLOSED_REMOTE;
    }

    /* Send window update */
    if (len > 0) {
        http2_send_window_update(conn, stream_id, (uint32_t)len);
        http2_send_window_update(conn, 0, (uint32_t)len);
    }
}

static void http2_process_headers(struct http2_conn *conn, uint32_t stream_id,
                                  const uint8_t *data, size_t len, uint8_t flags) {
    struct http2_stream *s = http2_find_stream(conn, stream_id);
    if (!s) return;

    /* Parse HPACK-encoded headers (simplified: extract :status) */
    size_t off = 0;
    while (off < len) {
        uint8_t byte = data[off];
        if (byte & 0x80) {
            /* Indexed header field */
            uint32_t idx = 0;
            size_t consumed = hpack_decode_int(data + off, len - off, 7, &idx);
            if (consumed == 0) break;
            off += consumed;
            if (idx >= 8 && idx <= 14) {
                /* :status entries in static table */
                const char *val = hpack_static_table[idx].value;
                uint16_t code = 0;
                while (*val >= '0' && *val <= '9')
                    code = code * 10 + (uint16_t)(*val++ - '0');
                s->status_code = code;
            }
        } else if (byte & 0x40) {
            /* Literal with incremental indexing */
            uint32_t name_idx = 0;
            size_t consumed = hpack_decode_int(data + off, len - off, 6, &name_idx);
            if (consumed == 0) break;
            off += consumed;
            if (name_idx == 0) {
                uint32_t nlen = 0;
                consumed = hpack_decode_int(data + off, len - off, 7, &nlen);
                if (consumed == 0) break;
                off += consumed + nlen;
            }
            uint32_t vlen = 0;
            consumed = hpack_decode_int(data + off, len - off, 7, &vlen);
            if (consumed == 0) break;
            off += consumed + vlen;
        } else {
            /* Literal without indexing or never indexed */
            uint32_t name_idx = 0;
            uint8_t prefix = (byte & 0x10) ? 4 : 4;
            size_t consumed = hpack_decode_int(data + off, len - off, prefix, &name_idx);
            if (consumed == 0) break;
            off += consumed;
            if (name_idx == 0) {
                uint32_t nlen = 0;
                consumed = hpack_decode_int(data + off, len - off, 7, &nlen);
                if (consumed == 0) break;
                off += consumed + nlen;
            }
            uint32_t vlen = 0;
            consumed = hpack_decode_int(data + off, len - off, 7, &vlen);
            if (consumed == 0) break;
            off += consumed + vlen;
        }
    }

    s->headers_done = true;
    if (flags & HTTP2_FLAG_END_STREAM) {
        s->data_done = true;
        s->state = STREAM_HALF_CLOSED_REMOTE;
    }
}

static void http2_process_frame(struct http2_conn *conn, struct http2_frame_header *fh,
                                uint8_t *payload, size_t len) {
    switch (fh->type) {
    case HTTP2_FRAME_DATA:
        http2_process_data(conn, fh->stream_id, payload, len, fh->flags);
        break;
    case HTTP2_FRAME_HEADERS:
        http2_process_headers(conn, fh->stream_id, payload, len, fh->flags);
        break;
    case HTTP2_FRAME_SETTINGS:
        if (!(fh->flags & HTTP2_FLAG_ACK))
            http2_send_settings_ack(conn);
        break;
    case HTTP2_FRAME_PING:
        if (!(fh->flags & HTTP2_FLAG_ACK))
            http2_send_frame(conn, HTTP2_FRAME_PING, HTTP2_FLAG_ACK, 0, payload, len);
        break;
    case HTTP2_FRAME_GOAWAY:
        conn->goaway_received = true;
        if (len >= 8)
            conn->last_stream_id = ntohl(*(uint32_t *)payload) & 0x7FFFFFFF;
        break;
    case HTTP2_FRAME_WINDOW_UPDATE:
        if (len >= 4) {
            uint32_t inc = ntohl(*(uint32_t *)payload) & 0x7FFFFFFF;
            if (fh->stream_id == 0) {
                conn->conn_window_size += (int32_t)inc;
            } else {
                struct http2_stream *s = http2_find_stream(conn, fh->stream_id);
                if (s) s->window_size += (int32_t)inc;
            }
        }
        break;
    case HTTP2_FRAME_RST_STREAM: {
        struct http2_stream *s = http2_find_stream(conn, fh->stream_id);
        if (s) s->state = STREAM_CLOSED;
        break;
    }
    default:
        break;
    }
}

/* ---- Public API ---- */

struct http2_conn *http2_connect(uint32_t dst_ip_be, uint16_t port_be) {
    struct http2_conn *conn = kmalloc(sizeof(*conn));
    if (!conn) return NULL;
    memset(conn, 0, sizeof(*conn));

    conn->tcp = tcp_connect(dst_ip_be, port_be, 10000);
    if (!conn->tcp) {
        kfree(conn);
        return NULL;
    }

    conn->next_stream_id = 1;
    conn->conn_window_size = (int32_t)HTTP2_WINDOW_SIZE;
    conn->max_frame_size = HTTP2_MAX_FRAME_SIZE;
    conn->max_concurrent = HTTP2_MAX_STREAMS;
    conn->peer_window_size = HTTP2_WINDOW_SIZE;

    /* Send connection preface */
    if (tcp_send(conn->tcp, HTTP2_PREFACE, HTTP2_PREFACE_LEN) < 0) {
        tcp_close(conn->tcp);
        kfree(conn);
        return NULL;
    }

    /* Send our SETTINGS */
    if (!http2_send_settings(conn)) {
        tcp_close(conn->tcp);
        kfree(conn);
        return NULL;
    }

    conn->connected = true;
    kprintf("[http2] connected\n");

    /* Read server SETTINGS */
    struct http2_frame_header fh;
    uint8_t payload[HTTP2_MAX_FRAME_SIZE];
    size_t plen;
    if (http2_recv_frame(conn, &fh, payload, sizeof(payload), &plen))
        http2_process_frame(conn, &fh, payload, plen);

    return conn;
}

uint32_t http2_request(struct http2_conn *conn, const char *method,
                       const char *path, const char *authority,
                       const void *body, size_t body_len) {
    if (!conn || !conn->connected) return 0;

    struct http2_stream *s = http2_alloc_stream(conn);
    if (!s) return 0;

    /* Encode headers */
    uint8_t hdrs[HTTP2_MAX_HEADER_SIZE];
    size_t hlen = 0;

    hlen += hpack_encode_header(hdrs + hlen, sizeof(hdrs) - hlen,
                                ":method", method);
    hlen += hpack_encode_header(hdrs + hlen, sizeof(hdrs) - hlen,
                                ":path", path);
    hlen += hpack_encode_header(hdrs + hlen, sizeof(hdrs) - hlen,
                                ":scheme", "http");
    hlen += hpack_encode_header(hdrs + hlen, sizeof(hdrs) - hlen,
                                ":authority", authority);

    uint8_t flags = HTTP2_FLAG_END_HEADERS;
    if (!body || body_len == 0) flags |= HTTP2_FLAG_END_STREAM;

    if (!http2_send_frame(conn, HTTP2_FRAME_HEADERS, flags, s->id, hdrs, hlen))
        return 0;

    /* Send body as DATA frame if present */
    if (body && body_len > 0) {
        if (!http2_send_frame(conn, HTTP2_FRAME_DATA, HTTP2_FLAG_END_STREAM,
                              s->id, body, body_len))
            return 0;
        s->state = STREAM_HALF_CLOSED_LOCAL;
    } else {
        s->state = STREAM_HALF_CLOSED_LOCAL;
    }

    return s->id;
}

int http2_read_response(struct http2_conn *conn, uint32_t stream_id,
                        void *buf, size_t cap, uint16_t *status_out) {
    if (!conn || !conn->connected) return -1;

    struct http2_stream *s = http2_find_stream(conn, stream_id);
    if (!s) return -1;

    /* Pump frames until we get our response */
    uint8_t frame_buf[HTTP2_MAX_FRAME_SIZE];
    int attempts = 0;
    while (!s->data_done && attempts < 100) {
        struct http2_frame_header fh;
        size_t plen;
        if (!http2_recv_frame(conn, &fh, frame_buf, sizeof(frame_buf), &plen))
            break;
        http2_process_frame(conn, &fh, frame_buf, plen);
        attempts++;
    }

    if (status_out) *status_out = s->status_code;

    size_t copy = s->recv_len < cap ? s->recv_len : cap;
    if (copy > 0 && s->recv_buf)
        memcpy(buf, s->recv_buf, copy);
    return (int)copy;
}

void http2_close(struct http2_conn *conn) {
    if (!conn) return;

    /* Send GOAWAY */
    uint8_t goaway[8];
    memset(goaway, 0, sizeof(goaway));
    *(uint32_t *)goaway = htonl(conn->last_stream_id);
    *(uint32_t *)(goaway + 4) = htonl(HTTP2_NO_ERROR);
    http2_send_frame(conn, HTTP2_FRAME_GOAWAY, 0, 0, goaway, 8);

    /* Free stream buffers */
    for (int i = 0; i < conn->stream_count; i++) {
        if (conn->streams[i].recv_buf)
            kfree(conn->streams[i].recv_buf);
    }

    tcp_close(conn->tcp);
    kfree(conn);
}
