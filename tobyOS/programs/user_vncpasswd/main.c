/* vncpasswd -- set the password the VNC server authenticates against.
 *
 * Writes /etc/vnc.passwd in the same format the reference vncpasswd tool
 * uses: the 8-byte password, NUL-padded, DES-encrypted under a fixed
 * well-known key. That obfuscation is NOT security -- it only keeps the
 * password from sitting on disk in the clear, and it means a file written
 * here works with other VNC servers and vice versa.
 *
 * RFB passwords are capped at 8 characters by the protocol itself (it is a
 * single DES key), so longer input is truncated, and we say so rather than
 * letting someone believe a 20-character password is protecting them.
 *
 * The kernel refuses to start the VNC server until this file exists.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>
#include <sys/stat.h>

/* Single translation unit: the program build rule compiles exactly one
 * .c file, so the DES implementation is pulled in directly rather than
 * duplicated. DES_HOSTED swaps its klibc include for <string.h>. */
#define DES_HOSTED 1
#include "../../src/des.c"

/* Must match include/tobyos/vnc.h. /etc is ramfs and is rebuilt each
 * boot, so the persistent copy under /data is the one that lasts. */
#define VNC_PASSWD_DIR  "/data/vnc"
#define VNC_PASSWD_PATH "/data/vnc/vnc.passwd"
#define VNC_PASSWD_ALT  "/etc/vnc.passwd"
#define VNC_PASSWD_LEN  8

/* Matches VNC_FIXED_KEY in src/vnc.c -- the published vncpasswd key. */
static const uint8_t FIXED_KEY[VNC_PASSWD_LEN] = { 23, 82, 107, 6, 35, 78, 88, 7 };

static long sys_chmod(const char *path, int mode) {
    long r;
    __asm__ volatile("syscall" : "=a"(r)
        : "0"(29L), "D"((long)path), "S"((long)mode)
        : "rcx", "r11", "memory");
    return r;
}

/* Read a line with terminal echo off, so the password is not left on the
 * screen. If the terminal will not cooperate we say so instead of quietly
 * echoing it. */
static int read_hidden(const char *prompt, char *buf, size_t cap) {
    struct termios old, quiet;
    int have_tty = (tcgetattr(0, &old) == 0);

    fputs(prompt, stdout);
    fflush(stdout);

    if (have_tty) {
        quiet = old;
        quiet.c_lflag &= (unsigned)~ECHO;
        if (tcsetattr(0, TCSANOW, &quiet) != 0) have_tty = 0;
    }
    if (!have_tty)
        fputs("(warning: echo could not be disabled) ", stdout);

    if (!fgets(buf, (int)cap, stdin)) {
        if (have_tty) tcsetattr(0, TCSANOW, &old);
        return -1;
    }
    if (have_tty) {
        tcsetattr(0, TCSANOW, &old);
        fputs("\n", stdout);
    }

    size_t n = strlen(buf);
    while (n && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) buf[--n] = '\0';
    return 0;
}

static int write_passwd(const char *plain) {
    uint8_t pw[VNC_PASSWD_LEN];
    memset(pw, 0, sizeof(pw));
    for (size_t i = 0; i < VNC_PASSWD_LEN && plain[i]; i++)
        pw[i] = (uint8_t)plain[i];

    struct des_ctx ctx;
    uint8_t enc[VNC_PASSWD_LEN];
    des_set_key_vnc(&ctx, FIXED_KEY);
    des_encrypt_block(&ctx, pw, enc);
    memset(pw, 0, sizeof(pw));

    /* Persistent location first; /etc only as a stated fallback. */
    const char *path = VNC_PASSWD_PATH;
    mkdir(VNC_PASSWD_DIR, 0700);
    FILE *f = fopen(path, "wb");
    if (!f) {
        path = VNC_PASSWD_ALT;
        f = fopen(path, "wb");
        if (f)
            fprintf(stderr, "vncpasswd: no %s; saving to %s, which does "
                            "not survive a reboot\n",
                    VNC_PASSWD_DIR, VNC_PASSWD_ALT);
    }
    if (!f) {
        fprintf(stderr, "vncpasswd: cannot write %s\n", VNC_PASSWD_PATH);
        return 1;
    }
    size_t wrote = fwrite(enc, 1, sizeof(enc), f);
    fclose(f);
    memset(enc, 0, sizeof(enc));

    if (wrote != sizeof(enc)) {
        fprintf(stderr, "vncpasswd: short write to %s\n", path);
        return 1;
    }

    /* Owner-only: the obfuscation is trivially reversible, so the file
     * permissions are what actually keep other users out. */
    if (sys_chmod(path, 0600) < 0)
        fprintf(stderr, "vncpasswd: warning: could not chmod 0600 %s\n",
                path);
    printf("Password written to %s\n", path);
    return 0;
}

int main(int argc, char **argv) {
    char pass[128], again[128];

    if (argc > 2) {
        fprintf(stderr, "usage: vncpasswd [password]\n");
        return 2;
    }

    if (argc == 2) {
        /* Convenient for scripts, but it lands in the shell history and in
         * any process listing, so it is worth one line of warning. */
        snprintf(pass, sizeof(pass), "%s", argv[1]);
        fprintf(stderr, "vncpasswd: note: a password given on the command "
                        "line is visible to other users\n");
    } else {
        if (read_hidden("Password: ", pass, sizeof(pass)) != 0) {
            fprintf(stderr, "vncpasswd: could not read password\n");
            return 1;
        }
        if (read_hidden("Verify:   ", again, sizeof(again)) != 0) {
            fprintf(stderr, "vncpasswd: could not read password\n");
            return 1;
        }
        if (strcmp(pass, again) != 0) {
            fprintf(stderr, "vncpasswd: passwords do not match\n");
            return 1;
        }
        memset(again, 0, sizeof(again));
    }

    if (pass[0] == '\0') {
        fprintf(stderr, "vncpasswd: empty password refused\n");
        return 1;
    }
    if (strlen(pass) > VNC_PASSWD_LEN)
        printf("vncpasswd: note: RFB uses only the first %d characters\n",
               VNC_PASSWD_LEN);

    int rc = write_passwd(pass);
    memset(pass, 0, sizeof(pass));
    if (rc == 0)
        printf("The VNC server picks this up within a few seconds.\n");
    return rc;
}
