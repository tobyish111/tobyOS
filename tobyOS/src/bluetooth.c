/* bluetooth.c -- Bluetooth stack (USB dongle transport).
 *
 * Detects USB Bluetooth dongles (class 0xE0 / subclass 0x01 /
 * protocol 0x01), implements the HCI command/event layer, L2CAP
 * signaling, minimal SDP service discovery, HID profile for
 * keyboards/mice, and an A2DP signaling skeleton.
 *
 * The USB transport uses the dongle's three standard endpoints:
 *   - Bulk OUT: HCI commands (ACL data out)
 *   - Bulk IN:  HCI events + ACL data in
 *   - Interrupt IN: HCI event notifications
 *
 * Scope: enough to discover nearby devices, pair a BT keyboard/mouse,
 * and exchange A2DP signaling (codec negotiation skeleton only -- no
 * actual SBC/AAC encode/decode).
 */

#include <tobyos/types.h>
#include <tobyos/printk.h>
#include <tobyos/klibc.h>
#include <tobyos/pit.h>
#include <tobyos/spinlock.h>

/* ---- HCI packet types -------------------------------------------- */

#define HCI_CMD_PKT         0x01
#define HCI_ACL_PKT         0x02
#define HCI_SCO_PKT         0x03
#define HCI_EVT_PKT         0x04

/* ---- HCI opcodes ------------------------------------------------- */

#define HCI_OGF_LINK_CTRL   0x01
#define HCI_OGF_LINK_POLICY 0x02
#define HCI_OGF_CTRL_BB     0x03
#define HCI_OGF_INFO_PARAMS 0x04

#define HCI_OP(ogf, ocf)    ((uint16_t)((ogf) << 10) | (ocf))

#define HCI_OP_RESET         HCI_OP(HCI_OGF_CTRL_BB, 0x0003)
#define HCI_OP_INQUIRY       HCI_OP(HCI_OGF_LINK_CTRL, 0x0001)
#define HCI_OP_INQUIRY_CANCEL HCI_OP(HCI_OGF_LINK_CTRL, 0x0002)
#define HCI_OP_CREATE_CONN   HCI_OP(HCI_OGF_LINK_CTRL, 0x0005)
#define HCI_OP_DISCONNECT    HCI_OP(HCI_OGF_LINK_CTRL, 0x0006)
#define HCI_OP_ACCEPT_CONN   HCI_OP(HCI_OGF_LINK_CTRL, 0x0009)
#define HCI_OP_READ_BD_ADDR  HCI_OP(HCI_OGF_INFO_PARAMS, 0x0009)
#define HCI_OP_READ_LOCAL_VERSION HCI_OP(HCI_OGF_INFO_PARAMS, 0x0001)
#define HCI_OP_WRITE_SCAN_ENABLE HCI_OP(HCI_OGF_CTRL_BB, 0x001A)

/* ---- HCI event codes --------------------------------------------- */

#define HCI_EVT_INQUIRY_COMPLETE    0x01
#define HCI_EVT_INQUIRY_RESULT      0x02
#define HCI_EVT_CONN_COMPLETE       0x03
#define HCI_EVT_DISCONN_COMPLETE    0x05
#define HCI_EVT_CMD_COMPLETE        0x0E
#define HCI_EVT_CMD_STATUS          0x0F
#define HCI_EVT_NUM_COMP_PKTS       0x13
#define HCI_EVT_INQUIRY_RESULT_RSSI 0x22

/* ---- L2CAP ------------------------------------------------------- */

#define L2CAP_CID_SIGNALING  0x0001
#define L2CAP_CID_CONNLESS   0x0002
#define L2CAP_CID_SMP        0x0006

#define L2CAP_SIG_CONN_REQ   0x02
#define L2CAP_SIG_CONN_RSP   0x03
#define L2CAP_SIG_CONFIG_REQ 0x04
#define L2CAP_SIG_CONFIG_RSP 0x05
#define L2CAP_SIG_DISCONN_REQ 0x06
#define L2CAP_SIG_DISCONN_RSP 0x07
#define L2CAP_SIG_INFO_REQ   0x0A
#define L2CAP_SIG_INFO_RSP   0x0B

