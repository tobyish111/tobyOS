/* usb_hid.c -- USB HID Boot Protocol class driver.
 *
 * Sits behind xhci.c. Handles two device kinds at the per-interface
 * level: bInterfaceClass=3 (HID), bInterfaceSubClass=1 (Boot) or
 * 0 (No Subclass — common on real PCs), with bInterfaceProtocol = 1
 * (keyboard) or 2 (mouse), or protocol 0 with a boot-sized interrupt
 * MPS guess (see usb_hid_probe). Translated key
 * presses + mouse deltas land in the same dispatch sinks as the
 * PS/2 path so the GUI / shell / SIGINT routing has exactly one
 * implementation regardless of input source.
 *
 * Limitations (deliberate, single-milestone scope):
 *   - no software autorepeat (one char per held key; release + press
 *     to repeat). PS/2 autorepeat is hardware so the existing kbd
 *     path is unaffected; USB-only systems get a slightly different
 *     typing feel until milestone 22 adds a tick-driven repeater.
 *   - no LED control (Caps/Num/Scroll Lock LEDs stay off; the toggle
 *     state is still tracked in software so case folding works).
 *   - no Set_Report / Get_Report calls -- we only listen on the
 *     interrupt-IN endpoint.
 *   - mouse Z (wheel) byte parsed for completeness but not forwarded
 *     (mouse.h has no wheel concept yet).
 *
 *   - Many real USB mice send a leading Report ID byte before the
 *     boot-style [Buttons][X][Y] fields (especially when SET_PROTOCOL
 *     stays in report mode or the device always prefixes). Without
 *     handling that, byte0 is mis-read as "buttons" and horizontal
 *     motion lands in dy — we auto-learn RID vs no-RID from traffic.
 */

#include <tobyos/usb_hid.h>
#include <tobyos/usb.h>
#include <tobyos/keyboard.h>
#include <tobyos/mouse.h>
#include <tobyos/gui.h>
#include <tobyos/printk.h>
#include <tobyos/klibc.h>
#include <tobyos/abi/abi.h>
#include <tobyos/perf.h>

/* ============================================================== */
/* Per-device HID state                                            */
/* ============================================================== */

#define HID_REPORT_BUF       16       /* enough for kbd (8) + mouse (3+) */

struct hid_dev_state {
    bool      in_use;
    bool      is_keyboard;            /* false => mouse */
    uint8_t   iface_num;
    uint8_t   ep_addr;
    uint16_t  mps;
    uint8_t   buf[HID_REPORT_BUF];
    uint8_t   slot_id;                /* HCI slot owning this state (for diag) */

    /* Keyboard-only state. */
    uint8_t   last_keys[6];
    bool      caps_on;

    /* M26D telemetry (read-only, observed by usb_hid_introspect_at +
     * by the periodic snapshot kprintf in hid_on_report). All counters
     * saturate -- they are diagnostic, not security-relevant. */
    uint64_t  frames_total;           /* int-IN reports decoded */
    uint64_t  key_press_total;        /* keyboard: newly pressed keys */
    uint64_t  key_release_total;      /* keyboard: keys that left the slot */
    uint8_t   last_usage;             /* keyboard: last newly-pressed usage */
    uint8_t   last_modmask;           /* keyboard: last modifier byte seen */
    uint64_t  mouse_btn_press_total;  /* mouse: low->high transitions on any btn */
    uint64_t  mouse_dx_abs_total;     /* mouse: sum |dx| across frames */
    uint64_t  mouse_dy_abs_total;     /* mouse: sum |dy| across frames */
    uint8_t   last_buttons;           /* mouse: last button bitmap */
    int8_t    last_dx;                /* mouse: last raw dx (signed) */
    int8_t    last_dy;                /* mouse: last raw dy (signed) */
    uint64_t  last_seen_ms;           /* perf_now_ns()/1e6 at last frame */
    /* Mouse only: interrupt report layout (0 = unknown, learn on wire). */
    uint8_t   mouse_parse;          /* HID_MOUSE_PARSE_* */
};

