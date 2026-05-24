/* print.c -- Print spooler with USB printer class support.
 *
 * Manages a queue of print jobs and dispatches them to detected
 * USB printers (class 7). Supports PLAIN_TEXT and POSTSCRIPT formats.
 */

#include <tobyos/types.h>
#include <tobyos/printk.h>
#include <tobyos/klibc.h>
#include <tobyos/slog.h>

#define PRINT_MAX_JOBS       16
#define PRINT_MAX_PRINTERS   4

enum print_format {
    PRINT_FMT_PLAIN_TEXT = 0,
    PRINT_FMT_POSTSCRIPT = 1,
};

enum print_status {
    PRINT_STATUS_QUEUED = 0,
    PRINT_STATUS_PRINTING,
    PRINT_STATUS_DONE,
    PRINT_STATUS_ERROR,
    PRINT_STATUS_CANCELLED,
};

struct print_job {
    bool             active;
    uint32_t         id;
    const void      *data;
    size_t           data_len;
    enum print_format format;
    enum print_status status;
    int              printer_id;
};

struct printer_device {
    bool     present;
    bool     busy;
    char     name[32];
    uint16_t vendor;
    uint16_t product;
    uint8_t  usb_class;
};

static struct print_job g_jobs[PRINT_MAX_JOBS];
static struct printer_device g_printers[PRINT_MAX_PRINTERS];
static uint32_t g_next_job_id = 1;
static bool g_initialized = false;

void print_init(void) {
    if (g_initialized) return;
    memset(g_jobs, 0, sizeof(g_jobs));
    memset(g_printers, 0, sizeof(g_printers));
    g_next_job_id = 1;
    g_initialized = true;
    kprintf("[print] spooler initialized (max jobs=%d, max printers=%d)\n",
            PRINT_MAX_JOBS, PRINT_MAX_PRINTERS);
    SLOG_INFO("print", "print spooler ready");
}

int print_detect_printer(uint16_t vendor, uint16_t product, const char *name) {
    for (int i = 0; i < PRINT_MAX_PRINTERS; i++) {
        if (!g_printers[i].present) {
            g_printers[i].present = true;
            g_printers[i].busy = false;
            g_printers[i].vendor = vendor;
            g_printers[i].product = product;
            g_printers[i].usb_class = 7;
            if (name) {
                size_t len = strlen(name);
                if (len > 31) len = 31;
                memcpy(g_printers[i].name, name, len);
                g_printers[i].name[len] = '\0';
            } else {
                ksnprintf(g_printers[i].name, 32, "USB Printer %d", i);
            }
            kprintf("[print] detected printer %d: %s (vendor=%04x product=%04x)\n",
                    i, g_printers[i].name, vendor, product);
            return i;
        }
    }
    return -1;
}

int print_submit(const void *data, size_t len, int format) {
    if (!data || len == 0) return -1;
    if (!g_initialized) return -1;

    int slot = -1;
    for (int i = 0; i < PRINT_MAX_JOBS; i++) {
        if (!g_jobs[i].active) { slot = i; break; }
    }
    if (slot < 0) return -1;

    struct print_job *j = &g_jobs[slot];
    j->active = true;
    j->id = g_next_job_id++;
    j->data = data;
    j->data_len = len;
    j->format = (enum print_format)format;
    j->status = PRINT_STATUS_QUEUED;
    j->printer_id = -1;

    /* Find available printer */
    for (int i = 0; i < PRINT_MAX_PRINTERS; i++) {
        if (g_printers[i].present && !g_printers[i].busy) {
            j->printer_id = i;
            break;
        }
    }

    kprintf("[print] job %u queued (%zu bytes, format=%d)\n",
            (unsigned)j->id, len, format);
    SLOG_INFO("print", "job %u queued (%zu bytes)", (unsigned)j->id, len);
    return (int)j->id;
}

int print_status(uint32_t job_id) {
    for (int i = 0; i < PRINT_MAX_JOBS; i++) {
        if (g_jobs[i].active && g_jobs[i].id == job_id) {
            return (int)g_jobs[i].status;
        }
    }
    return -1;
}

int print_cancel(uint32_t job_id) {
    for (int i = 0; i < PRINT_MAX_JOBS; i++) {
        if (g_jobs[i].active && g_jobs[i].id == job_id) {
            if (g_jobs[i].status == PRINT_STATUS_QUEUED) {
                g_jobs[i].status = PRINT_STATUS_CANCELLED;
                g_jobs[i].active = false;
                kprintf("[print] job %u cancelled\n", (unsigned)job_id);
                return 0;
            }
            return -1;
        }
    }
    return -1;
}

void print_process_queue(void) {
    if (!g_initialized) return;

    for (int i = 0; i < PRINT_MAX_JOBS; i++) {
        struct print_job *j = &g_jobs[i];
        if (!j->active || j->status != PRINT_STATUS_QUEUED) continue;
        if (j->printer_id < 0) continue;

        struct printer_device *p = &g_printers[j->printer_id];
        if (!p->present || p->busy) continue;

        /* "Print" the job (send to USB bulk endpoint -- stub) */
        p->busy = true;
        j->status = PRINT_STATUS_PRINTING;

        kprintf("[print] printing job %u to %s (%zu bytes)\n",
                (unsigned)j->id, p->name, j->data_len);

        /* In a real driver we would submit a USB bulk transfer here.
         * For now, mark as complete immediately. */
        j->status = PRINT_STATUS_DONE;
        j->active = false;
        p->busy = false;

        SLOG_INFO("print", "job %u complete on %s", (unsigned)j->id, p->name);
    }
}
