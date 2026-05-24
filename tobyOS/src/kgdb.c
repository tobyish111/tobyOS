/* src/kgdb.c -- GDB remote serial protocol stub (Phase 8).
 *
 * Implements enough of the GDB remote protocol to allow basic debugging:
 *   g - read registers
 *   G - write registers
 *   m - read memory
 *   M - write memory
 *   s - single step
 *   c - continue
 *   ? - query halt reason
 *   k - kill
 *
 * Communication is over the kernel serial port (COM1).
 */

#include <tobyos/kgdb.h>
#include <tobyos/serial.h>
#include <tobyos/printk.h>
#include <tobyos/klibc.h>

static int g_kgdb_active = 0;
static uint64_t *g_saved_regs = 0;
static int g_last_signal = 5;  /* SIGTRAP */

/* Direct COM1 port I/O for GDB (the kernel serial API is write-only) */
#define COM1_PORT 0x3F8

static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile ("outb %0, %1" :: "a"(val), "Nd"(port));
}
static inline uint8_t inb(uint16_t port) {
    uint8_t val;
    __asm__ volatile ("inb %1, %0" : "=a"(val) : "Nd"(port));
    return val;
}

/* Register indices in the saved register array */
#define REG_RAX  0
#define REG_RBX  1
#define REG_RCX  2
#define REG_RDX  3
#define REG_RSI  4
#define REG_RDI  5
#define REG_RBP  6
#define REG_RSP  7
#define REG_R8   8
#define REG_R9   9
#define REG_R10  10
#define REG_R11  11
#define REG_R12  12
#define REG_R13  13
#define REG_R14  14
#define REG_R15  15
#define REG_RIP  16
#define REG_RFLAGS 17
#define NUM_REGS 18

/* GDB register order for x86_64 (simplified -- GDB expects all 24+
 * but we send the important 18). */

/* ---- Serial I/O helpers ---- */

static int gdb_getchar(void) {
    /* Wait for data ready on COM1 */
    while (!(inb(COM1_PORT + 5) & 0x01)) { /* spin */ }
    return inb(COM1_PORT);
}

static void gdb_putchar(int c) {
    /* Wait for transmit ready */
    while (!(inb(COM1_PORT + 5) & 0x20)) { /* spin */ }
    outb(COM1_PORT, (uint8_t)c);
}

/* ---- Hex encoding ---- */

static const char hex_chars[] = "0123456789abcdef";

static int hex_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void put_hex_byte(uint8_t b) {
    gdb_putchar(hex_chars[(b >> 4) & 0xF]);
    gdb_putchar(hex_chars[b & 0xF]);
}

/* ---- Packet I/O ---- */

static char g_pkt_buf[4096];
static int  g_pkt_len;

static int receive_packet(void) {
    int checksum, received_cksum;

    for (;;) {
        /* Wait for '$' */
        int c;
        do { c = gdb_getchar(); } while (c != '$' && c >= 0);
        if (c < 0) return -1;

        g_pkt_len = 0;
        checksum = 0;

        while (1) {
            c = gdb_getchar();
            if (c < 0) return -1;
            if (c == '#') break;
            if (g_pkt_len < (int)sizeof(g_pkt_buf) - 1) {
                g_pkt_buf[g_pkt_len++] = (char)c;
                checksum += c;
            }
        }
        g_pkt_buf[g_pkt_len] = '\0';
        checksum &= 0xFF;

        /* Read checksum */
        int h1 = hex_val((char)gdb_getchar());
        int h2 = hex_val((char)gdb_getchar());
        received_cksum = (h1 << 4) | h2;

        if (checksum == received_cksum) {
            gdb_putchar('+');  /* ACK */
            return g_pkt_len;
        } else {
            gdb_putchar('-');  /* NAK */
        }
    }
}

static void send_packet(const char *data) {
    int checksum = 0;
    int len = 0;
    while (data[len]) {
        checksum += (uint8_t)data[len];
        len++;
    }
    checksum &= 0xFF;

    gdb_putchar('$');
    for (int i = 0; i < len; i++) gdb_putchar(data[i]);
    gdb_putchar('#');
    put_hex_byte((uint8_t)checksum);

    /* Wait for ACK (simplified -- don't retransmit) */
    gdb_getchar();
}

static void send_empty(void) {
    send_packet("");
}

/* ---- Command handlers ---- */

static void handle_query_halt(void) {
    char buf[8];
    buf[0] = 'S';
    buf[1] = hex_chars[(g_last_signal >> 4) & 0xF];
    buf[2] = hex_chars[g_last_signal & 0xF];
    buf[3] = '\0';
    send_packet(buf);
}

