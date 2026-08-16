/* snd_pcm.c -- /dev/snd: the Linux ALSA PCM ABI over the HDA driver.
 *
 * Audio slice 3. The counterpart of linux_drm.c: where that exposes a real
 * DRM render node so unmodified Mesa can drive our GPU, this exposes real
 * ALSA character devices so unmodified alsa-lib -- and therefore Chromium,
 * which dlopens libasound.so.2 and resolves 37 snd_pcm_* symbols by name --
 * can drive our HD Audio codec.
 *
 * Nodes:
 *   /dev/snd/controlC0   card enumeration (which PCM devices exist)
 *   /dev/snd/pcmC0D0p    playback substream
 *
 * SCOPE. Deliberately the non-mmap path only:
 *   - SNDRV_PCM_IOCTL_WRITEI_FRAMES for the data, not mmap'd buffers.
 *     alsa-lib falls back to writei whenever SNDRV_PCM_INFO_MMAP is absent
 *     from the info flags, so simply not advertising mmap steers every
 *     client onto the copy path -- which is the one our cyclic HDA ring
 *     already implements (audio_hda_pcm_write).
 *   - SNDRV_PCM_IOCTL_SYNC_PTR for the hardware pointer, which is how a
 *     non-mmap client learns hw_ptr/appl_ptr.
 *   - One card, one device, one substream, S16_LE, 1-2 channels, and the
 *     rates the HDA format word can express.
 *
 * THE ABI IS SIZE-CHECKED AT COMPILE TIME. Every _IOC request number
 * encodes sizeof(its argument struct) in bits 16..29, so a struct that is
 * laid out wrongly here cannot match the number a real alsa-lib sends. The
 * _Static_asserts below tie each struct to the published constant; if one
 * fires, the layout is wrong, not the constant.
 */

#include <tobyos/types.h>
#include <tobyos/klibc.h>
#include <tobyos/printk.h>
#include <tobyos/heap.h>
#include <tobyos/file.h>
#include <tobyos/proc.h>
#include <tobyos/uaccess.h>
#include <tobyos/audio_hda.h>
#include <tobyos/abi/abi.h>

/* ============================================================
 * ioctl encoding
 * ============================================================ */
#define IOC_NR(x)    ((unsigned)((x) & 0xFFu))
#define IOC_TYPE(x)  ((unsigned)(((x) >> 8) & 0xFFu))
#define IOC_SIZE(x)  ((unsigned)(((x) >> 16) & 0x3FFFu))

#define SNDRV_PCM_IOCTL_TYPE  'A'
#define SNDRV_CTL_IOCTL_TYPE  'U'

/* PCM ioctl nrs (type 'A'). */
#define PCM_NR_PVERSION       0x00
#define PCM_NR_INFO           0x01
#define PCM_NR_TSTAMP         0x02
#define PCM_NR_TTSTAMP        0x03
#define PCM_NR_USER_PVERSION  0x04
#define PCM_NR_HW_REFINE      0x10
#define PCM_NR_HW_PARAMS      0x11
#define PCM_NR_HW_FREE        0x12
#define PCM_NR_SW_PARAMS      0x13
#define PCM_NR_STATUS         0x20
#define PCM_NR_DELAY          0x21
#define PCM_NR_HWSYNC         0x22
#define PCM_NR_SYNC_PTR       0x23
#define PCM_NR_STATUS_EXT     0x24
#define PCM_NR_CHANNEL_INFO   0x32
#define PCM_NR_PREPARE        0x40
#define PCM_NR_RESET          0x41
#define PCM_NR_START          0x42
#define PCM_NR_DROP           0x43
#define PCM_NR_DRAIN          0x44
#define PCM_NR_PAUSE          0x45
#define PCM_NR_WRITEI_FRAMES  0x50
#define PCM_NR_READI_FRAMES   0x51

/* Control ioctl nrs (type 'U'). */
#define CTL_NR_PVERSION            0x00
#define CTL_NR_CARD_INFO           0x01
#define CTL_NR_PCM_NEXT_DEVICE     0x30
#define CTL_NR_PCM_INFO            0x31
#define CTL_NR_PCM_PREFER_SUBDEV   0x32

/* We report protocol 2.0.15, which is what alsa-lib 1.2.x expects to see. */
#define SNDRV_PCM_VERSION   ((2 << 16) | (0 << 8) | 15)
#define SNDRV_CTL_VERSION   ((2 << 16) | (0 << 8) | 9)

/* ============================================================
 * UAPI structures (linux/uapi/sound/asound.h)
 * ============================================================ */

struct snd_mask { uint32_t bits[8]; };          /* 256-bit mask */

struct snd_interval {
    unsigned int min, max;
    unsigned int openmin:1, openmax:1, integer:1, empty:1;
};

/* hw_params parameter indices. Masks 0..2, intervals 8..19. */
#define HWP_ACCESS         0
#define HWP_FORMAT         1
#define HWP_SUBFORMAT      2
#define HWP_FIRST_MASK     0
#define HWP_LAST_MASK      2
#define HWP_SAMPLE_BITS    8
#define HWP_FRAME_BITS     9
#define HWP_CHANNELS      10
#define HWP_RATE          11
#define HWP_PERIOD_TIME   12
#define HWP_PERIOD_SIZE   13
#define HWP_PERIOD_BYTES  14
#define HWP_PERIODS       15
#define HWP_BUFFER_TIME   16
#define HWP_BUFFER_SIZE   17
#define HWP_BUFFER_BYTES  18
#define HWP_TICK_TIME     19
#define HWP_FIRST_INTERVAL 8
#define HWP_LAST_INTERVAL 19

#define MASK_IDX(p)     ((p) - HWP_FIRST_MASK)
#define IVAL_IDX(p)     ((p) - HWP_FIRST_INTERVAL)

