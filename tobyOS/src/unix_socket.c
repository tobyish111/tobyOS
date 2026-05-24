/* unix_socket.c -- Unix domain sockets (Phase 1 M1.4).
 *
 * Provides local inter-process communication via a socket-like API.
 * Supports SOCK_STREAM (connection-oriented) with a circular buffer
 * between connected pairs.
 *
 * Simplified model:
 *   - A named socket is bound to a path in the VFS namespace
 *   - One process listens, another connects
 *   - Data flows through a kernel ring buffer
 */

#include <tobyos/proc.h>
#include <tobyos/sched.h>
#include <tobyos/heap.h>
#include <tobyos/klibc.h>
#include <tobyos/printk.h>
#include <tobyos/spinlock.h>

#define UNIX_SOCK_MAX      32
#define UNIX_BUF_SIZE      8192
#define UNIX_PATH_MAX      108
#define UNIX_BACKLOG_MAX   4

struct unix_socket {
    char     path[UNIX_PATH_MAX];
    bool     active;
    bool     listening;
    int      owner_pid;

    /* Ring buffer for connected stream sockets */
    uint8_t  buf[UNIX_BUF_SIZE];
    size_t   head;
    size_t   tail;
    size_t   count;

    /* Connected peer (bidirectional: each socket has its own buffer) */
    int      peer_idx;  /* index into g_unix_socks, or -1 */

    /* Accept queue */
    int      pending_connections[UNIX_BACKLOG_MAX];
    int      n_pending;

    /* Wait queues */
    struct proc *read_waiters;
    struct proc *write_waiters;
    struct proc *accept_waiters;

    spinlock_t lock;
};

static struct unix_socket g_unix_socks[UNIX_SOCK_MAX];
static spinlock_t g_unix_global_lock = SPINLOCK_INIT;

void unix_socket_init(void) {
    memset(g_unix_socks, 0, sizeof(g_unix_socks));
    for (int i = 0; i < UNIX_SOCK_MAX; i++) {
        spin_init(&g_unix_socks[i].lock);
        g_unix_socks[i].peer_idx = -1;
    }
    kprintf("[unix_sock] Unix domain socket subsystem ready\n");
}

static int alloc_socket(void) {
    for (int i = 0; i < UNIX_SOCK_MAX; i++) {
        if (!g_unix_socks[i].active) return i;
    }
    return -1;
}

/* Create a Unix socket. Returns socket id. */
long sys_unix_socket(void) {
    uint64_t f = spin_lock_irqsave(&g_unix_global_lock);
    int idx = alloc_socket();
    if (idx < 0) {
        spin_unlock_irqrestore(&g_unix_global_lock, f);
        return -12; /* ENOMEM */
    }

    struct unix_socket *s = &g_unix_socks[idx];
    memset(s, 0, sizeof(*s));
    spin_init(&s->lock);
    s->active    = true;
    s->peer_idx  = -1;
    s->owner_pid = current_proc() ? current_proc()->pid : 0;

    spin_unlock_irqrestore(&g_unix_global_lock, f);
    return idx;
}

/* Bind a socket to a path name. */
long sys_unix_bind(int sockfd, const char *path) {
    if (sockfd < 0 || sockfd >= UNIX_SOCK_MAX) return -22;
    if (!path || !path[0]) return -22;

    struct unix_socket *s = &g_unix_socks[sockfd];
    if (!s->active) return -22;

    size_t plen = strlen(path);
    if (plen >= UNIX_PATH_MAX) return -36; /* ENAMETOOLONG */

    memcpy(s->path, path, plen);
    s->path[plen] = '\0';
    return 0;
}

/* Listen for connections. */
long sys_unix_listen(int sockfd, int backlog) {
    if (sockfd < 0 || sockfd >= UNIX_SOCK_MAX) return -22;
    (void)backlog;

    struct unix_socket *s = &g_unix_socks[sockfd];
    if (!s->active || !s->path[0]) return -22;

    s->listening = true;
    return 0;
}

/* Connect to a listening socket by path. */
long sys_unix_connect(int sockfd, const char *path) {
    if (sockfd < 0 || sockfd >= UNIX_SOCK_MAX) return -22;
    if (!path) return -22;

    struct unix_socket *client = &g_unix_socks[sockfd];
    if (!client->active) return -22;

    /* Find the listening socket */
    int server_idx = -1;
    for (int i = 0; i < UNIX_SOCK_MAX; i++) {
        if (g_unix_socks[i].active && g_unix_socks[i].listening &&
            strcmp(g_unix_socks[i].path, path) == 0) {
            server_idx = i;
            break;
        }
    }
    if (server_idx < 0) return -2; /* ENOENT / ECONNREFUSED */

    struct unix_socket *server = &g_unix_socks[server_idx];

    /* Add to pending connections */
    uint64_t f = spin_lock_irqsave(&server->lock);
    if (server->n_pending >= UNIX_BACKLOG_MAX) {
        spin_unlock_irqrestore(&server->lock, f);
        return -16; /* EBUSY */
    }
    server->pending_connections[server->n_pending++] = sockfd;

    /* Wake accept waiters */
    struct proc *w = server->accept_waiters;
    if (w) {
        server->accept_waiters = w->next_wait;
        w->next_wait = 0;
        w->state = PROC_READY;
        sched_enqueue(w);
    }
    spin_unlock_irqrestore(&server->lock, f);

    /* Wait for the server to accept (simplified: just connect immediately) */
    /* The accept call will link us */
    return 0;
}

