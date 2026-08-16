#!/usr/bin/env python3
"""omnisearch.py -- gate the omnibox URL-vs-search decision.

Slice 121 made the omnibox send non-URL input to a search engine instead of
navigating to https://<whatever-you-typed>. The whole feature is one
predicate, looks_like_host(), and getting it wrong is quiet in both
directions: too strict and "example.com" runs a web search for it, too loose
and "3.5" becomes a DNS lookup. Neither shows up as a crash, and neither is
visible in a screenshot harness.

Booting QEMU to type into an omnibox would take minutes and prove less, so
the functions are EXTRACTED FROM main.c VERBATIM and compiled on the host.
They are pure string code with no OS dependencies, and extracting rather than
copying is deliberate: a copied test drifts from the shipped code silently,
which is the failure mode this file exists to prevent.

    python logs/omnisearch.py     -> exit 0 on pass, 1 on any failure
"""
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "programs", "chromewin", "main.c")
OUT = os.path.join(ROOT, "build", "omnisearch_test.c")
EXE = os.path.join(ROOT, "build", "omnisearch_test.exe")

TESTS_C = r"""
static int fails, checks;
static void want_host(const char *s, int expect) {
    int got = looks_like_host(s);
    checks++;
    if (got != !!expect) {
        printf("  FAIL looks_like_host(\"%s\") = %d, want %d\n", s, got, expect);
        fails++;
    }
}
static void want_tmpl(const char *t, int expect) {
    int got = template_ok(t);
    checks++;
    if (got != !!expect) {
        printf("  FAIL template_ok(\"%s\") = %d, want %d\n", t, got, expect);
        fails++;
    }
}
static void want_enc(const char *in, const char *expect) {
    char out[512];
    url_encode_query(in, out, sizeof out);
    checks++;
    if (strcmp(out, expect) != 0) {
        printf("  FAIL encode(\"%s\") = \"%s\", want \"%s\"\n", in, out, expect);
        fails++;
    }
}

int main(void) {
    /* --- must NAVIGATE: real hosts --- */
    want_host("example.com", 1);
    want_host("www.google.com", 1);
    want_host("en.wikipedia.org/wiki/Rust", 1);
    want_host("localhost", 1);              /* the one dotless host      */
    want_host("localhost:8080", 1);
    want_host("example.com:8443/path", 1);
    want_host("10.0.0.1", 1);
    want_host("192.168.1.1:80", 1);
    want_host("a.co", 1);                   /* shortest real TLD shape   */
    want_host("EXAMPLE.COM", 1);
    want_host("sub.domain.example.museum", 1);
    want_host("example.com/a b", 1);        /* space is in the PATH      */

    /* --- must SEARCH --- */
    want_host("cats", 0);                   /* the motivating case       */
    want_host("how do i cook rice", 0);
    want_host("what is 2+2", 0);
    want_host("rust vs c++", 0);
    want_host("weather today", 0);
    want_host("c", 0);
    want_host("hello.", 0);                 /* trailing dot, no TLD      */
    want_host(".com", 0);                   /* no first label            */
    want_host("note: buy milk", 0);         /* colon, but not a port     */
    /* Single-letter final labels. No TLD is one letter, and these are the
     * shapes people actually type -- a filename is not a website. */
    want_host("main.c", 0);
    want_host("foo.h", 0);
    want_host("a.b", 0);
    want_host("localhost:", 0);             /* empty port                */
    want_host("foo:bar", 0);
    /* Numeric shapes. A single numeric label after a dot used to pass as
     * "the last octet of an IP", which made every version number and every
     * bit of arithmetic a DNS lookup. Only a COMPLETE dotted quad counts. */
    want_host("3.5", 0);
    want_host("1.5", 0);
    want_host("2026.08", 0);
    want_host("1.2.3", 0);                  /* three labels              */
    want_host("1.2.3.4.5", 0);              /* five labels               */
    want_host("999.1.1.1", 0);              /* octet out of range        */
    want_host("1.2.3.4.", 0);               /* trailing dot              */

    /* --- query encoding: nothing may reach the CDP JSON unescaped --- */
    want_enc("hello world", "hello+world");
    want_enc("rust vs c++", "rust+vs+c%2B%2B");
    want_enc("a&b=c", "a%26b%3Dc");
    want_enc("\"quoted\"", "%22quoted%22");
    want_enc("back\\slash", "back%5Cslash");
    want_enc("safe-_.~", "safe-_.~");
    want_enc("100%", "100%25");
    want_enc("caf\xc3\xa9", "caf%C3%A9");   /* utf-8 escaped bytewise    */

    /* --- a config-file template is an snprintf FORMAT STRING --- */
    want_tmpl("https://www.google.com/search?q=%s", 1);
    want_tmpl("%s", 1);
    want_tmpl("https://x/100%%?q=%s", 1);   /* %% is a literal percent   */
    want_tmpl("https://x/?q=%s&r=%s", 0);   /* reads a missing argument  */
    want_tmpl("https://x/?q=%n", 0);        /* writes through an arg     */
    want_tmpl("https://x/?q=%d", 0);        /* wrong conversion          */
    want_tmpl("https://x/search", 0);       /* no substitution at all    */
    want_tmpl("https://x/?q=%", 0);         /* trailing bare percent     */
    want_tmpl("https://x/100%%", 0);        /* literal percent, no %s    */

    if (fails) {
        printf("\nomnisearch: %d FAILURE(S) of %d checks\n", fails, checks);
        return 1;
    }
    /* An empty battery must not read as success: a bad extraction that
     * dropped every assertion would otherwise print a cheerful PASS. */
    if (checks < 40) {
        printf("omnisearch: only %d checks ran -- expected >=40\n", checks);
        return 1;
    }
    printf("omnisearch: all %d checks PASS\n", checks);
    return 0;
}
"""


def grab(src, sig):
    """Copy from a function signature to its closing brace at column 0."""
    try:
        i = src.index(sig)
    except ValueError:
        sys.exit("omnisearch: %s not found in %s -- was it renamed?" % (sig, SRC))
    return src[i:src.index("\n}\n", i) + 3]


def main():
    src = open(SRC, encoding="utf-8", errors="replace").read()
    funcs = "\n".join(grab(src, s) for s in (
        "static void url_encode_query(",
        "static int template_ok(",
        "static int is_dotted_quad(",
        "static int looks_like_host(",
    ))
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    with open(OUT, "w", encoding="utf-8") as f:
        f.write("#include <stdio.h>\n#include <string.h>\n#include <stddef.h>\n\n")
        f.write(funcs)
        f.write(TESTS_C)

    cc = os.environ.get("HOST_CC", "gcc")
    r = subprocess.run([cc, "-O1", "-Wall", "-Wextra", "-o", EXE, OUT],
                       capture_output=True, text=True)
    if r.returncode:
        print("omnisearch: COMPILE FAILED\n" + r.stdout + r.stderr)
        return 1
    if r.stderr.strip():
        print("omnisearch: compiler warnings in extracted code:\n" + r.stderr)
    return subprocess.run([EXE]).returncode


if __name__ == "__main__":
    sys.exit(main())