struct snd_pcm_hw_params {
    unsigned int        flags;
    struct snd_mask     masks[HWP_LAST_MASK - HWP_FIRST_MASK + 1];
    struct snd_mask     mres[5];
    struct snd_interval intervals[HWP_LAST_INTERVAL - HWP_FIRST_INTERVAL + 1];
    struct snd_interval ires[9];
    unsigned int        rmask;
    unsigned int        cmask;
    unsigned int        info;
    unsigned int        msbits;
    unsigned int        rate_num;
    unsigned int        rate_den;
    uint64_t            fifo_size;
    unsigned char       reserved[64];
};
/* SNDRV_PCM_IOCTL_HW_PARAMS == 0xc2604111 -> size field 0x260 == 608. */
_Static_assert(sizeof(struct snd_pcm_hw_params) == 608,
               "snd_pcm_hw_params must be 608 bytes (see 0xc2604111)");

struct snd_pcm_sw_params {
    int          tstamp_mode;
    unsigned int period_step;
    unsigned int sleep_min;
    uint64_t     avail_min;
    uint64_t     xfer_align;
    uint64_t     start_threshold;
    uint64_t     stop_threshold;
    uint64_t     silence_threshold;
    uint64_t     silence_size;
    uint64_t     boundary;
    unsigned int proto;
    unsigned int tstamp_type;
    unsigned char reserved[56];
};
/* SNDRV_PCM_IOCTL_SW_PARAMS == 0xc0884113 -> 0x88 == 136. */
_Static_assert(sizeof(struct snd_pcm_sw_params) == 136,
               "snd_pcm_sw_params must be 136 bytes (see 0xc0884113)");

struct snd_xferi {
    int64_t     result;
    uint64_t    buf;          /* userspace pointer */
    uint64_t    frames;
};
/* SNDRV_PCM_IOCTL_WRITEI_FRAMES == 0x40184150 -> 0x18 == 24. */
_Static_assert(sizeof(struct snd_xferi) == 24,
               "snd_xferi must be 24 bytes (see 0x40184150)");

struct snd_pcm_info {
    unsigned int  device;
    unsigned int  subdevice;
    int           stream;
    int           card;
    unsigned char id[64];
    unsigned char name[80];
    unsigned char subname[32];
    int           dev_class;
    int           dev_subclass;
    unsigned int  subdevices_count;
    unsigned int  subdevices_avail;
    unsigned char sync[16];
    unsigned char reserved[64];
};
/* SNDRV_PCM_IOCTL_INFO == 0x81204101 -> 0x120 == 288. */
_Static_assert(sizeof(struct snd_pcm_info) == 288,
               "snd_pcm_info must be 288 bytes (see 0x81204101)");

struct snd_ctl_card_info {
    int           card;
    int           pad;
    unsigned char id[16];
    unsigned char driver[16];
    unsigned char name[32];
    unsigned char longname[80];
    unsigned char reserved_[16];
    unsigned char mixername[80];
    unsigned char components[128];
};
/* SNDRV_CTL_IOCTL_CARD_INFO == 0x81785501 -> 0x178 == 376. */
_Static_assert(sizeof(struct snd_ctl_card_info) == 376,
               "snd_ctl_card_info must be 376 bytes (see 0x81785501)");

struct snd_pcm_mmap_status {
    int          state;
    int          pad1;
    uint64_t     hw_ptr;
    int64_t      tstamp_sec, tstamp_nsec;
    int          suspended_state;
    int          pad2;
    int64_t      audio_tstamp_sec, audio_tstamp_nsec;
};

struct snd_pcm_mmap_control {
    uint64_t     appl_ptr;
    uint64_t     avail_min;
};

struct snd_pcm_sync_ptr {
    unsigned int flags;
    unsigned int pad1;
    union { struct snd_pcm_mmap_status  status;  unsigned char rsv[64]; } s;
    union { struct snd_pcm_mmap_control control; unsigned char rsv[64]; } c;
};
/* SNDRV_PCM_IOCTL_SYNC_PTR == 0xc0884123 -> 0x88 == 136. */
_Static_assert(sizeof(struct snd_pcm_sync_ptr) == 136,
               "snd_pcm_sync_ptr must be 136 bytes (see 0xc0884123)");

#define SYNC_PTR_HWSYNC     (1u << 0)
#define SYNC_PTR_APPL       (1u << 1)
#define SYNC_PTR_AVAIL_MIN  (1u << 2)

/* Stream states. */
#define ST_OPEN         0
#define ST_SETUP        1
#define ST_PREPARED     2
#define ST_RUNNING      3
#define ST_XRUN         4
#define ST_DRAINING     5
#define ST_PAUSED       6

/* Access / format bits we advertise. */
#define ACCESS_MMAP_INTERLEAVED     0
#define ACCESS_RW_INTERLEAVED       3
#define FORMAT_S16_LE               2

/* snd_pcm_info.stream */
#define STREAM_PLAYBACK 0

/* info flags reported in hw_params.info. Note the DELIBERATE absence of
 * SNDRV_PCM_INFO_MMAP (0x01): alsa-lib picks writei when mmap is not
 * advertised, and writei is the path our ring implements. */
#define INFO_INTERLEAVED    0x00000100
#define INFO_BLOCK_TRANSFER 0x00010000
#define INFO_PAUSE          0x00080000
#define INFO_RESUME         0x00400000

/* ============================================================
 * Substream state (one card, one device, one substream)
 * ============================================================ */
static struct {
    bool     in_use;
    int      state;
    uint32_t rate;
    uint32_t channels;
    uint32_t frame_bytes;
    uint64_t period_size;      /* frames */
    uint64_t buffer_size;      /* frames */
    uint64_t appl_ptr;         /* frames written by the app  */
    uint64_t pushed;           /* frames WE handed the device */
    uint64_t hw_base;          /* frames the DAC had consumed at start */
    uint64_t boundary;
    uint64_t avail_min;
    uint64_t start_threshold;
    bool     started;
    uint64_t xruns;
} g_sub;

/* Linux EBADFD. alsa-lib checks for this SPECIFICALLY to mean "the stream
 * is in the wrong state" (snd_pcm_writei on a non-prepared stream), and
 * maps other errnos to different recovery paths, so returning EINVAL here
 * would send it down the wrong branch. Not in abi.h, hence local. */
#define SND_EBADFD 77

/* klibc has strncmp but no strncpy, and these are fixed-size UAPI name
 * fields that must stay NUL-terminated. */
