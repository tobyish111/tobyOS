/* utmp.c -- /run/utmp: who is logged in, as Linux keeps it.
 *
 * GNU who and users read utmp records (Linux x86-64's struct utmp, 384
 * bytes), and procps' uptime counts its user processes. tobyOS had no
 * utmp, so the old who/users/uptime asked the desktop session instead and
 * never saw an SSH login (finding 151).
 *
 * The file is the kernel's: it is written whole from the table here at
 * each event -- boot, an SSH login with a terminal and its end, a desktop
 * login and logout, and the clock being set (the boot record is the boot
 * time, which moves with the clock as /proc/stat's btime does). Records,
 * as systemd and sshd leave them:
 *   BOOT_TIME  "~" "~~" user "reboot", host the kernel's release
 *   RUN_LVL    "~" "~~" user "runlevel", pid 'N' << 8 | '5'
 *   USER_PROCESS per login: its line, id the line's last four characters,
 *              user, host, pid, start time, address
 *   DEAD_PROCESS for a login that ended: line and id kept, user and host
 *              cleared, the time it ended (logout(3)'s record)
 * /var/run is a link to /run, as on Linux, so both names work; /run and
 * /var are tmpfs, 0755 (the initrd root is read-only), with /var/tmp. */

#include <tobyos/utmp.h>
#include <tobyos/vfs.h>
#include <tobyos/klibc.h>
#include <tobyos/printk.h>

#define UT_BOOT_TIME     2
#define UT_RUN_LVL       1
#define UT_USER_PROCESS  7
#define UT_DEAD_PROCESS  8

struct lx_utmp {
    int16_t  ut_type;
    int16_t  pad0;
    int32_t  ut_pid;
    char     ut_line[32];
    char     ut_id[4];
    char     ut_user[32];
    char     ut_host[256];
    int16_t  e_termination, e_exit;
    int32_t  ut_session;
    int32_t  tv_sec, tv_usec;
    int32_t  ut_addr_v6[4];
    char     unused[20];
};
_Static_assert(sizeof(struct lx_utmp) == 384, "Linux x86-64 struct utmp");

#define UT_MAX 16

struct ut_login {
    bool     used, dead;
    int      pid;
    char     line[32];
    char     user[32];
    char     host[256];
    uint32_t addr;
    int64_t  when;
};

static struct ut_login g_logins[UT_MAX];
static bool g_ready;

extern uint64_t lx_realtime_ns(uint64_t mono_ns);
extern uint64_t perf_now_ns(void);
extern int tmpfs_mount_at(const char *path, const char *opts);

static int64_t ut_now(void) {
    return (int64_t)(lx_realtime_ns(perf_now_ns()) / 1000000000ull);
}

static void ut_copy(char *dst, size_t cap, const char *src) {
    size_t n = src ? strlen(src) : 0;
    if (n > cap) n = cap;
    memset(dst, 0, cap);
    if (n) memcpy(dst, src, n);
}

static void ut_put(struct lx_utmp *r, int type, int pid, const char *line,
                   const char *user, const char *host, int64_t when, uint32_t addr) {
    memset(r, 0, sizeof *r);
    r->ut_type = (int16_t)type;
    r->ut_pid = pid;
    ut_copy(r->ut_line, sizeof r->ut_line, line);
    /* the id: "~~" for the boot records, else the line's last four */
    if (line && line[0] == '~') ut_copy(r->ut_id, sizeof r->ut_id, "~~");
    else if (line) {
        size_t n = strlen(line);
        ut_copy(r->ut_id, sizeof r->ut_id, n > 4 ? line + n - 4 : line);
    }
    ut_copy(r->ut_user, sizeof r->ut_user, user);
    ut_copy(r->ut_host, sizeof r->ut_host, host);
    r->tv_sec = (int32_t)when;
    r->ut_addr_v6[0] = (int32_t)addr;
}

void utmp_refresh(void) {
    if (!g_ready) return;
    static struct lx_utmp recs[2 + UT_MAX];
    size_t n = 0;
    uint64_t mono = perf_now_ns();
    uint64_t now = lx_realtime_ns(mono) / 1000000000ull, up = mono / 1000000000ull;
    int64_t btime = now > up ? (int64_t)(now - up) : 0;
    ut_put(&recs[n++], UT_BOOT_TIME, 0, "~", "reboot", "6.1.0-tobyos", btime, 0);
    ut_put(&recs[n++], UT_RUN_LVL, ('N' << 8) | '5', "~", "runlevel", "6.1.0-tobyos", btime, 0);
    for (int i = 0; i < UT_MAX; i++) {
        struct ut_login *l = &g_logins[i];
        if (!l->used) continue;
        if (l->dead) ut_put(&recs[n++], UT_DEAD_PROCESS, l->pid, l->line, "", "", l->when, 0);
        else ut_put(&recs[n++], UT_USER_PROCESS, l->pid, l->line, l->user, l->host, l->when, l->addr);
    }
    (void)vfs_truncate("/run/utmp", 0);     /* create() keeps a tmpfs file's bytes */
    int rc = vfs_write_all("/run/utmp", recs, n * sizeof recs[0]);
    if (rc != VFS_OK) kprintf("[utmp] writing /run/utmp: %s\n", vfs_strerror(rc));
}

void utmp_login(int pid, const char *line, const char *user,
                const char *host, uint32_t ipv4_be) {
    struct ut_login *slot = 0;
    /* the line's own record first (pututline finds it by id), else a free
     * one, else the oldest dead one */
    for (int i = 0; i < UT_MAX && !slot; i++)
        if (g_logins[i].used && strcmp(g_logins[i].line, line) == 0) slot = &g_logins[i];
    for (int i = 0; i < UT_MAX && !slot; i++)
        if (!g_logins[i].used) slot = &g_logins[i];
    for (int i = 0; i < UT_MAX && !slot; i++)
        if (g_logins[i].dead) slot = &g_logins[i];
    if (!slot) return;
    memset(slot, 0, sizeof *slot);
    slot->used = true;
    slot->pid = pid;
    ut_copy(slot->line, sizeof slot->line - 1, line);
    ut_copy(slot->user, sizeof slot->user - 1, user);
    ut_copy(slot->host, sizeof slot->host - 1, host);
    slot->addr = ipv4_be;
    slot->when = ut_now();
    utmp_refresh();
}

void utmp_logout(int pid) {
    bool any = false;
    for (int i = 0; i < UT_MAX; i++) {
        struct ut_login *l = &g_logins[i];
        if (!l->used || l->dead || l->pid != pid) continue;
        l->dead = true;
        l->user[0] = l->host[0] = 0;
        l->addr = 0;
        l->when = ut_now();
        any = true;
    }
    if (any) utmp_refresh();
}

void utmp_init(void) {
    int rc = tmpfs_mount_at("/run", "mode=755");
    int rv = tmpfs_mount_at("/var", "mode=755");
    if (rv == VFS_OK) {
        (void)vfs_symlink("/var/run", "/run");
        (void)vfs_mkdir_mode("/var/tmp", 01777u);
        (void)vfs_mkdir_mode("/var/log", 0755u);
    }
    kprintf("[boot] /run: %s, /var: %s\n",
            rc == VFS_OK ? "tmpfs" : "MOUNT FAILED",
            rv == VFS_OK ? "tmpfs, /var/run -> /run" : "MOUNT FAILED");
    if (rc != VFS_OK) return;
    g_ready = true;
    utmp_refresh();
}