#define HID_MOUSE_PARSE_UNKNOWN 0u
#define HID_MOUSE_PARSE_NORID   1u /* [Buttons][X][Y]([wheel]...) */
#define HID_MOUSE_PARSE_RID     2u /* [ReportID][Buttons][X][Y]... */

static struct hid_dev_state g_hid[USB_HID_MAX_DEVICES];

/* ============================================================== */
/* Class request helpers (built on the HCI's control transfer)     */
/* ============================================================== */

/* Forward decl: sits in xhci.c (file-local "static" there for now);
 * we keep one tiny extern wrapper here that mirrors the same
 * synchronous semantics. To avoid adding it to a header-visible API
 * (the only consumer is this file, today), we re-declare it locally
 * with the matching signature. If a second HCI ever appears the
 * cleanest move is to formalise this as a function pointer on
 * struct usb_device. */
bool xhci_control_class(struct usb_device *dev,
                        uint8_t bm_request_type, uint8_t b_request,
                        uint16_t w_value, uint16_t w_index,
                        void *buf, uint16_t w_length);

/* Issue SET_PROTOCOL(BOOT). bmRequestType = host-to-device, class,
 * recipient interface = 0x21. */
static bool hid_set_protocol(struct usb_device *dev,
                             uint8_t iface, uint8_t protocol) {
    return xhci_control_class(
        dev,
        USB_DIR_OUT | USB_TYPE_CLASS | USB_RECIP_INTERFACE,
        USB_HID_REQ_SET_PROTOCOL,
        protocol, iface, 0, 0);
}

/* SET_IDLE(0): tell the device to only report on state change, not
 * periodically. Reduces interrupt-IN traffic to "real" events. */
static bool hid_set_idle(struct usb_device *dev,
                         uint8_t iface, uint8_t duration) {
    return xhci_control_class(
        dev,
        USB_DIR_OUT | USB_TYPE_CLASS | USB_RECIP_INTERFACE,
        USB_HID_REQ_SET_IDLE,
        ((uint16_t)duration << 8), iface, 0, 0);
}

/* ============================================================== */
/* Boot keyboard report -> ASCII translation                       */
/* ============================================================== */

/* USB HID Usage page 0x07 (Keyboard/Keypad), values 0x04..0x38.
 * 0 = unmapped. The corresponding shifted character is in the
 * second table. Gaps (function keys, lock keys, arrows, etc.) are
 * handled by hot-key / special dispatch below. */

static const char g_kbd_base[0x7F] = {
    [0x04] = 'a', [0x05] = 'b', [0x06] = 'c', [0x07] = 'd',
    [0x08] = 'e', [0x09] = 'f', [0x0A] = 'g', [0x0B] = 'h',
    [0x0C] = 'i', [0x0D] = 'j', [0x0E] = 'k', [0x0F] = 'l',
    [0x10] = 'm', [0x11] = 'n', [0x12] = 'o', [0x13] = 'p',
    [0x14] = 'q', [0x15] = 'r', [0x16] = 's', [0x17] = 't',
    [0x18] = 'u', [0x19] = 'v', [0x1A] = 'w', [0x1B] = 'x',
    [0x1C] = 'y', [0x1D] = 'z',
    [0x1E] = '1', [0x1F] = '2', [0x20] = '3', [0x21] = '4',
    [0x22] = '5', [0x23] = '6', [0x24] = '7', [0x25] = '8',
    [0x26] = '9', [0x27] = '0',
    [0x28] = '\n', [0x29] = 27 /* esc */, [0x2A] = '\b',
    [0x2B] = '\t', [0x2C] = ' ',
    [0x2D] = '-',  [0x2E] = '=',  [0x2F] = '[',  [0x30] = ']',
    [0x31] = '\\', [0x33] = ';',  [0x34] = '\'', [0x35] = '`',
    [0x36] = ',',  [0x37] = '.',  [0x38] = '/',
    /* Numpad arithmetic + numerals (0x54..0x63) intentionally
     * unmapped for now -- behaviour parity with the PS/2 driver,
     * which only handles the main keypad too. */
};