static void sncpy(char *dst, const char *src, size_t cap) {
    size_t i = 0;
    if (cap == 0) return;
    for (; i + 1 < cap && src[i]; i++) dst[i] = src[i];
    dst[i] = '\0';
}

static void mask_zero(struct snd_mask *m) { memset(m, 0, sizeof(*m)); }
static void mask_set(struct snd_mask *m, unsigned bit) {
    if (bit < 256) m->bits[bit >> 5] |= (1u << (bit & 31));
}
static bool mask_test(const struct snd_mask *m, unsigned bit) {
    return bit < 256 && (m->bits[bit >> 5] & (1u << (bit & 31))) != 0;
}
static bool mask_empty(const struct snd_mask *m) {
    for (int i = 0; i < 8; i++) if (m->bits[i]) return false;
    return true;
}
/* Intersect `m` with the single-bit set we support; returns false if the
 * result is empty (the caller's request cannot be satisfied). */

/* Set by the ioctl entry points so a rejection can name itself. The refine
 * runs from both HW_REFINE -- where narrowing to empty is a normal
 * negotiation outcome and must stay quiet -- and HW_PARAMS, where it is a
 * failure the client reports as a bare EINVAL with no further detail. */
static bool g_trace_refine;

/* cmask accumulation.
 *
 * ALSA's cmask is a PER-PARAMETER bitmask of what the kernel changed (bit
 * N == parameter N), and alsa-lib uses it to decide which parameters to
 * re-propagate through its own constraint graph. Reporting the blanket
 * `changed ? rmask : 0` says "all twenty parameters moved" on every call,
 * which is never true and gives the library no usable information.
 *
 * The bases are recorded at the top of hw_refine so the refine helpers can
 * recover a parameter's index from its address, rather than threading an
 * index through every call site. */
static const struct snd_interval *g_ival_base;
static const struct snd_mask     *g_mask_base;
static unsigned                   g_cmask_acc;

static void cmask_note_ival(const struct snd_interval *v) {
    if (!g_ival_base) return;
    g_cmask_acc |= 1u << (unsigned)((v - g_ival_base) + HWP_FIRST_INTERVAL);
}
static void cmask_note_mask(const struct snd_mask *m) {
    if (!g_mask_base) return;
    g_cmask_acc |= 1u << (unsigned)((m - g_mask_base) + HWP_FIRST_MASK);
}

static bool mask_refine_to(struct snd_mask *m, const unsigned *allowed,
                           int n, bool *changed) {
    struct snd_mask out;
    mask_zero(&out);
    for (int i = 0; i < n; i++)
        if (mask_test(m, allowed[i])) mask_set(&out, allowed[i]);
    if (mask_empty(&out)) {
        if (g_trace_refine)
            kprintf("[snd] refine REJECT mask: client offered none of our "
                    "%d supported value(s)\n", n);
        return false;
    }
    if (memcmp(&out, m, sizeof(out)) != 0) {
        *m = out; *changed = true; cmask_note_mask(m);
    }
    return true;
}

/* Narrow `v` to [lo,hi]. `what` names the parameter so a rejection under
 * g_trace_refine identifies itself instead of surfacing as a bare EINVAL. */
static bool ival_refine_n(const char *what, struct snd_interval *v,
                          unsigned lo, unsigned hi, bool *changed) {
    unsigned omin = v->min, omax = v->max;
    if (v->min < lo) { v->min = lo; v->openmin = 0; }
    if (v->max > hi) { v->max = hi; v->openmax = 0; }

    /* Close open bounds by stepping inward -- but NEVER let that alone
     * empty the interval.
     *
     * Stepping both ends of [3..4] gives min=4, max=3, which the old code
     * reported as unsatisfiable. That is a bound-flag convention, not a
     * real conflict: chrome's `plug` layer asked for periods [3..4]
     * against our [2..2048] and was refused an obviously satisfiable
     * request, so it abandoned the configuration and never issued
     * HW_PARAMS at all -- the whole of slice 5's silence. Real ALSA
     * normalizes open bounds only for `integer` intervals and does not
     * synthesize emptiness this way. When stepping would empty it, keep
     * the closed reading instead; being permissive costs nothing, since
     * the exact values that finally arrive at HW_PARAMS are what we
     * actually honour. */
    {
        unsigned smin = v->min, smax = v->max;
        if (v->openmin) smin++;
        if (v->openmax) { if (smax == 0) return false; smax--; }
        if (smin <= smax) { v->min = smin; v->max = smax; }
        v->openmin = v->openmax = 0;
    }
    if (v->min > v->max) {
        v->empty = 1;
        if (g_trace_refine)
            kprintf("[snd] refine REJECT %s: asked [%u..%u], hw [%u..%u]\n",
                    what, omin, omax, lo, hi);
        return false;
    }
    if (v->min != omin || v->max != omax) { *changed = true; cmask_note_ival(v); }
    return true;
}
#define ival_refine(v, lo, hi, ch) ival_refine_n(#v, (v), (lo), (hi), (ch))


static bool ival_is_exact(const struct snd_interval *v) {
    return v->min == v->max;
}

/* Saturating 32-bit helpers for the constraint arithmetic below. Every
 * product here can overflow a u32 for wide-open intervals (period_size up
 * to UINT_MAX times a rate), and an overflow that wraps would silently
 * narrow an interval to nonsense rather than to "unbounded". */
#define IVAL_MAX 0xFFFFFFFFu

static unsigned sat_mul(unsigned a, unsigned b) {
    uint64_t r = (uint64_t)a * b;
    return r > IVAL_MAX ? IVAL_MAX : (unsigned)r;
}
/* floor(a*b/c) for a lower bound, saturating. */
static unsigned mul_div_lo(unsigned a, unsigned b, unsigned c) {
    if (c == 0) return 0;
    uint64_t r = (uint64_t)a * b / c;
    return r > IVAL_MAX ? IVAL_MAX : (unsigned)r;
}
/* ceil(a*b/c) for an upper bound, saturating. Rounding UP matters: a
 * truncated upper bound can exclude the very value the client is about to
 * request and turn a satisfiable configuration into EINVAL. */
static unsigned mul_div_hi(unsigned a, unsigned b, unsigned c) {
    if (c == 0) return IVAL_MAX;
    uint64_t r = ((uint64_t)a * b + (c - 1)) / c;
    return r > IVAL_MAX ? IVAL_MAX : (unsigned)r;
}

