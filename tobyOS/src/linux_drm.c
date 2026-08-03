/* linux_drm.c -- /dev/dri: the Linux DRM ABI over virtio-gpu (tier 3 Phase 1b).
 *
 * WHY THIS EXISTS. Chromium never talks to a GPU directly; it goes
 * EGL -> Mesa -> libdrm -> ioctl(/dev/dri/renderD128). Slice 97 proved
 * the pipe UNDER that stack works (a real virglrenderer answered
 * GET_CAPSET_INFO and CTX_CREATE), but nothing in the guest could reach
 * it, because tobyOS had no DRM device node at all -- `grep DRM_IOCTL
 * src/` returned nothing for the whole arc. This file is that missing
 * node.
 *
 * SCOPE, deliberately. This is the RENDER node contract (the one Mesa's
 * virgl driver needs): version, capabilities, context/resource
 * lifecycle, transfers and command submission. It is NOT a KMS/modeset
 * driver -- no CRTCs, connectors, or planes -- because display already
 * works through the existing 2D path and a 3D-only device never owns a
 * scanout (see virtio_gpu.c, slice 97).
 *
 * METHOD, per the arc's standing practice: instrument first. Every
 * ioctl this layer does not implement is logged ONCE with its decoded
 * number as `[drm] UNHANDLED ioctl` -- that log IS the gap list, exactly
 * as `[linux] UNHANDLED syscall` was for the Track B bring-up. Do not
 * guess what Mesa needs; run it and read the list.
 *
 * NAME NOTE. src/gpu_drm.c is an unrelated NATIVE display abstraction
 * that also uses the "drm" name (Milestone 2: displays, surfaces,
 * page-flip). This file is the LINUX ABI one; every symbol here is
 * lxdrm_* so the two can never be confused at a call site.
 *
 * ABI NOTE. Linux ioctl numbers encode direction and struct SIZE, so
 * matching on the whole 32-bit value would break the moment a struct
 * differs by a byte between kernel versions. We dispatch on the command
 * NUMBER (low 8 bits) plus the 'd' type byte, and validate sizes
 * ourselves -- the same tolerant shape the rest of Track B uses.
 */

#include <tobyos/types.h>
#include <tobyos/klibc.h>
#include <tobyos/printk.h>
#include <tobyos/heap.h>
#include <tobyos/pmm.h>
#include <tobyos/vmm.h>
#include <tobyos/proc.h>
#include <tobyos/file.h>
#include <tobyos/uaccess.h>
#include <tobyos/mmap.h>
#include <tobyos/abi/abi.h>
#include <tobyos/virtio_gpu.h>
#include <tobyos/linux_drm.h>

/* ---- ioctl encoding (asm-generic/ioctl.h) ---- */
#define IOC_NRBITS    8u
#define IOC_TYPEBITS  8u
#define IOC_SIZEBITS 14u
#define IOC_NR(x)    ((unsigned)((x) >> 0)  & ((1u << IOC_NRBITS)   - 1))
#define IOC_TYPE(x)  ((unsigned)((x) >> 8)  & ((1u << IOC_TYPEBITS) - 1))
#define IOC_SIZE(x)  ((unsigned)((x) >> 16) & ((1u << IOC_SIZEBITS) - 1))

#define DRM_IOCTL_TYPE 'd'

/* Core DRM command numbers (drm.h). */
#define DRM_NR_VERSION        0x00
#define DRM_NR_GET_UNIQUE     0x01
#define DRM_NR_GET_MAGIC      0x02
#define DRM_NR_IRQ_BUSID      0x03
#define DRM_NR_SET_VERSION    0x07
#define DRM_NR_GEM_CLOSE      0x09
#define DRM_NR_GEM_FLINK      0x0a
#define DRM_NR_GEM_OPEN       0x0b
#define DRM_NR_GET_CAP        0x0c
#define DRM_NR_SET_CLIENT_CAP 0x0d
#define DRM_NR_AUTH_MAGIC     0x11
#define DRM_NR_PRIME_H2FD     0x2d
#define DRM_NR_PRIME_FD2H     0x2e
#define DRM_NR_SYNCOBJ_CREATE 0xbf
#define DRM_NR_SYNCOBJ_DESTROY 0xc0

