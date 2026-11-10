/* Host test for the shared file-type table.
 *
 *   bash logs/filetypes.sh
 *
 * Compiles <tobyos/file_types.h> ON THE HOST and checks the table's own
 * invariants. The header has no #includes precisely so this is possible, and
 * so the SAME data this test reads is what the kernel and libtoby link.
 *
 * These are not style rules. A 190-row table hand-written in a macro grows
 * duplicate extensions, rows whose action says "open" with nothing to open
 * them, and rows that claim a handler that cannot cope with the type -- and
 * every one of those is invisible until someone double-clicks that exact
 * file. The compiler checks none of it.
 */

#include <stdio.h>
#include <string.h>
#include <tobyos/file_types.h>

static const struct file_type T[] = {
#define E(e, l, h, g, ed, a) { e, l, h, (unsigned char)(g), \
                               (unsigned char)(ed), (unsigned char)(a) },
    FILE_TYPE_TABLE(E)
#undef E
};
#define N ((int)(sizeof(T) / sizeof(T[0])))

static int pass, fail;
static void ok(const char *what)  { printf("  ok   %s\n", what); pass++; }
static void bad(const char *what, const char *why) {
    printf("  FAIL %s -- %s\n", what, why); fail++;
}

int main(void) {
    printf("=== shared file-type table ===\n");
    printf("  %d types\n", N);

    /* "Many" was the requirement; a table that silently collapsed to a
     * handful would still compile and still pass every other check. */
    if (N >= 100) ok("the table is broadly populated");
    else bad("the table is broadly populated", "fewer than 100 types");

    /* Duplicates: the FIRST match wins at lookup, so a duplicate row is
     * simply dead -- and it is the later, usually more considered, one. */
    int dup = 0;
    for (int i = 0; i < N && dup < 1; i++)
        for (int j = i + 1; j < N; j++)
            if (strcmp(T[i].ext, T[j].ext) == 0) {
                printf("       duplicate extension '%s'\n", T[i].ext);
                dup++; break;
            }
    if (!dup) ok("no duplicate extensions");
    else bad("no duplicate extensions", "a later row can never be reached");

    /* Lookup is case-insensitive against a lowercase key, so an uppercase
     * letter here makes the row unreachable. */
    int badext = 0;
    for (int i = 0; i < N; i++) {
        const char *e = T[i].ext;
        if (!e || !*e) { badext++; continue; }
        for (const char *p = e; *p; p++)
            if ((*p >= 'A' && *p <= 'Z') || *p == '.') { badext++; break; }
    }
    if (!badext) ok("every extension is lowercase and dot-free");
    else bad("every extension is lowercase and dot-free", "unreachable row");

    int nolabel = 0;
    for (int i = 0; i < N; i++)
        if (!T[i].label || !T[i].label[0]) nolabel++;
    if (!nolabel) ok("every type has a label for the Type column");
    else bad("every type has a label", "Explorer would show a blank cell");

    /* action and handler must agree. FTA_OPEN with no handler is a
     * double-click that does nothing at all -- the exact dead end this
     * work set out to remove. */
    int mismatch = 0;
    for (int i = 0; i < N; i++) {
        if (T[i].action == FTA_OPEN && !T[i].handler) mismatch++;
        if (T[i].action == FTA_NONE && T[i].handler)  mismatch++;
        if (T[i].action == FTA_RUN  && T[i].handler)  mismatch++;
    }
    if (!mismatch) ok("action and handler agree on every row");
    else bad("action and handler agree", "an FTA_OPEN row with nothing to open");

    /* A handler must be a program that is actually installed. A typo here
     * fails silently at runtime: spawn returns an error nobody reads. */
    int unknown = 0;
    for (int i = 0; i < N; i++) {
        const char *h = T[i].handler;
        if (!h) continue;
        if (strcmp(h, "/bin/gui_edit")    && strcmp(h, "/bin/gui_viewer") &&
            strcmp(h, "/bin/gui_browser") && strcmp(h, "/bin/mediaplayer")) {
            printf("       '%s' -> unknown handler '%s'\n", T[i].ext, h);
            unknown++;
        }
    }
    if (!unknown) ok("every handler is one of the installed apps");
    else bad("every handler is installed", "spawn would fail silently");

    /* Only a program should ever be executed by double-click. A .so or a
     * .doc reaching FTA_RUN would hand the loader a non-program. */
    int badrun = 0;
    for (int i = 0; i < N; i++)
        if (T[i].action == FTA_RUN && strcmp(T[i].ext, "elf") != 0) badrun++;
    if (!badrun) ok("only .elf is marked runnable");
    else bad("only .elf is marked runnable", "would exec a non-program");

    /* "Editable" sends the file to a TEXT editor, which would show a binary
     * as mojibake and could write it back mangled. */
    int badedit = 0;
    for (int i = 0; i < N; i++) {
        if (!T[i].editable) continue;
        unsigned g = T[i].group;
        if (g == FTG_IMAGE || g == FTG_AUDIO || g == FTG_VIDEO ||
            g == FTG_ARCHIVE || g == FTG_EXEC || g == FTG_FONT ||
            g == FTG_DISK || g == FTG_DOC) {
            printf("       '%s' is binary but marked editable\n", T[i].ext);
            badedit++;
        }
    }
    if (!badedit) ok("no binary type is marked editable");
    else bad("no binary type is marked editable", "editor would mangle it");

    /* Per-group routing: images to the viewer that can decode them, audio to
     * the player, and video nowhere, because nothing decodes video. */
    int badimg = 0, badaud = 0, badvid = 0;
    for (int i = 0; i < N; i++) {
        if (T[i].group == FTG_IMAGE &&
            (!T[i].handler || strcmp(T[i].handler, "/bin/gui_viewer")))
            badimg++;
        if (T[i].group == FTG_AUDIO && T[i].action == FTA_OPEN &&
            (!T[i].handler || strcmp(T[i].handler, "/bin/mediaplayer")))
            badaud++;
        if (T[i].group == FTG_VIDEO && T[i].action != FTA_NONE) badvid++;
    }
    if (!badimg) ok("every image routes to the viewer");
    else bad("every image routes to the viewer", "image mode would not run");
    if (!badaud) ok("every playable audio routes to the player");
    else bad("every playable audio routes to the player", "wrong app");
    if (!badvid) ok("no video claims a handler");
    else bad("no video claims a handler", "nothing decodes video yet");

    /* The four formats mediaplayer actually lists must be the FTA_OPEN
     * ones -- routing it a file it silently ignores is a dead click. */
    const char *playable[] = { "mp3", "wav", "ogg", "m4a" };
    int routed = 0;
    for (int i = 0; i < N; i++)
        if (T[i].group == FTG_AUDIO && T[i].action == FTA_OPEN) routed++;
    if (routed == 4) ok("exactly the 4 formats the player lists are routed");
    else bad("exactly the 4 playable formats are routed", "player would ignore it");
    for (unsigned k = 0; k < sizeof(playable)/sizeof(playable[0]); k++) {
        int found = 0;
        for (int i = 0; i < N; i++)
            if (!strcmp(T[i].ext, playable[k]) && T[i].action == FTA_OPEN) found = 1;
        if (!found) { bad("the 4 playable formats are present", playable[k]); break; }
    }

    /* Spot-check the handful a person actually double-clicks, by name. The
     * structural checks above would all pass on a table that routed .txt to
     * the media player. */
    struct { const char *ext, *handler; } expect[] = {
        { "txt",  "/bin/gui_edit"    },
        { "c",    "/bin/gui_edit"    },
        { "conf", "/bin/gui_edit"    },
        { "png",  "/bin/gui_viewer"  },
        { "jpg",  "/bin/gui_viewer"  },
        { "html", "/bin/gui_browser" },
        { "wav",  "/bin/mediaplayer" },
    };
    int wrong = 0;
    for (unsigned k = 0; k < sizeof(expect)/sizeof(expect[0]); k++) {
        int seen = 0;
        for (int i = 0; i < N; i++) {
            if (strcmp(T[i].ext, expect[k].ext)) continue;
            seen = 1;
            if (!T[i].handler || strcmp(T[i].handler, expect[k].handler)) {
                printf("       '%s' -> '%s', wanted '%s'\n", expect[k].ext,
                       T[i].handler ? T[i].handler : "(none)", expect[k].handler);
                wrong++;
            }
        }
        if (!seen) { printf("       '%s' missing\n", expect[k].ext); wrong++; }
    }
    if (!wrong) ok("the everyday types open in the right app");
    else bad("the everyday types open in the right app", "wrong handler");

    printf("TYPECOUNT=%d\n", N);
    printf("VERDICT: %s pass=%d fail=%d\n", fail ? "FAIL" : "PASS", pass, fail);
    return fail ? 1 : 0;
}
