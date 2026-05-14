/* mouse.c -- PS/2 AUX-port mouse driver.
 *
 * Real-hardware friendly version:
 *   - explicitly preserves/enables keyboard + mouse IRQ bits in 8042 config
 *   - drains all pending 8042 output bytes per IRQ, not just one
 *   - routes keyboard bytes to kbd_ps2_handle_scancode()
 *   - raises PS/2 mouse sample rate/resolution before enabling reports
 *   - removes hot-path kprintf() logging from IRQ/event paths
 */

 #include <tobyos/mouse.h>
 #include <tobyos/keyboard.h>
 #include <tobyos/cpu.h>
 #include <tobyos/isr.h>
 #include <tobyos/irq.h>
 #include <tobyos/pic.h>
 #include <tobyos/printk.h>
 
 #define KBD_DATA   0x60
 #define KBD_STATUS 0x64
 #define KBD_CMD    0x64
 
 /* 8042 status bits. */
 #define KBD_STATUS_OUT_FULL  0x01
 #define KBD_STATUS_IN_FULL   0x02
 #define KBD_STATUS_AUX_DATA  0x20
 
 /* 8042 controller commands. */
 #define CMD_READ_CONFIG    0x20
 #define CMD_WRITE_CONFIG   0x60
 #define CMD_DISABLE_AUX    0xA7
 #define CMD_ENABLE_AUX     0xA8
 #define CMD_DISABLE_KBD    0xAD
 #define CMD_ENABLE_KBD     0xAE
 #define CMD_AUX_WRITE_NEXT 0xD4
 
 /* AUX device commands. */
 #define AUX_SET_DEFAULTS    0xF6
 #define AUX_ENABLE_REPORT   0xF4
 #define AUX_SET_SAMPLE_RATE 0xF3
 #define AUX_SET_RESOLUTION  0xE8
 #define AUX_ACK             0xFA
 
 /* 8042 config bits. */
 #define CFG_IRQ1_ENABLE    (1u << 0)
 #define CFG_IRQ12_ENABLE   (1u << 1)
 #define CFG_KBD_DISABLE    (1u << 4)
 #define CFG_MOUSE_DISABLE  (1u << 5)
 
 /* Packet state. */
 static volatile uint8_t g_pkt[3];
 static volatile uint8_t g_pkt_pos = 0;
 static volatile uint8_t g_buttons = 0;
 
 /* Telemetry. */
 static volatile uint64_t g_aux_bytes_total;
 static volatile uint64_t g_events_total;
 static volatile uint64_t g_btn_press_total;
 static volatile uint64_t g_dx_abs_total;
 static volatile uint64_t g_dy_abs_total;
 static volatile int8_t   g_last_dx;
 static volatile int8_t   g_last_dy;
 static volatile uint8_t  g_last_buttons;
 
 static void noop_cb(int dx, int dy, uint8_t b) {
     (void)dx;
     (void)dy;
     (void)b;
 }
 
 static mouse_event_fn g_cb = noop_cb;
 
 /* ---- 8042 helpers ------------------------------------------------ */
 
 static void wait_in_clear(void) {
     for (int i = 0; i < 100000; i++) {
         if ((inb(KBD_STATUS) & KBD_STATUS_IN_FULL) == 0) return;
     }
 }
 
 static bool wait_out_full(void) {
     for (int i = 0; i < 100000; i++) {
         if (inb(KBD_STATUS) & KBD_STATUS_OUT_FULL) return true;
     }
     return false;
 }
 
 static int wait_aux_data(void) {
     for (int i = 0; i < 200000; i++) {
         uint8_t st = inb(KBD_STATUS);
         if ((st & KBD_STATUS_OUT_FULL) == 0) continue;
 
         uint8_t b = inb(KBD_DATA);
         if (st & KBD_STATUS_AUX_DATA) return b;
 
         /* Keyboard byte while mouse init is in progress. Drop it.
          * mouse_init() runs with interrupts disabled and the keyboard
          * port temporarily disabled, so this is safe. */
     }
     return -1;
 }
 
 static void ctl_write(uint8_t cmd) {
     wait_in_clear();
     outb(KBD_CMD, cmd);
 }
 
 static void ctl_write_data(uint8_t b) {
     wait_in_clear();
     outb(KBD_DATA, b);
 }
 
 static int ctl_read_data(void) {
     if (!wait_out_full()) return -1;
     return inb(KBD_DATA);
 }
 
 static bool aux_send(uint8_t b) {
     ctl_write(CMD_AUX_WRITE_NEXT);
     ctl_write_data(b);
     int r = wait_aux_data();
     return r == AUX_ACK;
 }
 
 static void drain_8042_output(void) {
     for (int i = 0; i < 64; i++) {
         if ((inb(KBD_STATUS) & KBD_STATUS_OUT_FULL) == 0) break;
         (void)inb(KBD_DATA);
     }
 }
 
 /* ---- PS/2 mouse packet handling --------------------------------- */
 
 void mouse_ps2_handle_byte(uint8_t b) {
     g_aux_bytes_total++;
 
     if (g_pkt_pos == 0 && (b & 0x08) == 0) {
         return;
     }
 
     g_pkt[g_pkt_pos++] = b;
     if (g_pkt_pos < 3) {
         return;
     }
 
     g_pkt_pos = 0;
 
     uint8_t flags = g_pkt[0];
 
     if (flags & 0xC0) {
         return;
     }
 
     int dx = (int)g_pkt[1] - ((flags << 4) & 0x100);
     int dy = (int)g_pkt[2] - ((flags << 3) & 0x100);
 
     dy = -dy;
 
     mouse_inject_event(dx, dy, flags & 0x07);
 }
 
 /* Drain all queued 8042 bytes. Both IRQ1 and IRQ12 use the same idea:
  * AUX bytes become mouse packets, non-AUX bytes become keyboard input. */
 static void ps2_drain_output(void) {
     for (int i = 0; i < 64; i++) {
         uint8_t st = inb(KBD_STATUS);
         if ((st & KBD_STATUS_OUT_FULL) == 0) break;
 
         uint8_t b = inb(KBD_DATA);
         if (st & KBD_STATUS_AUX_DATA) {
             mouse_ps2_handle_byte(b);
         } else {
             kbd_ps2_handle_scancode(b);
         }
     }
 }
 
 static void mouse_irq(struct regs *r) {
     (void)r;
     ps2_drain_output();
     irq_eoi_isa(12);
 }
 
 /* ---- public API -------------------------------------------------- */
 
 void mouse_set_callback(mouse_event_fn cb) {
     g_cb = cb ? cb : noop_cb;
 }
 
 uint8_t mouse_buttons(void) {
     return g_buttons;
 }
 
 void mouse_inject_event(int dx, int dy, uint8_t buttons) {
     uint8_t newly = (uint8_t)(buttons & ~g_buttons);
 
     if (newly & MOUSE_BTN_LEFT)   g_btn_press_total++;
     if (newly & MOUSE_BTN_RIGHT)  g_btn_press_total++;
     if (newly & MOUSE_BTN_MIDDLE) g_btn_press_total++;
 
     g_buttons      = buttons;
     g_last_buttons = buttons;
     g_last_dx      = (int8_t)((dx < -128) ? -128 : (dx > 127 ? 127 : dx));
     g_last_dy      = (int8_t)((dy < -128) ? -128 : (dy > 127 ? 127 : dy));
 
     g_events_total++;
     g_dx_abs_total += (uint64_t)(dx < 0 ? -dx : dx);
     g_dy_abs_total += (uint64_t)(dy < 0 ? -dy : dy);
 
     g_cb(dx, dy, buttons);
 }
 
 void mouse_init(void) {
     cli();
 
     ctl_write(CMD_DISABLE_KBD);
     ctl_write(CMD_DISABLE_AUX);
 
     drain_8042_output();
 
     ctl_write(CMD_ENABLE_AUX);
 
     ctl_write(CMD_READ_CONFIG);
     int cfg = ctl_read_data();
     if (cfg < 0) {
         kprintf("[mouse] WARN: couldn't read 8042 config -- PS/2 mouse may not work\n");
         ctl_write(CMD_ENABLE_KBD);
         sti();
         return;
     }
 
     cfg = (cfg | CFG_IRQ1_ENABLE | CFG_IRQ12_ENABLE) &
           ~(CFG_KBD_DISABLE | CFG_MOUSE_DISABLE);
 
     ctl_write(CMD_WRITE_CONFIG);
     ctl_write_data((uint8_t)cfg);
 
     if (!aux_send(AUX_SET_DEFAULTS)) {
         kprintf("[mouse] WARN: AUX 0xF6 set-defaults NACK/timeout\n");
     }
 
     /* Make PS/2 mouse less awful on real hardware. Defaults are often
      * 100 Hz. 200 Hz + max standard resolution feels much smoother. */
     if (!aux_send(AUX_SET_SAMPLE_RATE) || !aux_send(200)) {
         kprintf("[mouse] WARN: AUX set sample rate 200 failed\n");
     }
 
     if (!aux_send(AUX_SET_RESOLUTION) || !aux_send(3)) {
         kprintf("[mouse] WARN: AUX set resolution failed\n");
     }
 
     if (!aux_send(AUX_ENABLE_REPORT)) {
         kprintf("[mouse] WARN: AUX 0xF4 enable-reporting NACK/timeout\n");
     }
 
     drain_8042_output();
 
     ctl_write(CMD_ENABLE_KBD);
 
     irq_install_isa(12, mouse_irq);
     if (!irq_using_ioapic()) {
         pic_unmask(2);
     }
 
     sti();
 
     kprintf("[mouse] PS/2 driver up (IRQ12 routed via %s, sample=200Hz)\n",
             irq_using_ioapic() ? "IO APIC" : "PIC");
 }
 
 /* ---- telemetry --------------------------------------------------- */
 
 uint64_t mouse_events_total(void)    { return g_events_total; }
 uint64_t mouse_btn_press_total(void) { return g_btn_press_total; }
 uint64_t mouse_dx_abs_total(void)    { return g_dx_abs_total; }
 uint64_t mouse_dy_abs_total(void)    { return g_dy_abs_total; }
 uint8_t  mouse_last_buttons(void)    { return g_last_buttons; }
 int8_t   mouse_last_dx(void)         { return g_last_dx; }
 int8_t   mouse_last_dy(void)         { return g_last_dy; }