/* Driver-specific block: DRM_COMMAND_BASE .. DRM_COMMAND_END. */
#define DRM_COMMAND_BASE 0x40
#define VIRTGPU_NR_MAP                 (DRM_COMMAND_BASE + 0x01)
#define VIRTGPU_NR_EXECBUFFER          (DRM_COMMAND_BASE + 0x02)
#define VIRTGPU_NR_GETPARAM            (DRM_COMMAND_BASE + 0x03)
#define VIRTGPU_NR_RESOURCE_CREATE     (DRM_COMMAND_BASE + 0x04)
#define VIRTGPU_NR_RESOURCE_INFO       (DRM_COMMAND_BASE + 0x05)
#define VIRTGPU_NR_TRANSFER_FROM_HOST  (DRM_COMMAND_BASE + 0x06)
#define VIRTGPU_NR_TRANSFER_TO_HOST    (DRM_COMMAND_BASE + 0x07)
#define VIRTGPU_NR_WAIT                (DRM_COMMAND_BASE + 0x08)
#define VIRTGPU_NR_GET_CAPS            (DRM_COMMAND_BASE + 0x09)
#define VIRTGPU_NR_RESOURCE_CREATE_BLOB (DRM_COMMAND_BASE + 0x0a)
#define VIRTGPU_NR_CONTEXT_INIT        (DRM_COMMAND_BASE + 0x0b)

/* ---- uapi structs (drm.h / virtgpu_drm.h), x86-64 layout ---- */

struct drm_version_u {
    int      version_major;
    int      version_minor;
    int      version_patchlevel;
    uint64_t name_len;    uint64_t name;
    uint64_t date_len;    uint64_t date;
    uint64_t desc_len;    uint64_t desc;
};

struct drm_get_cap_u    { uint64_t capability; uint64_t value; };
struct drm_gem_close_u  { uint32_t handle; uint32_t pad; };

struct drm_virtgpu_getparam_u { uint64_t param; uint64_t value; };

struct drm_virtgpu_get_caps_u {
    uint32_t cap_set_id;
    uint32_t cap_set_ver;
    uint64_t addr;
    uint32_t size;
    uint32_t pad;
};

struct drm_virtgpu_resource_create_u {
    uint32_t target;      uint32_t format;
    uint32_t bind;        uint32_t width;
    uint32_t height;      uint32_t depth;
    uint32_t array_size;  uint32_t last_level;
    uint32_t nr_samples;  uint32_t flags;
    uint32_t bo_handle;   /* out */
    uint32_t res_handle;  /* out */
    uint32_t size;        uint32_t stride;
};

struct drm_virtgpu_map_u { uint64_t offset; uint32_t handle; uint32_t pad; };

struct drm_virtgpu_execbuffer_u {
    uint32_t flags;
    uint32_t size;
    uint64_t command;
    uint64_t bo_handles;
    uint32_t num_bo_handles;
    int32_t  fence_fd;
};

struct drm_virtgpu_3d_box_u { uint32_t x, y, z, w, h, d; };

struct drm_virtgpu_3d_transfer_u {          /* to_host and from_host */
    uint32_t bo_handle;
    struct drm_virtgpu_3d_box_u box;
    uint32_t level;
    uint32_t offset;
    uint32_t stride;
    uint32_t layer_stride;
};

struct drm_virtgpu_3d_wait_u { uint32_t handle; uint32_t flags; };

struct drm_virtgpu_context_init_u {
    uint32_t num_params;
    uint32_t pad;
    uint64_t ctx_set_params;
};

/* VIRTGPU_GETPARAM ids (virtgpu_drm.h). */
#define VIRTGPU_PARAM_3D_FEATURES          1
#define VIRTGPU_PARAM_CAPSET_QUERY_FIX     2
#define VIRTGPU_PARAM_RESOURCE_BLOB        3
#define VIRTGPU_PARAM_HOST_VISIBLE         4
#define VIRTGPU_PARAM_CROSS_DEVICE         5
#define VIRTGPU_PARAM_CONTEXT_INIT         6
#define VIRTGPU_PARAM_SUPPORTED_CAPSET_IDs 7
#define VIRTGPU_PARAM_EXPLICIT_DEBUG_NAME  8