static void handle_read_regs(void) {
    if (!g_saved_regs) { send_empty(); return; }

    char buf[NUM_REGS * 16 + 1];
    int pos = 0;
    for (int r = 0; r < NUM_REGS; r++) {
        uint64_t val = g_saved_regs[r];
        for (int i = 0; i < 8; i++) {
            uint8_t b = (uint8_t)(val >> (i * 8));
            buf[pos++] = hex_chars[(b >> 4) & 0xF];
            buf[pos++] = hex_chars[b & 0xF];
        }
    }
    buf[pos] = '\0';
    send_packet(buf);
}

static void handle_write_regs(void) {
    if (!g_saved_regs) { send_packet("E01"); return; }

    const char *p = g_pkt_buf + 1;
    for (int r = 0; r < NUM_REGS && *p; r++) {
        uint64_t val = 0;
        for (int i = 0; i < 8 && p[0] && p[1]; i++) {
            int h = (hex_val(p[0]) << 4) | hex_val(p[1]);
            val |= ((uint64_t)(uint8_t)h) << (i * 8);
            p += 2;
        }
        g_saved_regs[r] = val;
    }
    send_packet("OK");
}

static void handle_read_mem(void) {
    /* Format: mADDR,LENGTH */
    uint64_t addr = 0;
    int len = 0;
    const char *p = g_pkt_buf + 1;

    while (*p && *p != ',') {
        addr = (addr << 4) | hex_val(*p);
        p++;
    }
    if (*p == ',') p++;
    while (*p) {
        len = (len << 4) | hex_val(*p);
        p++;
    }

    if (len > 2000) len = 2000;

    char buf[4096];
    int pos = 0;
    uint8_t *mem = (uint8_t *)addr;

    for (int i = 0; i < len; i++) {
        buf[pos++] = hex_chars[(mem[i] >> 4) & 0xF];
        buf[pos++] = hex_chars[mem[i] & 0xF];
    }
    buf[pos] = '\0';
    send_packet(buf);
}

static void handle_write_mem(void) {
    /* Format: MADDR,LENGTH:DATA */
    uint64_t addr = 0;
    int len = 0;
    const char *p = g_pkt_buf + 1;

    while (*p && *p != ',') {
        addr = (addr << 4) | hex_val(*p);
        p++;
    }
    if (*p == ',') p++;
    while (*p && *p != ':') {
        len = (len << 4) | hex_val(*p);
        p++;
    }
    if (*p == ':') p++;

    uint8_t *mem = (uint8_t *)addr;
    for (int i = 0; i < len && p[0] && p[1]; i++) {
        mem[i] = (uint8_t)((hex_val(p[0]) << 4) | hex_val(p[1]));
        p += 2;
    }
    send_packet("OK");
}

static void handle_continue(void) {
    /* Return from the exception handler to resume execution */
}

static void handle_step(void) {
    /* Set the trap flag (TF) in RFLAGS to single-step */
    if (g_saved_regs) {
        g_saved_regs[REG_RFLAGS] |= (1ULL << 8);  /* TF bit */
    }
}

/* ---- Main debugger loop ---- */

static void kgdb_main_loop(void) {
    handle_query_halt();

    for (;;) {
        if (receive_packet() < 0) break;

        switch (g_pkt_buf[0]) {
        case '?': handle_query_halt(); break;
        case 'g': handle_read_regs(); break;
        case 'G': handle_write_regs(); break;
        case 'm': handle_read_mem(); break;
        case 'M': handle_write_mem(); break;
        case 'c': handle_continue(); return;
        case 's': handle_step(); return;
        case 'k': /* kill -- just continue */ return;
        case 'D': /* detach */
            send_packet("OK");
            g_kgdb_active = 0;
            return;
        default:
            send_empty();
            break;
        }
    }
}

/* ---- Public API ---- */

void kgdb_init(void) {
    g_kgdb_active = 1;
    kprintf("[kgdb] GDB stub ready on COM1 (115200 8N1)\n");
}

void kgdb_breakpoint(void) {
    if (!g_kgdb_active) return;
    __asm__ volatile ("int3");
}

void kgdb_handle_exception(int signal, uint64_t *regs) {
    if (!g_kgdb_active) return;

    g_last_signal = signal;
    g_saved_regs = regs;

    kgdb_main_loop();

    /* Clear TF if we were stepping and now continue */
    if (g_saved_regs) {
        g_saved_regs[REG_RFLAGS] &= ~(1ULL << 8);
    }
    g_saved_regs = 0;
}

int kgdb_active(void) {
    return g_kgdb_active;
}
