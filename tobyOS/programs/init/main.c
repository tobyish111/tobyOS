/* init/main.c -- PID 1 init process for tobyOS.
 *
 * Orchestrates boot sequence: verify root mount, mount /data, start
 * services, launch login, and manage user sessions in a loop.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <toby/config.h>

/* ---- Syscall numbers we use directly ---- */

#define SYS_NANOSLEEP   47
#define SYS_KILL        120
#define SYS_SIGACTION   117
#define SYS_FORK        141
#define SYS_MOUNT       200
#define SYS_UMOUNT      201
#define SYS_POWEROFF    202
#define SYS_SYNC        203

/* ---- Inline syscall wrappers ---- */

static inline long sc1(long nr, long a)
{
    long r;
    __asm__ volatile("syscall"
        : "=a"(r) : "0"(nr), "D"(a)
        : "rcx", "r11", "memory");
    return r;
}

static inline long sc2(long nr, long a, long b)
{
    long r;
    __asm__ volatile("syscall"
        : "=a"(r) : "0"(nr), "D"(a), "S"(b)
        : "rcx", "r11", "memory");
    return r;
}

__attribute__((unused))
static inline long sc3(long nr, long a, long b, long c)
{
    long r;
    __asm__ volatile("syscall"
        : "=a"(r) : "0"(nr), "D"(a), "S"(b), "d"(c)
        : "rcx", "r11", "memory");
    return r;
}

static void msleep(int ms)
{
    sc1(SYS_NANOSLEEP, (long)ms * 1000000L);
}

static int sys_kill(int pid, int sig)
{
    return (int)sc2(SYS_KILL, pid, sig);
}

static void sys_sync(void)
{
    sc1(SYS_SYNC, 0);
}

static void sys_poweroff(void)
{
    sc1(SYS_POWEROFF, 0);
}

static int sys_mount(const char *dev, const char *mnt, const char *type)
{
    (void)type;
    return (int)sc2(SYS_MOUNT, (long)dev, (long)mnt);
}

static int sys_umount(const char *path)
{
    return (int)sc1(SYS_UMOUNT, (long)path);
}

/* ---- Signal constants ---- */

#define SIGCHLD   17
#define SIGTERM   15
#define SIGHUP     1
#define SIGKILL    9

/* ---- Service tracking ---- */

#define MAX_SERVICES  16
#define NAME_MAX_SVC  32
#define PATH_MAX_SVC  64

struct init_service {
    char name[NAME_MAX_SVC];
    char path[PATH_MAX_SVC];
    pid_t pid;
    int running;
    int restart_count;
    int auto_restart;
    unsigned long last_start;
};

static struct init_service g_services[MAX_SERVICES];
static int g_num_services;

/* ---- Session tracking ---- */

static pid_t g_login_pid;
static pid_t g_shell_pid;
static int g_shutdown_requested;
static int g_restart_services;

/* ---- Spawn helper ---- */

extern pid_t toby_spawn(const char *path, char *const argv[],
                        char *const envp[], int fd0, int fd1, int fd2);

static pid_t spawn_program(const char *path, char *const envp[])
{
    char *argv[2];
    argv[0] = (char *)path;
    argv[1] = NULL;
    return toby_spawn(path, argv, envp, 0, 1, 2);
}

/* ---- Service management ---- */

__attribute__((unused))
static int find_service(const char *name)
{
    for (int i = 0; i < g_num_services; i++) {
        if (strcmp(g_services[i].name, name) == 0)
            return i;
    }
    return -1;
}

static void register_service(const char *name, const char *path,
                             int auto_restart)
{
    if (g_num_services >= MAX_SERVICES) return;
    struct init_service *svc = &g_services[g_num_services++];
    memset(svc, 0, sizeof(*svc));
    size_t nlen = strlen(name);
    if (nlen >= NAME_MAX_SVC) nlen = NAME_MAX_SVC - 1;
    memcpy(svc->name, name, nlen);
    size_t plen = strlen(path);
    if (plen >= PATH_MAX_SVC) plen = PATH_MAX_SVC - 1;
    memcpy(svc->path, path, plen);
    svc->auto_restart = auto_restart;
}

static void start_service(struct init_service *svc)
{
    if (svc->running) return;
    pid_t pid = spawn_program(svc->path, NULL);
    if (pid > 0) {
        svc->pid = pid;
        svc->running = 1;
        printf("[init] started service '%s' (pid %d)\n", svc->name, pid);
    } else {
        printf("[init] failed to start service '%s'\n", svc->name);
    }
}

static void start_all_services(void)
{
    for (int i = 0; i < g_num_services; i++) {
        start_service(&g_services[i]);
        msleep(100);
    }
}

static void stop_service(struct init_service *svc)
{
    if (!svc->running) return;
    sys_kill(svc->pid, SIGTERM);
    svc->running = 0;
    printf("[init] stopped service '%s'\n", svc->name);
}

static void stop_all_services(void)
{
    for (int i = 0; i < g_num_services; i++) {
        stop_service(&g_services[i]);
    }
}

static void handle_child_exit(pid_t pid, int status)
{
    if (pid == g_login_pid) {
        g_login_pid = 0;
        return;
    }
    if (pid == g_shell_pid) {
        g_shell_pid = 0;
        return;
    }

    for (int i = 0; i < g_num_services; i++) {
        struct init_service *svc = &g_services[i];
        if (svc->pid == pid) {
            svc->running = 0;
            svc->pid = 0;
            if (status != 0) {
                printf("[init] service '%s' crashed (status %d)\n",
                       svc->name, status);
                if (svc->auto_restart && svc->restart_count < 5) {
                    svc->restart_count++;
                    int delay = 1000 * (1 << (svc->restart_count - 1));
                    if (delay > 30000) delay = 30000;
                    msleep(delay);
                    start_service(svc);
                }
            } else {
                printf("[init] service '%s' exited normally\n", svc->name);
            }
            return;
        }
    }
}

