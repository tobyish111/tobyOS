/* Host test for the shared copy-naming rule, <tobyos/unique_name.h>.
 *
 * Run by logs/cpcopy.sh. The header is pure string building with no
 * filesystem and no #includes, so the SAME code that names files in
 * /bin/cp -U and in File Explorer's Duplicate runs here.
 *
 * The cases that matter are the ugly ones: dotfiles, double extensions, and
 * names long enough that something has to give. A rule that quietly drops
 * the "_1" produces a name that collides with the very file it was meant to
 * avoid -- the one failure mode that turns "copy" into "overwrite".
 */

#include <stdio.h>
#include <string.h>
#include <tobyos/unique_name.h>

static int pass, fail;

static void eq(const char *what, const char *base, unsigned nth,
               const char *want) {
    char got[256];
    int rc = unique_name_nth(got, sizeof got, base, nth);
    if (rc && strcmp(got, want) == 0) {
        printf("  ok   %s: '%s' #%u -> '%s'\n", what, base, nth, got);
        pass++;
    } else {
        printf("  FAIL %s: '%s' #%u -> '%s' (wanted '%s')\n",
               what, base, nth, rc ? got : "(refused)", want);
        fail++;
    }
}

int main(void) {
    printf("=== copy-naming rule ===\n");

    /* The example that was asked for. */
    eq("plain name",        "File x",       1, "File x_1");
    eq("plain name, again", "File x",       2, "File x_2");

    /* The suffix goes before the extension, or the copy stops being the
     * same kind of file: "report.txt_1" opens in nothing. */
    eq("extension kept",    "report.txt",   1, "report_1.txt");
    eq("extension kept #2", "report.txt",   2, "report_2.txt");
    eq("two-digit",         "report.txt",  10, "report_10.txt");

    /* nth 0 is the original, so callers can run one loop from 0 and get
     * "try the untaken name first" for free. */
    eq("nth 0 is identity", "report.txt",   0, "report.txt");

    /* A leading dot is part of the name, not an extension. */
    eq("dotfile",           ".gitignore",   1, ".gitignore_1");

    /* Last dot only. "archive_1.tar.gz" would need a compound-extension
     * list, and guessing wrong there mangles the name. */
    eq("double extension",  "archive.tar.gz", 1, "archive.tar_1.gz");

    /* Copying a copy must not renumber it: "chapter_1" is an ordinary
     * filename, and turning it into "chapter_2" would land it where a
     * different real file probably belongs. */
    eq("copy of a copy",    "chapter_1.txt", 1, "chapter_1_1.txt");

    eq("no stem, just ext", "a.c",          1, "a_1.c");
    eq("trailing dot",      "weird.",       1, "weird_1.");

    /* Truncation: the stem gives way, never the suffix or the extension. */
    {
        char got[16];
        /* cap 16 -> 15 usable: "aaaaaaaaa" + "_1" + ".txt" = 15 */
        int rc = unique_name_nth(got, sizeof got, "aaaaaaaaaaaaaaaaaaaa.txt", 1);
        if (rc && strlen(got) < sizeof got &&
            strstr(got, "_1.txt") == got + strlen(got) - 6) {
            printf("  ok   long name truncates the STEM: '%s'\n", got);
            pass++;
        } else {
            printf("  FAIL long name: '%s'\n", rc ? got : "(refused)");
            fail++;
        }
    }

    /* Too small to say anything true -> refuse, and leave the buffer empty.
     * Returning a name without its suffix would collide with the original. */
    {
        char got[4];
        int rc = unique_name_nth(got, sizeof got, "report.txt", 1);
        if (!rc && got[0] == '\0') {
            printf("  ok   refuses rather than drop the suffix\n"); pass++;
        } else {
            printf("  FAIL tiny buffer returned '%s' (rc=%d)\n", got, rc);
            fail++;
        }
    }

    /* Every candidate must differ from the original AND from each other --
     * this is the whole point of the rule. */
    {
        char seen[40][64];
        int clash = 0;
        for (unsigned n = 0; n < 40; n++) {
            if (!unique_name_nth(seen[n], 64, "notes.md", n)) { clash = 1; break; }
            for (unsigned m = 0; m < n; m++)
                if (!strcmp(seen[n], seen[m])) clash = 1;
        }
        if (!clash) { printf("  ok   40 candidates are all distinct\n"); pass++; }
        else { printf("  FAIL candidates collide\n"); fail++; }
    }

    printf("VERDICT: %s pass=%d fail=%d\n", fail ? "FAIL" : "PASS", pass, fail);
    return fail ? 1 : 0;
}