/* Accept a connection. Returns the new connected socket id. */
long sys_unix_accept(int sockfd) {
    if (sockfd < 0 || sockfd >= UNIX_SOCK_MAX) return -22;

    struct unix_socket *server = &g_unix_socks[sockfd];
    if (!server->active || !server->listening) return -22;

    uint64_t f = spin_lock_irqsave(&server->lock);

    if (server->n_pending == 0) {
        /* Block until a connection arrives */
        struct proc *p = current_proc();
        p->state = PROC_BLOCKED;
        p->next_wait = server->accept_waiters;
        server->accept_waiters = p;
        spin_unlock_irqrestore(&server->lock, f);
        sched_yield();

        /* Re-check after wakeup */
        f = spin_lock_irqsave(&server->lock);
        if (server->n_pending == 0) {
            spin_unlock_irqrestore(&server->lock, f);
            return -4; /* EINTR */
        }
    }

    /* Pop a pending connection */
    int client_idx = server->pending_connections[0];
    server->n_pending--;
    for (int i = 0; i < server->n_pending; i++)
        server->pending_connections[i] = server->pending_connections[i + 1];

    spin_unlock_irqrestore(&server->lock, f);

    /* Allocate a new socket for the server's end */
    uint64_t gf = spin_lock_irqsave(&g_unix_global_lock);
    int new_idx = alloc_socket();
    if (new_idx < 0) {
        spin_unlock_irqrestore(&g_unix_global_lock, gf);
        return -12;
    }

    struct unix_socket *accepted = &g_unix_socks[new_idx];
    memset(accepted, 0, sizeof(*accepted));
    spin_init(&accepted->lock);
    accepted->active    = true;
    accepted->peer_idx  = client_idx;
    accepted->owner_pid = current_proc() ? current_proc()->pid : 0;

    /* Link the client to the accepted socket */
    g_unix_socks[client_idx].peer_idx = new_idx;

    spin_unlock_irqrestore(&g_unix_global_lock, gf);
    return new_idx;
}

/* Send data on a connected socket. */
long sys_unix_send(int sockfd, const void *buf, size_t len) {
    if (sockfd < 0 || sockfd >= UNIX_SOCK_MAX) return -22;
    if (!buf || len == 0) return 0;

    struct unix_socket *s = &g_unix_socks[sockfd];
    if (!s->active || s->peer_idx < 0) return -32; /* EPIPE */

    /* Write into PEER's buffer (we're sending to them) */
    struct unix_socket *peer = &g_unix_socks[s->peer_idx];
    if (!peer->active) return -32;

    uint64_t f = spin_lock_irqsave(&peer->lock);

    size_t written = 0;
    const uint8_t *src = (const uint8_t *)buf;
    while (written < len && peer->count < UNIX_BUF_SIZE) {
        peer->buf[peer->head] = src[written++];
        peer->head = (peer->head + 1) % UNIX_BUF_SIZE;
        peer->count++;
    }

    /* Wake read waiters on the peer */
    struct proc *w = peer->read_waiters;
    if (w) {
        peer->read_waiters = w->next_wait;
        w->next_wait = 0;
        w->state = PROC_READY;
        sched_enqueue(w);
    }

    spin_unlock_irqrestore(&peer->lock, f);
    return (long)written;
}

/* Receive data from a connected socket. */
long sys_unix_recv(int sockfd, void *buf, size_t len) {
    if (sockfd < 0 || sockfd >= UNIX_SOCK_MAX) return -22;
    if (!buf || len == 0) return 0;

    struct unix_socket *s = &g_unix_socks[sockfd];
    if (!s->active) return -22;

    uint64_t f = spin_lock_irqsave(&s->lock);

    /* If buffer empty and peer is still connected, block */
    while (s->count == 0) {
        if (s->peer_idx < 0 || !g_unix_socks[s->peer_idx].active) {
            spin_unlock_irqrestore(&s->lock, f);
            return 0; /* EOF: peer closed */
        }

        struct proc *p = current_proc();
        p->state = PROC_BLOCKED;
        p->next_wait = s->read_waiters;
        s->read_waiters = p;
        spin_unlock_irqrestore(&s->lock, f);
        sched_yield();

        if (signal_pending_self()) return -4; /* EINTR */
        f = spin_lock_irqsave(&s->lock);
    }

    size_t read = 0;
    uint8_t *dst = (uint8_t *)buf;
    while (read < len && s->count > 0) {
        dst[read++] = s->buf[s->tail];
        s->tail = (s->tail + 1) % UNIX_BUF_SIZE;
        s->count--;
    }

    /* Wake write waiters */
    struct proc *w = s->write_waiters;
    if (w) {
        s->write_waiters = w->next_wait;
        w->next_wait = 0;
        w->state = PROC_READY;
        sched_enqueue(w);
    }

    spin_unlock_irqrestore(&s->lock, f);
    return (long)read;
}

/* Close a Unix socket. */
long sys_unix_close(int sockfd) {
    if (sockfd < 0 || sockfd >= UNIX_SOCK_MAX) return -22;

    struct unix_socket *s = &g_unix_socks[sockfd];
    if (!s->active) return -22;

    /* Notify peer of disconnection */
    if (s->peer_idx >= 0) {
        struct unix_socket *peer = &g_unix_socks[s->peer_idx];
        peer->peer_idx = -1;
        /* Wake any blocked readers on the peer so they see EOF */
        struct proc *w = peer->read_waiters;
        while (w) {
            struct proc *next = w->next_wait;
            w->state = PROC_READY;
            w->next_wait = 0;
            sched_enqueue(w);
            w = next;
        }
        peer->read_waiters = 0;
    }

    s->active   = false;
    s->peer_idx = -1;
    return 0;
}
