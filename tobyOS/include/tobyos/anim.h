/* anim.h -- Smooth window animation framework.
 *
 * Provides compositor-driven animations for window open/close/minimize/
 * restore transitions. The compositor calls anim_tick() each frame and
 * queries anim_get_transform() before rendering each window to apply
 * scale/translate/alpha effects.
 *
 * All values are integer-only (no FPU/SSE required). Alpha is represented
 * as 0..256 where 256 = fully opaque, 0 = fully transparent.
 */

#ifndef TOBYOS_ANIM_H
#define TOBYOS_ANIM_H

#include <tobyos/types.h>

#define ANIM_MAX_CONCURRENT 8
#define ANIM_ALPHA_OPAQUE   256
#define ANIM_ALPHA_HIDDEN   0

typedef enum {
    ANIM_NONE,
    ANIM_WINDOW_OPEN,
    ANIM_WINDOW_CLOSE,
    ANIM_WINDOW_MINIMIZE,
    ANIM_WINDOW_RESTORE,
    ANIM_FADE_IN,
    ANIM_FADE_OUT,
    ANIM_SLIDE_IN,
    ANIM_SLIDE_OUT,
} anim_type_t;

struct animation {
    int          wid;
    anim_type_t  type;
    uint32_t     start_ms;
    uint32_t     duration_ms;
    int          progress_fp;    /* fixed-point 0..1024 */
    int          x0, y0, w0, h0;
    int          x1, y1, w1, h1;
    int          alpha0, alpha1; /* 0..256 */
    int          active;
};

void anim_init(void);
int  anim_start(int wid, anim_type_t type, uint32_t duration_ms);
void anim_tick(uint32_t now_ms);
int  anim_get_transform(int wid, int *x, int *y, int *w, int *h, int *alpha);
int  anim_is_active(int wid);
int  anim_ease_out_cubic(int t);  /* input/output: 0..1024 */

#endif /* TOBYOS_ANIM_H */
