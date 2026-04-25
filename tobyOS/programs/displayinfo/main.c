/* programs/displayinfo/main.c -- M27A: dump kernel display state.
 *
 * Usage:
 *   displayinfo            -- one row per output, then a summary line
 *   displayinfo --json     -- machine-readable shape (one record/line)
 *   displayinfo --primary  -- print only the primary output
 *
 * Output (default, table):
 *
 *   IDX NAME       BACKEND      RES        BPP   PITCH     FORMAT    ORIGIN   FLAGS
 *   --- ----       -------      ---        ---   -----     ------    ------   -----
 *   0   fb0        limine-fb    1024x768   32    4096      XRGB8888  (0,0)    P*A flips=37
 *   PASS: displayinfo: 1 output(s); primary fb0 1024x768 backend=limine-fb layout=1024x768
 *
 * The trailing layout=WIDTHxHEIGHT field on the PASS summary line was
 * added in M27G so test scripts can grep for the multi-monitor virtual
 * screen extent without parsing every row.
 *
 * Exit codes (M27A convention):
 *   0  -- at least one display output (or zero on a true-headless box,
 *         which we still call PASS so test scripts don't FAIL on
 *         configurations the kernel itself accepts).
 *   1  -- syscall failed
 *   2  -- bad usage
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>

#include <tobyos_devtest.h>

static const char *flag_string(uint8_t status, char buf[8]) {
    int i = 0;
    buf[i++] = (status & ABI_DISPLAY_PRESENT) ? 'P' : '-';
    buf[i++] = (status & ABI_DISPLAY_PRIMARY) ? '*' : '-';
    buf[i++] = (status & ABI_DISPLAY_ACTIVE)  ? 'A' : '-';
    buf[i]   = '\0';
    return buf;
}

/* M27G: walk records and compute the bounding box that contains every
 * output. Used by both the JSON and the table renderers. */
static void compute_layout(const struct abi_display_info *recs, int n,
                           int *out_x, int *out_y,
                           unsigned *out_w, unsigned *out_h) {
    if (n <= 0) {
        *out_x = 0; *out_y = 0; *out_w = 0; *out_h = 0;
        return;
    }
    int min_x = recs[0].origin_x;
    int min_y = recs[0].origin_y;
    int max_x = recs[0].origin_x + (int)recs[0].width;
    int max_y = recs[0].origin_y + (int)recs[0].height;
    for (int i = 1; i < n; i++) {
        int lx = recs[i].origin_x;
        int ly = recs[i].origin_y;
        int rx = recs[i].origin_x + (int)recs[i].width;
        int ry = recs[i].origin_y + (int)recs[i].height;
        if (lx < min_x) min_x = lx;
        if (ly < min_y) min_y = ly;
        if (rx > max_x) max_x = rx;
        if (ry > max_y) max_y = ry;
    }
    *out_x = min_x;
    *out_y = min_y;
    *out_w = (unsigned)(max_x - min_x);
    *out_h = (unsigned)(max_y - min_y);
}

