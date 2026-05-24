/* wifi_mgr.c -- WiFi management kernel service for tobyOS.
 *
 * Provides scanning, connecting, disconnecting, and saved-network
 * management. Simulates WiFi on platforms without real wireless HW
 * (QEMU) by synthesising a handful of demo access points.
 */

#include <tobyos/wifi_mgr.h>
#include <tobyos/klibc.h>
#include <tobyos/printk.h>
#include <tobyos/net.h>

#define WIFI_SAVED_MAX   8
#define WIFI_SCAN_MAX   16

static struct wifi_network g_scan_results[WIFI_SCAN_MAX];
static int                 g_scan_count;

static char g_saved_ssids[WIFI_SAVED_MAX][33];
static char g_saved_pass[WIFI_SAVED_MAX][64];
static int  g_saved_count;

static struct wifi_network g_current;
static int                 g_connected;

static void populate_demo_networks(void)
{
    struct {
        const char *ssid;
        int dbm;
        int sec;
        int ch;
        uint8_t bssid[6];
    } demos[] = {
        { "HomeWiFi",       -45, 2, 6,  {0xAA,0xBB,0xCC,0x01,0x02,0x03} },
        { "OfficeNet",      -62, 2, 11, {0xDE,0xAD,0xBE,0xEF,0x00,0x01} },
        { "CoffeeShop",     -70, 0, 1,  {0x11,0x22,0x33,0x44,0x55,0x66} },
        { "SecureNet_5G",   -55, 3, 36, {0xAA,0xCC,0xDD,0xEE,0xFF,0x01} },
        { "Guest_Network",  -78, 1, 6,  {0x00,0x11,0x22,0x33,0x44,0x55} },
    };

    g_scan_count = 0;
    for (int i = 0; i < (int)(sizeof(demos)/sizeof(demos[0])); i++) {
        struct wifi_network *n = &g_scan_results[g_scan_count];
        memset(n, 0, sizeof(*n));
        int len = (int)strlen(demos[i].ssid);
        if (len > 32) len = 32;
        memcpy(n->ssid, demos[i].ssid, (size_t)len);
        n->ssid[len] = '\0';
        n->signal_dbm     = demos[i].dbm;
        n->signal_percent = 2 * (demos[i].dbm + 100);
        if (n->signal_percent > 100) n->signal_percent = 100;
        if (n->signal_percent < 0)   n->signal_percent = 0;
        n->security  = demos[i].sec;
        n->connected = 0;
        memcpy(n->bssid, demos[i].bssid, 6);
        n->channel = demos[i].ch;
        g_scan_count++;
    }
}

void wifi_mgr_init(void)
{
    memset(g_scan_results, 0, sizeof(g_scan_results));
    g_scan_count = 0;
    g_saved_count = 0;
    g_connected = 0;
    memset(&g_current, 0, sizeof(g_current));
    populate_demo_networks();
    kprintf("[wifi] wifi_mgr_init: %d demo networks available\n", g_scan_count);
}

int wifimgr_scan(struct wifi_network *results, int max_results)
{
    if (!results || max_results <= 0) return 0;
    int count = g_scan_count;
    if (count > max_results) count = max_results;
    memcpy(results, g_scan_results, (size_t)count * sizeof(struct wifi_network));
    for (int i = 0; i < count; i++)
        results[i].connected = (g_connected && strcmp(results[i].ssid, g_current.ssid) == 0);
    return count;
}

static int find_saved(const char *ssid)
{
    for (int i = 0; i < g_saved_count; i++)
        if (strcmp(g_saved_ssids[i], ssid) == 0) return i;
    return -1;
}

static int find_scan(const char *ssid)
{
    for (int i = 0; i < g_scan_count; i++)
        if (strcmp(g_scan_results[i].ssid, ssid) == 0) return i;
    return -1;
}

int wifimgr_connect(const char *ssid, const char *passphrase)
{
    if (!ssid) return -1;

    int idx = find_scan(ssid);
    if (idx < 0) {
        kprintf("[wifi] connect: network '%s' not found\n", ssid);
        return -1;
    }

    struct wifi_network *n = &g_scan_results[idx];
    if (n->security > 0 && (!passphrase || strlen(passphrase) == 0)) {
        kprintf("[wifi] connect: '%s' requires passphrase\n", ssid);
        return -1;
    }

    memcpy(&g_current, n, sizeof(g_current));
    g_current.connected = 1;
    g_connected = 1;

    /* Save network if not already saved */
    if (find_saved(ssid) < 0 && g_saved_count < WIFI_SAVED_MAX) {
        int slen = (int)strlen(ssid);
        if (slen > 32) slen = 32;
        memcpy(g_saved_ssids[g_saved_count], ssid, (size_t)slen);
        g_saved_ssids[g_saved_count][slen] = '\0';
        if (passphrase) {
            int plen = (int)strlen(passphrase);
            if (plen > 63) plen = 63;
            memcpy(g_saved_pass[g_saved_count], passphrase, (size_t)plen);
            g_saved_pass[g_saved_count][plen] = '\0';
        } else {
            g_saved_pass[g_saved_count][0] = '\0';
        }
        g_saved_count++;
    }

    kprintf("[wifi] connected to '%s' (signal %d%%)\n",
            g_current.ssid, g_current.signal_percent);
    return 0;
}

int wifimgr_disconnect(void)
{
    if (!g_connected) return -1;
    kprintf("[wifi] disconnected from '%s'\n", g_current.ssid);
    g_connected = 0;
    memset(&g_current, 0, sizeof(g_current));
    return 0;
}

int wifi_get_status(struct wifi_network *current)
{
    if (!current) return -1;
    if (!g_connected) {
        memset(current, 0, sizeof(*current));
        return -1;
    }
    memcpy(current, &g_current, sizeof(*current));
    return 0;
}

int wifi_saved_count(void) { return g_saved_count; }

int wifi_saved_get(int index, char *ssid_out, int max)
{
    if (index < 0 || index >= g_saved_count || !ssid_out || max <= 0)
        return -1;
    int len = (int)strlen(g_saved_ssids[index]);
    if (len >= max) len = max - 1;
    memcpy(ssid_out, g_saved_ssids[index], (size_t)len);
    ssid_out[len] = '\0';
    return 0;
}

int wifi_forget(const char *ssid)
{
    int idx = find_saved(ssid);
    if (idx < 0) return -1;

    if (g_connected && strcmp(g_current.ssid, ssid) == 0)
        wifi_disconnect();

    for (int i = idx; i < g_saved_count - 1; i++) {
        memcpy(g_saved_ssids[i], g_saved_ssids[i + 1], 33);
        memcpy(g_saved_pass[i], g_saved_pass[i + 1], 64);
    }
    g_saved_count--;
    return 0;
}