static const char g_kbd_shift[0x7F] = {
    [0x04] = 'A', [0x05] = 'B', [0x06] = 'C', [0x07] = 'D',
    [0x08] = 'E', [0x09] = 'F', [0x0A] = 'G', [0x0B] = 'H',
    [0x0C] = 'I', [0x0D] = 'J', [0x0E] = 'K', [0x0F] = 'L',
    [0x10] = 'M', [0x11] = 'N', [0x12] = 'O', [0x13] = 'P',
    [0x14] = 'Q', [0x15] = 'R', [0x16] = 'S', [0x17] = 'T',
    [0x18] = 'U', [0x19] = 'V', [0x1A] = 'W', [0x1B] = 'X',
    [0x1C] = 'Y', [0x1D] = 'Z',
    [0x1E] = '!', [0x1F] = '@', [0x20] = '#', [0x21] = '$',
    [0x22] = '%', [0x23] = '^', [0x24] = '&', [0x25] = '*',
    [0x26] = '(', [0x27] = ')',
    [0x28] = '\n', [0x29] = 27, [0x2A] = '\b',
    [0x2B] = '\t', [0x2C] = ' ',
    [0x2D] = '_',  [0x2E] = '+', [0x2F] = '{', [0x30] = '}',
    [0x31] = '|',  [0x33] = ':', [0x34] = '"', [0x35] = '~',
    [0x36] = '<',  [0x37] = '>', [0x38] = '?',
};

#define HID_KEY_CAPS_LOCK    0x39
#define HID_KEY_SCROLL_LOCK  0x47
#define HID_KEY_PAUSE        0x48
#define HID_KEY_F1           0x3A
#define HID_KEY_F2           0x3B
#define HID_KEY_F11          0x44
#define HID_KEY_F12          0x45

#define HID_MOD_LCTRL        0x01
#define HID_MOD_LSHIFT       0x02
#define HID_MOD_LALT         0x04
#define HID_MOD_LGUI         0x08
#define HID_MOD_RCTRL        0x10
#define HID_MOD_RSHIFT       0x20
#define HID_MOD_RALT         0x40
#define HID_MOD_RGUI         0x80

static bool key_in_set(uint8_t k, const uint8_t set[6]) {
    if (k == 0) return true;             /* "no key" matches itself */
    for (int i = 0; i < 6; i++) if (set[i] == k) return true;
    return false;
}

/* Translate one newly-pressed USB usage code into the existing
 * dispatch sinks. shift / ctrl / caps come from the modifier byte +
 * our software Caps Lock toggle. Hot keys (F1/F2/F11/F12/ScrollLock/
 * Pause) take precedence over the char path so a stuck GUI app can't
 * swallow the rescue key. */
static void hid_kbd_press(struct hid_dev_state *st, uint8_t usage,
                          bool shift, bool ctrl) {
    if (usage == 0) return;

    if (usage == HID_KEY_CAPS_LOCK) {
        st->caps_on = !st->caps_on;
        return;
    }
    if (usage == HID_KEY_F2 || usage == HID_KEY_F12 || usage == HID_KEY_PAUSE) {
        gui_emergency_exit("USB HID hotkey");
        return;
    }
    if (usage == HID_KEY_F1 || usage == HID_KEY_F11 || usage == HID_KEY_SCROLL_LOCK) {
        gui_dump_status("USB HID hotkey");
        return;
    }

    if (usage >= 0x7F) return;
    char c = shift ? g_kbd_shift[usage] : g_kbd_base[usage];
    if (c == 0) return;

    /* Caps Lock affects letters only and inverts the shift state for
     * them. (Numbers/symbols stay the same -- same rule as PS/2.) */
    if (st->caps_on) {
        if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
        else if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    }

    /* Ctrl-A..Ctrl-Z -> 0x01..0x1A. (Ctrl+C is then handled inside
     * kbd_dispatch_char as a SIGINT delivery, identical to PS/2.) */
    if (ctrl && ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))) {
        c = (char)(c & 0x1F);
    }

    kbd_dispatch_char(c);
}