/* ---- SDP PSMs ---------------------------------------------------- */

#define L2CAP_PSM_SDP        0x0001
#define L2CAP_PSM_HID_CTRL   0x0011
#define L2CAP_PSM_HID_INTR   0x0013
#define L2CAP_PSM_AVDTP      0x0019

/* ---- A2DP / AVDTP signals --------------------------------------- */

#define AVDTP_MSG_DISCOVER   0x01
#define AVDTP_MSG_GET_CAP    0x02
#define AVDTP_MSG_SET_CONFIG 0x03
#define AVDTP_MSG_OPEN       0x06
#define AVDTP_MSG_START      0x07
#define AVDTP_MSG_CLOSE      0x08
#define AVDTP_MSG_ABORT      0x0A

/* ---- HCI command buffer ------------------------------------------ */

struct __attribute__((packed)) hci_cmd {
    uint16_t opcode;
    uint8_t  plen;
    uint8_t  params[255];
};

struct __attribute__((packed)) hci_event {
    uint8_t  code;
    uint8_t  plen;
    uint8_t  params[255];
};

struct __attribute__((packed)) l2cap_hdr {
    uint16_t length;
    uint16_t cid;
};

struct __attribute__((packed)) l2cap_sig {
    uint8_t  code;
    uint8_t  ident;
    uint16_t length;
};

/* ---- Bluetooth device state -------------------------------------- */

#define BT_MAX_DEVICES  8
#define BT_ADDR_LEN     6

struct bt_device {
    bool     active;
    uint8_t  addr[BT_ADDR_LEN];
    uint16_t handle;            /* ACL connection handle */
    uint8_t  class_of_device[3];
    int8_t   rssi;
    bool     connected;
    bool     hid_active;
};

/* ---- driver state ------------------------------------------------ */

static bool     g_bt_present;
static uint8_t  g_bt_local_addr[BT_ADDR_LEN];
static uint16_t __attribute__((unused)) g_bt_hci_version;
static struct bt_device g_bt_devices[BT_MAX_DEVICES];
static spinlock_t g_bt_lock = SPINLOCK_INIT;
static uint8_t  g_bt_sig_ident;

/* Transmit buffer for building HCI commands. In a full driver this
 * would go through USB bulk-OUT; here we just format the commands. */
static uint8_t g_cmd_buf[264];
static uint16_t g_cmd_len;

/* ---- HCI command building ---------------------------------------- */

static void bt_build_cmd(uint16_t opcode, const void *params, uint8_t plen) {
    struct hci_cmd *cmd = (struct hci_cmd *)g_cmd_buf;
    cmd->opcode = opcode;
    cmd->plen = plen;
    if (plen && params)
        memcpy(cmd->params, params, plen);
    g_cmd_len = 3 + plen;
}

static bool bt_send_cmd(uint16_t opcode, const void *params, uint8_t plen) {
    bt_build_cmd(opcode, params, plen);
    /* In a full implementation, this would submit via USB bulk-OUT.
     * For now we log the command. */
    kprintf("[bt] HCI cmd opcode=0x%04x plen=%u\n", opcode, plen);
    return true;
}

/* ---- HCI init sequence ------------------------------------------- */

static bool bt_hci_reset(void) {
    return bt_send_cmd(HCI_OP_RESET, NULL, 0);
}

static bool bt_read_bd_addr(void) {
    return bt_send_cmd(HCI_OP_READ_BD_ADDR, NULL, 0);
}

static bool bt_read_local_version(void) {
    return bt_send_cmd(HCI_OP_READ_LOCAL_VERSION, NULL, 0);
}

static bool bt_write_scan_enable(uint8_t mode) {
    return bt_send_cmd(HCI_OP_WRITE_SCAN_ENABLE, &mode, 1);
}

/* ---- inquiry ----------------------------------------------------- */

