/* msgbus.c -- Milestone 7 system message bus.
 *
 * D-Bus-like pub/sub message passing. Processes subscribe to named
 * topics and poll for messages. The kernel can also publish events
 * internally (network changes, battery, volume, etc).
 */

#include <tobyos/msgbus.h>
#include <tobyos/klibc.h>
#include <tobyos/printk.h>
#include <tobyos/pit.h>
#include <tobyos/proc.h>

struct subscription {
    int  pid;
    char topic[MSG_TOPIC_MAX];
    int  active;
};

struct msg_queue_entry {
    struct bus_message msg;
    int valid;
};

struct per_proc_queue {
    int  pid;
    struct msg_queue_entry queue[MSGBUS_QUEUE_DEPTH];
    int  head;
    int  tail;
    int  count;
};

static struct {
    struct subscription    subs[MSGBUS_MAX_SUBS];
    struct per_proc_queue  queues[MSGBUS_MAX_SUBS];
    int                    num_subs;
    bool                   ready;
} g_bus;

static void bus_strncpy(char *dst, const char *src, size_t cap) {
    if (!src) { dst[0] = '\0'; return; }
    size_t i = 0;
    for (; i + 1 < cap && src[i]; i++) dst[i] = src[i];
    dst[i] = '\0';
}

static int bus_streq(const char *a, const char *b) {
    while (*a && *b) {
        if (*a != *b) return 0;
        a++; b++;
    }
    return *a == *b;
}

void msgbus_init(void) {
    memset(&g_bus, 0, sizeof(g_bus));
    g_bus.ready = true;
    kprintf("[msgbus] init: %d topics, %d subscribers max\n",
            MSGBUS_MAX_TOPICS, MSGBUS_MAX_SUBS);
}

static struct per_proc_queue *find_queue(int pid) __attribute__((unused));
static struct per_proc_queue *find_queue(int pid) {
    for (int i = 0; i < MSGBUS_MAX_SUBS; i++) {
        if (g_bus.queues[i].pid == pid && g_bus.subs[i].active)
            return &g_bus.queues[i];
    }
    return NULL;
}

static int topic_matches(const char *sub_topic, const char *msg_topic) {
    if (sub_topic[0] == '*' && sub_topic[1] == '\0')
        return 1;
    return bus_streq(sub_topic, msg_topic);
}

int msgbus_subscribe(const char *topic) {
    if (!g_bus.ready || !topic) return -1;

    struct proc *cur = current_proc();
    int pid = cur ? cur->pid : 0;

    for (int i = 0; i < MSGBUS_MAX_SUBS; i++) {
        if (g_bus.subs[i].active && g_bus.subs[i].pid == pid &&
            bus_streq(g_bus.subs[i].topic, topic))
            return 0; /* already subscribed */
    }

    for (int i = 0; i < MSGBUS_MAX_SUBS; i++) {
        if (!g_bus.subs[i].active) {
            g_bus.subs[i].active = 1;
            g_bus.subs[i].pid = pid;
            bus_strncpy(g_bus.subs[i].topic, topic, MSG_TOPIC_MAX);

            g_bus.queues[i].pid = pid;
            g_bus.queues[i].head = 0;
            g_bus.queues[i].tail = 0;
            g_bus.queues[i].count = 0;
            g_bus.num_subs++;
            return 0;
        }
    }
    return -1; /* full */
}

int msgbus_unsubscribe(const char *topic) {
    if (!g_bus.ready || !topic) return -1;

    struct proc *cur = current_proc();
    int pid = cur ? cur->pid : 0;

    for (int i = 0; i < MSGBUS_MAX_SUBS; i++) {
        if (g_bus.subs[i].active && g_bus.subs[i].pid == pid &&
            bus_streq(g_bus.subs[i].topic, topic)) {
            g_bus.subs[i].active = 0;
            g_bus.num_subs--;
            return 0;
        }
    }
    return -1;
}

static void enqueue_msg(int sub_idx, const struct bus_message *msg) {
    struct per_proc_queue *q = &g_bus.queues[sub_idx];
    int slot = q->head;
    q->queue[slot].msg = *msg;
    q->queue[slot].valid = 1;
    q->head = (q->head + 1) % MSGBUS_QUEUE_DEPTH;
    if (q->count < MSGBUS_QUEUE_DEPTH)
        q->count++;
    else
        q->tail = (q->tail + 1) % MSGBUS_QUEUE_DEPTH; /* overwrite oldest */
}

int msgbus_publish(const char *topic, const char *payload) {
    if (!g_bus.ready || !topic) return -1;

    struct proc *cur = current_proc();
    int pid = cur ? cur->pid : 0;

    struct bus_message msg;
    memset(&msg, 0, sizeof(msg));
    bus_strncpy(msg.topic, topic, MSG_TOPIC_MAX);
    bus_strncpy(msg.payload, payload ? payload : "", MSG_PAYLOAD_MAX);
    msg.sender_pid = pid;
    msg.timestamp  = (uint32_t)pit_ticks();

    int delivered = 0;
    for (int i = 0; i < MSGBUS_MAX_SUBS; i++) {
        if (g_bus.subs[i].active && topic_matches(g_bus.subs[i].topic, topic)) {
            enqueue_msg(i, &msg);
            delivered++;
        }
    }
    return delivered;
}

int msgbus_poll(struct bus_message *out) {
    if (!g_bus.ready || !out) return -1;

    struct proc *cur = current_proc();
    int pid = cur ? cur->pid : 0;

    for (int i = 0; i < MSGBUS_MAX_SUBS; i++) {
        if (g_bus.subs[i].active && g_bus.subs[i].pid == pid) {
            struct per_proc_queue *q = &g_bus.queues[i];
            if (q->count > 0) {
                *out = q->queue[q->tail].msg;
                q->queue[q->tail].valid = 0;
                q->tail = (q->tail + 1) % MSGBUS_QUEUE_DEPTH;
                q->count--;
                return 0;
            }
        }
    }
    return -1; /* no messages */
}

void msgbus_emit(const char *topic, const char *payload) {
    if (!g_bus.ready) return;

    struct bus_message msg;
    memset(&msg, 0, sizeof(msg));
    bus_strncpy(msg.topic, topic, MSG_TOPIC_MAX);
    bus_strncpy(msg.payload, payload ? payload : "", MSG_PAYLOAD_MAX);
    msg.sender_pid = -1; /* kernel */
    msg.timestamp  = (uint32_t)pit_ticks();

    for (int i = 0; i < MSGBUS_MAX_SUBS; i++) {
        if (g_bus.subs[i].active && topic_matches(g_bus.subs[i].topic, topic)) {
            enqueue_msg(i, &msg);
        }
    }
}
