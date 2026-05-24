/* input.c -- unified input event system.
 *
 * Provides a central input event queue that keyboard, mouse, touchpad,
 * and touchscreen backends feed into via input_report_*() helpers.
 * Consumers (GUI, shell) drain events via input_poll_event().
 *
 * Features:
 *   - Unified struct input_event with type/code/value/timestamp
 *   - Keyboard layout engine (US QWERTY default)
 *   - Multi-touch tracking (up to 10 simultaneous contacts)
 *   - Touchpad gesture recognition: two-finger scroll, pinch-to-zoom,
 *     three-finger swipe
 */

#include <tobyos/types.h>
#include <tobyos/printk.h>
#include <tobyos/klibc.h>
#include <tobyos/spinlock.h>
#include <tobyos/pit.h>

/* ---- event types ------------------------------------------------- */

#define INPUT_KEY_PRESS      0x01
#define INPUT_KEY_RELEASE    0x02
#define INPUT_MOUSE_MOVE     0x03
#define INPUT_MOUSE_BUTTON   0x04
#define INPUT_TOUCH_DOWN     0x05
#define INPUT_TOUCH_MOVE     0x06
#define INPUT_TOUCH_UP       0x07
#define INPUT_GESTURE        0x08

/* Gesture codes (value field for INPUT_GESTURE) */
#define GESTURE_SCROLL_UP    0x10
#define GESTURE_SCROLL_DOWN  0x11
#define GESTURE_PINCH_IN     0x12
#define GESTURE_PINCH_OUT    0x13
#define GESTURE_SWIPE_LEFT   0x14
#define GESTURE_SWIPE_RIGHT  0x15
#define GESTURE_SWIPE_UP     0x16
#define GESTURE_SWIPE_DOWN   0x17

/* ---- input event structure --------------------------------------- */

struct input_event {
    uint8_t  type;
    uint16_t code;
    int32_t  value;
    uint64_t timestamp;
};

/* ---- event ring buffer ------------------------------------------- */

#define INPUT_RING_SIZE 256

static struct input_event g_input_ring[INPUT_RING_SIZE];
static uint16_t g_input_head;
static uint16_t g_input_tail;
static spinlock_t g_input_lock = SPINLOCK_INIT;

static void input_push(const struct input_event *ev) {
    uint64_t flags = spin_lock_irqsave(&g_input_lock);
    uint16_t next = (g_input_head + 1) % INPUT_RING_SIZE;
    if (next != g_input_tail) {
        g_input_ring[g_input_head] = *ev;
        g_input_head = next;
    }
    spin_unlock_irqrestore(&g_input_lock, flags);
}

bool input_poll_event(struct input_event *out) {
    uint64_t flags = spin_lock_irqsave(&g_input_lock);
    if (g_input_tail == g_input_head) {
        spin_unlock_irqrestore(&g_input_lock, flags);
        return false;
    }
    *out = g_input_ring[g_input_tail];
    g_input_tail = (g_input_tail + 1) % INPUT_RING_SIZE;
    spin_unlock_irqrestore(&g_input_lock, flags);
    return true;
}

size_t input_pending_count(void) {
    uint64_t flags = spin_lock_irqsave(&g_input_lock);
    size_t count;
    if (g_input_head >= g_input_tail)
        count = g_input_head - g_input_tail;
    else
        count = INPUT_RING_SIZE - g_input_tail + g_input_head;
    spin_unlock_irqrestore(&g_input_lock, flags);
    return count;
}

/* ---- keyboard layout engine (US QWERTY) -------------------------- */

/* Scancode set 1 -> ASCII mapping tables. Index = scancode (0..127).
 * Shifted variant in a parallel table. 0 = no mapping. */

static const char g_sc1_normal[128] = {
    [0x01] = 0x1B, /* ESC */
    [0x02] = '1', [0x03] = '2', [0x04] = '3', [0x05] = '4',
    [0x06] = '5', [0x07] = '6', [0x08] = '7', [0x09] = '8',
    [0x0A] = '9', [0x0B] = '0', [0x0C] = '-', [0x0D] = '=',
    [0x0E] = '\b', [0x0F] = '\t',
    [0x10] = 'q', [0x11] = 'w', [0x12] = 'e', [0x13] = 'r',
    [0x14] = 't', [0x15] = 'y', [0x16] = 'u', [0x17] = 'i',
    [0x18] = 'o', [0x19] = 'p', [0x1A] = '[', [0x1B] = ']',
    [0x1C] = '\n',
    [0x1E] = 'a', [0x1F] = 's', [0x20] = 'd', [0x21] = 'f',
    [0x22] = 'g', [0x23] = 'h', [0x24] = 'j', [0x25] = 'k',
    [0x26] = 'l', [0x27] = ';', [0x28] = '\'',
    [0x29] = '`',
    [0x2B] = '\\',
    [0x2C] = 'z', [0x2D] = 'x', [0x2E] = 'c', [0x2F] = 'v',
    [0x30] = 'b', [0x31] = 'n', [0x32] = 'm', [0x33] = ',',
    [0x34] = '.', [0x35] = '/',
    [0x39] = ' ',
};