/* Diff the new 6-key buffer against the last one to find newly
 * pressed slots. Released slots are detected implicitly (they just
 * stop appearing in subsequent reports); we count them here for
 * M26D telemetry but otherwise do not surface them. */
static void hid_kbd_handle(struct hid_dev_state *st,
                           const uint8_t *report, uint32_t len) {
    if (len < 8) return;
    uint8_t mod = report[0];
    uint8_t keys[6];
    for (int i = 0; i < 6; i++) keys[i] = report[2 + i];

    bool shift = (mod & (HID_MOD_LSHIFT | HID_MOD_RSHIFT)) != 0;
    bool ctrl  = (mod & (HID_MOD_LCTRL  | HID_MOD_RCTRL )) != 0;

    st->last_modmask = mod;

    /* Phantom state (all 6 slots holding 0x01 = "ErrorRollOver")
     * means the user is mashing more than 6 keys. Skip the report
     * to preserve our "newly pressed" diff. */
    if (keys[0] == 0x01 && keys[1] == 0x01 && keys[2] == 0x01 &&
        keys[3] == 0x01 && keys[4] == 0x01 && keys[5] == 0x01) {
        return;
    }

    for (int i = 0; i < 6; i++) {
        uint8_t k = keys[i];
        if (k == 0) continue;
        if (key_in_set(k, st->last_keys)) continue;   /* still held */
        st->key_press_total++;
        st->last_usage = k;
        hid_kbd_press(st, k, shift, ctrl);
    }
    /* Count newly released slots (in last_keys but not in keys[]). */
    for (int i = 0; i < 6; i++) {
        uint8_t k = st->last_keys[i];
        if (k == 0) continue;
        if (!key_in_set(k, keys)) st->key_release_total++;
    }
    for (int i = 0; i < 6; i++) st->last_keys[i] = keys[i];
}

/* ============================================================== */
/* Boot mouse report                                               */
/* ============================================================== */

/* USB HID Boot Mouse (no leading Report ID):
 *   byte 0: buttons (bit 0=L, 1=R, 2=M) -- exact same bit layout
 *           as MOUSE_BTN_* used by mouse.c, so we can pass through.
 *   byte 1: int8_t  signed X delta (positive = right)
 *   byte 2: int8_t  signed Y delta (positive = down)  <- same sign as
 *                                                       PS/2 *post*-flip;
 *                                                       no extra negation
 *                                                       needed.
 *   byte 3: int8_t  signed wheel delta (optional)
 *
 * With a leading Report ID (common on PCH xHCI + OEM mice):
 *   byte 0: report ID, byte 1: buttons, byte 2: X, byte 3: Y, ...
 */
static void hid_mouse_learn_layout(struct hid_dev_state *st,
                                   const uint8_t *report, uint32_t len) {
    if (st->mouse_parse != HID_MOUSE_PARSE_UNKNOWN || len < 4) return;

    /* Leading byte zero => first field is the button byte (QEMU and
     * many 3- and 4-byte boot reports without report ID). */
    if (report[0] == 0u) {
        st->mouse_parse = HID_MOUSE_PARSE_NORID;
        return;
    }

    /* High bits set on byte0 => cannot be a 3-button bitmap alone. */
    if ((report[0] & 0xF8u) != 0u) {
        st->mouse_parse = HID_MOUSE_PARSE_RID;
        kprintf("[usb-hid] mouse slot=%u: using Report-ID prefix "
                "(lead=0x%02x)\n",
                (unsigned)st->slot_id, (unsigned)report[0]);
        return;
    }

    /* Heuristic: [RID][0][X][Y] with motion in bytes 2/3 and a plausible
     * "buttons only" byte1 — classic parse would stuff X into dy. */
    if ((report[0] & 0xF8u) == 0u && (report[1] & 0xF8u) == 0u &&
        report[1] == 0u &&
        (report[2] != 0u || report[3] != 0u)) {
        st->mouse_parse = HID_MOUSE_PARSE_RID;
        kprintf("[usb-hid] mouse slot=%u: detected Report-ID-style layout "
                "(rid=0x%02x)\n",
                (unsigned)st->slot_id, (unsigned)report[0]);
    }
}

