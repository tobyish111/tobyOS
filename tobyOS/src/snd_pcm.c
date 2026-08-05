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
static bool mask_refine_to(struct snd_mask *m, const unsigned *allowed,
                           int n, bool *changed) {
    struct snd_mask out;
    mask_zero(&out);
    for (int i = 0; i < n; i++)
        if (mask_test(m, allowed[i])) mask_set(&out, allowed[i]);
    if (mask_empty(&out)) return false;
    if (memcmp(&out, m, sizeof(out)) != 0) { *m = out; *changed = true; }
    return true;
}

static bool ival_refine(struct snd_interval *v, unsigned lo, unsigned hi,
                        bool *changed) {
    unsigned omin = v->min, omax = v->max;
    if (v->min < lo) { v->min = lo; v->openmin = 0; }
    if (v->max > hi) { v->max = hi; v->openmax = 0; }
    /* An open bound excludes its endpoint; close it by stepping inward so
     * the rest of the refine only has to reason about closed intervals. */
    if (v->openmin) { v->min++; v->openmin = 0; }
    if (v->openmax) { if (v->max == 0) return false; v->max--; v->openmax = 0; }
    if (v->min > v->max) { v->empty = 1; return false; }
    if (v->min != omin || v->max != omax) *changed = true;
    return true;
}

static void ival_set_exact(struct snd_interval *v, unsigned val,
                           bool *changed) {
    if (v->min != val || v->max != val) *changed = true;
    v->min = v->max = val;
    v->openmin = v->openmax = 0;
    v->integer = 1;
    v->empty = 0;
}

static bool ival_is_exact(const struct snd_interval *v) {
    return v->min == v->max;
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

    /* S16_LE only, so sample_bits is pinned. */
    if (!ival_refine(sbits, 16, 16, &changed))  return -ABI_EINVAL;
    if (!ival_refine(chan, 1, 2, &changed))     return -ABI_EINVAL;
    /* Rates the HDA format word can express (hda_fmt_word in audio_hda.c). */
    if (!ival_refine(rate, 8000, 96000, &changed)) return -ABI_EINVAL;

    /* Geometry. The HDA ring is 4 pages of 4096 B; keep clients inside it
     * so a period never exceeds what audio_hda_pcm_write can hold. */
    if (!ival_refine(periods, 2, 32, &changed))         return -ABI_EINVAL;
    if (!ival_refine(psize, 32, 8192, &changed))        return -ABI_EINVAL;
    if (!ival_refine(bsize, 64, 65536, &changed))       return -ABI_EINVAL;

    /* Derived: frame_bits = sample_bits * channels. Only resolvable once
     * channels is pinned; alsa-lib always gets there before HW_PARAMS. */
    if (ival_is_exact(chan)) {
        unsigned fb = 16u * chan->min;
        if (!ival_refine(fbits, fb, fb, &changed)) return -ABI_EINVAL;

        if (ival_is_exact(psize)) {
            unsigned pb = (unsigned)(psize->min * fb / 8u);
            if (!ival_refine(pbytes, pb, pb, &changed)) return -ABI_EINVAL;
        }
        if (ival_is_exact(bsize)) {
            unsigned bb = (unsigned)(bsize->min * fb / 8u);
            if (!ival_refine(bbytes, bb, bb, &changed)) return -ABI_EINVAL;
        }
    } else {
        if (!ival_refine(fbits, 16, 32, &changed)) return -ABI_EINVAL;
    }

    /* Derived: buffer_size = period_size * periods. */
    if (ival_is_exact(psize) && ival_is_exact(periods)) {
        unsigned bs = (unsigned)(psize->min * periods->min);
        if (!ival_refine(bsize, bs, bs, &changed)) return -ABI_EINVAL;
    }

    /* Derived: *_time in microseconds = frames * 1e6 / rate. */
    if (ival_is_exact(rate) && rate->min) {
        if (ival_is_exact(psize)) {
            unsigned t = (unsigned)((psize->min * 1000000ull) / rate->min);
            ival_set_exact(ptime, t, &changed);
        }
        if (ival_is_exact(bsize)) {
            unsigned t = (unsigned)((bsize->min * 1000000ull) / rate->min);
            ival_set_exact(btime, t, &changed);
        }
    }

    p->info     = INFO_INTERLEAVED | INFO_BLOCK_TRANSFER | INFO_PAUSE;
    p->msbits   = 16;
    p->rate_num = ival_is_exact(rate) ? rate->min : 48000;
    p->rate_den = 1;
    p->fifo_size = 0;
    p->cmask    = changed ? p->rmask : 0;
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

long lxsnd_ioctl(struct file *f, unsigned long req, unsigned long arg) {
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
        int rc = hw_refine(p);
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
        int rc = hw_refine(p);
        if (rc != 0) { kfree(p); return rc; }

        struct snd_interval *chan = &p->intervals[IVAL_IDX(HWP_CHANNELS)];
        struct snd_interval *rate = &p->intervals[IVAL_IDX(HWP_RATE)];
        struct snd_interval *psz  = &p->intervals[IVAL_IDX(HWP_PERIOD_SIZE)];
        struct snd_interval *bsz  = &p->intervals[IVAL_IDX(HWP_BUFFER_SIZE)];
        if (!ival_is_exact(chan) || !ival_is_exact(rate)) {
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
    else return 0;

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