/* ---- Config loading ---- */

static void load_services_config(void)
{
    struct config_file cf;
    if (config_load(&cf, "/system/config/services.conf") < 0) {
        /* Default services if no config */
        register_service("network", "/bin/netui", 1);
        register_service("audio",   "/bin/audiotest", 1);
        register_service("indexer", "/bin/indexer", 1);
        return;
    }

    for (int i = 0; i < cf.count; i++) {
        if (strncmp(cf.keys[i], "service.", 8) == 0) {
            const char *name = cf.keys[i] + 8;
            const char *path = cf.values[i];
            register_service(name, path, 1);
        }
    }
}

/* ---- Shutdown ---- */

static void graceful_shutdown(void)
{
    printf("[init] shutting down...\n");

    /* Stop user session */
    if (g_shell_pid > 0)  sys_kill(g_shell_pid, SIGTERM);
    if (g_login_pid > 0)  sys_kill(g_login_pid, SIGTERM);

    /* Stop services gracefully */
    stop_all_services();

    /* Wait up to 3 seconds for processes to exit */
    for (int i = 0; i < 30; i++) {
        int status;
        pid_t p = waitpid(-1, &status, 1 /* WNOHANG */);
        if (p <= 0) break;
        msleep(100);
    }

    /* Kill any remaining */
    for (int i = 0; i < g_num_services; i++) {
        if (g_services[i].running && g_services[i].pid > 0) {
            sys_kill(g_services[i].pid, SIGKILL);
        }
    }

    /* Sync and unmount */
    sys_sync();
    sys_umount("/data");

    printf("[init] system halted.\n");
    sys_poweroff();
}

/* ---- Boot banner ---- */

static void print_banner(void)
{
    printf("\n");
    printf("  ╔══════════════════════════════════╗\n");
    printf("  ║         tobyOS v1.0              ║\n");
    printf("  ║   Initializing system...         ║\n");
    printf("  ╚══════════════════════════════════╝\n");
    printf("\n");
}

/* ---- Verify root filesystem ---- */

static int verify_root(void)
{
    struct stat st;
    if (stat("/", &st) < 0) {
        printf("[init] ERROR: root filesystem not mounted!\n");
        return -1;
    }
    if (stat("/bin", &st) < 0) {
        printf("[init] WARNING: /bin not found on root\n");
    }
    printf("[init] root filesystem OK\n");
    return 0;
}

/* ---- Mount data partition ---- */

static int mount_data(void)
{
    struct stat st;
    if (stat("/data", &st) == 0) {
        printf("[init] /data already mounted\n");
        return 0;
    }

    int rc = sys_mount("/dev/sda2", "/data", "tobyfs");
    if (rc < 0) {
        printf("[init] /data mount skipped (no partition)\n");
        return -1;
    }
    printf("[init] /data mounted\n");
    return 0;
}

/* ---- Set session environment ---- */

static char *g_session_env[] = {
    "HOME=/home/user",
    "PATH=/bin:/apps",
    "USER=user",
    "SHELL=/bin/sh",
    "TERM=toby-term",
    NULL
};

/* ---- Main ---- */

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    print_banner();

    /* Step 2: Verify root */
    if (verify_root() < 0) {
        printf("[init] FATAL: cannot continue without root\n");
        for (;;) msleep(1000);
    }

    /* Step 3: Mount /data */
    mount_data();

    /* Step 4: Load and start services */
    printf("[init] loading service configuration...\n");
    load_services_config();
    printf("[init] starting %d service(s)...\n", g_num_services);
    start_all_services();

    printf("[init] boot complete, entering session loop\n");

    /* Steps 5-10: Session management loop */
    while (!g_shutdown_requested) {
        /* Step 5: Launch login */
        if (g_login_pid <= 0) {
            printf("[init] launching login...\n");
            g_login_pid = spawn_program("/bin/login", NULL);
            if (g_login_pid <= 0) {
                printf("[init] failed to start login, retrying in 2s\n");
                msleep(2000);
                continue;
            }
        }

        /* Step 6: Wait for login to exit (user authenticated) */
        int status = 0;
        pid_t exited = waitpid(-1, &status, 0);
        if (exited <= 0) {
            msleep(100);
            continue;
        }

        if (exited == g_login_pid) {
            g_login_pid = 0;
            if (status != 0) {
                printf("[init] login failed, restarting...\n");
                msleep(1000);
                continue;
            }

            /* Step 7-8: User logged in, launch shell */
            printf("[init] user authenticated, launching desktop shell\n");
            g_shell_pid = spawn_program("/bin/shell", g_session_env);
            if (g_shell_pid <= 0) {
                printf("[init] failed to start shell\n");
                msleep(1000);
                continue;
            }

            /* Step 9: Wait for shell to exit */
            while (g_shell_pid > 0 && !g_shutdown_requested) {
                int s2 = 0;
                pid_t ex2 = waitpid(-1, &s2, 0);
                if (ex2 <= 0) {
                    msleep(100);
                    continue;
                }
                if (ex2 == g_shell_pid) {
                    g_shell_pid = 0;
                    printf("[init] desktop session ended\n");
                } else {
                    handle_child_exit(ex2, s2);
                }
            }
            /* Step 10: Loop back to login */
            continue;
        }

        /* Handle other child exits (services) */
        handle_child_exit(exited, status);

        /* Check if services need restart */
        if (g_restart_services) {
            g_restart_services = 0;
            stop_all_services();
            msleep(500);
            start_all_services();
        }
    }

    graceful_shutdown();
    return 0;
}
