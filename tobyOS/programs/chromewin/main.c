/* chromewin/main.c -- FRONT C: real Chromium in a TobyTK window.
 *
 * Spawns a LONG-LIVED chrome-headless-shell with --remote-debugging-pipe
 * (DevTools JSON over fds 3/4, NUL-delimited), then drives it over the
 * Chrome DevTools Protocol:
 *
 *   Target.createTarget  -> tab for the start URL
 *   Target.attachToTarget (flatten) -> sessionId for page commands
 *   Page.captureScreenshot polled  -> base64 PNG -> toby_image_load ->
 *                                     tk_draw_blit into a TK_CANVAS
 *   TK mouse/key events -> Input.dispatchMouseEvent / dispatchKeyEvent
 *
 * Process plumbing: pipe() x2 + fork() + dup2 onto fds 3/4 + the RAW
 * ABI_SYS_EXECVE syscall. (libtoby's exec family is spawn-emulation that
 * only wires fds 0-2 -- it would drop the DevTools pipes; the raw syscall
 * preserves the whole fd table, and the loader flips personality to Linux
 * per-binary.) The chrome argv/envp mirror the proven CHROMIUM_BOOT
 * harness set (src/kernel.c slice 38) minus the one-shot --dump-dom/
 * --screenshot flags, plus --remote-debugging-pipe.
 */

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <tobyos/abi/abi.h>
#include <toby/tk.h>
#include <toby/image.h>

/* Raw syscalls: execve must NOT go through libtoby (spawn emulation). */
static long sc3(long n, long a, long b, long c) {
    long r;
    __asm__ volatile("syscall" : "=a"(r) : "0"(n), "D"(a), "S"(b), "d"(c)
                     : "rcx", "r11", "memory");
    return r;
}
static long sc0(long n) {
    long r;
    __asm__ volatile("syscall" : "=a"(r) : "0"(n) : "rcx", "r11", "memory");
    return r;
}
static long sys_clock_ms(void) { return sc0(ABI_SYS_CLOCK_MS); }

#define PAGE_W 800
#define PAGE_H 600
#define BAR_H  28
#define WIN_W  PAGE_W
#define WIN_H  (PAGE_H + BAR_H)

/* Video-pipeline probe (slice 49): a minimal local autoplay <video> page with a
 * tiny inline VP9 data: URI. Deterministic + fast (~10s vs YouTube's flaky 277s)
 * -- splits "does any video decode+paint" from "does YouTube's heavy app get
 * there". Flip LOCAL_HTML_FILE off (comment it) to use START_URL directly.
 *
 * NOTE: chrome served a file:///...vidtest.html over file:// as text/plain on
 * tobyOS (the raw HTML rendered as literal text -- the <video> never parsed), so
 * instead chromewin READS the local file at bootstrap and navigates to an
 * unambiguous data:text/html;base64,... URL (no MIME sniffing, no file:// origin
 * quirks). */
/* #define LOCAL_HTML_FILE "/opt/chrome/vidtest.html" */  /* data:text/html path */

/* Slice 54: LOCAL MSE PROBE. Slice 53 pinned the YouTube wall at MSE (player
 * renders, MediaSource attached, ZERO bytes ever buffered) but could not say
 * WHETHER MSE itself works on tobyOS or whether only YouTube's segment FETCH
 * fails. This injects a self-contained MSE test into about:blank: a tiny VP9
 * clip is embedded as base64 IN THE SCRIPT (no network at all), decoded to a
 * Uint8Array and appendBuffer'd into a SourceBuffer. window.__mse carries the
 * state machine (start / has-MediaSource / sourceopen / sb-added / appending /
 * updateend buf=N) and the probe reports it. If this buffers and plays, MSE is
 * fine and YouTube's problem is the segment fetch; if it stalls at a named
 * step, that step IS the bug -- with a 10-second deterministic repro.
 * Comment out to go back to normal browsing. */
/* #define MSE_TEST_JS "/opt/chrome/mse_test.js" */  /* slice 54: MSE PROVEN, off */
/* #define IPC_SIZE_LADDER 1 */
/* Slice 50: with the TLB-shootdown-ordering fix (munmap/madvise free path) the
 * embed player no longer crashes at rip 0x208c13a. Now retest the handoff's REAL
 * target -- the full WATCH page -- which hit that same corruption crash (~277s).
 * (Proven paths to flip back to: youtube.com/embed/<id> ; file:///opt/chrome/vid.webm ; example.com) */
#ifdef MSE_TEST_JS
#define START_URL "about:blank"        /* the MSE test needs no page at all */
#else
#define START_URL "https://www.youtube.com/watch?v=aqz-KE-bpKQ"
#endif

static struct tk_window win;
static toby_image_t *g_frame;        /* latest decoded screenshot */
static int  g_cmd_fd  = -1;          /* we write CDP commands here (chrome fd 3) */
static int  g_resp_fd = -1;          /* we read CDP messages here (chrome fd 4) */
static int  g_next_id = 1;
static char g_session[64];
static char g_status[128] = "starting chrome...";
static int  g_frames;
static long g_quit;

/* ---- CDP message buffers ------------------------------------------- *
 * A captureScreenshot response for 800x600 is ~20-40 KB of base64 inside
 * JSON. Static buffers; the pipe delivers in 4 KB chunks. */
#define MSG_MAX (1024 * 1024)
static char    g_rxbuf[MSG_MAX];     /* accumulates raw pipe bytes */
/* Slice 58c: CDP receive-drop accounting (see cdp_fill_nb). */
static long    g_drop_events, g_drop_midmsg;
static size_t  g_rxlen;
static char    g_msg[MSG_MAX];       /* one complete NUL-delimited message */
static uint8_t g_png[MSG_MAX];       /* decoded PNG bytes (also raw local-file buf) */

/* Big buffers for a data:text/html;base64,... navigation URL built from a local
 * HTML file (LOCAL_HTML_FILE). A ~60KB page base64s to ~80KB; the createTarget
 * JSON wraps that, so cdp_send's 2KB buf is far too small -- built separately. */
#define BIGURL_MAX (320 * 1024)
static char    g_dataurl[BIGURL_MAX];
static char    g_bigcmd[BIGURL_MAX + 256];

static void logln(const char *s) { printf("[chromewin] %s\n", s); }

/* ---- base64 (adapted from user_gui_browser's data:-URI decoder) ----- */
static int b64_val(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}
static long b64_decode_buf(const char *s, long n, uint8_t *out) {
    long o = 0; int acc = 0, nbits = 0;
    for (long i = 0; i < n; i++) {
        int v = b64_val(s[i]);
        if (v < 0) continue;
        acc = (acc << 6) | v; nbits += 6;
        if (nbits >= 8) { nbits -= 8; out[o++] = (uint8_t)(acc >> nbits); }
    }
    return o;
}