/* DRM_CAP ids we may be asked about (drm.h). */
#define DRM_CAP_PRIME       0x5
#define DRM_CAP_SYNCOBJ     0x13
#define DRM_CAP_SYNCOBJ_TIMELINE 0x14

/* ---- GEM object table ----------------------------------------------
 *
 * A DRM "buffer object" here is physically contiguous guest memory plus
 * the virtio-gpu resource id it backs. Contiguity keeps ATTACH_BACKING
 * to a single memory entry and makes the mmap trivial; Mesa's buffers in
 * this path are modest (command/staging/texture uploads), and an
 * allocation that cannot be satisfied fails loudly rather than silently
 * degrading. */
#define DRM_MAX_BO 256

struct drm_bo {
    bool     used;
    uint32_t handle;        /* GEM handle (1-based) */
    uint32_t res_id;        /* virtio-gpu resource id */
    uint64_t phys;          /* contiguous backing store */
    uint64_t size;          /* bytes (page-multiple) */
    uint32_t pages;
    uint64_t map_offset;    /* fake mmap cookie handed to VIRTGPU_MAP */
    uint64_t user_va;       /* current mapping, 0 if unmapped */
};

struct drm_ctx {
    bool          open;
    int           owner_tgid;
    uint32_t      ctx_id;       /* virtio-gpu 3D context, 0 = not created */
    struct drm_bo bo[DRM_MAX_BO];
    uint32_t      next_handle;
    uint32_t      next_res_id;
    uint64_t      next_map_off;
    uint64_t      ioctls;       /* served */
    uint64_t      submits;      /* EXECBUFFER commands submitted */
    uint64_t      submit_bytes;
};

static struct drm_ctx g_drm;

/* Resource ids 1..15 belong to the 2D display path (scanout, cursor,
 * per-window resources); 3D starts well clear of them. */
#define DRM_FIRST_RES_ID   64u
#define DRM_MAP_OFF_BASE   0x100000000ull

/* Backing-entry scratch for ATTACH_BACKING (one entry: contiguous BO). */
struct __attribute__((packed)) drm_mem_entry { uint64_t addr; uint32_t len; uint32_t pad; };
static uint64_t g_entry_phys;
static struct drm_mem_entry *g_entry_virt;

static struct drm_bo *bo_find(uint32_t handle) {
    if (handle == 0) return 0;
    for (int i = 0; i < DRM_MAX_BO; i++)
        if (g_drm.bo[i].used && g_drm.bo[i].handle == handle)
            return &g_drm.bo[i];
    return 0;
}

static struct drm_bo *bo_alloc(void) {
    for (int i = 0; i < DRM_MAX_BO; i++)
        if (!g_drm.bo[i].used) return &g_drm.bo[i];
    return 0;
}

static void bo_free(struct drm_bo *b) {
    if (!b || !b->used) return;
    if (b->phys) pmm_free_pages_range(b->phys, b->pages);
    memset(b, 0, sizeof(*b));
}

/* ---- open/close ---- */

bool lxdrm_available(void) {
    return virtio_gpu_virgl_available();
}

struct file *lxdrm_open(const char *node) {
    if (!lxdrm_available()) return 0;          /* no 3D device -> ENOENT */
    struct file *f = (struct file *)kmalloc(sizeof(*f));
    if (!f) return 0;
    memset(f, 0, sizeof(*f));
    f->kind = FILE_KIND_DRM;
    /* Slice 99: remember which node this is, so fstat can report the
     * right character-device minor (Linux: card* = 0.., renderD* = 128..).
     * Mesa/GBM reads the minor to classify the node as render vs primary,
     * and refuses an fd that is not a char device at all. */
    {
        const char *base = node;
        for (const char *p = node; *p; p++) if (*p == '/') base = p + 1;
        f->dir_off = (base[0] == 'r') ? 128u : 0u;   /* renderD128 : card0 */
    }