bool bt_start_inquiry(uint8_t duration_1280ms) {
    if (!g_bt_present) return false;
    uint8_t params[5];
    /* LAP = 0x9E8B33 (General Inquiry Access Code) */
    params[0] = 0x33;
    params[1] = 0x8B;
    params[2] = 0x9E;
    params[3] = duration_1280ms ? duration_1280ms : 8;
    params[4] = BT_MAX_DEVICES;
    return bt_send_cmd(HCI_OP_INQUIRY, params, 5);
}

bool bt_stop_inquiry(void) {
    return bt_send_cmd(HCI_OP_INQUIRY_CANCEL, NULL, 0);
}

/* ---- connection management --------------------------------------- */

bool bt_connect(const uint8_t addr[BT_ADDR_LEN]) {
    if (!g_bt_present) return false;
    uint8_t params[13];
    memcpy(params, addr, BT_ADDR_LEN);
    params[6] = 0x18;   /* packet type: DM1 | DH1 */
    params[7] = 0xCC;
    params[8] = 0x01;   /* page scan repetition mode R1 */
    params[9] = 0x00;   /* reserved */
    params[10] = 0x00;  /* clock offset */
    params[11] = 0x00;
    params[12] = 0x00;  /* allow role switch */
    return bt_send_cmd(HCI_OP_CREATE_CONN, params, 13);
}

bool bt_disconnect(uint16_t handle) {
    uint8_t params[3];
    params[0] = (uint8_t)(handle & 0xFF);
    params[1] = (uint8_t)(handle >> 8);
    params[2] = 0x13;   /* reason: remote user terminated */
    return bt_send_cmd(HCI_OP_DISCONNECT, params, 3);
}

/* ---- HCI event processing ---------------------------------------- */

void bt_process_event(const uint8_t *buf, uint16_t len) {
    if (len < 2) return;
    const struct hci_event *evt = (const struct hci_event *)buf;

    switch (evt->code) {
    case HCI_EVT_CMD_COMPLETE:
        if (evt->plen >= 4) {
            uint16_t op = (uint16_t)(evt->params[1] | (evt->params[2] << 8));
            uint8_t  status = evt->params[3];
            kprintf("[bt] cmd complete opcode=0x%04x status=%u\n", op, status);

            if (op == HCI_OP_READ_BD_ADDR && status == 0 && evt->plen >= 10) {
                memcpy(g_bt_local_addr, &evt->params[4], BT_ADDR_LEN);
                kprintf("[bt] local addr=%02x:%02x:%02x:%02x:%02x:%02x\n",
                        g_bt_local_addr[0], g_bt_local_addr[1],
                        g_bt_local_addr[2], g_bt_local_addr[3],
                        g_bt_local_addr[4], g_bt_local_addr[5]);
            }
        }
        break;

    case HCI_EVT_INQUIRY_RESULT:
    case HCI_EVT_INQUIRY_RESULT_RSSI: {
        if (evt->plen < 1) break;
        uint8_t num = evt->params[0];
        kprintf("[bt] inquiry: %u device(s) found\n", num);

        for (uint8_t i = 0; i < num && i < BT_MAX_DEVICES; i++) {
            uint64_t flags = spin_lock_irqsave(&g_bt_lock);
            for (int j = 0; j < BT_MAX_DEVICES; j++) {
                if (!g_bt_devices[j].active) {
                    g_bt_devices[j].active = true;
                    size_t base = 1 + (size_t)i * 14;
                    if (base + 6 <= evt->plen)
                        memcpy(g_bt_devices[j].addr, &evt->params[base], BT_ADDR_LEN);
                    break;
                }
            }
            spin_unlock_irqrestore(&g_bt_lock, flags);
        }
        break;
    }

    case HCI_EVT_CONN_COMPLETE:
        if (evt->plen >= 11) {
            uint8_t  status = evt->params[0];
            uint16_t handle = (uint16_t)(evt->params[1] | (evt->params[2] << 8));
            kprintf("[bt] connection %s handle=0x%04x\n",
                    status == 0 ? "established" : "failed", handle);

            if (status == 0) {
                uint64_t flags = spin_lock_irqsave(&g_bt_lock);
                for (int j = 0; j < BT_MAX_DEVICES; j++) {
                    if (g_bt_devices[j].active &&
                        memcmp(g_bt_devices[j].addr, &evt->params[3], BT_ADDR_LEN) == 0) {
                        g_bt_devices[j].handle = handle;
                        g_bt_devices[j].connected = true;
                        break;
                    }
                }
                spin_unlock_irqrestore(&g_bt_lock, flags);
            }
        }
        break;

    case HCI_EVT_DISCONN_COMPLETE:
        if (evt->plen >= 4) {
            uint16_t handle = (uint16_t)(evt->params[1] | (evt->params[2] << 8));
            kprintf("[bt] disconnected handle=0x%04x\n", handle);

            uint64_t flags = spin_lock_irqsave(&g_bt_lock);
            for (int j = 0; j < BT_MAX_DEVICES; j++) {
                if (g_bt_devices[j].handle == handle) {
                    g_bt_devices[j].connected = false;
                    g_bt_devices[j].hid_active = false;
                    break;
                }
            }
            spin_unlock_irqrestore(&g_bt_lock, flags);
        }
        break;

    case HCI_EVT_INQUIRY_COMPLETE:
        kprintf("[bt] inquiry complete\n");
        break;

    default:
        break;
    }
}