/* base64 ENCODE (for the local-file -> data:text/html URL path). */
static const char B64E[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static long b64_encode_buf(const uint8_t *in, long n, char *out) {
    long o = 0, i = 0;
    for (; i + 3 <= n; i += 3) {
        uint32_t v = ((uint32_t)in[i] << 16) | ((uint32_t)in[i+1] << 8) | in[i+2];
        out[o++] = B64E[(v >> 18) & 63]; out[o++] = B64E[(v >> 12) & 63];
        out[o++] = B64E[(v >> 6) & 63];  out[o++] = B64E[v & 63];
    }
    long rem = n - i;
    if (rem == 1) {
        uint32_t v = (uint32_t)in[i] << 16;
        out[o++] = B64E[(v >> 18) & 63]; out[o++] = B64E[(v >> 12) & 63];
        out[o++] = '='; out[o++] = '=';
    } else if (rem == 2) {
        uint32_t v = ((uint32_t)in[i] << 16) | ((uint32_t)in[i+1] << 8);
        out[o++] = B64E[(v >> 18) & 63]; out[o++] = B64E[(v >> 12) & 63];
        out[o++] = B64E[(v >> 6) & 63];  out[o++] = '=';
    }
    out[o] = 0;
    return o;
}

#ifdef LOCAL_HTML_FILE
/* Read LOCAL_HTML_FILE and build a data:text/html;base64,<...> URL in g_dataurl.
 * Returns the URL, or NULL on failure. */
static const char *build_local_data_url(void) {
    int fd = open(LOCAL_HTML_FILE, O_RDONLY, 0);
    if (fd < 0) { logln("open(LOCAL_HTML_FILE) failed"); return NULL; }
    long total = 0, r;
    while (total < MSG_MAX &&
           (r = read(fd, g_png + total, (long)(MSG_MAX - total))) > 0)
        total += r;
    close(fd);
    if (total <= 0) { logln("read(LOCAL_HTML_FILE) empty"); return NULL; }
    const char *pfx = "data:text/html;base64,";
    long pl = (long)strlen(pfx);
    if (pl + (total + 2) / 3 * 4 + 1 >= BIGURL_MAX) {
        logln("local page too big for data URL"); return NULL;
    }
    memcpy(g_dataurl, pfx, pl);
    long el = b64_encode_buf(g_png, total, g_dataurl + pl);
    printf("[chromewin] local page %ld bytes -> data URL %ld bytes\n",
           total, pl + el);
    return g_dataurl;
}
#endif

/* ---- tiny JSON field scanners (CDP replies are flat enough) --------- */

/* Find "key":"value" and copy value (no escape handling -- session ids,
 * target ids and base64 payloads never contain escapes). Returns len or -1. */
static long json_str(const char *msg, const char *key, char *out, long cap) {
    char pat[64];
    snprintf(pat, sizeof pat, "\"%s\":\"", key);
    const char *p = strstr(msg, pat);
    if (!p) return -1;
    p += strlen(pat);
    const char *e = strchr(p, '"');
    if (!e) return -1;
    long n = e - p;
    if (n > cap - 1) n = cap - 1;
    memcpy(out, p, n); out[n] = 0;
    return n;
}

/* Does this message carry "id":<id> at top level? */
static int json_has_id(const char *msg, int id) {
    char pat[32];
    snprintf(pat, sizeof pat, "\"id\":%d", id);
    return strstr(msg, pat) != NULL;
}

/* Find "key":<number> and return the int (the FIRST occurrence whose value is a
 * digit -- so it picks the numeric screencast sessionId in a screencastFrame
 * event's params, NOT the flat string "sessionId":"<hex>" top-level field).
 * Returns -1 if not found. */
static int json_int(const char *msg, const char *key) {
    char pat[64];
    snprintf(pat, sizeof pat, "\"%s\":", key);
    const char *p = msg;
    while ((p = strstr(p, pat)) != NULL) {
        p += strlen(pat);
        if (*p >= '0' && *p <= '9') return atoi(p);   /* numeric value */
        p++;                                           /* was a string/obj: keep scanning */
    }
    return -1;
}

/* ---- CDP transport --------------------------------------------------- */

static void cdp_write(const char *json) {
    size_t n = strlen(json) + 1;              /* include the NUL terminator */
    size_t off = 0;
    while (off < n) {
        long w = write(g_cmd_fd, json + off, n - off);
        if (w <= 0) { g_quit = 1; return; }
        off += (size_t)w;
    }
    printf("[chromewin] wrote %lu bytes to cmd pipe\n", (unsigned long)n);
}

/* Pull one NUL-delimited message into g_msg (blocking). 0 on EOF/err. */
static int cdp_read_msg(void) {
    for (;;) {
        char *z = memchr(g_rxbuf, 0, g_rxlen);
        if (z) {
            size_t n = (size_t)(z - g_rxbuf);
            if (n >= MSG_MAX) n = MSG_MAX - 1;
            memcpy(g_msg, g_rxbuf, n); g_msg[n] = 0;
            size_t rest = g_rxlen - (n + 1);
            memmove(g_rxbuf, z + 1, rest);
            g_rxlen = rest;
            return 1;
        }
        if (g_rxlen >= MSG_MAX - 4096) {       /* runaway message: drop half */
            memmove(g_rxbuf, g_rxbuf + MSG_MAX / 2, g_rxlen - MSG_MAX / 2);
            g_rxlen -= MSG_MAX / 2;
        }
        long r = read(g_resp_fd, g_rxbuf + g_rxlen, MSG_MAX - g_rxlen);
        if (r <= 0) { g_quit = 1; return 0; }
        g_rxlen += (size_t)r;
    }
}

/* Non-blocking read of the CDP pipe (slice 45 ABI_SYS_READ_NB). Returns bytes
 * read (>0), 0 on EOF, or -11 (-ABI_EAGAIN) when nothing is buffered yet. Lets
 * the TK loop drain pushed screencast frames without a blocking read stalling
 * input. */
static long read_nb(int fd, void *buf, long len) {
    return sc3(ABI_SYS_READ_NB, (long)fd, (long)(uintptr_t)buf, len);
}

/* Pull whatever bytes are available into g_rxbuf. 1 got data, 0 none-now
 * (EAGAIN), -1 EOF/err. */
static int cdp_fill_nb(void) {
    if (g_rxlen >= MSG_MAX - 4096) {
        /* Behind on drain (a fast video floods faster than we decode): drop
         * whole buffered messages up to the LAST NUL so we resync on a message
         * BOUNDARY. Dropping a fixed half cut a frame mid-message -> a corrupt
         * JPEG -> "decode failed" for that frame and, once perpetually behind,
         * for every frame after. Keep only the trailing (incomplete) message. */
        size_t cut = 0;
        for (size_t i = g_rxlen; i > 0; i--) {
            if (g_rxbuf[i - 1] == 0) { cut = i; break; }   /* i-1 = last NUL */
        }
        /* Slice 58c: count drops and flag the dangerous variant. cut == 0
         * means NO message boundary was buffered, so we discard the head of
         * a message still in flight -- its TAIL then arrives, hits the next
         * NUL, and parses as if it were a WHOLE message. That is the one way
         * this program can fabricate a plausible-but-wrong field (e.g. the
         * run-15 `expire=8922279409`) with no kernel bug involved. */
        g_drop_events++;
        if (cut == 0) {
            g_drop_midmsg++;
            cut = g_rxlen;                    /* one giant partial: drop it all */
        }
        printf("[cdp] rx drop #%ld (%s) rxlen=%u\n", g_drop_events,
               g_drop_midmsg && cut == g_rxlen ? "MID-MESSAGE, next parse is a"
                                                 " FRAGMENT" : "at boundary",
               (unsigned)g_rxlen);
        memmove(g_rxbuf, g_rxbuf + cut, g_rxlen - cut);
        g_rxlen -= cut;
    }
    long r = read_nb(g_resp_fd, g_rxbuf + g_rxlen, (long)(MSG_MAX - g_rxlen));
    if (r > 0)   { g_rxlen += (size_t)r; return 1; }
    if (r == -11) return 0;                    /* EAGAIN: no data right now */
    return -1;                                 /* 0 = EOF, other = error */
}

/* Extract one complete NUL-delimited message from g_rxbuf into g_msg. 1 if a
 * full message was available, 0 if only a partial (left buffered). */
static int cdp_take_msg(void) {
    char *z = memchr(g_rxbuf, 0, g_rxlen);
    if (!z) return 0;
    size_t n = (size_t)(z - g_rxbuf);
    if (n >= MSG_MAX) n = MSG_MAX - 1;
    memcpy(g_msg, g_rxbuf, n); g_msg[n] = 0;
    size_t rest = g_rxlen - (n + 1);
    memmove(g_rxbuf, z + 1, rest);
    g_rxlen = rest;
    return 1;
}

/* Send a browser-level or session command; return its id. */
static int cdp_send(const char *method, const char *params_json, int with_session) {
    static char buf[2048];
    int id = g_next_id++;
    if (with_session && g_session[0])
        snprintf(buf, sizeof buf,
                 "{\"id\":%d,\"sessionId\":\"%s\",\"method\":\"%s\",\"params\":%s}",
                 id, g_session, method, params_json ? params_json : "{}");
    else
        snprintf(buf, sizeof buf,
                 "{\"id\":%d,\"method\":\"%s\",\"params\":%s}",
                 id, method, params_json ? params_json : "{}");
    cdp_write(buf);
    return id;
}

/* ---- screencast frame handling (slice 45) ---------------------------- *
 * Page.startScreencast makes chrome PUSH a Page.screencastFrame event (base64
 * JPEG + a numeric sessionId) on every damage, instead of us polling
 * Page.captureScreenshot -- which queued behind a busy renderer and starved the
 * frame rate (YouTube returned ~2 frames). Each frame MUST be acked with
 * Page.screencastFrameAck{sessionId} or chrome stops sending. */

static long g_last_frame_ms;

/* Decode the base64 "data" field currently in g_msg and install it as the
 * displayed frame. Shared by the pushed-screencast path and the polled
 * captureScreenshot fallback. Returns 1 if a frame was installed. */
static int install_b64_frame(void) {
    const char *p = strstr(g_msg, "\"data\":\"");
    if (!p) return 0;
    p += 8;
    const char *e = strchr(p, '"');
    if (!e) return 0;
    long n = b64_decode_buf(p, e - p, g_png);
    if (n <= 8) return 0;
    toby_image_t *img = toby_image_load(g_png, (size_t)n);   /* JPEG */
    if (!img) { logln("JPEG decode failed"); return 0; }
    toby_image_t *old = g_frame;
    g_frame = img;
    if (old) toby_image_free(old);
    g_frames++;
    g_last_frame_ms = sys_clock_ms();
    if (g_frames == 1 || (g_frames % 30) == 0)
        printf("[chromewin] frame %d: %dx%d jpeg=%ld bytes\n",
               g_frames, img->width, img->height, n);
    tk_redraw(&win);
    return 1;
}

static void handle_screencast_frame(void) {
    int sid = json_int(g_msg, "sessionId");        /* numeric screencast session */
    install_b64_frame();
    if (sid >= 0) {                                /* ack -> chrome sends the next */
        char params[48];
        snprintf(params, sizeof params, "{\"sessionId\":%d}", sid);
        cdp_send("Page.screencastFrameAck", params, 1);
    }
}

/* Slice 52: POLLED-SCREENSHOT FALLBACK. Page.startScreencast only pushes when
 * the page DAMAGES something. A fully-loaded STATIC page streams NOTHING --
 * measured with the page-state probe: readyState=complete, real title, laid-out
 * body, and frames=0 forever (example.com finishes painting before the
 * screencast is even started). From outside that is indistinguishable from a
 * network/renderer hang, and it cost this arc a wrong "response-body data pipe"
 * conclusion. So when no pushed frame has arrived recently, ASK for one.
 * Screencast still carries dynamic/video content at full rate; this fires only
 * in the gaps, with at most one request outstanding. */
static int g_shot_id;                              /* outstanding request, 0=none */

/* Slice 54: IPC SIZE LADDER. A ~12KB Runtime.evaluate silently KILLS the CDP
 * session (chrome keeps reading our commands but never writes another byte),
 * while the same command at ~2.8KB works perfectly -- and an ~83KB data: URL
 * previously crashed the NetworkService with VALIDATION_ERROR_UNEXPECTED_
 * STRUCT_HEADER. That smells like one bug in large-message delivery, and it is
 * the prime suspect for YouTube's media segments (hundreds of KB each) never
 * arriving. This walks a ladder of message sizes and logs which ones chrome
 * answers, giving the exact threshold in ONE run instead of a rebuild per
 * guess. Each rung is a trivial script padded to size, so only the MESSAGE
 * size varies. */
#ifdef IPC_SIZE_LADDER
static const int g_ladder[] = { 1024, 2048, 4096, 6144, 8192, 10240,
                                12288, 16384, 24576, 32768, 49152, 65536, 0 };
static int g_lad_i;

static void send_ladder_probe(void) {
    int n = g_ladder[g_lad_i];
    if (!n) return;
    int id = g_next_id++;
    int off = snprintf(g_bigcmd, sizeof g_bigcmd,
                       "{\"id\":%d,\"sessionId\":\"%s\",\"method\":\"Runtime.evaluate\","
                       "\"params\":{\"expression\":\"var s='", id, g_session);
    for (int i = 0; i < n; i++) g_bigcmd[off + i] = 'A';
    off += n;
    off += snprintf(g_bigcmd + off, sizeof g_bigcmd - (size_t)off,
                    "';'ladder%d ok'\",\"returnByValue\":true}}", n);
    printf("[chromewin] ladder SEND pad=%d id=%d msgbytes=%d\n", n, id, off);
    cdp_write(g_bigcmd);
    g_lad_i++;
}
#endif

static void request_screenshot(void) {
    if (g_shot_id) return;
    g_shot_id = cdp_send("Page.captureScreenshot",
                         "{\"format\":\"jpeg\",\"quality\":60}", 1);
}

/* Dispatch one event message currently in g_msg. Only screencastFrame needs
 * action; command responses and other events are ignored (fire-and-forget). */
/* Slice 55: NETWORK-DOMAIN INSTRUMENT. Decode (49), display (52) and MSE (54)
 * are all proven working, so YouTube's remaining failure is that its media
 * SEGMENTS never arrive. Rather than infer that from the kernel's TLS byte
 * counts, have chrome report its OWN requests: Network.enable gives
 * requestWillBeSent / responseReceived / loadingFinished / loadingFailed, which
 * answers the three questions that matter -- are segment requests even ISSUED,
 * do they FAIL (with chrome's own error text), or do they hang forever?
 * Media URLs are the chatty part of a watch page, so only those are logged
 * individually; everything else is counted. */
static int g_req_total, g_req_media, g_resp_media, g_fail_total, g_fin_media;
/* Slice 59b: the page-richness data plane. thumb = i.ytimg.com fetches
 * (thumbnail images -- are they even ISSUED?); api = /youtubei/ calls (the
 * `next` API delivers related-video tiles AND comment metadata; if it never
 * completes, tiles=2/cmt=0 is a DATA problem, not a rendering one). */
static int g_req_thumb, g_req_api, g_resp_api_ok, g_resp_api_bad;

static void note_network_event(void) {
    static char url[160], err[96];

    if (strstr(g_msg, "\"Network.requestWillBeSent\"")) {
        g_req_total++;
        if (json_str(g_msg, "url", url, sizeof url) <= 0) return;
        if (strstr(url, "ytimg.com")) { g_req_thumb++; return; }
        if (strstr(url, "/youtubei/")) {
            g_req_api++;
            if (g_req_api <= 8)
                printf("[net] API REQ #%d: %.100s\n", g_req_api, url);
            return;
        }
        if (strstr(url, "videoplayback") || strstr(url, "googlevideo")) {
            g_req_media++;
            /* Slice 58b: is the URL WE LOG actually what chrome sent, or an
             * artifact of our own CDP reassembly? A media URL's expire= is
             * unix seconds ~now (1.7e9); run 15 logged 8922279409 (year 2252)
             * on the requests YouTube 403'd. Two candidate causes: chrome's
             * URL really is corrupt (kernel memory corruption in chrome's
             * space), or cdp_fill_nb's overflow drop left a FRAGMENT that
             * parsed as a whole message (our bug, log-only). Print the raw
             * message length + whether it looks like a well-formed CDP frame
             * ({"method":...} with a matching tail) so a fragment is
             * self-evident, and tag the requestId so the 403 response can be
             * paired to the exact URL that earned it. */
            char rid[48]; rid[0] = 0;
            json_str(g_msg, "requestId", rid, sizeof rid);
            const char *ex = strstr(url, "expire=");
            long exv = ex ? atol(ex + 7) : 0;
            /* Slice 58c: only URLs that HAVE an expire= can have a bogus one.
             * The previous form flagged every expire-less googlevideo URL
             * (generate_204 etc.) as corrupt -- a false positive that made
             * run 16 look like it had corruption when it did not. */
            int bogus = ex && (exv < 1700000000L || exv > 2000000000L);
            size_t mlen = strlen(g_msg);
            if (g_req_media <= 12 || bogus)
                printf("[net] MEDIA REQ #%d rid=%s exp=%ld%s mlen=%u "
                       "head=%.24s: %.90s\n",
                       g_req_media, rid[0] ? rid : "?", exv,
                       bogus ? " BOGUS-EXPIRE" : "", (unsigned)mlen,
                       g_msg, url);
        }
        return;
    }
    if (strstr(g_msg, "\"Network.loadingFailed\"")) {
        g_fail_total++;
        err[0] = 0;
        json_str(g_msg, "errorText", err, sizeof err);
        if (g_fail_total <= 12)
            printf("[net] FAILED #%d: %s\n", g_fail_total, err[0] ? err : "(no text)");
        return;
    }
    if (strstr(g_msg, "\"Network.responseReceived\"")) {
        if (json_str(g_msg, "url", url, sizeof url) <= 0) return;
        if (strstr(url, "/youtubei/")) {
            int ast = json_int(g_msg, "status");
            if (ast >= 200 && ast < 300) g_resp_api_ok++;
            else                          g_resp_api_bad++;
            if (g_resp_api_ok + g_resp_api_bad <= 8)
                printf("[net] API RESP status=%d %.80s\n", ast, url);
            return;
        }
        if (strstr(url, "videoplayback") || strstr(url, "googlevideo")) {
            g_resp_media++;
            /* Slice 58b: tag rid + the response's OWN expire so a 403 can be
             * paired to the exact URL (and its expire) that earned it. */
            char rid[48]; rid[0] = 0;
            json_str(g_msg, "requestId", rid, sizeof rid);
            const char *ex = strstr(url, "expire=");
            long exv = ex ? atol(ex + 7) : 0;
            int st = json_int(g_msg, "status");
            if (g_resp_media <= 12 || st != 200)
                printf("[net] MEDIA RESP #%d rid=%s status=%d exp=%ld\n",
                       g_resp_media, rid[0] ? rid : "?", st, exv);
        }
        return;
    }
    if (strstr(g_msg, "\"Network.loadingFinished\"")) {
        g_fin_media++;      /* not URL-tagged; a count is enough to see progress */
        return;
    }
}

static void cdp_dispatch(void) {
    if (strstr(g_msg, "\"method\":\"Page.screencastFrame\"")) {
        handle_screencast_frame();
        return;
    }
    if (strstr(g_msg, "\"method\":\"Network.")) { note_network_event(); return; }
    if (g_shot_id && json_has_id(g_msg, g_shot_id)) {   /* polled screenshot reply */
        g_shot_id = 0;
        install_b64_frame();
        return;
    }
    /* Slice 52: surface the page-state probe reply (and any CDP error) so the
     * serial log shows what chrome thinks the page is. Truncated: a full
     * Runtime.evaluate reply is small, but an error can carry a big stack. */
    if (strstr(g_msg, "tobyprobe") || strstr(g_msg, "ladder") ||
        strstr(g_msg, "\"error\"")) {
        char line[300];
        size_t n = strlen(g_msg);
        if (n > sizeof line - 1) n = sizeof line - 1;
        memcpy(line, g_msg, n); line[n] = 0;
        printf("[chromewin] CDP: %s\n", line);
    }
}

/* Read until the response for `id` arrives; events encountered while waiting
 * are DISPATCHED (so a screencast frame pushed mid-bootstrap is drawn + acked,
 * not lost). The response is left in g_msg. Returns 1 on success. */
static int cdp_wait(int id) {
    while (!g_quit) {
        if (!cdp_read_msg()) return 0;
        if (json_has_id(g_msg, id)) return 1;
        cdp_dispatch();
    }
    return 0;
}

/* ---- chrome spawn ---------------------------------------------------- */

static int spawn_chrome(void) {
    int p2c[2], c2p[2];                       /* parent->chrome, chrome->parent */
    if (pipe(p2c) != 0 || pipe(c2p) != 0) { logln("pipe() failed"); return -1; }

    long pid = fork();
    if (pid < 0) { logln("fork() failed"); return -1; }

    if (pid == 0) {
        /* Child: DevTools pipe convention -- chrome READS commands on fd 3,
         * WRITES messages on fd 4. The pipe ends land on fds 3-6 in a fresh
         * process, i.e. INSIDE the target range -- naive dup2(3,3)+close(3)
         * closed the command fd (run 1: "Remote debugging pipe file
         * descriptors are not open"). Move both ends above the range first. */
        int rfd = p2c[0], wfd = c2p[1];
        while (rfd == 3 || rfd == 4) rfd = dup(rfd);
        while (wfd == 3 || wfd == 4) wfd = dup(wfd);
        close(p2c[1]); close(c2p[0]);
        if (p2c[0] != rfd && p2c[0] > 2) close(p2c[0]);
        if (c2p[1] != wfd && c2p[1] > 2) close(c2p[1]);
        dup2(rfd, 3);
        dup2(wfd, 4);
        if (rfd != 3) close(rfd);
        if (wfd != 4) close(wfd);

        /* argv/envp mirror the proven CHROMIUM_BOOT set (kernel.c slice 38),
         * minus one-shot --dump-dom/--screenshot/--timeout/--log-net-log,
         * plus --remote-debugging-pipe for the long-lived session. */
        char *argv[] = {
            (char *)"/opt/chrome/chrome-headless-shell",
            /* MANDATORY: we run as root; chrome exit(1)s at ~4s with
             * "Running as root without --no-sandbox is not supported"
             * otherwise (zygote_host_impl_linux.cc:101).  This exact
             * omission cost a full 440s run that looked like a hang. */
            (char *)"--no-sandbox",
            (char *)"--no-zygote",
            (char *)"--remote-debugging-pipe",
            (char *)"--use-gl=angle",
            (char *)"--use-angle=swiftshader",
            (char *)"--enable-unsafe-swiftshader",
            (char *)"--in-process-gpu",
            (char *)"--disable-kill-after-bad-ipc",
            (char *)"--disable-dev-shm-usage",
            (char *)"--disable-crash-reporter",
            (char *)"--disable-in-process-stack-traces",
            (char *)"--no-first-run",
            (char *)"--enable-logging=stderr",
            /* FRESH profile dir (was /data/cr): /data/cr persists on disk.img
             * across runs, so corrupted/poisoned profile state (cookies,
             * consent redirects, cache) reproduces "identical failure on both
             * kernels" exactly like IP throttling would -- the A/B revert-test
             * could not distinguish them. A fresh dir isolates the theory. */
            (char *)"--user-data-dir=/data/cr2",
            (char *)"--window-size=800,600",
            (char *)"--ignore-certificate-errors",
            (char *)"--allow-file-access-from-files",
            (char *)"--autoplay-policy=no-user-gesture-required",
            /* Slice 56: tobyOS has no ALSA device. With audio enabled, the
             * watch-page renderer initialised playback at ~42s (AudioService
             * spawned, PcmOpen failed, SyncReader timed out, broken pipe) and
             * the RENDERER exit(0)'d ~1s later -- taking the whole page (and
             * all MSE fetches) with it, with no crash and no respawn. Disable
             * audio output entirely (and mute as belt-and-braces) so playback
             * runs video-only until an audio device exists. */
            (char *)"--mute-audio",
            (char *)"--disable-audio-output",
            /* Slice 59: present as ordinary desktop Chrome. chrome-headless-
             * shell otherwise advertises "HeadlessChrome/151...", which
             * YouTube treats as a bot: it serves a proof-of-origin/signature
             * -gated URL set (far-future expire=, different CDN host) that
             * 403s without a PO token, and it degrades the page (skeleton
             * metadata, fewer thumbnails, no comment fetch). A normal UA is
             * what a "normal browser" run means here. Window size + a real
             * Accept-Language round out the fingerprint. */
            (char *)"--user-agent=Mozilla/5.0 (X11; Linux x86_64) "
                    "AppleWebKit/537.36 (KHTML, like Gecko) "
                    "Chrome/151.0.0.0 Safari/537.36",
            (char *)"--lang=en-US",
            (char *)"--accept-lang=en-US,en;q=0.9",
            /* Slice 56d (wall R1): the watch-page renderer cleanly
             * exit_group(0)s at ~22-28s in most runs with no successor -- the
             * shape of a browser-INSTRUCTED shutdown. Before building kernel
             * forensics, ask the browser directly: VLOG the RenderProcessHost
             * and navigation machinery so the shutdown decision (and the
             * navigation that triggered it) is named on stderr. */
            (char *)"--vmodule=render_process_host*=2,render_frame_host*=2,"
                    "navigation_request=2,navigator=2",
            0,
        };
        char *envp[] = {
            (char *)"HOME=/data",
            (char *)"TMPDIR=/data",
            (char *)"PATH=/bin",
            (char *)"LD_LIBRARY_PATH=/opt/chrome:/opt/chrome/sysroot",
            (char *)"LANG=C",
            (char *)"VK_ICD_FILENAMES=/opt/chrome/vk_swiftshader_icd.json",
            (char *)"DISPLAY=:0",
            (char *)"LD_PRELOAD=/opt/chrome/libvulkan.so.1:/opt/chrome/libvk_swiftshader.so",
            (char *)"FONTCONFIG_FILE=/etc/fonts/fonts.conf",
            0,
        };
        sc3(ABI_SYS_EXECVE, (long)argv[0], (long)argv, (long)envp);
        _exit(127);                            /* execve failed */
    }

    /* Parent keeps the far ends. */
    close(p2c[0]); close(c2p[1]);
    g_cmd_fd  = p2c[1];
    g_resp_fd = c2p[0];
    printf("[chromewin] chrome pid=%ld cmd_fd=%d resp_fd=%d\n",
           pid, g_cmd_fd, g_resp_fd);
    return 0;
}

/* Force the first <video> to play + loop (muted). A MediaDocument (direct
 * navigation to a .webm) creates a <video controls> that does NOT autoplay even
 * with --autoplay-policy; userGesture:true satisfies the gesture requirement.
 * Fire-and-forget: the response drains in the main loop. Null-safe (no <video>
 * -> no-op), so it is harmless on non-video pages. */
static void kick_play(void) {
    cdp_send("Runtime.evaluate",
             "{\"expression\":\"var v=document.querySelector('video');"
             "if(v){v.muted=true;v.loop=true;v.play&&v.play();}\","
             "\"userGesture\":true}", 1);
}

/* Slice 52: PAGE-STATE PROBE. Frames stopped arriving for every NETWORK page
 * (local file:// pages still paint), while chrome stays busy and logs no error
 * -- so "did the page even load?" and "did it load but never paint?" are
 * indistinguishable from outside. Ask chrome itself: readyState / URL / title /
 * body size / paint-relevant geometry. Fire-and-forget; cdp_dispatch logs any
 * reply carrying the tobyprobe marker. */
static void probe_page(void) {
    /* Slice 53: also interrogate the <video> element. For a YouTube watch page
     * that renders its player but sits on the buffering spinner, the media
     * element's own state says WHICH stage stalled:
     *   n(etworkState) 0=EMPTY 1=IDLE 2=LOADING 3=NO_SOURCE
     *   r(eadyState)   0=HAVE_NOTHING .. 4=HAVE_ENOUGH_DATA
     *   b(uffered end) = how many seconds MSE actually appended
     *   e(rror code)   1=ABORTED 2=NETWORK 3=DECODE 4=SRC_NOT_SUPPORTED
     * b>0 with r>=2 means bytes ARE arriving and decoding; b='-' with n=2 means
     * the MSE fetch never delivers. */
    cdp_send("Runtime.evaluate",
             "{\"expression\":\"(function(){var v=document.querySelector('video');"
             "return 'tobyprobe rs='+document.readyState"
             "+' title='+document.title.slice(0,20)"
             "+' blen='+(document.body?document.body.innerHTML.length:-1)"
             "+' vid='+(v?('r'+v.readyState+' n'+v.networkState"
             "+' t'+v.currentTime.toFixed(1)"
             "+' d'+((v.duration||0).toFixed(0))"
             "+' b'+(v.buffered.length?v.buffered.end(0).toFixed(1):'-')"
             "+' e'+(v.error?v.error.code:'-')"
             "+' p'+(v.paused?1:0)"
             "+' src'+(v.src?v.src.slice(0,12):'-')):'novideo')"
             "+' mse='+(window.__mse||'-')"
             /* Slice 59: PAGE RICHNESS -- the "works like a normal browser"
              * metrics. imgs=<img> that actually decoded (naturalWidth>0, so
              * thumbnails really painted, not just tags present); vids=video
              * tiles in the sidebar/grid (clickable targets); cmt=rendered
              * comment threads; views/likes = whether metadata populated. */
             "+' imgs='+[].filter.call(document.images,"
                        "function(i){return i.naturalWidth>0;}).length"
             "+'/'+document.images.length"
             "+' tiles='+document.querySelectorAll("
                        "'ytd-compact-video-renderer,ytd-video-renderer,"
                        "a#thumbnail').length"
             "+' cmt='+document.querySelectorAll("
                        "'ytd-comment-thread-renderer,#content-text').length"
             "+' meta='+((document.body.innerText.match("
                        "/[0-9.,]+[KM]? views/)||['-'])[0]).slice(0,12)"
             /* Slice 59b: sy proves whether our injected scroll ever took
              * effect (YouTube may scroll an inner container; sy=0 after 8
              * scrollBy calls = the scroll never happened at document level). */
             /* Read the REAL scroll offset: on YouTube the scrolling element
              * is not always window (run 19 read sy=0 while the app scrolled
              * its own container), so take the max of both views. */
             "+' sy='+Math.max(window.scrollY,"
                      "(document.scrollingElement||document.body).scrollTop);})()\","
             "\"returnByValue\":true}", 1);
}

/* ---- CDP bootstrap: tab + flat session ------------------------------- */

static int cdp_bootstrap(void) {
    char params[256], tid[64];

    snprintf(g_status, sizeof g_status, "waiting for DevTools...");
    tk_redraw(&win); tk_pump(&win);

    const char *navurl = START_URL;
#ifdef LOCAL_HTML_FILE
    const char *du = build_local_data_url();
    if (du) navurl = du;
#endif
    /* createTarget with a possibly-huge (data:) URL: build in g_bigcmd, since
     * cdp_send's 2KB static buffer can't hold an ~80KB base64 page. */
    int id = g_next_id++;
    snprintf(g_bigcmd, sizeof g_bigcmd,
             "{\"id\":%d,\"method\":\"Target.createTarget\",\"params\":{\"url\":\"%s\"}}",
             id, navurl);
    cdp_write(g_bigcmd);
    printf("[chromewin] sent Target.createTarget id=%d urllen=%lu; waiting for reply\n",
           id, (unsigned long)strlen(navurl));
    if (!cdp_wait(id)) { logln("createTarget: pipe closed / no reply"); return -1; }
    logln("createTarget reply received");
    if (json_str(g_msg, "targetId", tid, sizeof tid) < 0) {
        logln("no targetId in createTarget reply"); return -1;
    }
    printf("[chromewin] targetId=%s\n", tid);

    snprintf(params, sizeof params,
             "{\"targetId\":\"%s\",\"flatten\":true}", tid);
    id = cdp_send("Target.attachToTarget", params, 0);
    if (!cdp_wait(id)) return -1;
    if (json_str(g_msg, "sessionId", g_session, sizeof g_session) < 0) {
        logln("no sessionId in attachToTarget reply"); return -1;
    }
    printf("[chromewin] sessionId=%s\n", g_session);

    /* Slice 55: let chrome report its own request lifecycle (see
     * note_network_event). maxPostDataSize 0 keeps the events small. */
    id = cdp_send("Network.enable",
                  "{\"maxTotalBufferSize\":1000000,\"maxResourceBufferSize\":100000}", 1);
    if (!cdp_wait(id)) return -1;

    id = cdp_send("Page.enable", "{}", 1);
    if (!cdp_wait(id)) return -1;

    /* Slice 45: push frames instead of polling captureScreenshot. Slice 50: a
     * PLAYING video pushes ~30fps of 800x600 damage, which stb_image can't decode
     * fast enough on tobyOS -> g_rxbuf backs up -> the overflow drop truncates
     * frames -> all later frames corrupt (the "display freezes at ~frame 150 while
     * chrome keeps streaming" symptom). Throttle the SOURCE so decode keeps up:
     * everyNthFrame=3 (~10fps) at 640x480 (~1.6x fewer px) -- still clearly shows
     * moving video. quality 60 balances size vs legibility. */
    id = cdp_send("Page.startScreencast",
                  "{\"format\":\"jpeg\",\"quality\":60,"
                  "\"maxWidth\":640,\"maxHeight\":480,\"everyNthFrame\":3}", 1);
    if (!cdp_wait(id)) return -1;

    kick_play();          /* start any <video> (a MediaDocument won't autoplay) */

#ifdef MSE_TEST_JS
    /* Inject the local MSE test (see MSE_TEST_JS above). The script is one
     * line of single-quoted JS with an embedded base64 clip, so it drops
     * straight into the CDP JSON; it is ~12KB, far past cdp_send's 2KB buffer,
     * hence the g_bigcmd path. */
    {
        int fd = open(MSE_TEST_JS, O_RDONLY, 0);
        if (fd < 0) {
            logln("MSE test: open failed");
        } else {
            long total = 0, r;
            while (total < (long)BIGURL_MAX - 1 &&
                   (r = read(fd, g_dataurl + total, BIGURL_MAX - 1 - total)) > 0)
                total += r;
            close(fd);
            g_dataurl[total > 0 ? total : 0] = 0;
            if (total <= 0) {
                logln("MSE test: empty script");
            } else {
                int mid = g_next_id++;
                snprintf(g_bigcmd, sizeof g_bigcmd,
                         "{\"id\":%d,\"sessionId\":\"%s\",\"method\":\"Runtime.evaluate\","
                         "\"params\":{\"expression\":\"%s\",\"returnByValue\":true,"
                         "\"userGesture\":true}}",
                         mid, g_session, g_dataurl);
                cdp_write(g_bigcmd);
                printf("[chromewin] MSE test injected (%ld bytes of JS)\n", total);
            }
        }
    }
#endif

#ifdef LOCAL_HTML_FILE
    snprintf(g_status, sizeof g_status, "%s", LOCAL_HTML_FILE);
#else
    snprintf(g_status, sizeof g_status, "%s", START_URL);
#endif
    return 0;
}

/* ---- input forwarding -------------------------------------------------- *
 * Input.* commands are FIRE-AND-FORGET: with the screencast pump, a blocking
 * cdp_wait here would stall the loop (and drop pushed frames). The command
 * responses just drain + get ignored in the main loop. */

static const char *btn_name(uint8_t b) {
    if (b & TK_BTN_RIGHT)  return "right";
    if (b & TK_BTN_MIDDLE) return "middle";
    return "left";
}

static void send_mouse(const char *type, int x, int y, uint8_t button, int clicks) {
    char params[256];
    snprintf(params, sizeof params,
             "{\"type\":\"%s\",\"x\":%d,\"y\":%d,\"button\":\"%s\",\"clickCount\":%d}",
             type, x, y, clicks ? btn_name(button) : "none", clicks);
    cdp_send("Input.dispatchMouseEvent", params, 1);
}

static void send_key(uint8_t key) {
    char params[256];
    int id;
    (void)id;
    if (key == TK_KEY_ENTER) {
        cdp_send("Input.dispatchKeyEvent",
                 "{\"type\":\"rawKeyDown\",\"windowsVirtualKeyCode\":13,"
                 "\"key\":\"Enter\",\"text\":\"\\r\"}", 1);
        cdp_send("Input.dispatchKeyEvent",
                 "{\"type\":\"keyUp\",\"windowsVirtualKeyCode\":13,"
                 "\"key\":\"Enter\"}", 1);
        return;
    }
    if (key == TK_KEY_BACKSPACE) {
        cdp_send("Input.dispatchKeyEvent",
                 "{\"type\":\"rawKeyDown\",\"windowsVirtualKeyCode\":8,"
                 "\"key\":\"Backspace\"}", 1);
        cdp_send("Input.dispatchKeyEvent",
                 "{\"type\":\"keyUp\",\"windowsVirtualKeyCode\":8,"
                 "\"key\":\"Backspace\"}", 1);
        return;
    }
    if (key < 0x20 || key >= 0x80) return;     /* arrows etc.: skip in MVP */
    char esc[8];
    if (key == '"' || key == '\\') snprintf(esc, sizeof esc, "\\%c", key);
    else snprintf(esc, sizeof esc, "%c", key);
    snprintf(params, sizeof params, "{\"type\":\"char\",\"text\":\"%s\"}", esc);
    cdp_send("Input.dispatchKeyEvent", params, 1);
}

/* ---- TobyTK glue -------------------------------------------------------- */

static void paint(struct tk_window *w, struct tk_widget *cv) {
    (void)cv;
    /* URL/status bar */
    tk_draw_fill(w, 0, 0, WIN_W, BAR_H, 0x00202028u);
    tk_draw_text(w, 8, 6, g_status, 0x00d0d0d8u, 14, 0);
    /* page area */
    if (g_frame) {
        int fw = g_frame->width  < PAGE_W ? g_frame->width  : PAGE_W;
        int fh = g_frame->height < PAGE_H ? g_frame->height : PAGE_H;
        tk_draw_blit(w, 0, BAR_H, fw, fh, g_frame->pixels, g_frame->width);
    } else {
        tk_draw_fill(w, 0, BAR_H, PAGE_W, PAGE_H, 0x00303038u);
        tk_draw_text(w, 300, BAR_H + 280, "connecting to chrome...",
                     0x00a0a0b0u, 16, 1);
    }
}

static void on_event(struct tk_window *w, struct tk_widget *cv,
                     struct tk_event *ev) {
    (void)w; (void)cv;
    if (!g_session[0]) return;
    int py = ev->y - BAR_H;                    /* canvas is window-wide; the
                                                * page starts below the bar */
    if (py < 0) return;
    switch (ev->type) {
    case TK_EV_MOUSE_MOVE: send_mouse("mouseMoved",    ev->x, py, ev->button, 0); break;
    case TK_EV_MOUSE_DOWN: send_mouse("mousePressed",  ev->x, py, ev->button, 1); break;
    case TK_EV_MOUSE_UP:   send_mouse("mouseReleased", ev->x, py, ev->button, 1); break;
    default: break;
    }
}

static void on_key(struct tk_window *w, struct tk_event *ev) {
    (void)w;
    if (ev->key == 27) { g_quit = 1; return; } /* ESC quits the host */
    if (g_session[0]) send_key(ev->key);
}

int main(void) {
    logln("chromium window host starting");
    if (spawn_chrome() != 0) return 1;

    if (tk_window_open(&win, WIN_W, WIN_H, "Chromium") != 0) {
        logln("tk_window_open failed"); return 1;
    }
    tk_on_key(&win, on_key);
    struct tk_widget *root = tk_root(&win);
    tk_pad(root, 0);
    struct tk_widget *cv = tk_canvas(&win, root, paint);
    tk_grow(cv, 1);
    tk_on_event(cv, on_event);
    tk_redraw(&win); tk_pump(&win);

    if (cdp_bootstrap() != 0) {
        snprintf(g_status, sizeof g_status, "DevTools bootstrap FAILED");
        logln("bootstrap failed");
        tk_redraw(&win);
        /* keep the window up so the failure is visible */
        for (int i = 0; i < 400 && !tk_pump(&win); i++) usleep(50000);
        return 1;
    }
    logln("bootstrap OK; screencast started");

    /* Event-driven main loop (slice 45): chrome PUSHES screencast frames; we
     * drain the pipe non-blocking so TK input is never stalled behind a frame.
     *   tk_pump -> forwards mouse/keys as Input.* (fire-and-forget)
     *   drain   -> reads every buffered CDP message; screencastFrame -> blit+ack
     * No nav-retry: under WHPX the first navigation's TLS handshake is fast
     * and heavy pages (youtube / a watch page) take >45s to first paint, so
     * re-navigating only RESTARTS the load. Target.createTarget already
     * navigated; startScreencast delivers a frame when the page paints. */
    g_last_frame_ms = sys_clock_ms();
    long t0 = sys_clock_ms();
    int replays = 0;
    while (!g_quit) {
        if (tk_pump(&win)) break;              /* input -> Input.* */

        for (;;) {                             /* drain all buffered CDP msgs */
            int f = cdp_fill_nb();
            if (f < 0) { g_quit = 1; break; }  /* pipe EOF: chrome gone */
            int any = 0;
            while (cdp_take_msg()) { cdp_dispatch(); any = 1; }
            if (f == 0 && !any) break;         /* EAGAIN and nothing buffered */
        }

        /* Re-kick play() a few times over the first ~10s: a MediaDocument's
         * <video> can appear slightly after bootstrap, and the first play()
         * may land before the element exists. */
        if (replays < 5 && sys_clock_ms() - t0 > (long)(replays + 1) * 2000) {
            kick_play();
            replays++;
        }

        /* Slice 52: no pushed frame for a while (static page, or a page that
         * finished painting before the screencast started) -> pull one. */
        if (sys_clock_ms() - g_last_frame_ms > 2000)
            request_screenshot();

#ifdef IPC_SIZE_LADDER
        /* One rung every 3s, starting 5s in (after the MSE test settles). */
        {
            static long next_rung;
            long now = sys_clock_ms();
            if (next_rung == 0) next_rung = t0 + 5000;
            if (now >= next_rung) { send_ladder_probe(); next_rung = now + 3000; }
        }
#endif

        /* Slice 59: SCROLL like a user. YouTube lazy-loads: sidebar
         * thumbnails only fetch when they near the viewport, and the comment
         * section is not even requested until you scroll to it. Without this
         * the page legitimately reports imgs=2/65 cmt=0 -- correct behaviour
         * for an 800x600 viewport that never moved, and easily mistaken for
         * broken image decoding. Scroll in steps from 25s (page settled),
         * then return to the top so the player stays visible for screenshots. */
        {
            static int scrolls; static long next_scroll;
            long nows = sys_clock_ms();
            if (next_scroll == 0) next_scroll = t0 + 25000;
            if (scrolls < 8 && nows >= next_scroll) {
                scrolls++;
                next_scroll = nows + 4000;
                /* Slice 59b: REAL wheel input, not injected JS. Run 19 proved
                 * the JS route did nothing (sy=0 after 8 window.scrollBy
                 * calls) -- YouTube's app owns the scroll, so a synthetic
                 * mouseWheel through the same Input domain the window host
                 * already uses is both effective and faithful to "what a
                 * normal browser does". Negative deltaY at the end scrolls
                 * back up so the player is on screen for the screenshots. */
                char sp[192];
                snprintf(sp, sizeof sp,
                         "{\"type\":\"mouseWheel\",\"x\":%d,\"y\":%d,"
                         "\"deltaX\":0,\"deltaY\":%d,\"modifiers\":0}",
                         PAGE_W / 2, PAGE_H / 2,
                         scrolls < 7 ? 600 : -4200);
                cdp_send("Input.dispatchMouseEvent", sp, 1);
            }
        }

        /* Slice 52: probe the page state every 10s and report the frame count
         * alongside, so a stalled run says WHY it is stalled. (Slice 56: cap
         * raised 12 -> 36; the 120s cutoff left the back half of every 360s
         * run blind.) */
        {
            static int probes;
            if (probes < 36 && sys_clock_ms() - t0 > (long)(probes + 1) * 10000) {
                probes++;
                printf("[chromewin] probe #%d at %lds: frames=%d "
                       "net{req=%d media=%d resp=%d fin=%d fail=%d} "
                       "rich{thumb=%d api=%d ok=%d bad=%d} "
                       "cdpdrop{n=%ld mid=%ld}\n",
                       probes, (long)((sys_clock_ms() - t0) / 1000), g_frames,
                       g_req_total, g_req_media, g_resp_media,
                       g_fin_media, g_fail_total,
                       g_req_thumb, g_req_api, g_resp_api_ok, g_resp_api_bad,
                       g_drop_events, g_drop_midmsg);
                probe_page();
            }
        }

        usleep(15000);
    }
    printf("[chromewin] exiting; frames=%d\n", g_frames);
    return 0;
}
