/* programs/linux-sndtest/main.c
 *
 * Audio slice 3 guard: drive /dev/snd the way alsa-lib does, from a plain
 * static Linux ELF with raw syscalls and no libc.
 *
 * This proves the KERNEL side of the ALSA ABI independently of alsa-lib,
 * so that when slice 4 drops the real libasound.so.2 on top and something
 * misbehaves, we already know which half to suspect.
 *
 * The walk, in alsa-lib's own order:
 *   open(/dev/snd/controlC0)
 *   SNDRV_CTL_IOCTL_PVERSION        -> protocol 2.x
 *   SNDRV_CTL_IOCTL_CARD_INFO       -> a named card
 *   SNDRV_CTL_IOCTL_PCM_NEXT_DEVICE -> device 0 exists
 *   SNDRV_CTL_IOCTL_PCM_INFO        -> playback substream present
 *   open(/dev/snd/pcmC0D0p)
 *   SNDRV_PCM_IOCTL_PVERSION
 *   SNDRV_PCM_IOCTL_HW_REFINE       -> hardware narrows our wide request
 *   SNDRV_PCM_IOCTL_HW_PARAMS       -> commit 48 kHz / S16_LE / stereo
 *   SNDRV_PCM_IOCTL_SW_PARAMS
 *   SNDRV_PCM_IOCTL_PREPARE
 *   SNDRV_PCM_IOCTL_WRITEI_FRAMES   -> a 1000 Hz square wave, repeatedly
 *   SNDRV_PCM_IOCTL_SYNC_PTR        -> hw_ptr must ADVANCE (the real proof:
 *                                      the DAC consumed what we wrote)
 *
 * exit_group(): 0 = PASS, otherwise 10 + the failing step number, so a
 * failure names itself in one line of serial output.
 */

typedef unsigned long      u64;
typedef long               i64;
typedef unsigned int       u32;
typedef int                i32;
typedef unsigned short     u16;
typedef short              i16;
typedef unsigned char      u8;

static inline i64 lx3(i64 n, i64 a, i64 b, i64 c) {
    i64 r;
    __asm__ volatile ("syscall" : "=a"(r)
        : "0"(n), "D"(a), "S"(b), "d"(c) : "rcx", "r11", "memory");
    return r;
}
#define SYS_write 1
#define SYS_open  2
#define SYS_close 3
#define SYS_ioctl 16
#define SYS_nanosleep 35
#define SYS_exit_group 231

static void put(const char *s) {
    i64 n = 0; while (s[n]) n++;
    lx3(SYS_write, 1, (i64)s, n);
}
static void putn(u64 v) {
    char b[24]; int i = 23; b[i--] = 0;
    if (!v) b[i--] = '0';
    while (v) { b[i--] = (char)('0' + (v % 10)); v /= 10; }
    put(&b[i + 1]);
}
static void nsleep(u64 ms) {
    struct { i64 s, ns; } ts = { (i64)(ms / 1000), (i64)((ms % 1000) * 1000000) };
    lx3(SYS_nanosleep, (i64)&ts, 0, 0);
}
static void *zero(void *p, u64 n) {
    u8 *b = (u8 *)p; while (n--) *b++ = 0; return p;
}

/* ---- ALSA UAPI ------------------------------------------------------- */
struct snd_mask { u32 bits[8]; };
struct snd_interval {
    u32 min, max;
    u32 flags;              /* openmin:1 openmax:1 integer:1 empty:1 */
};
#define IV_INTEGER (1u << 2)

#define HWP_ACCESS 0
#define HWP_FORMAT 1
#define HWP_SUBFORMAT 2
#define IV_SAMPLE_BITS   (8  - 8)
#define IV_FRAME_BITS    (9  - 8)
#define IV_CHANNELS      (10 - 8)
#define IV_RATE          (11 - 8)
#define IV_PERIOD_TIME   (12 - 8)
#define IV_PERIOD_SIZE   (13 - 8)
#define IV_PERIOD_BYTES  (14 - 8)
#define IV_PERIODS       (15 - 8)
#define IV_BUFFER_TIME   (16 - 8)
#define IV_BUFFER_SIZE   (17 - 8)
#define IV_BUFFER_BYTES  (18 - 8)
#define IV_TICK_TIME     (19 - 8)

struct snd_pcm_hw_params {
    u32 flags;
    struct snd_mask     masks[3];
    struct snd_mask     mres[5];
    struct snd_interval intervals[12];
    struct snd_interval ires[9];
    u32 rmask, cmask, info, msbits, rate_num, rate_den;
    u64 fifo_size;
    u8  reserved[64];
};

