/* crashdump.c -- crash reporting for tobyOS.
 *
 * Saves register state and process info to /system/crashdump.bin,
 * and displays a BSOD-like screen with crash details.
 */

#include <tobyos/crashdump.h>
#include <tobyos/klibc.h>
#include <tobyos/printk.h>
#include <tobyos/gfx.h>
#include <tobyos/vfs.h>

static struct crash_info g_last_crash;
static int               g_has_crash;

void crashdump_init(void)
{
    memset(&g_last_crash, 0, sizeof(g_last_crash));
    g_has_crash = 0;
    kprintf("[crashdump] initialised\n");
}

void crashdump_save(const struct crash_info *info)
{
    if (!info) return;
    memcpy(&g_last_crash, info, sizeof(g_last_crash));
    g_has_crash = 1;

    vfs_write_all("/system/crashdump.bin", info, sizeof(*info));

    kprintf("[crashdump] RIP=%lx RSP=%lx PID=%d '%s'\n",
            info->rip, info->rsp, info->pid, info->process_name);
    kprintf("[crashdump] msg: %s\n", info->message);
}

void crashdump_display(const struct crash_info *info)
{
    if (!info) return;

    if (!gfx_ready()) return;
    int w = (int)gfx_width();
    int h = (int)gfx_height();

    uint32_t blue = 0x0000AA;
    gfx_fill_rect(0, 0, w, h, blue);

    int y = 40;
    int x = 40;

    gfx_draw_text16(x, y, "tobyOS has encountered a critical error", 0xFFFFFF, blue);
    y += 30;
    gfx_draw_text16(x, y, "---------------------------------------", 0xFFFFFF, blue);
    y += 30;

    char line[128];

    ksnprintf(line, sizeof(line), "Error: %s", info->message);
    gfx_draw_text16(x, y, line, 0xFFFFFF, blue);
    y += 20;

    ksnprintf(line, sizeof(line), "Process: %s (PID %d)", info->process_name, info->pid);
    gfx_draw_text16(x, y, line, 0xFFFFFF, blue);
    y += 20;

    ksnprintf(line, sizeof(line), "RIP: 0x%lx", info->rip);
    gfx_draw_text16(x, y, line, 0xFFFFFF, blue);
    y += 20;

    ksnprintf(line, sizeof(line), "RSP: 0x%lx  RBP: 0x%lx", info->rsp, info->rbp);
    gfx_draw_text16(x, y, line, 0xFFFFFF, blue);
    y += 20;

    ksnprintf(line, sizeof(line), "RAX: 0x%lx  RBX: 0x%lx  RCX: 0x%lx", info->rax, info->rbx, info->rcx);
    gfx_draw_text16(x, y, line, 0xFFFFFF, blue);
    y += 20;

    ksnprintf(line, sizeof(line), "RDX: 0x%lx  RSI: 0x%lx  RDI: 0x%lx", info->rdx, info->rsi, info->rdi);
    gfx_draw_text16(x, y, line, 0xFFFFFF, blue);
    y += 20;

    ksnprintf(line, sizeof(line), "CR2: 0x%lx  CR3: 0x%lx  ERR: 0x%x", info->cr2, info->cr3, info->error_code);
    gfx_draw_text16(x, y, line, 0xFFFFFF, blue);
    y += 30;

    gfx_draw_text16(x, y, "Press any key to restart", 0xCCCCCC, blue);

    gfx_flip();
}