    if (!g_drm.open) {
        struct proc *p = current_proc();
        memset(&g_drm, 0, sizeof(g_drm));
        g_drm.open        = true;
        g_drm.owner_tgid  = p ? (p->is_thread ? p->tgid : p->pid) : 0;
        g_drm.next_handle = 1;
        g_drm.next_res_id = DRM_FIRST_RES_ID;
        g_drm.next_map_off = DRM_MAP_OFF_BASE;
        kprintf("[drm] open %s (virgl capset=%u max=%u)\n", node,
                virtio_gpu_capset_id(), virtio_gpu_capset_max_size());
    }
    return f;
}

void lxdrm_close(struct file *f) {
    (void)f;
    /* Objects live until the process exits; a real driver reaps per-fd.
     * Single-client scope (see the header comment) makes that safe here,
     * and holding resources is far cheaper than tearing down host state
     * a still-live Mesa context may reference. */
}

/* ---- the gap-list instrument ---- */

static void drm_unhandled(unsigned nr) {
    static unsigned seen[64];
    static int nseen;
    for (int i = 0; i < nseen; i++) if (seen[i] == nr) return;
    if (nseen < 64) seen[nseen++] = nr;
    kprintf("[drm] UNHANDLED ioctl nr=0x%x%s -- gap list\n", nr,
            nr >= DRM_COMMAND_BASE ? " (virtgpu-specific)" : " (core DRM)");
}

/* ---- ioctl handlers ---- */

static long drm_ioc_version(unsigned long arg) {
    struct drm_version_u v;
    if (copy_from_user(&v, (void *)(uintptr_t)arg, sizeof(v)) != 0)
        return -ABI_EFAULT;
    /* Mesa matches the driver by NAME; it must read "virtio_gpu". */
    static const char name[] = "virtio_gpu";
    static const char date[] = "20200626";
    static const char desc[] = "virtio GPU (tobyOS)";
    v.version_major = 0; v.version_minor = 1; v.version_patchlevel = 0;

    /* Linux semantics: a caller with NULL pointers is asking for the
     * LENGTHS only (the classic two-pass query). Copy only what fits. */
    uint64_t nl = sizeof(name) - 1, dl = sizeof(date) - 1, sl = sizeof(desc) - 1;
    if (v.name && v.name_len) {
        uint64_t n = v.name_len < nl ? v.name_len : nl;
        if (copy_to_user((void *)(uintptr_t)v.name, name, n) != 0)
            return -ABI_EFAULT;
    }
    if (v.date && v.date_len) {
        uint64_t n = v.date_len < dl ? v.date_len : dl;
        if (copy_to_user((void *)(uintptr_t)v.date, date, n) != 0)
            return -ABI_EFAULT;
    }
    if (v.desc && v.desc_len) {
        uint64_t n = v.desc_len < sl ? v.desc_len : sl;
        if (copy_to_user((void *)(uintptr_t)v.desc, desc, n) != 0)
            return -ABI_EFAULT;
    }
    v.name_len = nl; v.date_len = dl; v.desc_len = sl;
    if (copy_to_user((void *)(uintptr_t)arg, &v, sizeof(v)) != 0)
        return -ABI_EFAULT;
    return 0;
}

static long drm_ioc_get_cap(unsigned long arg) {
    struct drm_get_cap_u c;
    if (copy_from_user(&c, (void *)(uintptr_t)arg, sizeof(c)) != 0)
        return -ABI_EFAULT;
    switch (c.capability) {
    case DRM_CAP_PRIME: c.value = 0; break;   /* no dma-buf export yet */
    default:            c.value = 0; break;
    }
    if (copy_to_user((void *)(uintptr_t)arg, &c, sizeof(c)) != 0)
        return -ABI_EFAULT;
    return 0;
}

