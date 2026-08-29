/* Host test for the editor's file->buffer parse. Exercises the REAL header
 * the editor compiles, not a copy of it. */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "textbuf.h"

#define L 8
#define C 16
static char lines[L][C];
static int pass, fail;

static void chk(const char *name, int cond, const char *detail) {
    if (cond) { printf("  ok   %-38s %s\n", name, detail); pass++; }
    else      { printf("  FAIL %-38s %s\n", name, detail); fail++; }
}

static struct textbuf_result run(const char *src, int more) {
    struct textbuf_result r;
    memset(lines, 0, sizeof lines);
    textbuf_parse(&lines[0][0], L, C, src, (long)strlen(src), more, &r);
    return r;
}

int main(void) {
    char d[256];
    struct textbuf_result r;

    r = run("hello\nworld\n", 0);
    snprintf(d,sizeof d,"lines=%d trunc=%d",r.line_count,r.truncated);
    chk("ordinary file is NOT truncated", !r.truncated && r.line_count==2, d);
    chk("  and its text round-trips", !strcmp(lines[0],"hello") && !strcmp(lines[1],"world"), lines[0]);

    r = run("", 0);
    snprintf(d,sizeof d,"lines=%d trunc=%d",r.line_count,r.truncated);
    chk("empty file gives one empty line", !r.truncated && r.line_count==1, d);

    r = run("no newline", 0);   /* short enough to fit C */
    snprintf(d,sizeof d,"lines=%d '%s'",r.line_count,lines[0]);
    chk("last line without newline is kept", !r.truncated && r.line_count==1 && !strcmp(lines[0],"no newline"), d);

    /* the three truncation paths */
    r = run("short\n", 1);
    snprintf(d,sizeof d,"why=%s",r.why?r.why:"(none)");
    chk("reader said more remains -> truncated", r.truncated, d);

    r = run("a\nb\nc\nd\ne\nf\ng\nh\ni\nj\n", 0);   /* 10 lines into L=8 */
    snprintf(d,sizeof d,"lines=%d why=%s",r.line_count,r.why?r.why:"(none)");
    chk("more lines than fit -> truncated", r.truncated && r.line_count<=L, d);

    r = run("0123456789ABCDEFGHIJ\n", 0);            /* 20 chars into C=16 */
    snprintf(d,sizeof d,"why=%s kept='%s'",r.why?r.why:"(none)",lines[0]);
    chk("line longer than fits -> truncated", r.truncated, d);
    chk("  overflow line still NUL-terminated", strlen(lines[0]) < (size_t)C, lines[0]);

    /* the boundary that used to be wrong: exactly full is NOT truncated */
    r = run("a\nb\nc\nd\ne\nf\ng\n", 0);              /* 7 lines + final = 7 */
    snprintf(d,sizeof d,"lines=%d trunc=%d",r.line_count,r.truncated);
    chk("exactly-fits file is NOT truncated", !r.truncated, d);

    printf("\nVERDICT: pass=%d fail=%d\n", pass, fail);
    return fail ? 1 : 0;
}