static int print_json(const struct abi_display_info *recs, int n) {
    /* One record per line, deliberately tiny so it's grep-able from
     * the M27A validation script even when printf isn't available
     * (e.g. on a degraded boot). M27G adds origin_x/origin_y per
     * record so a multi-monitor layout can be reconstructed without
     * extra syscalls. */
    for (int i = 0; i < n; i++) {
        const struct abi_display_info *r = &recs[i];
        char flags[8];
        flag_string(r->status, flags);
        /* libtoby printf supports %lu but not %llu (single l only); cast
         * to unsigned long. Same caveat as tobydisp_print_record. */
        printf("{\"idx\":%u,\"name\":\"%s\",\"backend\":\"%s\","
               "\"backend_id\":%u,\"width\":%u,\"height\":%u,\"bpp\":%u,"
               "\"pitch_bytes\":%u,\"pixel_format\":%u,"
               "\"origin_x\":%d,\"origin_y\":%d,"
               "\"flags\":\"%s\",\"flips\":%lu}\n",
               (unsigned)r->index,
               r->name[0]    ? r->name    : "",
               r->backend[0] ? r->backend : "",
               (unsigned)r->backend_id,
               (unsigned)r->width, (unsigned)r->height,
               (unsigned)r->bpp,   (unsigned)r->pitch_bytes,
               (unsigned)r->pixel_format,
               (int)r->origin_x, (int)r->origin_y,
               flags,
               (unsigned long)r->flips);
    }
    /* Trailing layout-summary record so consumers can pull the virtual
     * screen extent in one grep. Format intentionally matches the
     * `layout=` summary key the table renderer also emits. */
    int      lx = 0, ly = 0;
    unsigned lw = 0, lh = 0;
    compute_layout(recs, n, &lx, &ly, &lw, &lh);
    printf("{\"layout\":{\"x\":%d,\"y\":%d,\"w\":%u,\"h\":%u,\"count\":%d}}\n",
           lx, ly, lw, lh, n);
    printf("PASS: displayinfo: %d output(s) layout=%ux%u\n", n, lw, lh);
    return 0;
}

int main(int argc, char **argv) {
    int json     = 0;
    int primary  = 0;

    for (int i = 1; i < argc; i++) {
        if      (!strcmp(argv[i], "--json"))    json    = 1;
        else if (!strcmp(argv[i], "--primary")) primary = 1;
        else {
            fprintf(stderr, "FAIL: displayinfo: unknown flag '%s'\n", argv[i]);
            fprintf(stderr, "usage: displayinfo [--json|--primary]\n");
            return 2;
        }
    }

    static struct abi_display_info recs[ABI_DISPLAY_MAX_OUTPUTS];
    int n = tobydisp_list(recs, ABI_DISPLAY_MAX_OUTPUTS);
    if (n < 0) {
        fprintf(stderr, "FAIL: displayinfo: tobydisp_list errno=%d\n", errno);
        return 1;
    }

    if (json) return print_json(recs, n);

    /* Default human-friendly table renderer. */
    if (n == 0) {
        /* Headless / kernel didn't init gfx -- still PASS, just say so. */
        printf("INFO: displayinfo: no display outputs registered "
               "(kernel may be headless)\n");
        printf("PASS: displayinfo: 0 output(s)\n");
        return 0;
    }

    tobydisp_print_header(stdout);
    int prim_idx = -1;
    for (int i = 0; i < n; i++) {
        if (primary && !(recs[i].status & ABI_DISPLAY_PRIMARY)) continue;
        tobydisp_print_record(stdout, &recs[i]);
        if (recs[i].status & ABI_DISPLAY_PRIMARY) prim_idx = i;
    }

    /* M27G: compute the layout extent so the summary line can show
     * the virtual screen. Single-monitor systems get layout==primary
     * geometry (the auto-layout starts at (0,0) for the first slot). */
    int      lx = 0, ly = 0;
    unsigned lw = 0, lh = 0;
    compute_layout(recs, n, &lx, &ly, &lw, &lh);

    /* Summary line. Required by test scripts; format pinned. The
     * layout=WxH suffix is M27G; pre-M27G regexes still match the
     * leading "PASS: displayinfo: ... primary ..." prefix. */
    if (prim_idx >= 0) {
        const struct abi_display_info *p = &recs[prim_idx];
        printf("PASS: displayinfo: %d output(s); primary %s %ux%u backend=%s "
               "layout=%ux%u\n",
               n, p->name[0] ? p->name : "(unnamed)",
               (unsigned)p->width, (unsigned)p->height,
               p->backend[0] ? p->backend : tobydisp_backend_str(p->backend_id),
               lw, lh);
    } else {
        printf("PASS: displayinfo: %d output(s); no primary set layout=%ux%u\n",
               n, lw, lh);
    }
    return 0;
}