static long drm_ioc_getparam(unsigned long arg) {
    struct drm_virtgpu_getparam_u g;
    if (copy_from_user(&g, (void *)(uintptr_t)arg, sizeof(g)) != 0)
        return -ABI_EFAULT;
    uint64_t val;
    switch (g.param) {
    case VIRTGPU_PARAM_3D_FEATURES:      val = 1; break;
    case VIRTGPU_PARAM_CAPSET_QUERY_FIX: val = 1; break;
    case VIRTGPU_PARAM_CONTEXT_INIT:     val = 1; break;
    /* Blob resources and host-visible memory are a later slice: saying
     * "no" keeps Mesa on the classic resource path this layer serves. */
    case VIRTGPU_PARAM_RESOURCE_BLOB:    val = 0; break;
    case VIRTGPU_PARAM_HOST_VISIBLE:     val = 0; break;
    case VIRTGPU_PARAM_CROSS_DEVICE:     val = 0; break;
    case VIRTGPU_PARAM_SUPPORTED_CAPSET_IDs:
        val = (uint64_t)1u << virtio_gpu_capset_id();
        break;
    default:
        kprintf("[drm] GETPARAM unknown id=%lu -> 0 (gap list)\n",
                (unsigned long)g.param);
        val = 0;
        break;
    }
    if (!g.value) return -ABI_EINVAL;
    if (copy_to_user((void *)(uintptr_t)g.value, &val, sizeof(val)) != 0)
        return -ABI_EFAULT;
    return 0;
}

static long drm_ioc_get_caps(unsigned long arg) {
    struct drm_virtgpu_get_caps_u c;
    if (copy_from_user(&c, (void *)(uintptr_t)arg, sizeof(c)) != 0)
        return -ABI_EFAULT;
    if (!c.addr || c.size == 0) return -ABI_EINVAL;

    uint32_t want = c.size;
    uint32_t max  = virtio_gpu_capset_max_size();
    if (max && want > max) want = max;

    void *buf = kmalloc(want);
    if (!buf) return -ABI_ENOMEM;
    memset(buf, 0, want);
    int rc = virtio_gpu_get_capset(c.cap_set_id ? c.cap_set_id
                                                : virtio_gpu_capset_id(),
                                   c.cap_set_ver, buf, want);
    if (rc == 0 && copy_to_user((void *)(uintptr_t)c.addr, buf, want) != 0)
        rc = -1;
    kfree(buf);
    if (rc != 0) return -ABI_EINVAL;
    kprintf("[drm] GET_CAPS id=%u ver=%u -> %u bytes\n",
            c.cap_set_id, c.cap_set_ver, want);
    return 0;
}

/* Lazily create the single virtio-gpu 3D context this fd renders in.
 * Mesa may or may not call CONTEXT_INIT; either way the first command
 * that needs a context gets one. */
static int drm_ensure_ctx(void) {
    if (g_drm.ctx_id) return 0;
    uint32_t id = 2;                    /* 1 is the probe's proof context */
    if (virtio_gpu_ctx_create(id, "mesa-virgl") != 0) {
        kprintf("[drm] CTX_CREATE(%u) failed\n", id);
        return -1;
    }
    g_drm.ctx_id = id;
    kprintf("[drm] 3D context %u created\n", id);
    return 0;
}

static long drm_ioc_context_init(unsigned long arg) {
    struct drm_virtgpu_context_init_u ci;
    if (copy_from_user(&ci, (void *)(uintptr_t)arg, sizeof(ci)) != 0)
        return -ABI_EFAULT;
    /* Params select capset/debug-name; we run one capset, so honouring
     * the call is enough. */
    if (drm_ensure_ctx() != 0) return -ABI_EINVAL;
    return 0;
}

