/* secureboot.c -- Kernel image signature verification (Ed25519).
 *
 * Verifies kernel image signatures at boot using Ed25519 via Monocypher.
 * Maintains a trust database of public keys stored in /system/keys/.
 * Includes TPM stubs for future integration.
 */

#include <tobyos/types.h>
#include <tobyos/printk.h>
#include <tobyos/klibc.h>
#include <tobyos/heap.h>
#include <tobyos/vfs.h>
#include <tobyos/slog.h>
#include "monocypher.h"
#include "monocypher-ed25519.h"

#define SECBOOT_MAX_KEYS     8
#define SECBOOT_KEY_SIZE     32
#define SECBOOT_SIG_SIZE     64
#define SECBOOT_KEY_ID_MAX   32

struct secboot_key {
    bool    valid;
    uint8_t pubkey[SECBOOT_KEY_SIZE];
    char    id[SECBOOT_KEY_ID_MAX];
};

static struct secboot_key g_keys[SECBOOT_MAX_KEYS];
static int g_key_count = 0;
static bool g_initialized = false;

/* TPM state (stubs) */
static bool g_tpm_present = false;
static uint8_t g_pcr[24][32]; /* 24 PCRs, each 32 bytes (SHA-256 size) */

void tpm_init(void) {
    g_tpm_present = false;
    memset(g_pcr, 0, sizeof(g_pcr));
    kprintf("[secureboot] TPM: not present (stub)\n");
    SLOG_INFO("secboot", "TPM init (stub, no hardware detected)");
}

void tpm_extend_pcr(int pcr_index, const uint8_t *data, size_t len) {
    if (pcr_index < 0 || pcr_index >= 24) return;
    (void)data; (void)len;
    kprintf("[secureboot] TPM: extend PCR[%d] (stub, %zu bytes)\n",
            pcr_index, len);
    SLOG_DEBUG("secboot", "TPM extend PCR[%d] len=%zu (stub)", pcr_index, len);
}

static int load_trust_db(void) {
    void *data = NULL;
    size_t size = 0;
    int rc = vfs_read_all("/system/keys/trust.db", &data, &size);
    if (rc != 0 || !data) {
        kprintf("[secureboot] no trust database found at /system/keys/trust.db\n");
        return -1;
    }

    /* trust.db format: lines of "KEY_ID hex_pubkey\n" */
    const char *p = (const char *)data;
    const char *end = p + size;

    while (p < end && g_key_count < SECBOOT_MAX_KEYS) {
        while (p < end && (*p == '\n' || *p == '\r')) p++;
        if (p >= end) break;

        const char *line = p;
        while (p < end && *p != '\n' && *p != '\r') p++;

        size_t line_len = (size_t)(p - line);
        if (line_len < 10) continue;

        /* Parse: find space separator */
        const char *sp = line;
        while (sp < line + line_len && *sp != ' ' && *sp != '\t') sp++;
        if (sp >= line + line_len) continue;

        size_t id_len = (size_t)(sp - line);
        if (id_len >= SECBOOT_KEY_ID_MAX) id_len = SECBOOT_KEY_ID_MAX - 1;

        struct secboot_key *k = &g_keys[g_key_count];
        memcpy(k->id, line, id_len);
        k->id[id_len] = '\0';

        /* Skip whitespace to hex key */
        while (sp < line + line_len && (*sp == ' ' || *sp == '\t')) sp++;

        /* Parse hex key (64 hex chars = 32 bytes) */
        size_t hex_len = (size_t)((line + line_len) - sp);
        if (hex_len < SECBOOT_KEY_SIZE * 2) continue;

        for (int i = 0; i < SECBOOT_KEY_SIZE; i++) {
            uint8_t hi = 0, lo = 0;
            char ch = sp[i * 2];
            if (ch >= '0' && ch <= '9') hi = (uint8_t)(ch - '0');
            else if (ch >= 'a' && ch <= 'f') hi = (uint8_t)(ch - 'a' + 10);
            else if (ch >= 'A' && ch <= 'F') hi = (uint8_t)(ch - 'A' + 10);
            ch = sp[i * 2 + 1];
            if (ch >= '0' && ch <= '9') lo = (uint8_t)(ch - '0');
            else if (ch >= 'a' && ch <= 'f') lo = (uint8_t)(ch - 'a' + 10);
            else if (ch >= 'A' && ch <= 'F') lo = (uint8_t)(ch - 'A' + 10);
            k->pubkey[i] = (uint8_t)((hi << 4) | lo);
        }
        k->valid = true;
        g_key_count++;
    }

    kfree(data);
    kprintf("[secureboot] loaded %d trusted keys\n", g_key_count);
    return 0;
}

void secureboot_init(void) {
    if (g_initialized) return;
    g_key_count = 0;
    memset(g_keys, 0, sizeof(g_keys));
    load_trust_db();
    tpm_init();
    g_initialized = true;
    SLOG_INFO("secboot", "secure boot initialized (%d keys)", g_key_count);
}

bool secureboot_verify(const void *image_data, size_t image_size,
                       const uint8_t *signature) {
    if (!image_data || image_size == 0 || !signature) {
        SLOG_ERROR("secboot", "verify: invalid parameters");
        return false;
    }

    for (int i = 0; i < g_key_count; i++) {
        if (!g_keys[i].valid) continue;

        int rc = crypto_ed25519_check(signature, g_keys[i].pubkey,
                                      image_data, image_size);
        if (rc == 0) {
            kprintf("[secureboot] image verified OK with key '%s'\n",
                    g_keys[i].id);
            SLOG_INFO("secboot", "image verified (key=%s, size=%zu)",
                      g_keys[i].id, image_size);
            tpm_extend_pcr(0, (const uint8_t *)image_data, image_size);
            return true;
        }
    }

    kprintf("[secureboot] VERIFICATION FAILED: no trusted key matched\n");
    SLOG_ERROR("secboot", "image verification FAILED (size=%zu, tried %d keys)",
               image_size, g_key_count);
    return false;
}

int secureboot_add_key(const char *id, const uint8_t *pubkey) {
    if (!id || !pubkey) return -1;
    if (g_key_count >= SECBOOT_MAX_KEYS) return -1;

    struct secboot_key *k = &g_keys[g_key_count];
    size_t id_len = strlen(id);
    if (id_len >= SECBOOT_KEY_ID_MAX) id_len = SECBOOT_KEY_ID_MAX - 1;
    memcpy(k->id, id, id_len);
    k->id[id_len] = '\0';
    memcpy(k->pubkey, pubkey, SECBOOT_KEY_SIZE);
    k->valid = true;
    g_key_count++;
    return 0;
}
