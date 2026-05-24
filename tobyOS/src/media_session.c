/* media_session.c -- system-wide media key handling and now-playing info.
 *
 * Tracks which application currently "owns" audio playback and routes
 * media keys (Play/Pause, Next, Prev) to that app via signals.
 */

#include <tobyos/types.h>
#include <tobyos/printk.h>
#include <tobyos/klibc.h>
#include <tobyos/media_session.h>
#include <tobyos/spinlock.h>

static void ms_copy(char *dst, size_t cap, const char *src) {
    if (!dst || cap == 0) return;
    if (!src) src = "";
    size_t i = 0;
    while (i + 1 < cap && src[i]) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

#define MAX_SESSIONS 8

static struct media_session g_sessions[MAX_SESSIONS];
static int g_active_idx = -1;
static spinlock_t g_ms_lock = SPINLOCK_INIT;

void media_session_init(void) {
    memset(g_sessions, 0, sizeof(g_sessions));
    g_active_idx = -1;
    kprintf("[media] media session manager initialized\n");
}

int media_session_register(int pid) {
    uint64_t flags = spin_lock_irqsave(&g_ms_lock);
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (g_sessions[i].pid == 0) {
            memset(&g_sessions[i], 0, sizeof(g_sessions[i]));
            g_sessions[i].pid = pid;
            g_active_idx = i;
            spin_unlock_irqrestore(&g_ms_lock, flags);
            kprintf("[media] session registered pid=%d slot=%d\n", pid, i);
            return 0;
        }
    }
    spin_unlock_irqrestore(&g_ms_lock, flags);
    return -1;
}

void media_session_update(int pid, const char *title, const char *artist,
                          int duration_ms, int pos_ms, int playing) {
    uint64_t flags = spin_lock_irqsave(&g_ms_lock);
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (g_sessions[i].pid == pid) {
            if (title)
                ms_copy(g_sessions[i].title, sizeof(g_sessions[i].title), title);
            if (artist)
                ms_copy(g_sessions[i].artist, sizeof(g_sessions[i].artist), artist);
            g_sessions[i].duration_ms = duration_ms;
            g_sessions[i].position_ms = pos_ms;
            g_sessions[i].playing = playing;
            g_active_idx = i;
            break;
        }
    }
    spin_unlock_irqrestore(&g_ms_lock, flags);
}

void media_session_unregister(int pid) {
    uint64_t flags = spin_lock_irqsave(&g_ms_lock);
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (g_sessions[i].pid == pid) {
            g_sessions[i].pid = 0;
            g_sessions[i].title[0] = '\0';
            g_sessions[i].artist[0] = '\0';
            g_sessions[i].playing = 0;
            if (g_active_idx == i) {
                g_active_idx = -1;
                for (int j = 0; j < MAX_SESSIONS; j++) {
                    if (g_sessions[j].pid != 0) {
                        g_active_idx = j;
                        break;
                    }
                }
            }
            break;
        }
    }
    spin_unlock_irqrestore(&g_ms_lock, flags);
}

int media_session_get_active(struct media_session *out) {
    uint64_t flags = spin_lock_irqsave(&g_ms_lock);
    if (g_active_idx < 0 || g_sessions[g_active_idx].pid == 0) {
        spin_unlock_irqrestore(&g_ms_lock, flags);
        return -1;
    }
    memcpy(out, &g_sessions[g_active_idx], sizeof(*out));
    spin_unlock_irqrestore(&g_ms_lock, flags);
    return 0;
}

/* Signal numbers for media control (SIGUSR1 + offset). Apps register
 * signal handlers to respond to these. */
#define SIG_MEDIA_PLAYPAUSE  10
#define SIG_MEDIA_NEXT       12
#define SIG_MEDIA_PREV       13

/* Forward-declared from signal.c / proc.c */
int signal_send(int pid, int sig);

void media_session_play_pause(void) {
    uint64_t flags = spin_lock_irqsave(&g_ms_lock);
    if (g_active_idx >= 0 && g_sessions[g_active_idx].pid > 0) {
        int pid = g_sessions[g_active_idx].pid;
        g_sessions[g_active_idx].playing = !g_sessions[g_active_idx].playing;
        spin_unlock_irqrestore(&g_ms_lock, flags);
        signal_send(pid, SIG_MEDIA_PLAYPAUSE);
        return;
    }
    spin_unlock_irqrestore(&g_ms_lock, flags);
}

void media_session_next(void) {
    uint64_t flags = spin_lock_irqsave(&g_ms_lock);
    if (g_active_idx >= 0 && g_sessions[g_active_idx].pid > 0) {
        int pid = g_sessions[g_active_idx].pid;
        spin_unlock_irqrestore(&g_ms_lock, flags);
        signal_send(pid, SIG_MEDIA_NEXT);
        return;
    }
    spin_unlock_irqrestore(&g_ms_lock, flags);
}

void media_session_prev(void) {
    uint64_t flags = spin_lock_irqsave(&g_ms_lock);
    if (g_active_idx >= 0 && g_sessions[g_active_idx].pid > 0) {
        int pid = g_sessions[g_active_idx].pid;
        spin_unlock_irqrestore(&g_ms_lock, flags);
        signal_send(pid, SIG_MEDIA_PREV);
        return;
    }
    spin_unlock_irqrestore(&g_ms_lock, flags);
}