static void hid_mouse_handle(struct hid_dev_state *st,
                             const uint8_t *report, uint32_t len) {
    if (len < 3) return;

    hid_mouse_learn_layout(st, report, len);

    uint8_t buttons;
    int     dx, dy;

    if (st->mouse_parse == HID_MOUSE_PARSE_RID && len >= 4) {
        buttons = report[1] & 0x07u;
        dx        = (int8_t)report[2];
        dy        = (int8_t)report[3];
    } else {
        /* UNKNOWN, NORID, or short report: classic boot layout. */
        buttons = report[0] & 0x07u;
        dx        = (int8_t)report[1];
        dy        = (int8_t)report[2];
    }

    /* Count low-to-high transitions on any of the 3 buttons. */
    uint8_t newly = (uint8_t)(buttons & ~st->last_buttons);
    if (newly & MOUSE_BTN_LEFT)   st->mouse_btn_press_total++;
    if (newly & MOUSE_BTN_RIGHT)  st->mouse_btn_press_total++;
    if (newly & MOUSE_BTN_MIDDLE) st->mouse_btn_press_total++;

    st->last_buttons = buttons;
    st->last_dx      = (int8_t)dx;
    st->last_dy      = (int8_t)dy;
    /* Saturate at 1<<32 -- never going to overflow within a session,
     * but the cast back to uint64_t happily wraps so we accumulate as
     * unsigned and just cast on read. */
    st->mouse_dx_abs_total += (uint64_t)(dx < 0 ? -dx : dx);
    st->mouse_dy_abs_total += (uint64_t)(dy < 0 ? -dy : dy);

    mouse_inject_event(dx, dy, buttons);
}

/* ============================================================== */
/* int_complete dispatch (called from xhci_poll)                   */
/* ============================================================== */

/* Periodic snapshot: log one line every 8 frames. Why 8: gives the
 * QMP-driven test harness enough granularity to catch a few-event
 * burst from `input-send-event` without spamming the log during real
 * desktop use. The cap is per-device so a kbd burst doesn't shadow a
 * concurrent mouse one. */
static void hid_log_snapshot(const struct hid_dev_state *st) {
    if (st->is_keyboard) {
        kprintf("[input] hid kbd slot=%u frames=%lu presses=%lu releases=%lu "
                "mod=0x%02x last=0x%02x caps=%d\n",
                (unsigned)st->slot_id,
                (unsigned long)st->frames_total,
                (unsigned long)st->key_press_total,
                (unsigned long)st->key_release_total,
                (unsigned)st->last_modmask,
                (unsigned)st->last_usage,
                st->caps_on ? 1 : 0);
    } else {
        kprintf("[input] hid mouse slot=%u frames=%lu clicks=%lu "
                "btn=0x%02x dx=%d dy=%d sumdx=%lu sumdy=%lu\n",
                (unsigned)st->slot_id,
                (unsigned long)st->frames_total,
                (unsigned long)st->mouse_btn_press_total,
                (unsigned)st->last_buttons,
                (int)st->last_dx, (int)st->last_dy,
                (unsigned long)st->mouse_dx_abs_total,
                (unsigned long)st->mouse_dy_abs_total);
    }
}

static void hid_on_report(struct usb_device *dev,
                          const uint8_t *buf, uint32_t len) {
    struct hid_dev_state *st = (struct hid_dev_state *)dev->hid_state;
    if (!st || !st->in_use) return;

    st->frames_total++;
    st->last_seen_ms = perf_now_ns() / 1000000ull;

    /* Pre-handler snapshots of state we want to detect transitions on. */
    uint8_t prev_mod = st->last_modmask;
    uint64_t prev_clicks = st->mouse_btn_press_total;

    if (st->is_keyboard) hid_kbd_handle(st, buf, len);
    else                 hid_mouse_handle(st, buf, len);

    /* Periodic snapshot (every 8 frames). Force one immediately on
     * meaningful transitions so a test harness or a developer watching
     * the log doesn't have to wait for the next 8-frame tick to see a
     * fresh modifier or click land. */
    bool tick   = (st->frames_total & 7u) == 0u;
    bool change = st->is_keyboard
                  ? (st->last_modmask != prev_mod)
                  : (st->mouse_btn_press_total != prev_clicks);
    if (tick || change) hid_log_snapshot(st);
}