/* ============================================================
 * hw_params refinement
 *
 * alsa-lib calls HW_REFINE repeatedly, each time narrowing what it asks
 * for, and expects the kernel to return the intersection with what the
 * hardware can do -- plus the derived relationships between parameters
 * (frame_bits = sample_bits * channels, buffer_bytes = buffer_size *
 * frame_bits/8, buffer_size = period_size * periods, and the *_time
 * parameters in microseconds). Getting those relationships wrong is the
 * classic way a PCM device "opens" and then produces nothing.
 * ============================================================ */
static int hw_refine(struct snd_pcm_hw_params *p) {
    g_ival_base = p->intervals;
    g_mask_base = p->masks;
    g_cmask_acc = 0;
    static const unsigned ok_access[] = { ACCESS_RW_INTERLEAVED };
    static const unsigned ok_format[] = { FORMAT_S16_LE };
    static const unsigned ok_subfmt[] = { 0 /* STD */ };
    bool changed = false;

    if (!mask_refine_to(&p->masks[MASK_IDX(HWP_ACCESS)],
                        ok_access, 1, &changed)) return -ABI_EINVAL;
    if (!mask_refine_to(&p->masks[MASK_IDX(HWP_FORMAT)],
                        ok_format, 1, &changed)) return -ABI_EINVAL;
    if (!mask_refine_to(&p->masks[MASK_IDX(HWP_SUBFORMAT)],
                        ok_subfmt, 1, &changed)) return -ABI_EINVAL;

    struct snd_interval *chan   = &p->intervals[IVAL_IDX(HWP_CHANNELS)];
    struct snd_interval *rate   = &p->intervals[IVAL_IDX(HWP_RATE)];
    struct snd_interval *sbits  = &p->intervals[IVAL_IDX(HWP_SAMPLE_BITS)];
    struct snd_interval *fbits  = &p->intervals[IVAL_IDX(HWP_FRAME_BITS)];
    struct snd_interval *psize  = &p->intervals[IVAL_IDX(HWP_PERIOD_SIZE)];
    struct snd_interval *pbytes = &p->intervals[IVAL_IDX(HWP_PERIOD_BYTES)];
    struct snd_interval *periods= &p->intervals[IVAL_IDX(HWP_PERIODS)];
    struct snd_interval *bsize  = &p->intervals[IVAL_IDX(HWP_BUFFER_SIZE)];
    struct snd_interval *bbytes = &p->intervals[IVAL_IDX(HWP_BUFFER_BYTES)];
    struct snd_interval *ptime  = &p->intervals[IVAL_IDX(HWP_PERIOD_TIME)];
    struct snd_interval *btime  = &p->intervals[IVAL_IDX(HWP_BUFFER_TIME)];
    struct snd_interval *tick   = &p->intervals[IVAL_IDX(HWP_TICK_TIME)];

    /* S16_LE only, so sample_bits is pinned. */
    if (!ival_refine(sbits, 16, 16, &changed))  return -ABI_EINVAL;
    if (!ival_refine(chan, 1, 2, &changed))     return -ABI_EINVAL;
    /* Rates the HDA format word can express (hda_fmt_word in audio_hda.c). */
    if (!ival_refine(rate, 8000, 96000, &changed)) return -ABI_EINVAL;

    /* Geometry, and it MUST be self-consistent with the real ring.
     *
     * The HDA cyclic buffer is HDA_PCM_PERIODS * HDA_PCM_PERIOD_BYTES =
     * 16384 B, i.e. 4096 frames at stereo S16. Advertising buffer_size up
     * to 65536 frames -- 16x more than the hardware holds -- was a lie
     * that our own constraints then contradicted: alsa-lib pinned
     * period_size to 240, asked for the 65536 we had promised, and that
     * needs 273 periods against a periods cap of 32. The refine rejected
     * buffer_size against [1920..7680] and the library reported a bare
     * EINVAL from snd_pcm_hw_params, having never issued HW_PARAMS.
     *
     * So: bound buffer_size by what the device really has, and keep the
     * three bounds mutually satisfiable --
     *   psize.min * periods.min <= bsize.min, and
     *   psize.max * periods.max >= bsize.max. */
    /* NOTE the buffer_size cap is NOT the DMA ring size.
     *
     * ALSA's buffer_size is the CLIENT's logical ring, not our hardware
     * one: audio_hda_pcm_write already short-returns when the DMA buffer
     * is full, so a client may reason about a larger buffer perfectly
     * safely. Tying the advertised maximum to the physical 4096 frames
     * looked honest but was actively harmful -- alsa-lib pinned
     * period_size=240 from period_time, then asked for the 4096 we had
     * just offered, and 4096/240 is not an integer, so the product rule
     * refused it against 240*17=4080. Divisibility cannot be expressed in
     * interval arithmetic, so the cap must be loose enough that some
     * (period_size, periods) pair always exists. */
    unsigned max_frames = 65536;
    (void)HDA_PCM_RING_BYTES;
    /* periods must reach buffer_size / smallest period_size, or the
     * product bound contradicts the buffer_size we advertise. With a cap
     * of 32, a client that pinned period_size=240 and then asked for the
     * 4096-frame buffer we had just offered was refused against
     * 240*17=4080 -- the request was only unsatisfiable because the cap
     * was, and alsa-lib had no way to know that when it chose 240. */
    unsigned min_psize = 32;
    if (!ival_refine(psize, min_psize, max_frames / 2, &changed))
        return -ABI_EINVAL;
    if (!ival_refine(periods, 2, max_frames / min_psize, &changed))
        return -ABI_EINVAL;
    if (!ival_refine(bsize, 64, max_frames, &changed))
        return -ABI_EINVAL;

    /* ---- constraint propagation ------------------------------------
     * The derived parameters must be narrowed as RANGES, not merely
     * pinned once everything else happens to be exact.
     *
     * This is what alsa-lib's convergence loop actually depends on: it
     * repeatedly asks "given what I have fixed so far, what is still
     * possible for X?" and closes in. Leaving period_time / buffer_time /
     * period_bytes / buffer_bytes at [0 .. UINT_MAX] -- which the
     * exact-only version did, since rate and the sizes are ranges for most
     * of the negotiation -- means the answer is always "anything", the
     * space never closes, and the library gives up with EINVAL WITHOUT
     * ever issuing HW_PARAMS. That is precisely what was observed: 30+
     * HW_REFINE calls all returning 0, then a bare EINVAL from
     * snd_pcm_hw_params with no ioctl behind it.
     *
     * Rules (Linux sound/core/pcm_native.c):
     *   frame_bits   = sample_bits * channels
     *   period_bytes = period_size * frame_bits / 8
     *   buffer_bytes = buffer_size * frame_bits / 8
     *   buffer_size  = period_size * periods
     *   period_time  = period_size * 1e6 / rate
     *   buffer_time  = buffer_size * 1e6 / rate
     * applied in both directions and iterated to a fixed point. */
    for (int pass = 0; pass < 6; pass++) {
        bool before = changed;

        if (!ival_refine(fbits, 16u * chan->min, 16u * chan->max, &changed))
            return -ABI_EINVAL;

        /* sizes <-> bytes */
        if (!ival_refine(pbytes, mul_div_lo(psize->min, fbits->min, 8),
                                 mul_div_hi(psize->max, fbits->max, 8),
                                 &changed)) return -ABI_EINVAL;
        if (!ival_refine(bbytes, mul_div_lo(bsize->min, fbits->min, 8),
                                 mul_div_hi(bsize->max, fbits->max, 8),
                                 &changed)) return -ABI_EINVAL;
        if (fbits->max)
            if (!ival_refine(psize, mul_div_lo(pbytes->min, 8, fbits->max),
                                    mul_div_hi(pbytes->max, 8, fbits->min),
                                    &changed)) return -ABI_EINVAL;

        /* buffer_size <-> period_size * periods */
        if (!ival_refine(bsize, sat_mul(psize->min, periods->min),
                                sat_mul(psize->max, periods->max),
                                &changed)) return -ABI_EINVAL;
        /* CEIL for the upper bound. Plain division truncates, and the
         * truncation propagates straight back into buffer_size on the
         * next pass: with period_size pinned at 240 and buffer_size
         * 65536, floor gave periods<=273, hence buffer_size<=240*273=
         * 65520 -- sixteen frames short of the very value the client had
         * just been offered, so a satisfiable request was refused. */
        if (psize->max)
            if (!ival_refine(periods, bsize->min / psize->max,
                                      mul_div_hi(bsize->max, 1, psize->min),
                                      &changed)) return -ABI_EINVAL;

        /* frames <-> microseconds, with ONE MICROSECOND of slack on each
         * side.
         *
         * The conversion is lossy in both directions and alsa-lib does its
         * own rounding, so an exact answer here can contradict the library
         * over a single unit and collapse the interval. Observed: with
         * period_size pinned at 240 and rate 48000, the true period_time
         * is exactly 5000 us; the library asked for >= 5001 and our
         * [5000..5000] left an empty intersection -- EINVAL for a
         * configuration that is perfectly achievable. Widening by a unit
         * costs nothing (we honour period_size, not period_time) and
         * absorbs the disagreement. */
        if (rate->max) {
            unsigned lo = mul_div_lo(psize->min, 1000000u, rate->max);
            unsigned hi = rate->min ? mul_div_hi(psize->max, 1000000u,
                                                 rate->min) : IVAL_MAX;
            if (!ival_refine(ptime, lo ? lo - 1 : 0,
                             hi < IVAL_MAX ? hi + 1 : hi, &changed))
                return -ABI_EINVAL;
        }
        if (rate->max) {
            unsigned lo = mul_div_lo(bsize->min, 1000000u, rate->max);
            unsigned hi = rate->min ? mul_div_hi(bsize->max, 1000000u,
                                                 rate->min) : IVAL_MAX;
            if (!ival_refine(btime, lo ? lo - 1 : 0,
                             hi < IVAL_MAX ? hi + 1 : hi, &changed))
                return -ABI_EINVAL;
        }
        /* and back: period_size implied by the time the client asked for */
        if (!ival_refine(psize, mul_div_lo(ptime->min, rate->min, 1000000u),
                                mul_div_hi(ptime->max, rate->max, 1000000u),
                                &changed)) return -ABI_EINVAL;
        if (!ival_refine(bsize, mul_div_lo(btime->min, rate->min, 1000000u),
                                mul_div_hi(btime->max, rate->max, 1000000u),
                                &changed)) return -ABI_EINVAL;

        if (changed == before) break;          /* fixed point reached */
    }

    /* tick_time is a legacy parameter; pin it so it is never left open. */
    if (!ival_refine(tick, 0, 0, &changed)) return -ABI_EINVAL;

    p->info     = INFO_INTERLEAVED | INFO_BLOCK_TRANSFER | INFO_PAUSE;
    p->msbits   = 16;
    p->rate_num = ival_is_exact(rate) ? rate->min : 48000;
    p->rate_den = 1;
    p->fifo_size = 0;
    p->cmask    = g_cmask_acc & p->rmask;
    (void)changed;

#ifdef SND_TRACE
    {   /* What is alsa-lib actually asking for, and what did we hand back?
         * The library runs its own convergence loop over HW_REFINE and can
         * conclude the space is empty WITHOUT ever issuing HW_PARAMS -- so
         * "every refine returned 0" is not evidence the negotiation is
         * healthy. Dump the parameters that matter. */
        static int n;
        if (n < 40) { n++;
            kprintf("[refine] rmask=0x%x cmask=0x%x rate[%u..%u] ch[%u..%u] "
                    "psz[%u..%u] per[%u..%u] bsz[%u..%u] pt[%u..%u] "
                    "bt[%u..%u] pb[%u..%u] bb[%u..%u]\n",
                    p->rmask, p->cmask,
                    rate->min, rate->max, chan->min, chan->max,
                    psize->min, psize->max, periods->min, periods->max,
                    bsize->min, bsize->max, ptime->min, ptime->max,
                    btime->min, btime->max, pbytes->min, pbytes->max,
                    bbytes->min, bbytes->max);
        }
    }
#endif
    return 0;
}

