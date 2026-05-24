/* firewall.c -- basic packet filter for tobyOS.
 *
 * Maintains an ordered rule list checked linearly on each packet.
 * Default policy is ALLOW when no rule matches or the firewall is
 * disabled.
 */

#include <tobyos/firewall.h>
#include <tobyos/klibc.h>
#include <tobyos/printk.h>

#define FW_RULE_MAX  64

static struct fw_rule g_rules[FW_RULE_MAX];
static int            g_rule_count;
static int            g_next_id = 1;
static int            g_enabled;

void firewall_init(void)
{
    memset(g_rules, 0, sizeof(g_rules));
    g_rule_count = 0;
    g_next_id    = 1;
    g_enabled    = 0;
    kprintf("[firewall] initialised (disabled by default)\n");
}

int firewall_add_rule(const struct fw_rule *rule)
{
    if (!rule || g_rule_count >= FW_RULE_MAX) return -1;

    struct fw_rule *r = &g_rules[g_rule_count];
    memcpy(r, rule, sizeof(*r));
    r->id = g_next_id++;
    r->enabled = 1;
    g_rule_count++;

    kprintf("[firewall] added rule %d (action=%d dir=%d proto=%d)\n",
            r->id, r->action, r->direction, r->protocol);
    return r->id;
}

int firewall_remove_rule(int rule_id)
{
    for (int i = 0; i < g_rule_count; i++) {
        if (g_rules[i].id == rule_id) {
            for (int j = i; j < g_rule_count - 1; j++)
                g_rules[j] = g_rules[j + 1];
            g_rule_count--;
            kprintf("[firewall] removed rule %d\n", rule_id);
            return 0;
        }
    }
    return -1;
}

int firewall_check_packet(uint32_t src_ip, uint32_t dst_ip,
                          uint16_t src_port, uint16_t dst_port,
                          uint8_t proto, enum fw_direction dir)
{
    if (!g_enabled) return 1; /* allow everything when disabled */

    for (int i = 0; i < g_rule_count; i++) {
        const struct fw_rule *r = &g_rules[i];
        if (!r->enabled) continue;

        if (r->direction != FW_BOTH && r->direction != dir)
            continue;
        if (r->protocol != 0 && r->protocol != proto)
            continue;
        if (r->src_ip != 0 && r->src_ip != src_ip)
            continue;
        if (r->dst_ip != 0 && r->dst_ip != dst_ip)
            continue;
        if (r->src_port != 0 && r->src_port != src_port)
            continue;
        if (r->dst_port != 0 && r->dst_port != dst_port)
            continue;

        if (r->action == FW_LOG) {
            kprintf("[firewall] LOG: %u.%u.%u.%u:%u -> %u.%u.%u.%u:%u proto=%u\n",
                    src_ip & 0xFF, (src_ip >> 8) & 0xFF,
                    (src_ip >> 16) & 0xFF, (src_ip >> 24) & 0xFF,
                    src_port,
                    dst_ip & 0xFF, (dst_ip >> 8) & 0xFF,
                    (dst_ip >> 16) & 0xFF, (dst_ip >> 24) & 0xFF,
                    dst_port, proto);
            continue;
        }

        return (r->action == FW_ALLOW) ? 1 : 0;
    }

    return 1; /* default allow */
}

int firewall_get_rules(struct fw_rule *out, int max)
{
    if (!out || max <= 0) return 0;
    int count = g_rule_count;
    if (count > max) count = max;
    memcpy(out, g_rules, (size_t)count * sizeof(struct fw_rule));
    return count;
}

int firewall_enable(int enabled)
{
    g_enabled = enabled ? 1 : 0;
    kprintf("[firewall] %s\n", g_enabled ? "enabled" : "disabled");
    return 0;
}