static long drm_ioc_resource_create(unsigned long arg) {
    struct drm_virtgpu_resource_create_u r;
    if (copy_from_user(&r, (void *)(uintptr_t)arg, sizeof(r)) != 0)
        return -ABI_EFAULT;
    if (drm_ensure_ctx() != 0) return -ABI_EINVAL;

    struct drm_bo *b = bo_alloc();
    if (!b) return -ABI_ENOMEM;

    /* Size: honour the caller's when given, else derive a linear guess.
     * (Mesa fills `size` for buffers and leaves it 0 for textures.) */
    uint64_t sz = r.size;
    if (sz == 0) {
        uint32_t h = r.height ? r.height : 1;
        uint32_t d = r.depth  ? r.depth  : 1;
        uint32_t stride = r.stride ? r.stride : r.width * 4u;
        sz = (uint64_t)stride * h * d;
    }
    if (sz == 0) sz = PAGE_SIZE;
    uint32_t pages = (uint32_t)((sz + PAGE_SIZE - 1) / PAGE_SIZE);
    uint64_t phys = pmm_alloc_pages(pages);
    if (!phys) {
        kprintf("[drm] RESOURCE_CREATE: OOM for %u pages\n", pages);
        return -ABI_ENOMEM;
    }
    memset((void *)pmm_phys_to_virt(phys), 0, (size_t)pages * PAGE_SIZE);

    uint32_t res_id = g_drm.next_res_id++;
    if (virtio_gpu_res_create_3d(res_id, r.target, r.format, r.bind,
                                 r.width, r.height, r.depth,
                                 r.array_size, r.last_level,
                                 r.nr_samples, r.flags) != 0) {
        pmm_free_pages_range(phys, pages);
        return -ABI_EINVAL;
    }

    /* Back it with our contiguous pages (one memory entry). */
    if (!g_entry_virt) {
        g_entry_phys = pmm_alloc_page();
        if (g_entry_phys)
            g_entry_virt = (struct drm_mem_entry *)pmm_phys_to_virt(g_entry_phys);
    }
    if (g_entry_virt) {
        g_entry_virt[0].addr = phys;
        g_entry_virt[0].len  = (uint32_t)(pages * PAGE_SIZE);
        g_entry_virt[0].pad  = 0;
        if (virtio_gpu_res_attach_backing(res_id, g_entry_phys, 1) != 0)
            kprintf("[drm] ATTACH_BACKING(res=%u) failed -- transfers will "
                    "not see guest data\n", res_id);
    }
    virtio_gpu_ctx_resource(g_drm.ctx_id, res_id, true);

    b->used       = true;
    b->handle     = g_drm.next_handle++;
    b->res_id     = res_id;
    b->phys       = phys;
    b->pages      = pages;
    b->size       = (uint64_t)pages * PAGE_SIZE;
    b->map_offset = g_drm.next_map_off;
    g_drm.next_map_off += b->size;

    r.bo_handle  = b->handle;
    r.res_handle = res_id;
    r.size       = (uint32_t)b->size;
    if (copy_to_user((void *)(uintptr_t)arg, &r, sizeof(r)) != 0)
        return -ABI_EFAULT;
    return 0;
}

static long drm_ioc_map(unsigned long arg) {
    struct drm_virtgpu_map_u m;
    if (copy_from_user(&m, (void *)(uintptr_t)arg, sizeof(m)) != 0)
        return -ABI_EFAULT;
    struct drm_bo *b = bo_find(m.handle);
    if (!b) return -ABI_EINVAL;
    m.offset = b->map_offset;           /* cookie for the following mmap */
    if (copy_to_user((void *)(uintptr_t)arg, &m, sizeof(m)) != 0)
        return -ABI_EFAULT;
    return 0;
}

static long drm_ioc_execbuffer(unsigned long arg) {
    struct drm_virtgpu_execbuffer_u e;
    if (copy_from_user(&e, (void *)(uintptr_t)arg, sizeof(e)) != 0)
        return -ABI_EFAULT;
    if (!e.command || e.size == 0) return -ABI_EINVAL;
    if (drm_ensure_ctx() != 0) return -ABI_EINVAL;

    void *cmd = kmalloc(e.size);
    if (!cmd) return -ABI_ENOMEM;
    if (copy_from_user(cmd, (void *)(uintptr_t)e.command, e.size) != 0) {
        kfree(cmd);
        return -ABI_EFAULT;
    }
    int rc = virtio_gpu_submit_3d(g_drm.ctx_id, cmd, e.size);
    kfree(cmd);
    if (rc != 0) return -ABI_EINVAL;

    g_drm.submits++;
    g_drm.submit_bytes += e.size;
    if (g_drm.submits <= 4 || (g_drm.submits % 256) == 0)
        kprintf("[drm] EXECBUFFER #%lu size=%u (total %lu KiB)\n",
                (unsigned long)g_drm.submits, e.size,
                (unsigned long)(g_drm.submit_bytes / 1024));

    /* Synchronous submission: the command is retired by the time
     * virtio_gpu_submit_3d returns, so an out-fence is already signalled.
     * -1 means "no fence fd", which libdrm treats as immediate. */
    if (e.fence_fd != -1) {
        e.fence_fd = -1;
        (void)copy_to_user((void *)(uintptr_t)arg, &e, sizeof(e));
    }
    return 0;
}