/* ============================================================
 * PCM ioctls
 * ============================================================ */

/* Frames the DAC has consumed since this substream started.
 *
 * Derived from what WE pushed to the device, never from appl_ptr: appl_ptr
 * is under the application's control (SYNC_PTR without the APPL flag means
 * "adopt the value userspace just handed you"), so deriving hw_ptr from it
 * makes the hardware pointer collapse to zero the moment a client syncs a
 * zero appl_ptr -- which is exactly what linux-sndtest did, reporting a
 * dead DAC while the WAV capture proved audio was playing. */
static uint64_t sub_hw_ptr(void) {
    long pend = audio_hda_pcm_pending();
    if (pend < 0) return g_sub.pushed;
    return (g_sub.pushed > (uint64_t)pend) ? g_sub.pushed - (uint64_t)pend : 0;
}

static long pcm_writei(unsigned long arg) {
    struct snd_xferi x;
    if (copy_from_user(&x, (const void *)arg, sizeof(x)) != 0)
        return -ABI_EFAULT;
    if (g_sub.state != ST_PREPARED && g_sub.state != ST_RUNNING)
        return -SND_EBADFD;
    if (x.frames == 0) { x.result = 0; goto out; }

    uint32_t fb = g_sub.frame_bytes ? g_sub.frame_bytes : 4;
    uint64_t want = x.frames;

    /* Bound one transfer by what the device ring can take right now; a
     * client that asks for more gets a short result and comes back, which
     * is exactly ALSA's contract for a non-blocking writei. */
    long freefr = audio_hda_pcm_free();
    if (freefr < 0) return -ABI_EIO;
    if (freefr == 0) return -ABI_EAGAIN;
    if (want > (uint64_t)freefr) want = (uint64_t)freefr;

    /* Bounce through the kernel: the HDA ring is written by the driver, and
     * user pages must not be touched outside a uaccess window. */
    size_t bytes = (size_t)want * fb;
    if (bytes > 64 * 1024) { bytes = 64 * 1024; want = bytes / fb; }
    int16_t *k = (int16_t *)kmalloc(bytes);
    if (!k) return -ABI_ENOMEM;
    if (copy_from_user(k, (const void *)(uintptr_t)x.buf, bytes) != 0) {
        kfree(k);
        return -ABI_EFAULT;
    }
    long w = audio_hda_pcm_write(k, (size_t)want);
    kfree(k);
    if (w < 0) return -ABI_EIO;

    g_sub.pushed   += (uint64_t)w;
    g_sub.appl_ptr += (uint64_t)w;
    if (g_sub.boundary && g_sub.appl_ptr >= g_sub.boundary)
        g_sub.appl_ptr -= g_sub.boundary;
    if (!g_sub.started && g_sub.state == ST_PREPARED) {
        g_sub.state = ST_RUNNING;
        g_sub.started = true;
    }
    x.result = w;
out:
    if (copy_to_user((void *)arg, &x, sizeof(x)) != 0) return -ABI_EFAULT;
    return 0;
}

