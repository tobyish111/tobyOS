#ifndef TOBYOS_WIFI_MGR_H
#define TOBYOS_WIFI_MGR_H

#include <tobyos/types.h>

struct wifi_network {
    char ssid[33];
    int signal_dbm;
    int signal_percent;
    int security;       /* 0=open, 1=WEP, 2=WPA2, 3=WPA3 */
    int connected;
    uint8_t bssid[6];
    int channel;
};

void wifi_mgr_init(void);
int  wifi_scan(struct wifi_network *results, int max_results);
int  wifi_connect(const char *ssid, const char *passphrase);
int  wifi_disconnect(void);
int  wifi_get_status(struct wifi_network *current);
int  wifi_saved_count(void);
int  wifi_saved_get(int index, char *ssid_out, int max);
int  wifi_forget(const char *ssid);

#endif /* TOBYOS_WIFI_MGR_H */