struct snd_pcm_sw_params {
    i32 tstamp_mode;
    u32 period_step, sleep_min;
    u64 avail_min, xfer_align, start_threshold, stop_threshold;
    u64 silence_threshold, silence_size, boundary;
    u32 proto, tstamp_type;
    u8  reserved[56];
};

struct snd_xferi { i64 result; u64 buf; u64 frames; };

struct snd_pcm_sync_ptr {
    u32 flags, pad1;
    union { struct { i32 state, pad; u64 hw_ptr; i64 ts, tns;
                     i32 susp, pad2; i64 ats, atns; } status;
            u8 rsv[64]; } s;
    union { struct { u64 appl_ptr, avail_min; } control; u8 rsv[64]; } c;
};

struct snd_ctl_card_info {
    i32 card, pad;
    u8 id[16], driver[16], name[32], longname[80];
    u8 reserved_[16], mixername[80], components[128];
};

struct snd_pcm_info {
    u32 device, subdevice; i32 stream, card;
    u8 id[64], name[80], subname[32];
    i32 dev_class, dev_subclass;
    u32 subdevices_count, subdevices_avail;
    u8 sync[16], reserved[64];
};

#define CTL_PVERSION        0x80045500u
#define CTL_CARD_INFO       0x81785501u
#define CTL_PCM_NEXT_DEVICE 0xc0045530u
#define CTL_PCM_INFO        0xc1205531u

#define PCM_PVERSION   0x80044100u
#define PCM_HW_REFINE  0xc2604110u
#define PCM_HW_PARAMS  0xc2604111u
#define PCM_SW_PARAMS  0xc0884113u
#define PCM_PREPARE    0x00004140u
#define PCM_START      0x00004142u
#define PCM_DRAIN      0x00004144u
#define PCM_SYNC_PTR   0xc0884123u
#define PCM_WRITEI     0x40184150u

#define ACCESS_RW_INTERLEAVED 3
#define FORMAT_S16_LE         2

#define RATE     48000
#define CHANS        2
#define TONE_HZ   1000
#define PERIOD     480              /* 10 ms, 10 whole 1000 Hz periods */
#define AMP      10000

static void mset(struct snd_mask *m, u32 bit) {
    zero(m, sizeof(*m));
    m->bits[bit >> 5] |= (1u << (bit & 31));
}
static void ivset(struct snd_interval *v, u32 lo, u32 hi) {
    v->min = lo; v->max = hi; v->flags = IV_INTEGER;
}

static i16 g_tone[PERIOD * CHANS];