static long pcm_sync_ptr(unsigned long arg) {
    struct snd_pcm_sync_ptr sp;
    if (copy_from_user(&sp, (const void *)arg, sizeof(sp)) != 0)
        return -ABI_EFAULT;

    if (!(sp.flags & SYNC_PTR_APPL))
        g_sub.appl_ptr = sp.c.control.appl_ptr;
    if (!(sp.flags & SYNC_PTR_AVAIL_MIN))
        g_sub.avail_min = sp.c.control.avail_min;

    memset(&sp.s, 0, sizeof(sp.s));
    memset(&sp.c, 0, sizeof(sp.c));
    sp.s.status.state    = g_sub.state;
    sp.s.status.hw_ptr   = sub_hw_ptr();
    sp.c.control.appl_ptr  = g_sub.appl_ptr;
    sp.c.control.avail_min = g_sub.avail_min;

    if (copy_to_user((void *)arg, &sp, sizeof(sp)) != 0) return -ABI_EFAULT;
    return 0;
}

static void fill_pcm_info(struct snd_pcm_info *pi) {
    memset(pi, 0, sizeof(*pi));
    pi->device    = 0;
    pi->subdevice = 0;
    pi->stream    = STREAM_PLAYBACK;
    pi->card      = 0;
    sncpy((char *)pi->id, "HDA", sizeof(pi->id));
    sncpy((char *)pi->name, "tobyOS HD Audio", sizeof(pi->name));
    sncpy((char *)pi->subname, "subdevice #0", sizeof(pi->subname));
    pi->subdevices_count = 1;
    pi->subdevices_avail = 1;
}

/* Compact ioctl trace. Bounded so a busy stream cannot flood the serial
 * log; enough to see the whole open/negotiate/prepare handshake, which is
 * the part that goes wrong. */
static long lxsnd_ioctl_inner(struct file *f, unsigned long req,
                              unsigned long arg);

/* Reset per client session (see lxsnd_open) so each program gets its own
 * trace window -- otherwise the first client to run burns the whole budget
 * and the one you are actually debugging shows nothing. */
static int g_trace_n;

long lxsnd_ioctl(struct file *f, unsigned long req, unsigned long arg) {
    long rc = lxsnd_ioctl_inner(f, req, arg);
#ifdef SND_TRACE
    if (g_trace_n < 120) {
        g_trace_n++;
        kprintf("[sndio] %s nr=0x%02x size=%u -> %ld\n",
                f->dir_off == 0 ? "ctl" : "pcm",
                IOC_NR(req), IOC_SIZE(req), rc);
    }
#endif
    return rc;
}

