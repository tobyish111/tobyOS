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
 #include <tobyos/evdev.h>
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
 #define AUX_GET_DEVICE_ID   0xF2   /* answers 3 after the IntelliMouse knock */
 #define AUX_ENABLE_REPORT   0xF4
 #define AUX_SET_SAMPLE_RATE 0xF3
 #define AUX_SET_RESOLUTION  0xE8
 #define AUX_ACK             0xFA
 
 /* 8042 config bits. */
 #define CFG_IRQ1_ENABLE    (1u << 0)
 #define CFG_IRQ12_ENABLE   (1u << 1)
 #define CFG_KBD_DISABLE    (1u << 4)
 #define CFG_MOUSE_DISABLE  (1u << 5)

 /* Packet state. 4 bytes once the IntelliMouse knock succeeds (byte 3 is
  * the wheel); 3 otherwise. g_pkt_len is what the decoder waits for. */
 static volatile uint8_t g_pkt[4];
 static volatile uint8_t g_pkt_pos = 0;
 static volatile uint8_t g_pkt_len = 3;
 static volatile bool    g_has_wheel = false;
 static volatile uint8_t g_buttons = 0;
 
 /* Telemetry. */
 static volatile uint64_t g_aux_bytes_total;
 static volatile uint64_t g_events_total;
 static volatile uint64_t g_btn_press_total;
 static volatile uint64_t g_dx_abs_total;
 static volatile uint64_t g_dy_abs_total;
 static volatile int8_t   g_last_dx;
 static volatile int8_t   g_last_dy;
 static volatile int8_t   g_last_dz;
 static volatile uint64_t g_wheel_total;
 static volatile uint8_t  g_last_buttons;

 static void noop_cb(int dx, int dy, int dz, uint8_t b) {
     (void)dx;
     (void)dy;
     (void)dz;
     (void)b;
 }
 
 static mouse_event_fn g_cb = noop_cb;

 /* Mouse event queue. IRQ/HID paths preserve individual reports here
  * instead of collapsing them into one large delta. Collapsing made
  * missed flush windows feel terrible because GUI acceleration was
  * applied to the combined movement, producing visible bursts. */
 #define MOUSE_Q_SIZE 64u
 struct mouse_q_event {
     int dx;
     int dy;
     int dz;              /* wheel detents (see mouse.h) */
     uint8_t buttons;
     bool edge;
 };
 static volatile struct mouse_q_event g_q[MOUSE_Q_SIZE];
 static volatile uint8_t g_q_head;
 static volatile uint8_t g_q_tail;
 static volatile uint8_t g_current_buttons;
 
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
     if (g_pkt_pos < g_pkt_len) {
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

     /* IntelliMouse byte 3: the low NIBBLE is a signed wheel delta
      * (0x0..0x7 = 0..+7, 0x8..0xF = -8..-1). The upper nibble carries
      * buttons 4/5 on 5-button variants -- masked off here because the
      * event ABI models three buttons. Sign-extending the whole BYTE is
      * the classic bug: it turns a 4/5-button press into a huge bogus
      * scroll. */
     int dz = 0;
     if (g_pkt_len == 4) {
         dz = (int)(g_pkt[3] & 0x0F);
         if (dz > 7) dz -= 16;
     }

     mouse_inject_event(dx, dy, dz, flags & 0x07);
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
 
void mouse_inject_event(int dx, int dy, int dz, uint8_t buttons) {
    /* Track B input: mirror this report into /dev/input/event1 (evdev mouse)
     * as EV_REL motion + BTN_* edges, before g_last_buttons is updated below.
     * A Linux app reading the device sees a genuine PS/2-mouse event stream. */
    evdev_feed_mouse(dx, dy, dz, buttons, g_last_buttons);

    uint8_t newly = (uint8_t)(buttons & ~g_last_buttons);
 
     if (newly & MOUSE_BTN_LEFT)   g_btn_press_total++;
     if (newly & MOUSE_BTN_RIGHT)  g_btn_press_total++;
     if (newly & MOUSE_BTN_MIDDLE) g_btn_press_total++;
 
     g_last_buttons = buttons;
     g_last_dx      = (int8_t)((dx < -128) ? -128 : (dx > 127 ? 127 : dx));
     g_last_dy      = (int8_t)((dy < -128) ? -128 : (dy > 127 ? 127 : dy));
     g_last_dz      = (int8_t)((dz < -128) ? -128 : (dz > 127 ? 127 : dz));

     g_events_total++;
     g_dx_abs_total += (uint64_t)(dx < 0 ? -dx : dx);
     g_dy_abs_total += (uint64_t)(dy < 0 ? -dy : dy);
     g_wheel_total  += (uint64_t)(dz < 0 ? -dz : dz);

     uint8_t next = (uint8_t)((g_q_head + 1u) & (MOUSE_Q_SIZE - 1u));
     bool edge = (buttons != g_current_buttons);
     g_current_buttons = buttons;

     if (next == g_q_tail) {
         /* Queue full: merge into the most recent queued report so we
          * preserve forward progress without reverting to a giant
          * queue-wide burst. Wheel detents ACCUMULATE rather than being
          * overwritten -- dropping them would silently swallow a fast
          * flick, which is exactly when the queue fills. */
         uint8_t prev = (uint8_t)((g_q_head - 1u) & (MOUSE_Q_SIZE - 1u));
         g_q[prev].dx += dx;
         g_q[prev].dy += dy;
         g_q[prev].dz += dz;
         g_q[prev].buttons = buttons;
         g_q[prev].edge = g_q[prev].edge || edge;
         return;
     }

     g_q[g_q_head].dx = dx;
     g_q[g_q_head].dy = dy;
     g_q[g_q_head].dz = dz;
     g_q[g_q_head].buttons = buttons;
     g_q[g_q_head].edge = edge;
     g_q_head = next;
 }

 void mouse_flush_pending(void) {
     for (int n = 0; n < (int)MOUSE_Q_SIZE; n++) {
         uint64_t flags;
         __asm__ volatile ("pushfq; popq %0; cli" : "=r"(flags) :: "memory");

         if (g_q_tail == g_q_head) {
             if (flags & (1ULL << 9)) sti();
             break;
         }

         struct mouse_q_event ev;
         ev.dx = g_q[g_q_tail].dx;
         ev.dy = g_q[g_q_tail].dy;
         ev.dz = g_q[g_q_tail].dz;
         ev.buttons = g_q[g_q_tail].buttons;
         ev.edge = g_q[g_q_tail].edge;
         g_q_tail = (uint8_t)((g_q_tail + 1u) & (MOUSE_Q_SIZE - 1u));
         g_buttons = ev.buttons;

         if (flags & (1ULL << 9)) sti();

         /* dz alone is a real event: a wheel notch moves no pointer and
          * presses no button, so leaving it out of this test would drop
          * every scroll made on a stationary mouse -- i.e. all of them. */
         if (ev.dx || ev.dy || ev.dz || ev.edge) {
             g_cb(ev.dx, ev.dy, ev.dz, ev.buttons);
         }
     }
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
 
     /* IntelliMouse knock (Microsoft, 1996; every PS/2 wheel mouse and
      * every emulated one implements it). Sample rates 200-100-80 in
      * that exact order are a magic sequence, not a real configuration:
      * a wheel mouse answers the following Get-Device-ID (0xF2) with 3
      * instead of 0 and then reports 4-byte packets. A plain 2-button
      * mouse stays at id 0 and keeps sending 3 -- so this probe is safe
      * on hardware with no wheel, which is why the packet length is
      * driven by the ANSWER rather than assumed.
      *
      * THIS MUST BE THE FIRST SAMPLE-RATE TRAFFIC AFTER SET-DEFAULTS.
      * The detector is a state machine over CONSECUTIVE set-rate values,
      * so an unrelated "make the pointer smooth" 200 ahead of it makes
      * the device read the knock as 200-200-100 and never arm. Measured,
      * not theorised: with the smoothing call first, QEMU answered device
      * id 0 and stayed in 3-byte mode. Smoothing now happens AFTER.
      *
      * Also before enable-reporting: the device must be quiet while the
      * knock runs, or its motion packets interleave with the ACKs and
      * wait_aux_data() reads a data byte as the reply. */
     if (aux_send(AUX_SET_SAMPLE_RATE) && aux_send(200) &&
         aux_send(AUX_SET_SAMPLE_RATE) && aux_send(100) &&
         aux_send(AUX_SET_SAMPLE_RATE) && aux_send(80)  &&
         aux_send(AUX_GET_DEVICE_ID)) {
         int id = wait_aux_data();
         if (id == 3) {
             g_has_wheel = true;
             g_pkt_len   = 4;
         }
     }
     /* Make PS/2 mouse less awful on real hardware. Defaults are often
      * 100 Hz. 200 Hz + max standard resolution feels much smoother --
      * and this also undoes the 80 Hz the knock leaves behind. */
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
 
     kprintf("[mouse] PS/2 driver up (IRQ12 routed via %s, sample=200Hz, "
             "wheel=%s, %u-byte packets)\n",
             irq_using_ioapic() ? "IO APIC" : "PIC",
             g_has_wheel ? "yes" : "no", (unsigned)g_pkt_len);
 }
 
 /* ---- telemetry --------------------------------------------------- */
 
 uint64_t mouse_events_total(void)    { return g_events_total; }
 uint64_t mouse_btn_press_total(void) { return g_btn_press_total; }
 uint64_t mouse_dx_abs_total(void)    { return g_dx_abs_total; }
 uint64_t mouse_dy_abs_total(void)    { return g_dy_abs_total; }
 uint8_t  mouse_last_buttons(void)    { return g_last_buttons; }
 int8_t   mouse_last_dx(void)         { return g_last_dx; }
 int8_t   mouse_last_dy(void)         { return g_last_dy; }
 int8_t   mouse_last_dz(void)         { return g_last_dz; }
 uint64_t mouse_wheel_total(void)     { return g_wheel_total; }
 bool     mouse_ps2_has_wheel(void)   { return g_has_wheel; }

#ifdef WHEEL_SELFTEST
/* ---- wheel decode self-test (-DWHEEL_SELFTEST) --------------------
 *
 * Drives mouse_ps2_handle_byte() with synthetic packets and checks what
 * reaches the shared sink. Exists because the interesting cases are the
 * ones a hand-wave gets wrong: the wheel is a signed NIBBLE, not a byte,
 * and a 5-button mouse puts button state in the high nibble of the same
 * byte -- sign-extending the whole byte turns a button press into a
 * ~17-detent scroll. QEMU's PS/2 model does answer the IntelliMouse
 * knock, but a self-test that does not depend on the emulator (or on a
 * human spinning a wheel) is what keeps this honest on real hardware. */
 static volatile int  g_st_dx, g_st_dy, g_st_dz;
 static volatile uint8_t g_st_btn;
 static volatile int  g_st_calls;

 static void st_cb(int dx, int dy, int dz, uint8_t b) {
     g_st_dx = dx; g_st_dy = dy; g_st_dz = dz; g_st_btn = b; g_st_calls++;
 }

 static void st_feed(uint8_t b0, uint8_t b1, uint8_t b2, uint8_t b3,
                     uint8_t len) {
     g_st_dx = g_st_dy = g_st_dz = 0; g_st_btn = 0; g_st_calls = 0;
     g_pkt_pos = 0;
     g_pkt_len = len;
     mouse_ps2_handle_byte(b0);
     mouse_ps2_handle_byte(b1);
     mouse_ps2_handle_byte(b2);
     if (len == 4) mouse_ps2_handle_byte(b3);
     /* The decoder only ENQUEUES; the callback runs on the flush that the
      * idle loop normally drives. (First version of this test omitted it
      * and read 0 calls for every case.) */
     mouse_flush_pending();
 }

 void mouse_wheel_selftest(void) {
     mouse_event_fn saved = g_cb;
     uint8_t saved_len = g_pkt_len;
     int pass = 0, total = 0;
     mouse_set_callback(st_cb);

     /* bit0: a plain 3-byte packet still decodes, and reports NO wheel. */
     total++;
     st_feed(0x08, 5, 3, 0, 3);
     if (g_st_calls == 1 && g_st_dx == 5 && g_st_dy == -3 && g_st_dz == 0) pass++;
     else kprintf("[wheel] FAIL 3-byte: calls=%d dx=%d dy=%d dz=%d\n",
                  g_st_calls, g_st_dx, g_st_dy, g_st_dz);

     /* bit1: one notch away from the user = +1. */
     total++;
     st_feed(0x08, 0, 0, 0x01, 4);
     if (g_st_calls == 1 && g_st_dz == 1) pass++;
     else kprintf("[wheel] FAIL +1: calls=%d dz=%d\n", g_st_calls, g_st_dz);

     /* bit2: one notch toward the user = -1 (0x0F is -1 in the nibble). */
     total++;
     st_feed(0x08, 0, 0, 0x0F, 4);
     if (g_st_calls == 1 && g_st_dz == -1) pass++;
     else kprintf("[wheel] FAIL -1: calls=%d dz=%d\n", g_st_calls, g_st_dz);

     /* bit3: the nibble floor, -8. */
     total++;
     st_feed(0x08, 0, 0, 0x08, 4);
     if (g_st_calls == 1 && g_st_dz == -8) pass++;
     else kprintf("[wheel] FAIL -8: calls=%d dz=%d\n", g_st_calls, g_st_dz);

     /* bit4: THE TRAP -- buttons 4/5 set in the high nibble alongside a
      * +1 wheel. Byte-wide sign extension would report dz=17. */
     total++;
     st_feed(0x08, 0, 0, 0x31, 4);
     if (g_st_calls == 1 && g_st_dz == 1) pass++;
     else kprintf("[wheel] FAIL btn4/5+wheel: dz=%d (want 1; byte-extend bug gives 0x31=49)\n",
                  g_st_dz);

     /* bit5: motion and wheel in the SAME packet both survive. */
     total++;
     st_feed(0x08, 4, 2, 0x0F, 4);
     if (g_st_calls == 1 && g_st_dx == 4 && g_st_dy == -2 && g_st_dz == -1) pass++;
     else kprintf("[wheel] FAIL combined: dx=%d dy=%d dz=%d\n",
                  g_st_dx, g_st_dy, g_st_dz);

     g_pkt_len = saved_len;
     g_pkt_pos = 0;
     mouse_set_callback(saved);
     kprintf("[wheel] SELFTEST: %d/%d %s (ps2 wheel detected=%s, pkt=%u)\n",
             pass, total, pass == total ? "PASS" : "FAIL",
             g_has_wheel ? "yes" : "no", (unsigned)saved_len);
 }
#endif /* WHEEL_SELFTEST */