void _start(void) {
    int step = 1;
    put("[LXSND] start\n");

    /* ---- 1. control device ---- */
    i64 cfd = lx3(SYS_open, (i64)"/dev/snd/controlC0", 2 /*O_RDWR*/, 0);
    if (cfd < 0) { put("[LXSND] open controlC0 failed\n"); goto fail; }

    step = 2;
    i32 ver = 0;
    if (lx3(SYS_ioctl, cfd, (i64)CTL_PVERSION, (i64)&ver) < 0) goto fail;
    put("[LXSND] ctl protocol "); putn((u64)((ver >> 16) & 0xffff));
    put("."); putn((u64)((ver >> 8) & 0xff));
    put("."); putn((u64)(ver & 0xff)); put("\n");
    if (((ver >> 16) & 0xffff) != 2) goto fail;

    step = 3;
    static struct snd_ctl_card_info ci;
    zero(&ci, sizeof(ci));
    if (lx3(SYS_ioctl, cfd, (i64)CTL_CARD_INFO, (i64)&ci) < 0) goto fail;
    put("[LXSND] card: "); put((const char *)ci.name);
    put(" / driver "); put((const char *)ci.driver); put("\n");
    if (!ci.name[0]) goto fail;

    step = 4;
    i32 dev = -1;
    if (lx3(SYS_ioctl, cfd, (i64)CTL_PCM_NEXT_DEVICE, (i64)&dev) < 0) goto fail;
    put("[LXSND] first pcm device: "); putn((u64)dev); put("\n");
    if (dev != 0) goto fail;

    step = 5;
    static struct snd_pcm_info pi;
    zero(&pi, sizeof(pi));
    pi.device = 0; pi.stream = 0; pi.subdevice = 0;
    if (lx3(SYS_ioctl, cfd, (i64)CTL_PCM_INFO, (i64)&pi) < 0) goto fail;
    put("[LXSND] pcm0: "); put((const char *)pi.name); put("\n");
    lx3(SYS_close, cfd, 0, 0);

    /* ---- 6. pcm device ---- */
    step = 6;
    i64 pfd = lx3(SYS_open, (i64)"/dev/snd/pcmC0D0p", 2, 0);
    if (pfd < 0) { put("[LXSND] open pcmC0D0p failed\n"); goto fail; }

    step = 7;
    if (lx3(SYS_ioctl, pfd, (i64)PCM_PVERSION, (i64)&ver) < 0) goto fail;

    /* ---- 8. HW_REFINE: ask WIDE and let the kernel narrow it ---- */
    step = 8;
    static struct snd_pcm_hw_params hw;
    zero(&hw, sizeof(hw));
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 8; j++) hw.masks[i].bits[j] = 0xffffffffu;
    for (int i = 0; i < 12; i++) ivset(&hw.intervals[i], 0, 0xffffffffu);
    hw.rmask = 0xffffffffu;
    if (lx3(SYS_ioctl, pfd, (i64)PCM_HW_REFINE, (i64)&hw) < 0) {
        put("[LXSND] HW_REFINE failed\n"); goto fail;
    }
    put("[LXSND] refine: rate ");
    putn(hw.intervals[IV_RATE].min); put("-");
    putn(hw.intervals[IV_RATE].max);
    put(", channels ");
    putn(hw.intervals[IV_CHANNELS].min); put("-");
    putn(hw.intervals[IV_CHANNELS].max);
    put(", sample_bits "); putn(hw.intervals[IV_SAMPLE_BITS].min);
    put("\n");
    /* The refine must have narrowed the wide-open request to real hardware
     * limits; if it echoed our 0..UINT_MAX back, the kernel did nothing. */
    if (hw.intervals[IV_RATE].max == 0xffffffffu) goto fail;
    if (hw.intervals[IV_SAMPLE_BITS].min != 16) goto fail;

    /* ---- 9. HW_PARAMS: commit exact settings ---- */
    step = 9;
    zero(&hw, sizeof(hw));
    mset(&hw.masks[HWP_ACCESS],    ACCESS_RW_INTERLEAVED);
    mset(&hw.masks[HWP_FORMAT],    FORMAT_S16_LE);
    mset(&hw.masks[HWP_SUBFORMAT], 0);
    ivset(&hw.intervals[IV_SAMPLE_BITS], 16, 16);
    ivset(&hw.intervals[IV_FRAME_BITS],  16 * CHANS, 16 * CHANS);
    ivset(&hw.intervals[IV_CHANNELS],    CHANS, CHANS);
    ivset(&hw.intervals[IV_RATE],        RATE, RATE);
    ivset(&hw.intervals[IV_PERIOD_SIZE], PERIOD, PERIOD);
    ivset(&hw.intervals[IV_PERIODS],     4, 4);
    ivset(&hw.intervals[IV_BUFFER_SIZE], PERIOD * 4, PERIOD * 4);
    ivset(&hw.intervals[IV_PERIOD_TIME],  0, 0xffffffffu);
    ivset(&hw.intervals[IV_BUFFER_TIME],  0, 0xffffffffu);
    ivset(&hw.intervals[IV_PERIOD_BYTES], 0, 0xffffffffu);
    ivset(&hw.intervals[IV_BUFFER_BYTES], 0, 0xffffffffu);
    ivset(&hw.intervals[IV_TICK_TIME],    0, 0xffffffffu);
    hw.rmask = 0xffffffffu;
    if (lx3(SYS_ioctl, pfd, (i64)PCM_HW_PARAMS, (i64)&hw) < 0) {
        put("[LXSND] HW_PARAMS failed\n"); goto fail;
    }
    put("[LXSND] hw_params committed: period_bytes ");
    putn(hw.intervals[IV_PERIOD_BYTES].min);
    put(", period_time_us "); putn(hw.intervals[IV_PERIOD_TIME].min);
    put("\n");
    /* The kernel must have DERIVED these, not just accepted them. */
    if (hw.intervals[IV_PERIOD_BYTES].min != PERIOD * CHANS * 2) {
        put("[LXSND] period_bytes not derived\n"); goto fail;
    }
    /* period_time is derived, but NOT to the microsecond: the frames<->us
     * conversion is lossy both ways and the kernel deliberately reports a
     * unit of slack, because alsa-lib's own rounding can disagree by one
     * and an exact answer then leaves an empty interval (that cost slice 4
     * a real EINVAL on a perfectly achievable configuration). So assert
     * the value is CLOSE, not equal -- demanding equality here would be
     * asserting a bug back into existence. */
    {
        u32 want = (PERIOD * 1000000u) / RATE;
        u32 got  = hw.intervals[IV_PERIOD_TIME].min;
        u32 diff = got > want ? got - want : want - got;
        if (diff > 2) {
            put("[LXSND] period_time not derived (want ~"); putn(want);
            put(", got "); putn(got); put(")\n");
            goto fail;
        }
    }

    /* ---- 10. SW_PARAMS ---- */
    step = 10;
    static struct snd_pcm_sw_params sw;
    zero(&sw, sizeof(sw));
    sw.avail_min       = PERIOD;
    sw.start_threshold = PERIOD;
    sw.stop_threshold  = PERIOD * 4;
    sw.boundary        = (u64)PERIOD * 4 * 1024;
    sw.proto           = ver;
    if (lx3(SYS_ioctl, pfd, (i64)PCM_SW_PARAMS, (i64)&sw) < 0) goto fail;

    /* ---- 11. PREPARE ---- */
    step = 11;
    if (lx3(SYS_ioctl, pfd, (i64)PCM_PREPARE, 0) < 0) goto fail;

    /* ---- 12. write a 1000 Hz square wave ---- */
    step = 12;
    for (int i = 0; i < PERIOD; i++) {
        i16 v = ((i % (RATE / TONE_HZ)) < (RATE / TONE_HZ / 2))
                ? (i16)AMP : (i16)-AMP;
        g_tone[i * CHANS + 0] = v;
        g_tone[i * CHANS + 1] = v;
    }
    u64 total = 0;
    /* 60 periods x 10 ms = 600 ms, matching /bin/audioplay. */
    for (int p = 0; p < 60; p++) {
        u64 done = 0;
        int guard = 0;
        while (done < PERIOD) {
            struct snd_xferi x;
            x.result = 0;
            x.buf    = (u64)&g_tone[done * CHANS];
            x.frames = PERIOD - done;
            i64 rc = lx3(SYS_ioctl, pfd, (i64)PCM_WRITEI, (i64)&x);
            if (rc < 0) {
                if (++guard > 2000) { put("[LXSND] writei stuck\n"); goto fail; }
                nsleep(2);              /* ring full: EAGAIN, back off */
                continue;
            }
            if (x.result <= 0) {
                if (++guard > 2000) { put("[LXSND] writei no progress\n"); goto fail; }
                nsleep(2);
                continue;
            }
            done  += (u64)x.result;
            total += (u64)x.result;
        }
    }
    put("[LXSND] wrote "); putn(total); put(" frames\n");

    /* ---- 13. SYNC_PTR: hw_ptr must have ADVANCED ---- */
    step = 13;
    /* A pure QUERY must set both "do not adopt my values" flags. The flag
     * senses are inverted: SNDRV_PCM_SYNC_PTR_APPL means "don't take
     * appl_ptr from userspace". Sending flags=0 tells the kernel to adopt
     * whatever appl_ptr the struct carries -- zero -- which is exactly how
     * this test first reported a dead DAC while the WAV capture proved
     * 600 ms of audio had played. */
