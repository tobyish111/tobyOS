/* toby/net.h -- Userland networking API.
 *
 * Provides TCP, TLS, and HTTP/1.1 client access from user programs.
 */

#ifndef TOBY_NET_H
#define TOBY_NET_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- TCP sockets ---- */

/* Connect to a remote host. Returns connection ID >= 0, or -1 on error. */
int toby_tcp_connect(uint32_t ip_be, uint16_t port_be, uint32_t timeout_ms);

/* Connect by hostname (does DNS resolution). */
int toby_tcp_connect_host(const char *host, uint16_t port, uint32_t timeout_ms);

/* Send data. Returns bytes sent or -1. */
int toby_tcp_send(int conn, const void *data, size_t len);

/* Receive data. Returns bytes read, 0 on EOF, or -1 on error. */
int toby_tcp_recv(int conn, void *buf, size_t len);

/* Close connection. */
int toby_tcp_close(int conn);

/* Listen on a port. Returns listen ID or -1. */
int toby_tcp_listen(uint16_t port_be, int backlog);

/* Accept a connection. Returns conn ID or -1. */
int toby_tcp_accept(int listen_id);

/* ---- TLS (secure) sockets ---- */

/* Connect with TLS. hostname is used for SNI. */
int toby_tls_connect(uint32_t ip_be, uint16_t port_be, const char *hostname);

/* Connect by hostname with TLS. */
int toby_tls_connect_host(const char *host, uint16_t port);

/* Send over TLS. */
int toby_tls_send(int conn, const void *data, size_t len);

/* Receive over TLS. */
int toby_tls_recv(int conn, void *buf, size_t len);

/* Close TLS connection. */
int toby_tls_close(int conn);

/* ---- HTTP/1.1 Client ---- */

#define HTTP_METHOD_GET    0
#define HTTP_METHOD_POST   1
#define HTTP_METHOD_PUT    2
#define HTTP_METHOD_DELETE 3
#define HTTP_METHOD_HEAD   4

#define HTTP_MAX_HEADERS   32
#define HTTP_MAX_COOKIES   16

struct http_header {
    char name[64];
    char value[256];
};

struct http_response {
    int status_code;
    int header_count;
    struct http_header headers[HTTP_MAX_HEADERS];
    char *body;
    size_t body_len;
    size_t body_cap;
    char content_type[128];
};

struct http_request {
    int method;
    char url[512];
    int header_count;
    struct http_header headers[HTTP_MAX_HEADERS];
    const char *body;
    size_t body_len;
    int follow_redirects;  /* max redirect count (0=none, 5=default) */
    size_t max_body;       /* max response body size (0=unlimited) */
};

/* Perform an HTTP request. Caller must call http_response_free() after. */
int http_request(const struct http_request *req, struct http_response *resp);

/* Convenience: GET a URL, return body. Caller frees resp->body. */
int http_get(const char *url, struct http_response *resp);

/* Convenience: POST data to a URL. */
int http_post(const char *url, const char *content_type,
              const void *body, size_t body_len,
              struct http_response *resp);

/* Free response body memory. */
void http_response_free(struct http_response *resp);

/* Find a response header value (case-insensitive). Returns NULL if not found. */
const char *http_get_header(const struct http_response *resp, const char *name);

/* ---- DNS resolution ---- */

/* Resolve hostname to IPv4 address (network byte order). Returns 0 on success. */
int toby_dns_resolve(const char *hostname, uint32_t *ip_out);

/* ---- URL parsing ---- */

struct parsed_url {
    char scheme[8];      /* "http" or "https" */
    char host[256];
    uint16_t port;
    char path[512];
    int is_https;
};

int url_parse(const char *url, struct parsed_url *out);

#ifdef __cplusplus
}
#endif

#endif /* TOBY_NET_H */
