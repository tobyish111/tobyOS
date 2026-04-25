/* display.h -- M27A: display introspection + output enumeration.
 *
 * Sits one layer above gfx.c. The gfx module owns the back buffer and
 * the active backend (Limine FB or virtio-gpu). The display module
 * owns the OUTPUTS -- in M27A there is exactly one output (the
 * primary scanout), but the loop is the forward-compat shape M27G
 * fleshes out into multi-monitor.
 *
 * Public responsibilities:
 *   - publish a list of displays via display_enumerate() + the
 *     SYS_DISPLAY_INFO syscall;
 *   - expose live counters (frame count, last-flip timestamp) so
 *     userland can render an instantaneous health snapshot;
 *   - own the primary-display selection knob the desktop uses to
 *     decide where to composite (M27G groundwork);
 *   - register the M27 self-tests with the existing devtest registry
 *     so `drvtest display` Just Works alongside every other M26
 *     subsystem.
 *
 * The module never touches hardware directly. Everything it knows
 * comes from gfx_* getters + a small "register a secondary output"
 * hook drivers will call later.
 */

#ifndef TOBYOS_DISPLAY_H
#define TOBYOS_DISPLAY_H

#include <tobyos/types.h>
#include <tobyos/abi/abi.h>

/* Forward decl -- defined in display.c. */
struct display_output;

/* Capacity for the static output array. Matches ABI cap so the syscall
 * never has to truncate. Multi-monitor groundwork only; today exactly
 * one output is populated by gfx_layer_init(). */
#define DISPLAY_MAX_OUTPUTS  ABI_DISPLAY_MAX_OUTPUTS

/* Initialise the display registry. Must be called AFTER gfx_init() so
 * the primary output's geometry is valid. Idempotent -- safe to call
 * twice (the second call is a no-op). */
void display_init(void);

/* Append (or update) a secondary output record.
 *
 * - If `name` already exists in the registry, that slot is rewritten
 *   in place (atomic geometry / backend swap). The output's current
 *   origin is preserved so a re-registration doesn't reshuffle the
 *   monitor layout.
 * - Otherwise a new slot is allocated and the output is auto-laid-out
 *   to the right of the rightmost existing output (simple horizontal
 *   layout, M27G default). The first registered output sits at (0, 0).
 *
 * Returns the assigned index (0..DISPLAY_MAX_OUTPUTS-1) or -1 on
 * overflow. The caller can override the auto-assigned origin with
 * display_set_origin() after the fact. */
int  display_register_output(const char *name,
                             uint32_t width, uint32_t height,
                             uint32_t pitch_bytes, uint32_t bpp,
                             uint8_t pixel_format, uint8_t backend_id,
                             const char *backend_name);

/* Remove the output at `idx` from the registry. The slot becomes
 * available for a future display_register_output. If the removed
 * output was the primary, a new primary is auto-elected (lowest
 * remaining index), or the registry is left primary-less if the
 * registry is now empty. Returns 0 on success, -1 if `idx` is out
 * of range or the slot was empty. */
int  display_unregister_output(int idx);

/* Override an output's layout origin. Used by future multi-monitor
 * configuration code; in M27G the auto horizontal layout is the
 * default. Returns 0 on success, -1 if `idx` is invalid. */
int  display_set_origin(int idx, int32_t origin_x, int32_t origin_y);

/* Compute the bounding box that contains all registered outputs.
 * Useful for the desktop layer to know its full virtual screen
 * extent. Out parameters are zeroed on empty registry. Returns the
 * number of outputs that contributed (0..DISPLAY_MAX_OUTPUTS). */
int  display_total_bounds(int32_t *out_x, int32_t *out_y,
                          uint32_t *out_w, uint32_t *out_h);

/* Mark the output at `idx` as primary (the one the compositor flips
 * into). A second call demotes the previous primary. Idempotent.
 * Returns 0 on success, -1 if `idx` is out of range. */
int  display_set_primary(int idx);

/* Return the primary output's index, or -1 if no displays are
 * registered yet. The compositor uses this to pick which output's
 * geometry to flip against. */
int  display_primary_index(void);

/* Look up an output by index. Read-only. Returns NULL if `idx` is out
 * of range or the slot is empty. */
const struct display_output *display_get(int idx);

/* Number of registered outputs (primary + secondaries). */
int  display_count(void);

/* Update the last-flip timestamp + frame counter for the primary
 * output. Called from gfx_flip(). Cheap (two stores). */
void display_record_flip(void);

/* Userland-facing enumeration. Fills up to `cap` records into `out`,
 * returns the number written. Used by syscall.c (SYS_DISPLAY_INFO)
 * and by the devlist tool when ABI_DEVT_BUS_DISPLAY is requested. */
int  display_enumerate(struct abi_display_info *out, int cap);

/* M27A boot-time inventory dump (analogous to devtest_dump_kprintf).
 * Prints "[display] N output(s)" plus one line per output. Called
 * from devtest_boot_run via a thin shim so the M27A serial log shows
 * the display stack alongside the M26 peripheral inventory. */
void display_dump_kprintf(void);

/* Devtest self-tests. Registered into the standard devtest registry
 * by display_init() so existing harness paths cover them. Each
 * follows the (msg, cap) -> rc convention from <tobyos/devtest.h>. */
int  display_test_basic    (char *msg, size_t cap);
int  display_test_draw     (char *msg, size_t cap);
int  display_test_render   (char *msg, size_t cap);
int  display_test_alpha    (char *msg, size_t cap);
int  display_test_dirty    (char *msg, size_t cap);
int  display_test_font     (char *msg, size_t cap);
int  display_test_multi    (char *msg, size_t cap);

#endif /* TOBYOS_DISPLAY_H */