#define SYNC_APPL      (1u << 1)
#define SYNC_AVAIL_MIN (1u << 2)
    static struct snd_pcm_sync_ptr sp;
    zero(&sp, sizeof(sp));
    sp.flags = SYNC_APPL | SYNC_AVAIL_MIN;
    if (lx3(SYS_ioctl, pfd, (i64)PCM_SYNC_PTR, (i64)&sp) < 0) goto fail;
    u64 hw0 = sp.s.status.hw_ptr;
    u64 ap0 = sp.c.control.appl_ptr;
    put("[LXSND] hw_ptr "); putn(hw0);
    put(" appl_ptr "); putn(ap0); put("\n");
    nsleep(120);
    zero(&sp, sizeof(sp));
    sp.flags = SYNC_APPL | SYNC_AVAIL_MIN;
    if (lx3(SYS_ioctl, pfd, (i64)PCM_SYNC_PTR, (i64)&sp) < 0) goto fail;
    u64 hw1 = sp.s.status.hw_ptr;
    put("[LXSND] hw_ptr after 120ms: "); putn(hw1); put("\n");
    if (hw1 <= hw0) {
        put("[LXSND] hw_ptr did NOT advance -- DAC is not consuming\n");
        goto fail;
    }
    if (ap0 != total) {
        put("[LXSND] appl_ptr disagrees with frames written\n");
        goto fail;
    }

    lx3(SYS_ioctl, pfd, (i64)PCM_DRAIN, 0);
    lx3(SYS_close, pfd, 0, 0);
    put("[LXSND] VERDICT: PASS\n");
    lx3(SYS_exit_group, 0, 0, 0);

fail:
    put("[LXSND] VERDICT: FAIL at step "); putn((u64)step); put("\n");
    lx3(SYS_exit_group, 10 + step, 0, 0);
    for (;;) { }
}