static const char g_sc1_shifted[128] = {
    [0x02] = '!', [0x03] = '@', [0x04] = '#', [0x05] = '$',
    [0x06] = '%', [0x07] = '^', [0x08] = '&', [0x09] = '*',
    [0x0A] = '(', [0x0B] = ')', [0x0C] = '_', [0x0D] = '+',
    [0x0E] = '\b', [0x0F] = '\t',
    [0x10] = 'Q', [0x11] = 'W', [0x12] = 'E', [0x13] = 'R',
    [0x14] = 'T', [0x15] = 'Y', [0x16] = 'U', [0x17] = 'I',
    [0x18] = 'O', [0x19] = 'P', [0x1A] = '{', [0x1B] = '}',
    [0x1C] = '\n',
    [0x1E] = 'A', [0x1F] = 'S', [0x20] = 'D', [0x21] = 'F',
    [0x22] = 'G', [0x23] = 'H', [0x24] = 'J', [0x25] = 'K',
    [0x26] = 'L', [0x27] = ':', [0x28] = '"',
    [0x29] = '~',
    [0x2B] = '|',
    [0x2C] = 'Z', [0x2D] = 'X', [0x2E] = 'C', [0x2F] = 'V',
    [0x30] = 'B', [0x31] = 'N', [0x32] = 'M', [0x33] = '<',
    [0x34] = '>', [0x35] = '?',
    [0x39] = ' ',
};

/* Modifier state tracked across scancode events. */
static bool g_shift_held;
static bool g_ctrl_held;
static bool g_alt_held;
static bool g_caps_lock;

char input_scancode_to_char(uint8_t scancode) {
    if (scancode >= 128) return 0;
    bool shifted = g_shift_held ^ g_caps_lock;
    char c = shifted ? g_sc1_shifted[scancode] : g_sc1_normal[scancode];
    if (g_ctrl_held && c >= 'a' && c <= 'z')
        c = (char)(c - 'a' + 1);
    return c;
}

/* ---- multi-touch tracking ---------------------------------------- */

#define INPUT_MAX_TOUCHES 10

struct touch_slot {
    bool     active;
    uint8_t  id;
    int32_t  x, y;
    int32_t  start_x, start_y;
    uint64_t start_time;
};

static struct touch_slot g_touches[INPUT_MAX_TOUCHES];
static int g_active_touch_count;

static struct touch_slot *touch_find(uint8_t id) {
    for (int i = 0; i < INPUT_MAX_TOUCHES; i++) {
        if (g_touches[i].active && g_touches[i].id == id)
            return &g_touches[i];
    }
    return NULL;
}

static struct touch_slot *touch_alloc(uint8_t id) {
    for (int i = 0; i < INPUT_MAX_TOUCHES; i++) {
        if (!g_touches[i].active) {
            memset(&g_touches[i], 0, sizeof(g_touches[i]));
            g_touches[i].active = true;
            g_touches[i].id = id;
            g_active_touch_count++;
            return &g_touches[i];
        }
    }
    return NULL;
}

/* ---- gesture recognition ----------------------------------------- */

