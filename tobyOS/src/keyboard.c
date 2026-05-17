/* keyboard.c -- PS/2 keyboard driver, scancode set 1.
 *
 * Real-hardware friendly version:
 *   - explicitly enables keyboard IRQ1 in the 8042 config byte
 *   - drains all pending 8042 output bytes per IRQ, not just one
 *   - routes AUX bytes to mouse_ps2_handle_byte()
 *   - removes hot-path kprintf() logging from IRQ/dispatch paths
 */

 #include <tobyos/keyboard.h>
 #include <tobyos/mouse.h>
 #include <tobyos/isr.h>
 #include <tobyos/irq.h>
 #include <tobyos/cpu.h>
 #include <tobyos/signal.h>
 #include <tobyos/gui.h>
 #include <tobyos/printk.h>
 #include <tobyos/abi/abi.h>
 #include <tobyos/klibc.h>
 
 #define KBD_DATA   0x60
 #define KBD_STATUS 0x64
 #define KBD_CMD    0x64
 
 #define KBD_BUF_SIZE 256u  /* must be a power of two */
 
 /* 8042 status bits. */
 #define KBD_STATUS_OUT_FULL  0x01
 #define KBD_STATUS_IN_FULL   0x02
 #define KBD_STATUS_AUX_DATA  0x20
 
 /* 8042 controller commands. */
 #define CMD_READ_CONFIG    0x20
 #define CMD_WRITE_CONFIG   0x60
 #define CMD_ENABLE_KBD     0xAE

 #define KBD_DEV_SET_SCANCODE     0xF0
 #define KBD_DEV_ENABLE_SCANNING  0xF4
 #define KBD_DEV_ACK              0xFA
 #define KBD_DEV_RESEND           0xFE
 
 /* 8042 config bits. */
 #define CFG_IRQ1_ENABLE    (1u << 0)
 #define CFG_KBD_DISABLE    (1u << 4)
 #define CFG_TRANSLATE_SET1 (1u << 6)
 
 /* Ring buffer. head = producer, tail = consumer. */
 static volatile uint8_t g_buf[KBD_BUF_SIZE];
 static volatile uint8_t g_head = 0;
 static volatile uint8_t g_tail = 0;

 /* Modifier state. */
 static volatile bool g_shift  = false;
 static volatile bool g_ctrl   = false;
 static volatile bool g_caps   = false;
 static volatile bool g_ext_e0 = false;
 
 /* Telemetry. */
 static volatile uint64_t g_chars_dispatched;
 static volatile uint64_t g_irqs_total;
 static volatile uint8_t  g_last_scancode;
 static volatile char     g_last_char;
 
 /* US-QWERTY scancode set 1 -> ASCII. */
 static const char g_map_base[128] = {
 /* 0x00 */  0,    27,  '1', '2', '3', '4', '5', '6',
 /* 0x08 */ '7',  '8', '9', '0', '-', '=', '\b','\t',
 /* 0x10 */ 'q',  'w', 'e', 'r', 't', 'y', 'u', 'i',
 /* 0x18 */ 'o',  'p', '[', ']','\n',  0,  'a', 's',
 /* 0x20 */ 'd',  'f', 'g', 'h', 'j', 'k', 'l', ';',
 /* 0x28 */ '\'', '`',  0, '\\', 'z', 'x', 'c', 'v',
 /* 0x30 */ 'b',  'n', 'm', ',', '.', '/',  0,  '*',
 /* 0x38 */  0,   ' ',  0,   0,   0,   0,   0,   0,
 };
 
 static const char g_map_shift[128] = {
 /* 0x00 */  0,    27,  '!', '@', '#', '$', '%', '^',
 /* 0x08 */ '&',  '*', '(', ')', '_', '+', '\b','\t',
 /* 0x10 */ 'Q',  'W', 'E', 'R', 'T', 'Y', 'U', 'I',
 /* 0x18 */ 'O',  'P', '{', '}','\n',  0,  'A', 'S',
 /* 0x20 */ 'D',  'F', 'G', 'H', 'J', 'K', 'L', ':',
 /* 0x28 */ '"',  '~',  0,  '|', 'Z', 'X', 'C', 'V',
 /* 0x30 */ 'B',  'N', 'M', '<', '>', '?',  0,  '*',
 /* 0x38 */  0,   ' ',  0,   0,   0,   0,   0,   0,
 };
 
 static void buf_push(char c) {
     uint8_t next = (uint8_t)((g_head + 1u) & (KBD_BUF_SIZE - 1u));
     if (next == g_tail) {
         return;
     }
     g_buf[g_head] = (uint8_t)c;
     g_head = next;
 }

 int kbd_trygetc(void) {
     if (g_head == g_tail) return -1;
     char c = (char)g_buf[g_tail];
     g_tail = (uint8_t)((g_tail + 1u) & (KBD_BUF_SIZE - 1u));
     return (uint8_t)c;
 }
 
 int kbd_getc(void) {
     int c;
     while ((c = kbd_trygetc()) < 0) {
         hlt();
     }
     return c;
 }
 
 /* ---- 8042 helpers ------------------------------------------------ */
 
 static void kbd_wait_in_clear(void) {
     for (int i = 0; i < 100000; i++) {
         if ((inb(KBD_STATUS) & KBD_STATUS_IN_FULL) == 0) return;
     }
 }
 
 static bool kbd_wait_out_full(void) {
     for (int i = 0; i < 100000; i++) {
         if (inb(KBD_STATUS) & KBD_STATUS_OUT_FULL) return true;
     }
     return false;
 }
 
 static void kbd_ctl_write(uint8_t cmd) {
     kbd_wait_in_clear();
     outb(KBD_CMD, cmd);
 }
 
 static void kbd_ctl_write_data(uint8_t b) {
     kbd_wait_in_clear();
     outb(KBD_DATA, b);
 }
 
 static int kbd_ctl_read_data(void) {
     if (!kbd_wait_out_full()) return -1;
     return inb(KBD_DATA);
 }

 static int kbd_wait_kbd_data(void) {
     for (int i = 0; i < 200000; i++) {
         uint8_t st = inb(KBD_STATUS);
         if ((st & KBD_STATUS_OUT_FULL) == 0) continue;
         uint8_t b = inb(KBD_DATA);
         if ((st & KBD_STATUS_AUX_DATA) == 0) return b;
         mouse_ps2_handle_byte(b);
     }
     return -1;
 }

 static bool kbd_send_dev_cmd(uint8_t cmd) {
     for (int tries = 0; tries < 3; tries++) {
         kbd_ctl_write_data(cmd);
         int r = kbd_wait_kbd_data();
         if (r == KBD_DEV_ACK) return true;
         if (r != KBD_DEV_RESEND) return false;
     }
     return false;
 }
 
 static void kbd_drain_input(void) {
     for (int i = 0; i < 64; i++) {
         if ((inb(KBD_STATUS) & KBD_STATUS_OUT_FULL) == 0) break;
         (void)inb(KBD_DATA);
     }
 }
 
 static void kbd_controller_enable_irq1(void) {
     kbd_ctl_write(CMD_ENABLE_KBD);
 
     kbd_ctl_write(CMD_READ_CONFIG);
     int cfg = kbd_ctl_read_data();
     if (cfg < 0) {
         kprintf("[kbd] WARN: couldn't read 8042 config; keyboard IRQ may not work\n");
         return;
     }
 
     cfg = (cfg | CFG_IRQ1_ENABLE | CFG_TRANSLATE_SET1) & ~CFG_KBD_DISABLE;

     kbd_ctl_write(CMD_WRITE_CONFIG);
     kbd_ctl_write_data((uint8_t)cfg);
 }

 static void kbd_device_enable_scanning(void) {
     /* Some BIOSes leave the keyboard in a half-initialised state after
      * handing off USB-legacy or PS/2 emulation. Translation above makes
      * the controller deliver set-1 bytes to our parser; this command
      * makes sure the keyboard is actually producing make/break codes. */
     if (!kbd_send_dev_cmd(KBD_DEV_ENABLE_SCANNING)) {
         kprintf("[kbd] WARN: keyboard enable-scanning command failed\n");
     }
 }
 
 /* ---- byte-level scancode handling ------------------------------- */
 
 void kbd_ps2_handle_scancode(uint8_t sc) {
     g_last_scancode = sc;
 
     if (gui_trace_level() >= GUI_TRACE_VERBOSE) {
         gui_trace_logf("kbd raw scancode 0x%02x e0=%d",
                        (unsigned)sc, (int)g_ext_e0);
     }
 
     if (sc == 0xE0) {
         g_ext_e0 = true;
         return;
     }

     if (sc == KBD_DEV_ACK || sc == KBD_DEV_RESEND) {
         return;
     }
 
     if (sc == 0xE1) {
         gui_emergency_exit("Pause/Break hotkey");
         return;
     }
 
     if (g_ext_e0) {
         g_ext_e0 = false;
         return;
     }
 
     bool released = (sc & 0x80) != 0;
     uint8_t code  = (uint8_t)(sc & 0x7F);
 
     switch (code) {
     case 0x2A:
     case 0x36:
         g_shift = !released;
         return;
     case 0x1D:
         g_ctrl = !released;
         return;
     case 0x3A:
         if (!released) g_caps = !g_caps;
         return;
     default:
         break;
     }
 
     if (released) return;
     if (code >= 128) return;
 
     if (code == 0x58 /* F12 */ || code == 0x3C /* F2 */) {
         gui_emergency_exit(code == 0x58 ? "F12 hotkey" : "F2 hotkey");
         return;
     }
 
     if (code == 0x57 /* F11 */ || code == 0x3B /* F1 */) {
         gui_dump_status(code == 0x57 ? "F11 hotkey" : "F1 hotkey");
         return;
     }
 
     if (code == 0x46 /* ScrollLock */) {
         gui_dump_status("ScrollLock hotkey");
         return;
     }
 
     char c = g_shift ? g_map_shift[code] : g_map_base[code];
     if (c == 0) return;
 
     if (g_caps && c >= 'a' && c <= 'z') {
         c = (char)(c - 'a' + 'A');
     } else if (g_caps && c >= 'A' && c <= 'Z') {
         c = (char)(c - 'A' + 'a');
     }
 
     if (g_ctrl && ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))) {
         c = (char)(c & 0x1F);
     }
 
     kbd_dispatch_char(c);
 }
 
 /* Drain all queued 8042 bytes. Both IRQ1 and IRQ12 use this pattern
  * so neither side leaves bytes stuck in the shared output buffer. */
 static void ps2_drain_output(void) {
     for (int i = 0; i < 64; i++) {
         uint8_t st = inb(KBD_STATUS);
         if ((st & KBD_STATUS_OUT_FULL) == 0) break;
 
         uint8_t b = inb(KBD_DATA);
         if (st & KBD_STATUS_AUX_DATA) {
             mouse_ps2_handle_byte(b);
         } else {
             g_irqs_total++;
             kbd_ps2_handle_scancode(b);
         }
     }
 }
 
 static void kbd_irq(struct regs *r) {
     (void)r;
     ps2_drain_output();
     irq_eoi_isa(1);
 }
 
 /* ---- shared dispatch sink --------------------------------------- */

 void kbd_dispatch_char(char c) {
     if (c == 0) return;

     g_chars_dispatched++;
     g_last_char = c;
 
     if (c == 0x03) {
         int fg = signal_get_foreground();
         if (fg > 0) signal_send_to_pid(fg, SIGINT);
         return;
     }
 
     if (gui_active()) {
         gui_post_key((uint8_t)c);
         return;
     }
 
     buf_push(c);
 }

 void kbd_flush_pending(void) {
     /* Keyboard is delivered immediately from the PS/2/USB HID input
      * path. Mouse keeps a deferred queue because cursor motion is a
      * rendering hot path; keypresses must be visible to GUI apps at
      * once, otherwise typing feels like it arrives in delayed batches. */
 }
 
 void kbd_init(void) {
     cli();
 
     kbd_drain_input();
     kbd_controller_enable_irq1();
     kbd_device_enable_scanning();
     kbd_drain_input();
 
     irq_install_isa(1, kbd_irq);
 
     kprintf("[kbd] PS/2 driver up (IRQ1 enabled + unmasked)\n");
 }
 
 /* ---- telemetry --------------------------------------------------- */
 
 uint64_t kbd_chars_dispatched(void) { return g_chars_dispatched; }
 uint64_t kbd_irqs_total(void)       { return g_irqs_total; }
 bool     kbd_caps_state(void)       { return g_caps; }
 bool     kbd_shift_state(void)      { return g_shift; }
 bool     kbd_ctrl_state(void)       { return g_ctrl; }
 uint8_t  kbd_last_scancode(void)    { return g_last_scancode; }