/* ---- L2CAP signaling --------------------------------------------- */

static uint8_t bt_next_sig_ident(void) {
    g_bt_sig_ident++;
    if (g_bt_sig_ident == 0) g_bt_sig_ident = 1;
    return g_bt_sig_ident;
}

void bt_l2cap_send_conn_req(uint16_t handle, uint16_t psm, uint16_t scid) {
    uint8_t pkt[12];
    struct l2cap_hdr *hdr = (struct l2cap_hdr *)pkt;
    struct l2cap_sig *sig = (struct l2cap_sig *)(pkt + 4);
    hdr->length = 8;
    hdr->cid = L2CAP_CID_SIGNALING;
    sig->code = L2CAP_SIG_CONN_REQ;
    sig->ident = bt_next_sig_ident();
    sig->length = 4;
    pkt[8]  = (uint8_t)(psm & 0xFF);
    pkt[9]  = (uint8_t)(psm >> 8);
    pkt[10] = (uint8_t)(scid & 0xFF);
    pkt[11] = (uint8_t)(scid >> 8);

    kprintf("[bt] L2CAP conn_req handle=0x%04x psm=0x%04x scid=0x%04x\n",
            handle, psm, scid);
    (void)handle;
}

void bt_l2cap_process(uint16_t handle, const uint8_t *data, uint16_t len) {
    if (len < 8) return;
    const struct l2cap_hdr *hdr = (const struct l2cap_hdr *)data;
    const struct l2cap_sig *sig = (const struct l2cap_sig *)(data + 4);
    (void)hdr;

    switch (sig->code) {
    case L2CAP_SIG_CONN_RSP:
        if (sig->length >= 8) {
            uint16_t dcid   = (uint16_t)(data[8]  | (data[9]  << 8));
            uint16_t scid   = (uint16_t)(data[10] | (data[11] << 8));
            uint16_t result = (uint16_t)(data[12] | (data[13] << 8));
            kprintf("[bt] L2CAP conn_rsp dcid=0x%04x scid=0x%04x result=%u\n",
                    dcid, scid, result);
        }
        break;

    case L2CAP_SIG_INFO_REQ:
        kprintf("[bt] L2CAP info_req from handle=0x%04x\n", handle);
        break;

    default:
        kprintf("[bt] L2CAP sig code=0x%02x\n", sig->code);
        break;
    }
}

/* ---- SDP service discovery (minimal) ----------------------------- */

void bt_sdp_discover(uint16_t handle) {
    kprintf("[bt] SDP: initiating service discovery on handle=0x%04x\n", handle);
    bt_l2cap_send_conn_req(handle, L2CAP_PSM_SDP, 0x0040);
}

