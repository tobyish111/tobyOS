/* msgbus.h -- Milestone 7 system message bus (D-Bus-like IPC).
 *
 * Pub/sub message passing between kernel subsystems and userspace.
 * Processes subscribe to named topics, then poll for messages.
 */

#ifndef TOBYOS_MSGBUS_H
#define TOBYOS_MSGBUS_H

#include <tobyos/types.h>
#include <stdint.h>

#define MSG_TOPIC_MAX    32
#define MSG_PAYLOAD_MAX  256
#define MSGBUS_MAX_TOPICS     32
#define MSGBUS_MAX_SUBS       64
#define MSGBUS_QUEUE_DEPTH    16

struct bus_message {
    char     topic[MSG_TOPIC_MAX];
    char     payload[MSG_PAYLOAD_MAX];
    int      sender_pid;
    uint32_t timestamp;
};

void msgbus_init(void);
int  msgbus_subscribe(const char *topic);
int  msgbus_unsubscribe(const char *topic);
int  msgbus_publish(const char *topic, const char *payload);
int  msgbus_poll(struct bus_message *out);

/* Kernel-internal convenience for subsystem events */
void msgbus_emit(const char *topic, const char *payload);

#endif /* TOBYOS_MSGBUS_H */
