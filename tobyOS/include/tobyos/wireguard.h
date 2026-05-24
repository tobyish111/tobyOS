#ifndef TOBYOS_WIREGUARD_H
#define TOBYOS_WIREGUARD_H

#include <tobyos/types.h>

struct wg_peer {
    uint8_t  public_key[32];
    uint8_t  preshared_key[32];
    uint32_t endpoint_ip;
    uint16_t endpoint_port;
    uint32_t allowed_ip;
    uint32_t allowed_mask;
    int      keepalive_seconds;
};

struct wg_interface {
    uint8_t        private_key[32];
    uint8_t        public_key[32];
    uint16_t       listen_port;
    uint32_t       local_ip;
    struct wg_peer peers[4];
    int            peer_count;
    int            active;
};

void wireguard_init(void);
int  wg_create_interface(struct wg_interface *iface);
int  wg_destroy_interface(void);
int  wg_add_peer(const struct wg_peer *peer);
int  wg_send(const void *packet, size_t len);
int  wg_recv(void *buf, size_t max);

#endif /* TOBYOS_WIREGUARD_H */