static long drm_ioc_transfer(unsigned long arg, bool to_host) {
    struct drm_virtgpu_3d_transfer_u t;
    if (copy_from_user(&t, (void *)(uintptr_t)arg, sizeof(t)) != 0)
        return -ABI_EFAULT;
    struct drm_bo *b = bo_find(t.bo_handle);
    if (!b) return -ABI_EINVAL;
    if (drm_ensure_ctx() != 0) return -ABI_EINVAL;

    if (virtio_gpu_transfer_3d(g_drm.ctx_id, b->res_id, to_host,
                               t.box.x, t.box.y, t.box.z,
                               t.box.w, t.box.h, t.box.d,
                               t.offset, t.level, t.stride,
                               t.layer_stride) != 0)
        return -ABI_EINVAL;
    return 0;
}

static long drm_ioc_wait(unsigned long arg) {
    (void)arg;
    /* Every submission path here is synchronous, so a wait is already
     * satisfied. (When fences arrive this must actually block.) */
    return 0;
}

static long drm_ioc_gem_close(unsigned long arg) {
    struct drm_gem_close_u c;
    if (copy_from_user(&c, (void *)(uintptr_t)arg, sizeof(c)) != 0)
        return -ABI_EFAULT;
    struct drm_bo *b = bo_find(c.handle);
    if (!b) return 0;                        /* already gone: not an error */
    if (g_drm.ctx_id) virtio_gpu_ctx_resource(g_drm.ctx_id, b->res_id, false);
    bo_free(b);
    return 0;
}

static long lxdrm_ioctl_inner(unsigned nr, unsigned long arg);

/* Slice 99: name every ioctl and its result for the first N calls. The
 * gap list tells us which ioctls we DON'T implement; this tells us
 * which implemented one Mesa disliked -- measured need: the virgl
 * driver loaded, issued 2 ioctls, and fell back to swrast without ever
 * reaching GET_CAPS, which "UNHANDLED" alone could never explain. */
static const char *drm_nr_name(unsigned nr) {
    switch (nr) {
    case DRM_NR_VERSION:                return "VERSION";
    case DRM_NR_GET_CAP:                return "GET_CAP";
    case DRM_NR_SET_CLIENT_CAP:         return "SET_CLIENT_CAP";
    case DRM_NR_SET_VERSION:            return "SET_VERSION";
    case DRM_NR_GEM_CLOSE:              return "GEM_CLOSE";
    case DRM_NR_GET_UNIQUE:             return "GET_UNIQUE";
    case DRM_NR_GET_MAGIC:              return "GET_MAGIC";
    case DRM_NR_PRIME_H2FD:             return "PRIME_HANDLE_TO_FD";
    case DRM_NR_PRIME_FD2H:             return "PRIME_FD_TO_HANDLE";
    case VIRTGPU_NR_MAP:                return "VIRTGPU_MAP";
    case VIRTGPU_NR_EXECBUFFER:         return "VIRTGPU_EXECBUFFER";
    case VIRTGPU_NR_GETPARAM:           return "VIRTGPU_GETPARAM";
    case VIRTGPU_NR_RESOURCE_CREATE:    return "VIRTGPU_RESOURCE_CREATE";
    case VIRTGPU_NR_RESOURCE_INFO:      return "VIRTGPU_RESOURCE_INFO";
    case VIRTGPU_NR_TRANSFER_FROM_HOST: return "VIRTGPU_TRANSFER_FROM_HOST";
    case VIRTGPU_NR_TRANSFER_TO_HOST:   return "VIRTGPU_TRANSFER_TO_HOST";
    case VIRTGPU_NR_WAIT:               return "VIRTGPU_WAIT";
    case VIRTGPU_NR_GET_CAPS:           return "VIRTGPU_GET_CAPS";
    case VIRTGPU_NR_RESOURCE_CREATE_BLOB: return "VIRTGPU_RES_CREATE_BLOB";
    case VIRTGPU_NR_CONTEXT_INIT:       return "VIRTGPU_CONTEXT_INIT";
    default:                            return "?";
    }
}

