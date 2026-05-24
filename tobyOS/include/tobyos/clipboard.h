/* clipboard.h -- Phase 2 M2.7: kernel-side clipboard system.
 *
 * Provides a shared clipboard buffer (up to 64KB) supporting multiple
 * data formats. Accessible from any process via syscall. */

#ifndef TOBYOS_CLIPBOARD_H
#define TOBYOS_CLIPBOARD_H

#include <tobyos/types.h>
#include <tobyos/abi/abi.h>

void clipboard_init(void);

/* Copy data into the clipboard. Returns bytes stored or negative ABI_E*. */
long clipboard_set(const char *data, uint32_t len, uint32_t format);

/* Paste from clipboard into buf. Returns bytes copied or negative ABI_E*. */
long clipboard_get(char *buf, uint32_t buf_sz, uint32_t format);

/* Clear the clipboard. Returns 0. */
long clipboard_clear(void);

#endif /* TOBYOS_CLIPBOARD_H */