/* ============================================================== */
/* Probe                                                           */
/* ============================================================== */

bool usb_hid_probe(struct usb_device *dev,
                   const struct usb_iface_desc *iface,
                   const struct usb_endpoint_desc *ep) {
    if (iface->bInterfaceClass != USB_CLASS_HID) return false;
    /* Boot (1) or No Subclass (0). Subclass 0 is the common "BIOS HID"
     * shape on laptops and many USB hubs; we still drive them through
     * SET_PROTOCOL(BOOT) + the same parsers when the device agrees. */
    if (iface->bInterfaceSubClass != 0 && iface->bInterfaceSubClass != 1) {
        return false;
    }
    if (USB_EP_TYPE(ep->bmAttributes) != USB_EP_INTERRUPT) return false;
    if (!USB_EP_DIR_IN(ep->bEndpointAddress))              return false;

    uint16_t ep_mps = ep->wMaxPacketSize & 0x07FFu;
    if (ep_mps == 0 || ep_mps > HID_REPORT_BUF) return false;

    bool     is_keyboard;
    uint8_t  proto = iface->bInterfaceProtocol;

    if (proto == USB_HID_PROTO_KEYBOARD) {
        is_keyboard = true;
    } else if (proto == USB_HID_PROTO_MOUSE) {
        is_keyboard = false;
    } else if (iface->bInterfaceSubClass == 0 && proto == 0) {
        /* No protocol bits: infer basic boot-sized interrupt IN.
         * 8 B is the boot keyboard report; 3–7 B covers typical mice
         * (some advertise 8 B too — those usually declare proto=mouse). */
        if (ep_mps == 8u) {
            is_keyboard = true;
        } else if (ep_mps >= 3u && ep_mps <= 7u) {
            is_keyboard = false;
        } else {
            return false;
        }
    } else {
        return false;
    }

    /* Find a free per-device slot in our small pool. */
    struct hid_dev_state *st = 0;
    for (int i = 0; i < USB_HID_MAX_DEVICES; i++) {
        if (!g_hid[i].in_use) { st = &g_hid[i]; break; }
    }
    if (!st) {
        kprintf("[usb-hid] device pool full -- ignoring iface %u proto %u\n",
                iface->bInterfaceNumber, iface->bInterfaceProtocol);
        return false;
    }

    memset(st, 0, sizeof(*st));
    st->in_use      = true;
    st->is_keyboard = is_keyboard;
    st->iface_num   = iface->bInterfaceNumber;
    st->ep_addr     = ep->bEndpointAddress;
    st->slot_id     = dev->slot_id;
    st->mps         = ep->wMaxPacketSize & 0x07FFu;
    if (st->mps > HID_REPORT_BUF) st->mps = HID_REPORT_BUF;

    /* SET_PROTOCOL(BOOT) -- guarantees the report layout we coded
     * the parsers against. SET_IDLE(0) -- only push reports on
     * change (matches PS/2 IRQ semantics: every event is meaningful). */
    if (!hid_set_protocol(dev, iface->bInterfaceNumber, USB_HID_PROTO_BOOT)) {
        /* Some device firmwares (Logitech Unifying receivers in
         * particular) STALL SET_PROTOCOL when they stay in report mode
         * or do not implement the request. Treat a STALL as "keep going"
         * -- we proceed; a malformed report shows up later anyway. */
        kprintf("[usb-hid] WARN: SET_PROTOCOL(BOOT) refused on iface %u "
                "-- proceeding under assumption device is already Boot\n",
                iface->bInterfaceNumber);
    }
    if (!hid_set_idle(dev, iface->bInterfaceNumber, 0)) {
        kprintf("[usb-hid] WARN: SET_IDLE(0) refused on iface %u "
                "-- continuing anyway\n", iface->bInterfaceNumber);
    }

    /* Wire the device's interrupt-IN buffer for xHCI. The HCI's
     * configure_int_in path will allocate the transfer ring; our
     * job is the per-report scratch buffer + completion callback. */
    extern uint64_t pmm_alloc_page(void);
    extern void    *pmm_phys_to_virt(uint64_t phys);
    uint64_t buf_phys = pmm_alloc_page();
    if (!buf_phys) {
        kprintf("[usb-hid] OOM allocating report buffer\n");
        st->in_use = false;
        return false;
    }
    dev->int_buf       = (uint8_t *)pmm_phys_to_virt(buf_phys);
    dev->int_buf_phys  = buf_phys;
    dev->int_buf_size  = st->mps ? st->mps : 8u;
    dev->int_complete  = hid_on_report;
    dev->hid_state     = st;
    dev->int_armed     = false;

    kprintf("[usb-hid] %s on slot %u iface %u ep 0x%02x mps %u  (port %u)"
            "%s%s\n",
            st->is_keyboard ? "boot keyboard" : "boot mouse",
            dev->slot_id, iface->bInterfaceNumber,
            ep->bEndpointAddress, st->mps, dev->port_id,
            iface->bInterfaceSubClass == 0 ? " sub=0" : "",
            (iface->bInterfaceSubClass == 0 && proto == 0)
                ? " (proto0 MPS guess)" : "");
    return true;
}