static void input_check_gestures(void) {
    if (g_active_touch_count == 2) {
        struct touch_slot *t0 = NULL, *t1 = NULL;
        for (int i = 0; i < INPUT_MAX_TOUCHES && (!t0 || !t1); i++) {
            if (g_touches[i].active) {
                if (!t0) t0 = &g_touches[i];
                else t1 = &g_touches[i];
            }
        }
        if (!t0 || !t1) return;

        /* Two-finger scroll: both fingers moving in the same Y direction */
        int32_t dy0 = t0->y - t0->start_y;
        int32_t dy1 = t1->y - t1->start_y;
        if (dy0 > 20 && dy1 > 20) {
            struct input_event ev = {
                .type = INPUT_GESTURE, .code = GESTURE_SCROLL_DOWN,
                .value = (dy0 + dy1) / 2, .timestamp = pit_ticks()
            };
            input_push(&ev);
            t0->start_y = t0->y;
            t1->start_y = t1->y;
        } else if (dy0 < -20 && dy1 < -20) {
            struct input_event ev = {
                .type = INPUT_GESTURE, .code = GESTURE_SCROLL_UP,
                .value = (dy0 + dy1) / 2, .timestamp = pit_ticks()
            };
            input_push(&ev);
            t0->start_y = t0->y;
            t1->start_y = t1->y;
        }

        /* Pinch-to-zoom: fingers moving toward/away from each other */
        int32_t dx_cur = t1->x - t0->x;
        int32_t dy_cur = t1->y - t0->y;
        int32_t dx_start = t1->start_x - t0->start_x;
        int32_t dy_start = t1->start_y - t0->start_y;

        int64_t dist_cur_sq = (int64_t)dx_cur * dx_cur + (int64_t)dy_cur * dy_cur;
        int64_t dist_start_sq = (int64_t)dx_start * dx_start + (int64_t)dy_start * dy_start;

        if (dist_cur_sq > dist_start_sq + 2000) {
            struct input_event ev = {
                .type = INPUT_GESTURE, .code = GESTURE_PINCH_OUT,
                .value = (int32_t)(dist_cur_sq - dist_start_sq),
                .timestamp = pit_ticks()
            };
            input_push(&ev);
        } else if (dist_cur_sq < dist_start_sq - 2000) {
            struct input_event ev = {
                .type = INPUT_GESTURE, .code = GESTURE_PINCH_IN,
                .value = (int32_t)(dist_start_sq - dist_cur_sq),
                .timestamp = pit_ticks()
            };
            input_push(&ev);
        }
    }

    if (g_active_touch_count == 3) {
        /* Three-finger swipe */
        int32_t sum_dx = 0, sum_dy = 0;
        int count = 0;
        for (int i = 0; i < INPUT_MAX_TOUCHES && count < 3; i++) {
            if (!g_touches[i].active) continue;
            sum_dx += g_touches[i].x - g_touches[i].start_x;
            sum_dy += g_touches[i].y - g_touches[i].start_y;
            count++;
        }
        int32_t avg_dx = sum_dx / 3;
        int32_t avg_dy = sum_dy / 3;

        uint16_t gesture = 0;
        if (avg_dx > 50) gesture = GESTURE_SWIPE_RIGHT;
        else if (avg_dx < -50) gesture = GESTURE_SWIPE_LEFT;
        else if (avg_dy > 50) gesture = GESTURE_SWIPE_DOWN;
        else if (avg_dy < -50) gesture = GESTURE_SWIPE_UP;

        if (gesture) {
            struct input_event ev = {
                .type = INPUT_GESTURE, .code = gesture,
                .value = (avg_dx != 0) ? avg_dx : avg_dy,
                .timestamp = pit_ticks()
            };
            input_push(&ev);

            for (int i = 0; i < INPUT_MAX_TOUCHES; i++) {
                if (g_touches[i].active) {
                    g_touches[i].start_x = g_touches[i].x;
                    g_touches[i].start_y = g_touches[i].y;
                }
            }
        }
    }
}

/* ---- backend reporting API --------------------------------------- */

void input_report_key(uint8_t scancode, bool pressed) {
    /* Update modifier state */
    if (scancode == 0x2A || scancode == 0x36) g_shift_held = pressed;
    if (scancode == 0x1D) g_ctrl_held = pressed;
    if (scancode == 0x38) g_alt_held = pressed;
    if (scancode == 0x3A && pressed) g_caps_lock = !g_caps_lock;

    struct input_event ev = {
        .type = pressed ? INPUT_KEY_PRESS : INPUT_KEY_RELEASE,
        .code = scancode,
        .value = input_scancode_to_char(scancode),
        .timestamp = pit_ticks()
    };
    input_push(&ev);
}

void input_report_mouse_move(int32_t dx, int32_t dy) {
    struct input_event ev = {
        .type = INPUT_MOUSE_MOVE,
        .code = 0,
        .value = (dx & 0xFFFF) | ((dy & 0xFFFF) << 16),
        .timestamp = pit_ticks()
    };
    input_push(&ev);
}

void input_report_mouse_button(uint8_t buttons) {
    struct input_event ev = {
        .type = INPUT_MOUSE_BUTTON,
        .code = buttons,
        .value = buttons,
        .timestamp = pit_ticks()
    };
    input_push(&ev);
}

void input_report_touch(uint8_t type, uint8_t id, int32_t x, int32_t y) {
    struct input_event ev = {
        .type = type,
        .code = id,
        .value = (x & 0xFFFF) | ((y & 0xFFFF) << 16),
        .timestamp = pit_ticks()
    };
    input_push(&ev);

    if (type == INPUT_TOUCH_DOWN) {
        struct touch_slot *slot = touch_alloc(id);
        if (slot) {
            slot->x = slot->start_x = x;
            slot->y = slot->start_y = y;
            slot->start_time = pit_ticks();
        }
    } else if (type == INPUT_TOUCH_MOVE) {
        struct touch_slot *slot = touch_find(id);
        if (slot) {
            slot->x = x;
            slot->y = y;
        }
        input_check_gestures();
    } else if (type == INPUT_TOUCH_UP) {
        struct touch_slot *slot = touch_find(id);
        if (slot) {
            slot->active = false;
            g_active_touch_count--;
        }
    }
}

/* ---- init -------------------------------------------------------- */

void input_init(void) {
    memset(g_input_ring, 0, sizeof(g_input_ring));
    g_input_head = 0;
    g_input_tail = 0;
    memset(g_touches, 0, sizeof(g_touches));
    g_active_touch_count = 0;
    g_shift_held = false;
    g_ctrl_held = false;
    g_alt_held = false;
    g_caps_lock = false;
    kprintf("[input] unified input stack initialized\n");
}
