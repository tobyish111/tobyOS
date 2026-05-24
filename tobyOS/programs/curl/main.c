/* curl/main.c -- HTTP client for tobyOS.
 *
 * Uses toby/net.h for HTTP requests. Supports:
 *   curl URL             -- GET, print body to stdout
 *   -o FILE              -- save response to file
 *   -I                   -- headers only (HEAD request)
 *   -X METHOD            -- set HTTP method
 *   -d DATA              -- POST body (also sets method to POST)
 *   -H "Header: value"   -- custom header
 *   -L                   -- follow redirects (default 5)
 *   -v                   -- verbose (show request/response headers)
 *   -s                   -- silent (no progress bar)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <toby/net.h>

#define MAX_HEADERS 16

static int g_verbose;
static int g_silent;
static int g_head_only;
static int g_follow_redirects;
static const char *g_output_file;
static const char *g_data;
static int g_method = -1;
static struct http_header g_custom_headers[MAX_HEADERS];
static int g_nheaders;

static int method_from_str(const char *s) {
    if (strcmp(s, "GET") == 0)    return HTTP_METHOD_GET;
    if (strcmp(s, "POST") == 0)   return HTTP_METHOD_POST;
    if (strcmp(s, "PUT") == 0)    return HTTP_METHOD_PUT;
    if (strcmp(s, "DELETE") == 0) return HTTP_METHOD_DELETE;
    if (strcmp(s, "HEAD") == 0)   return HTTP_METHOD_HEAD;
    return HTTP_METHOD_GET;
}

static const char *method_str(int m) {
    switch (m) {
    case HTTP_METHOD_GET:    return "GET";
    case HTTP_METHOD_POST:   return "POST";
    case HTTP_METHOD_PUT:    return "PUT";
    case HTTP_METHOD_DELETE: return "DELETE";
    case HTTP_METHOD_HEAD:   return "HEAD";
    default:                 return "GET";
    }
}

static void print_progress(size_t current, size_t total) {
    if (g_silent || g_verbose) return;
    if (total > 0) {
        int pct = (int)(current * 100 / total);
        fprintf(stderr, "\r  %% Total    Received\r  %3d    %8zu", pct, current);
    } else {
        fprintf(stderr, "\r  Received: %zu bytes", current);
    }
}

static void usage(void) {
    fprintf(stderr, "Usage: curl [options] URL\n");
    fprintf(stderr, "  -o FILE    Save output to FILE\n");
    fprintf(stderr, "  -I         Headers only (HEAD)\n");
    fprintf(stderr, "  -X METHOD  HTTP method\n");
    fprintf(stderr, "  -d DATA    POST data\n");
    fprintf(stderr, "  -H HEADER  Custom header\n");
    fprintf(stderr, "  -L         Follow redirects\n");
    fprintf(stderr, "  -v         Verbose output\n");
    fprintf(stderr, "  -s         Silent mode\n");
}

static int parse_header_arg(const char *s) {
    if (g_nheaders >= MAX_HEADERS) return -1;
    const char *colon = strchr(s, ':');
    if (!colon) return -1;
    size_t nlen = (size_t)(colon - s);
    if (nlen > 63) nlen = 63;
    memcpy(g_custom_headers[g_nheaders].name, s, nlen);
    g_custom_headers[g_nheaders].name[nlen] = '\0';
    const char *val = colon + 1;
    while (*val == ' ') val++;
    strncpy(g_custom_headers[g_nheaders].value, val, 255);
    g_custom_headers[g_nheaders].value[255] = '\0';
    g_nheaders++;
    return 0;
}

int main(int argc, char **argv) {
    const char *url = NULL;
    g_follow_redirects = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
            g_output_file = argv[++i];
        } else if (strcmp(argv[i], "-I") == 0) {
            g_head_only = 1;
        } else if (strcmp(argv[i], "-X") == 0 && i + 1 < argc) {
            g_method = method_from_str(argv[++i]);
        } else if (strcmp(argv[i], "-d") == 0 && i + 1 < argc) {
            g_data = argv[++i];
            if (g_method < 0) g_method = HTTP_METHOD_POST;
        } else if (strcmp(argv[i], "-H") == 0 && i + 1 < argc) {
            parse_header_arg(argv[++i]);
        } else if (strcmp(argv[i], "-L") == 0) {
            g_follow_redirects = 5;
        } else if (strcmp(argv[i], "-v") == 0) {
            g_verbose = 1;
        } else if (strcmp(argv[i], "-s") == 0) {
            g_silent = 1;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            usage();
            return 0;
        } else if (argv[i][0] != '-') {
            url = argv[i];
        }
    }

    if (!url) {
        fprintf(stderr, "curl: no URL specified\n");
        usage();
        return 1;
    }

    if (g_method < 0) g_method = g_head_only ? HTTP_METHOD_HEAD : HTTP_METHOD_GET;

    /* Build request */
    struct http_request req;
    memset(&req, 0, sizeof(req));
    req.method = g_method;
    strncpy(req.url, url, 511);
    req.follow_redirects = g_follow_redirects;

    /* Copy custom headers */
    for (int i = 0; i < g_nheaders && i < HTTP_MAX_HEADERS; i++)
        req.headers[req.header_count++] = g_custom_headers[i];

    if (g_data) {
        req.body = g_data;
        req.body_len = strlen(g_data);
        /* Add Content-Type if not already set */
        int has_ct = 0;
        for (int i = 0; i < req.header_count; i++)
            if (strcmp(req.headers[i].name, "Content-Type") == 0) { has_ct = 1; break; }
        if (!has_ct && req.header_count < HTTP_MAX_HEADERS) {
            strncpy(req.headers[req.header_count].name, "Content-Type", 63);
            strncpy(req.headers[req.header_count].value,
                    "application/x-www-form-urlencoded", 255);
            req.header_count++;
        }
    }

    if (g_verbose) {
        fprintf(stderr, "> %s %s\n", method_str(req.method), url);
        for (int i = 0; i < req.header_count; i++)
            fprintf(stderr, "> %s: %s\n", req.headers[i].name, req.headers[i].value);
        fprintf(stderr, ">\n");
    }

    /* Perform request */
    struct http_response resp;
    memset(&resp, 0, sizeof(resp));

    int rc = http_request(&req, &resp);
    if (rc != 0) {
        fprintf(stderr, "curl: (%d) Failed to connect to server\n", rc);
        return 1;
    }

    if (g_verbose) {
        fprintf(stderr, "< HTTP %d\n", resp.status_code);
        for (int i = 0; i < resp.header_count; i++)
            fprintf(stderr, "< %s: %s\n", resp.headers[i].name, resp.headers[i].value);
        fprintf(stderr, "<\n");
    }

    if (g_head_only) {
        printf("HTTP %d\n", resp.status_code);
        for (int i = 0; i < resp.header_count; i++)
            printf("%s: %s\n", resp.headers[i].name, resp.headers[i].value);
        http_response_free(&resp);
        return 0;
    }

    /* Output body */
    if (resp.body && resp.body_len > 0) {
        if (!g_silent && !g_verbose) {
            print_progress(resp.body_len, resp.body_len);
            fprintf(stderr, "\n");
        }

        if (g_output_file) {
            FILE *f = fopen(g_output_file, "w");
            if (!f) {
                fprintf(stderr, "curl: cannot open %s for writing\n", g_output_file);
                http_response_free(&resp);
                return 1;
            }
            fwrite(resp.body, 1, resp.body_len, f);
            fclose(f);
            if (!g_silent)
                fprintf(stderr, "Saved %zu bytes to %s\n", resp.body_len, g_output_file);
        } else {
            fwrite(resp.body, 1, resp.body_len, stdout);
        }
    }

    http_response_free(&resp);
    return (resp.status_code >= 200 && resp.status_code < 400) ? 0 : 22;
}