long lxdrm_ioctl(struct file *f, unsigned long req, unsigned long arg) {
    (void)f;
    if (IOC_TYPE(req) != DRM_IOCTL_TYPE) return -ABI_ENOTTY;
    g_drm.ioctls++;
    unsigned nr = IOC_NR(req);
    long trc = lxdrm_ioctl_inner(nr, arg);
    if (g_drm.ioctls <= 40)
        kprintf("[drm] ioctl #%lu %s (nr=0x%x sz=%u) -> %ld\n",
                (unsigned long)g_drm.ioctls, drm_nr_name(nr), nr,
                IOC_SIZE(req), trc);
    return trc;
}

static long lxdrm_ioctl_inner(unsigned nr, unsigned long arg) {
    switch (nr) {
    case DRM_NR_VERSION:               return drm_ioc_version(arg);
    case DRM_NR_GET_CAP:               return drm_ioc_get_cap(arg);
    case DRM_NR_SET_CLIENT_CAP:        return 0;      /* accept + ignore */
    case DRM_NR_SET_VERSION:           return 0;
    case DRM_NR_GEM_CLOSE:             return drm_ioc_gem_close(arg);
    case VIRTGPU_NR_GETPARAM:          return drm_ioc_getparam(arg);
    case VIRTGPU_NR_GET_CAPS:          return drm_ioc_get_caps(arg);
    case VIRTGPU_NR_CONTEXT_INIT:      return drm_ioc_context_init(arg);
    case VIRTGPU_NR_RESOURCE_CREATE:   return drm_ioc_resource_create(arg);
    case VIRTGPU_NR_MAP:               return drm_ioc_map(arg);
    case VIRTGPU_NR_EXECBUFFER:        return drm_ioc_execbuffer(arg);
    case VIRTGPU_NR_TRANSFER_TO_HOST:  return drm_ioc_transfer(arg, true);
    case VIRTGPU_NR_TRANSFER_FROM_HOST:return drm_ioc_transfer(arg, false);
    case VIRTGPU_NR_WAIT:              return drm_ioc_wait(arg);
    default:
        drm_unhandled(nr);
        return -ABI_ENOTTY;
    }
}

/* mmap(/dev/dri/...) with the cookie VIRTGPU_MAP handed back: map the
 * BO's contiguous pages into the caller at a fresh user address. */
long lxdrm_mmap(uint64_t offset, uint64_t len) {
    struct drm_bo *b = 0;
    for (int i = 0; i < DRM_MAX_BO; i++) {
        if (g_drm.bo[i].used && g_drm.bo[i].map_offset == offset) {
            b = &g_drm.bo[i];
            break;
        }
    }
    if (!b) {
        kprintf("[drm] mmap: no BO for offset 0x%lx\n", (unsigned long)offset);
        return -ABI_EINVAL;
    }
    if (len > b->size) len = b->size;
    long va = mmap_map_phys_user(b->phys, len);
    if (va < 0) return -ABI_ENOMEM;
    b->user_va = (uint64_t)va;
    return va;
}

void lxdrm_dump_stats(void) {
    if (!g_drm.open) return;
    kprintf("[drm] ioctls=%lu submits=%lu (%lu KiB) ctx=%u bos=",
            (unsigned long)g_drm.ioctls, (unsigned long)g_drm.submits,
            (unsigned long)(g_drm.submit_bytes / 1024), g_drm.ctx_id);
    int n = 0;
    for (int i = 0; i < DRM_MAX_BO; i++) if (g_drm.bo[i].used) n++;
    kprintf("%d\n", n);
}