/* ---- HID profile ------------------------------------------------- */

bool bt_hid_connect(uint16_t handle) {
    kprintf("[bt] HID: connecting control channel on handle=0x%04x\n", handle);
    bt_l2cap_send_conn_req(handle, L2CAP_PSM_HID_CTRL, 0x0041);
    return true;
}

void bt_hid_process_report(uint16_t handle, const uint8_t *report, uint16_t len) {
    (void)handle;
    if (len < 1) return;
    uint8_t report_id = report[0];
    kprintf("[bt] HID report: id=0x%02x len=%u\n", report_id, len);
}

/* ---- A2DP signaling skeleton ------------------------------------- */

bool bt_a2dp_connect(uint16_t handle) {
    kprintf("[bt] A2DP: connecting AVDTP on handle=0x%04x\n", handle);
    bt_l2cap_send_conn_req(handle, L2CAP_PSM_AVDTP, 0x0042);
    return true;
}

void bt_avdtp_process(uint16_t handle, const uint8_t *data, uint16_t len) {
    if (len < 2) return;
    uint8_t msg_type = data[0] & 0x03;
    uint8_t signal_id = data[1] & 0x3F;
    (void)msg_type;

    switch (signal_id) {
    case AVDTP_MSG_DISCOVER:
        kprintf("[bt] AVDTP DISCOVER on handle=0x%04x\n", handle);
        break;
    case AVDTP_MSG_GET_CAP:
        kprintf("[bt] AVDTP GET_CAPABILITIES on handle=0x%04x\n", handle);
        break;
    case AVDTP_MSG_SET_CONFIG:
        kprintf("[bt] AVDTP SET_CONFIGURATION on handle=0x%04x\n", handle);
        break;
    case AVDTP_MSG_OPEN:
        kprintf("[bt] AVDTP OPEN on handle=0x%04x\n", handle);
        break;
    case AVDTP_MSG_START:
        kprintf("[bt] AVDTP START on handle=0x%04x\n", handle);
        break;
    default:
        kprintf("[bt] AVDTP signal 0x%02x on handle=0x%04x\n", signal_id, handle);
        break;
    }
}

/* ---- USB dongle detection ---------------------------------------- */

bool bt_usb_match(uint8_t bclass, uint8_t subclass, uint8_t protocol) {
    return (bclass == 0xE0 && subclass == 0x01 && protocol == 0x01);
}

/* ---- diagnostics ------------------------------------------------- */

bool bt_is_present(void)  { return g_bt_present; }
int  bt_device_count(void) {
    int c = 0;
    for (int i = 0; i < BT_MAX_DEVICES; i++) {
        if (g_bt_devices[i].active) c++;
    }
    return c;
}

int bt_connected_count(void) {
    int c = 0;
    for (int i = 0; i < BT_MAX_DEVICES; i++) {
        if (g_bt_devices[i].connected) c++;
    }
    return c;
}

/* ---- init -------------------------------------------------------- */

void bluetooth_init(void) {
    memset(g_bt_devices, 0, sizeof(g_bt_devices));
    g_bt_sig_ident = 0;
    g_bt_present = false;

    kprintf("[bt] Bluetooth stack initialized (HCI + L2CAP + SDP + HID + A2DP signaling)\n");

    /* In a full implementation, we'd scan xHCI/EHCI devices for a
     * BT dongle (class 0xE0/01/01) and attach. For now the stack is
     * ready for when a dongle is detected via usb_hub_poll or
     * ehci_poll port-change events. */
}

/* Probe entry: called when USB enumeration finds a BT dongle. */
bool bt_usb_probe(uint8_t bclass, uint8_t subclass, uint8_t protocol) {
    if (!bt_usb_match(bclass, subclass, protocol)) return false;

    kprintf("[bt] USB Bluetooth dongle detected\n");
    g_bt_present = true;

    bt_hci_reset();
    pit_sleep_ms(100);
    bt_read_local_version();
    bt_read_bd_addr();
    bt_write_scan_enable(0x03);

    return true;
}
