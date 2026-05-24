#ifndef TOBYOS_FIREWALL_H
#define TOBYOS_FIREWALL_H

#include <tobyos/types.h>

enum fw_action    { FW_ALLOW, FW_DENY, FW_LOG };
enum fw_direction { FW_IN, FW_OUT, FW_BOTH };

struct fw_rule {
    int               id;
    enum fw_action    action;
    enum fw_direction direction;
    uint32_t          src_ip;       /* 0 = any */
    uint32_t          dst_ip;       /* 0 = any */
    uint16_t          src_port;     /* 0 = any */
    uint16_t          dst_port;     /* 0 = any */
    uint8_t           protocol;     /* 0=any, 6=TCP, 17=UDP */
    int               enabled;
};

void firewall_init(void);
int  firewall_add_rule(const struct fw_rule *rule);
int  firewall_remove_rule(int rule_id);
int  firewall_check_packet(uint32_t src_ip, uint32_t dst_ip,
                           uint16_t src_port, uint16_t dst_port,
                           uint8_t proto, enum fw_direction dir);
int  firewall_get_rules(struct fw_rule *out, int max);
int  firewall_enable(int enabled);

#endif /* TOBYOS_FIREWALL_H */