static long lxsnd_ioctl_inner(struct file *f, unsigned long req,
                              unsigned long arg) {
    unsigned type = IOC_TYPE(req), nr = IOC_NR(req);

    /* ---- control device ---- */
    if (f->dir_off == 0) {
        if (type != SNDRV_CTL_IOCTL_TYPE) return -ABI_ENOTTY;
        switch (nr) {
        case CTL_NR_PVERSION: {
            int v = SNDRV_CTL_VERSION;
            return copy_to_user((void *)arg, &v, sizeof(v)) ? -ABI_EFAULT : 0;
        }
        case CTL_NR_CARD_INFO: {
            struct snd_ctl_card_info ci;
            memset(&ci, 0, sizeof(ci));
            ci.card = 0;
            sncpy((char *)ci.id, "HDA", sizeof(ci.id));
            sncpy((char *)ci.driver, "HDA-Intel", sizeof(ci.driver));
            sncpy((char *)ci.name, "tobyOS HD Audio", sizeof(ci.name));
            sncpy((char *)ci.longname, "tobyOS HD Audio at HDA controller", sizeof(ci.longname));
            sncpy((char *)ci.mixername, "tobyOS", sizeof(ci.mixername));
            return copy_to_user((void *)arg, &ci, sizeof(ci)) ? -ABI_EFAULT : 0;
        }
        case CTL_NR_PCM_NEXT_DEVICE: {
            int dev = 0;
            if (copy_from_user(&dev, (const void *)arg, sizeof(dev)) != 0)
                return -ABI_EFAULT;
            /* -1 asks for the first; anything at or past our only device
             * ends the enumeration with -1. */
            dev = (dev < 0) ? 0 : -1;
            return copy_to_user((void *)arg, &dev, sizeof(dev)) ? -ABI_EFAULT : 0;
        }
        case CTL_NR_PCM_INFO: {
            struct snd_pcm_info pi;
            if (copy_from_user(&pi, (const void *)arg, sizeof(pi)) != 0)
                return -ABI_EFAULT;
            if (pi.device != 0 || pi.stream != STREAM_PLAYBACK)
                return -ABI_ENOENT;          /* capture is not implemented */
            fill_pcm_info(&pi);
            return copy_to_user((void *)arg, &pi, sizeof(pi)) ? -ABI_EFAULT : 0;
        }
        case CTL_NR_PCM_PREFER_SUBDEV:
            return 0;
        default:
            return -ABI_ENOTTY;
        }
    }

    /* ---- pcm playback device ---- */
    if (type != SNDRV_PCM_IOCTL_TYPE) return -ABI_ENOTTY;
    switch (nr) {
    case PCM_NR_PVERSION: {
        int v = SNDRV_PCM_VERSION;
        return copy_to_user((void *)arg, &v, sizeof(v)) ? -ABI_EFAULT : 0;
    }
    case PCM_NR_INFO: {
        struct snd_pcm_info pi;
        fill_pcm_info(&pi);
        return copy_to_user((void *)arg, &pi, sizeof(pi)) ? -ABI_EFAULT : 0;
    }
    case PCM_NR_TSTAMP:
    case PCM_NR_TTSTAMP:
    case PCM_NR_USER_PVERSION:
        return 0;                              /* accepted, no effect */

    case PCM_NR_HW_REFINE: {
        struct snd_pcm_hw_params *p =
            (struct snd_pcm_hw_params *)kmalloc(sizeof(*p));
        if (!p) return -ABI_ENOMEM;
        if (copy_from_user(p, (const void *)arg, sizeof(*p)) != 0) {
            kfree(p); return -ABI_EFAULT;
        }
        /* Trace rejections here too, not only on the HW_PARAMS path.
         * A plugin (chrome reaches us through `plug`) resolves its slave's
         * parameters with HW_REFINE and simply gives up if one comes back
         * EINVAL -- it never issues HW_PARAMS at all. With tracing off for
         * refine that failure was completely silent from the kernel side:
         * no ioctl, no reject, no clue. */
        g_trace_refine = true;
        int rc = hw_refine(p);
        g_trace_refine = false;
        if (rc == 0 && copy_to_user((void *)arg, p, sizeof(*p)) != 0)
            rc = -ABI_EFAULT;
        kfree(p);
        return rc;
    }
    case PCM_NR_HW_PARAMS: {
        struct snd_pcm_hw_params *p =
            (struct snd_pcm_hw_params *)kmalloc(sizeof(*p));
        if (!p) return -ABI_ENOMEM;
        if (copy_from_user(p, (const void *)arg, sizeof(*p)) != 0) {
            kfree(p); return -ABI_EFAULT;
        }
        g_trace_refine = true;
        int rc = hw_refine(p);
        g_trace_refine = false;
        if (rc != 0) { kfree(p); return rc; }

        struct snd_interval *chan = &p->intervals[IVAL_IDX(HWP_CHANNELS)];
        struct snd_interval *rate = &p->intervals[IVAL_IDX(HWP_RATE)];
        struct snd_interval *psz  = &p->intervals[IVAL_IDX(HWP_PERIOD_SIZE)];
        struct snd_interval *bsz  = &p->intervals[IVAL_IDX(HWP_BUFFER_SIZE)];
        if (!ival_is_exact(chan) || !ival_is_exact(rate)) {
            kprintf("[snd] hw_params REJECT: not exact -- channels [%u..%u] "
                    "rate [%u..%u] period_size [%u..%u] buffer_size [%u..%u]\n",
                    chan->min, chan->max, rate->min, rate->max,
                    psz->min, psz->max, bsz->min, bsz->max);
            kfree(p); return -ABI_EINVAL;
        }
        g_sub.channels    = chan->min;
        g_sub.rate        = rate->min;
        g_sub.frame_bytes = 2u * g_sub.channels;
        g_sub.period_size = psz->min;
        g_sub.buffer_size = bsz->min;
        g_sub.appl_ptr    = 0;
        g_sub.pushed      = 0;
        g_sub.started     = false;
        g_sub.state       = ST_SETUP;

        int hrc = audio_hda_pcm_open(g_sub.rate, (uint8_t)g_sub.channels);
        if (hrc != 0) { kfree(p); return -ABI_EIO; }

        kprintf("[snd] hw_params: %u Hz %u ch S16_LE period=%lu buffer=%lu\n",
                g_sub.rate, g_sub.channels,
                (unsigned long)g_sub.period_size,
                (unsigned long)g_sub.buffer_size);
        int rc2 = copy_to_user((void *)arg, p, sizeof(*p)) ? -ABI_EFAULT : 0;
        kfree(p);
        return rc2;
    }
    case PCM_NR_HW_FREE:
        audio_hda_pcm_close();
        g_sub.state   = ST_OPEN;
        g_sub.started = false;
        return 0;

    case PCM_NR_SW_PARAMS: {
        struct snd_pcm_sw_params sw;
        if (copy_from_user(&sw, (const void *)arg, sizeof(sw)) != 0)
            return -ABI_EFAULT;
        g_sub.avail_min       = sw.avail_min;
        g_sub.start_threshold = sw.start_threshold;
        /* boundary must be a power-of-two multiple of buffer_size; alsa-lib
         * proposes one and we honour it, since appl_ptr wraps modulo it. */
        g_sub.boundary = sw.boundary;
        return copy_to_user((void *)arg, &sw, sizeof(sw)) ? -ABI_EFAULT : 0;
    }

    case PCM_NR_PREPARE:
        g_sub.state    = ST_PREPARED;
        g_sub.appl_ptr = 0;
        g_sub.pushed   = 0;
        g_sub.started  = false;
        return 0;
    case PCM_NR_RESET:
        g_sub.appl_ptr = 0;
        return 0;
    case PCM_NR_START:
        if (g_sub.state != ST_PREPARED) return -SND_EBADFD;
        g_sub.state   = ST_RUNNING;
        g_sub.started = true;
        return 0;
    case PCM_NR_DROP:
        g_sub.state   = ST_SETUP;
        g_sub.started = false;
        return 0;
    case PCM_NR_DRAIN:
        /* The ring drains on its own; a blocking drain would have to spin
         * inside a syscall holding the BKL, which this kernel forbids. */
        g_sub.state = ST_SETUP;
        return 0;
    case PCM_NR_PAUSE:
        return 0;
    case PCM_NR_HWSYNC:
        return 0;

    case PCM_NR_DELAY: {
        long pend = audio_hda_pcm_pending();
        int64_t d = (pend > 0) ? pend : 0;
        return copy_to_user((void *)arg, &d, sizeof(d)) ? -ABI_EFAULT : 0;
    }
    case PCM_NR_SYNC_PTR:
        return pcm_sync_ptr(arg);

    case PCM_NR_WRITEI_FRAMES:
        return pcm_writei(arg);

    case PCM_NR_READI_FRAMES:
        return -ABI_ENOTTY;                    /* playback only */

    case PCM_NR_STATUS:
    case PCM_NR_STATUS_EXT: {
        /* Built by offset rather than as a struct: snd_pcm_status has
         * grown across kernel versions (audio_tstamp, driver_tstamp), so
         * the size in the request is authoritative. The leading fields we
         * populate have been stable since 2.0.x. */
        unsigned sz = IOC_SIZE(req);
        if (sz < 96 || sz > 512) return -ABI_EINVAL;
        unsigned char *buf = (unsigned char *)kmalloc(sz);
        if (!buf) return -ABI_ENOMEM;
        memset(buf, 0, sz);
        uint64_t hw = sub_hw_ptr();
        long pend   = audio_hda_pcm_pending();
        uint64_t avail = g_sub.buffer_size > (uint64_t)(pend > 0 ? pend : 0)
                         ? g_sub.buffer_size - (uint64_t)(pend > 0 ? pend : 0)
                         : 0;
        *(int *)(buf + 0)       = g_sub.state;       /* state          */
        *(uint64_t *)(buf + 40) = g_sub.appl_ptr;    /* appl_ptr       */
        *(uint64_t *)(buf + 48) = hw;                /* hw_ptr         */
        *(int64_t  *)(buf + 56) = (pend > 0) ? pend : 0;  /* delay     */
        *(uint64_t *)(buf + 64) = avail;             /* avail          */
        *(uint64_t *)(buf + 72) = g_sub.buffer_size; /* avail_max      */
        int rc = copy_to_user((void *)arg, buf, sz) ? -ABI_EFAULT : 0;
        kfree(buf);
        return rc;
    }

    case PCM_NR_CHANNEL_INFO:
        return -ABI_ENOTTY;                    /* mmap path not offered */

    default:
        return -ABI_ENOTTY;
    }
}