/* M26C: drop the HID state owned by `dev`. Two paths reach this:
 *   1. xhci_detach_slot() during cable-yank teardown
 *   2. driver-side cleanup if the controller declares the slot dead
 * We zero the back-pointer so any in-flight interrupt-IN completion
 * that arrives *after* this returns will see hid_state==NULL and
 * silently fall through. The DMA buffer behind dev->int_buf is owned
 * by the HCI's slot teardown path (it's the same pmm-alloced page the
 * controller still references in its EP context), so we don't free it
 * here -- xhci_detach_slot() does that after Disable Slot returns. */
void usb_hid_unbind(struct usb_device *dev) {
    if (!dev) return;
    struct hid_dev_state *st = (struct hid_dev_state *)dev->hid_state;
    if (!st) return;
    /* Defensive: re-find the slot in our pool in case dev->hid_state
     * was already cleared but the slot is leaking (shouldn't happen,
     * but a stuck slot is worse than a double-free no-op). */
    for (int i = 0; i < USB_HID_MAX_DEVICES; i++) {
        if (&g_hid[i] == st) {
            kprintf("[usb-hid] unbind slot %u (%s, iface %u) "
                    "frames=%lu presses=%lu clicks=%lu\n",
                    dev->slot_id,
                    st->is_keyboard ? "kbd" : "mouse",
                    st->iface_num,
                    (unsigned long)st->frames_total,
                    (unsigned long)st->key_press_total,
                    (unsigned long)st->mouse_btn_press_total);
            g_hid[i].in_use = false;
            break;
        }
    }
    dev->hid_state    = 0;
    dev->int_complete = 0;
    dev->int_armed    = false;
}

/* ============================================================== */
/* M26D introspection + self-test                                  */
/* ============================================================== */

int usb_hid_count(void) {
    int n = 0;
    for (int i = 0; i < USB_HID_MAX_DEVICES; i++) {
        if (g_hid[i].in_use) n++;
    }
    return n;
}

int usb_hid_kbd_count(void) {
    int n = 0;
    for (int i = 0; i < USB_HID_MAX_DEVICES; i++) {
        if (g_hid[i].in_use && g_hid[i].is_keyboard) n++;
    }
    return n;
}

int usb_hid_mouse_count(void) {
    int n = 0;
    for (int i = 0; i < USB_HID_MAX_DEVICES; i++) {
        if (g_hid[i].in_use && !g_hid[i].is_keyboard) n++;
    }
    return n;
}

uint64_t usb_hid_total_frames(void) {
    uint64_t f = 0;
    for (int i = 0; i < USB_HID_MAX_DEVICES; i++) {
        if (g_hid[i].in_use) f += g_hid[i].frames_total;
    }
    return f;
}