/* ============================================================
 * open / close / availability
 * ============================================================ */

bool lxsnd_available(void) {
    return audio_hda_present();
}

/* node is the part after "/dev/", e.g. "snd/pcmC0D0p". dir_off carries
 * which node this fd is: 0 = control, 1 = pcm playback (mirrors how
 * linux_drm.c stashes the DRM minor). */
struct file *lxsnd_open(const char *node) {
    if (!lxsnd_available()) return 0;
    const char *n = node + 4;                  /* skip "snd/" */
    int which;
    if (strcmp(n, "controlC0") == 0)      which = 0;
    else if (strcmp(n, "pcmC0D0p") == 0)  which = 1;
    else {
        kprintf("[snd] open REJECT unknown node '%s'\n", node);
        return 0;
    }
    kprintf("[snd] open %s\n", node);
    /* Reset on the PCM open, not the control open: chrome reopens
     * controlC0 constantly, which kept resetting the window and hid
     * the playback ioctls that actually matter. */
    if (which == 1) g_trace_n = 0;

    if (which == 1) {
        if (g_sub.in_use) return 0;            /* single substream */
        memset(&g_sub, 0, sizeof(g_sub));
        g_sub.in_use = true;
        g_sub.state  = ST_OPEN;
    }

    struct file *f = (struct file *)kmalloc(sizeof(*f));
    if (!f) return 0;
    memset(f, 0, sizeof(*f));
    f->kind      = FILE_KIND_SND;
    f->dir_off   = which;
    f->o_accmode = 2;                          /* O_RDWR */
    return f;
}

void lxsnd_close(struct file *f) {
    if (!f || f->kind != FILE_KIND_SND) return;
    if (f->dir_off == 1) {
        audio_hda_pcm_close();
        memset(&g_sub, 0, sizeof(g_sub));
    }
}

/* A PCM fd is writable whenever the device ring has room; poll/select and
 * epoll route here so an event-driven client (which is what Chromium's
 * audio thread is) can wait on it. */
bool lxsnd_poll_writable(struct file *f) {
    if (!f || f->dir_off != 1) return true;
    long freefr = audio_hda_pcm_free();
    if (freefr < 0) return false;
    return (uint64_t)freefr >= (g_sub.avail_min ? g_sub.avail_min : 1);
}