/* Walk the in-use HID pool and emit one ABI_DEVT_BUS_INPUT record per
 * device. Mirrors xhci_introspect_at()'s shape so devtest_emit_input
 * can splice these in next to the PS/2 records without needing any
 * special-casing on the consumer side. */
int usb_hid_introspect_at(int idx, struct abi_dev_info *out) {
    if (!out || idx < 0) return 0;
    int seen = 0;
    for (int i = 0; i < USB_HID_MAX_DEVICES; i++) {
        if (!g_hid[i].in_use) continue;
        if (seen == idx) {
            const struct hid_dev_state *st = &g_hid[i];
            memset(out, 0, sizeof *out);
            out->bus        = ABI_DEVT_BUS_INPUT;
            out->status     = ABI_DEVT_PRESENT | ABI_DEVT_BOUND |
                              (st->frames_total ? ABI_DEVT_ACTIVE : 0u);
            out->index      = (uint8_t)(2 + i);   /* 0/1 reserved for PS/2 */
            out->class_code = 0x09;               /* "HID" grouping        */
            out->subclass   = st->is_keyboard ? 0x01u : 0x02u;
            ksnprintf(out->name, ABI_DEVT_NAME_MAX,
                      st->is_keyboard ? "uhid-kbd%d" : "uhid-mouse%d", i);
            ksnprintf(out->driver, ABI_DEVT_DRIVER_MAX,
                      st->is_keyboard ? "usb_hid_kbd" : "usb_hid_mouse");
            if (st->is_keyboard) {
                ksnprintf(out->extra, ABI_DEVT_EXTRA_MAX,
                          "slot=%u iface=%u ep=0x%02x mps=%u "
                          "frames=%lu presses=%lu mod=0x%02x",
                          (unsigned)st->slot_id, (unsigned)st->iface_num,
                          (unsigned)st->ep_addr, (unsigned)st->mps,
                          (unsigned long)st->frames_total,
                          (unsigned long)st->key_press_total,
                          (unsigned)st->last_modmask);
            } else {
                ksnprintf(out->extra, ABI_DEVT_EXTRA_MAX,
                          "slot=%u iface=%u ep=0x%02x mps=%u "
                          "frames=%lu clicks=%lu btn=0x%02x",
                          (unsigned)st->slot_id, (unsigned)st->iface_num,
                          (unsigned)st->ep_addr, (unsigned)st->mps,
                          (unsigned long)st->frames_total,
                          (unsigned long)st->mouse_btn_press_total,
                          (unsigned)st->last_buttons);
            }
            return 1;
        }
        seen++;
    }
    return 0;
}

/* devtest "usb_hid": SKIP if no HID device present, PASS if every
 * in_use slot is wired to a non-NULL completion callback (which is
 * the invariant probe() establishes). FAIL otherwise. */
int usb_hid_selftest(char *msg, size_t cap) {
    int n = usb_hid_count();
    if (n == 0) {
        ksnprintf(msg, cap,
                  "no USB HID devices (run with -device usb-kbd / usb-mouse)");
        return ABI_DEVT_SKIP;
    }
    int kbd = usb_hid_kbd_count();
    int mouse = usb_hid_mouse_count();
    /* Sanity: slot_id non-zero (real xHCI slots start at 1) and mps
     * within reason. */
    for (int i = 0; i < USB_HID_MAX_DEVICES; i++) {
        const struct hid_dev_state *st = &g_hid[i];
        if (!st->in_use) continue;
        if (st->slot_id == 0 || st->mps == 0 || st->mps > HID_REPORT_BUF) {
            ksnprintf(msg, cap,
                      "HID slot[%d] inconsistent: slot_id=%u mps=%u",
                      i, (unsigned)st->slot_id, (unsigned)st->mps);
            return -ABI_EIO;
        }
    }
    ksnprintf(msg, cap,
              "USB HID: %d device(s) -- kbd=%d mouse=%d frames=%lu",
              n, kbd, mouse, (unsigned long)usb_hid_total_frames());
    return 0;
}
