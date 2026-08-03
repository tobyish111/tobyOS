/* programs/linux-drmtest/main.c
 *
 * Tier 3 Phase 1b proof: exercise /dev/dri/renderD128 exactly the way
 * Mesa's virgl driver does at start-up, from a plain static Linux ELF.
 * This is the guard that says the DRM render node is REAL -- not "the
 * node exists", but "a real virglrenderer answered through it".
 *
 * The walk, in Mesa's own order:
 *   open(/dev/dri/renderD128)
 *   DRM_IOCTL_VERSION            -> driver name must be "virtio_gpu"
 *   VIRTGPU_GETPARAM(3D_FEATURES)-> must be 1 (host has virgl)
 *   VIRTGPU_GET_CAPS             -> a non-zero capability blob
 *   VIRTGPU_CONTEXT_INIT         -> a rendering context
 *   VIRTGPU_RESOURCE_CREATE      -> a 3D resource + GEM handle
 *   VIRTGPU_MAP + mmap           -> CPU-visible buffer, write/read it
 *   VIRTGPU_TRANSFER_TO_HOST     -> push those bytes to the host GPU
 *   VIRTGPU_EXECBUFFER           -> submit a virgl command stream
 *   GEM_CLOSE                    -> release
 *
 * exit_group(): 3 = PASS, 2 = ERROR/setup, or 10+step on the failing
 * step, so a failure names itself in one line of serial.
 */

typedef unsigned long u64;
typedef long          i64;
typedef unsigned int  u32;
typedef int           i32;

static inline i64 lx6(i64 n, i64 a, i64 b, i64 c, i64 d, i64 e, i64 f) {
    register i64 r10 __asm__("r10") = d;
    register i64 r8  __asm__("r8")  = e;
    register i64 r9  __asm__("r9")  = f;
    i64 ret;
    __asm__ volatile("syscall" : "=a"(ret)
                     : "a"(n), "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8), "r"(r9)
                     : "rcx", "r11", "memory");
    return ret;
}
#define SC1(n,a)           lx6(n,(i64)(a),0,0,0,0,0)
#define SC3(n,a,b,c)       lx6(n,(i64)(a),(i64)(b),(i64)(c),0,0,0)
#define SC6(n,a,b,c,d,e,f) lx6(n,(i64)(a),(i64)(b),(i64)(c),(i64)(d),(i64)(e),(i64)(f))

#define SYS_write 1
#define SYS_open  2
#define SYS_close 3
#define SYS_mmap  9
#define SYS_ioctl 16
#define SYS_exit_group 231

#define O_RDWR 2

/* ioctl encoding */
#define _IOC(dir,type,nr,size) (((dir)<<30)|((size)<<16)|((type)<<8)|(nr))
#define _IOWR(t,nr,sz) _IOC(3u,(t),(nr),(sz))
#define _IOW(t,nr,sz)  _IOC(1u,(t),(nr),(sz))
#define DRM_T 'd'
#define DRM_COMMAND_BASE 0x40

struct drm_version {
    int v_major, v_minor, v_patch;
    u64 name_len; u64 name;
    u64 date_len; u64 date;
    u64 desc_len; u64 desc;
};
struct drm_gem_close { u32 handle, pad; };
struct vg_getparam   { u64 param; u64 value; };
struct vg_get_caps   { u32 cap_set_id, cap_set_ver; u64 addr; u32 size, pad; };
struct vg_res_create {
    u32 target, format, bind, width, height, depth, array_size,
        last_level, nr_samples, flags, bo_handle, res_handle, size, stride;
};
struct vg_map        { u64 offset; u32 handle, pad; };
struct vg_execbuffer { u32 flags, size; u64 command, bo_handles;
                       u32 num_bo_handles; i32 fence_fd; };
struct vg_box        { u32 x, y, z, w, h, d; };
struct vg_transfer   { u32 bo_handle; struct vg_box box;
                       u32 level, offset, stride, layer_stride; };
struct vg_ctx_init   { u32 num_params, pad; u64 ctx_set_params; };

#define DRM_IOCTL_VERSION    _IOWR(DRM_T, 0x00, sizeof(struct drm_version))
#define DRM_IOCTL_GEM_CLOSE  _IOW (DRM_T, 0x09, sizeof(struct drm_gem_close))
#define VG_IOCTL_MAP         _IOWR(DRM_T, DRM_COMMAND_BASE+0x01, sizeof(struct vg_map))
#define VG_IOCTL_EXECBUFFER  _IOW (DRM_T, DRM_COMMAND_BASE+0x02, sizeof(struct vg_execbuffer))
#define VG_IOCTL_GETPARAM    _IOWR(DRM_T, DRM_COMMAND_BASE+0x03, sizeof(struct vg_getparam))
#define VG_IOCTL_RES_CREATE  _IOWR(DRM_T, DRM_COMMAND_BASE+0x04, sizeof(struct vg_res_create))
#define VG_IOCTL_XFER_TO     _IOW (DRM_T, DRM_COMMAND_BASE+0x07, sizeof(struct vg_transfer))
#define VG_IOCTL_GET_CAPS    _IOWR(DRM_T, DRM_COMMAND_BASE+0x09, sizeof(struct vg_get_caps))
#define VG_IOCTL_CTX_INIT    _IOWR(DRM_T, DRM_COMMAND_BASE+0x0b, sizeof(struct vg_ctx_init))

#define VIRTGPU_PARAM_3D_FEATURES 1

static void put(const char *s){ long n=0; while(s[n])n++; SC3(SYS_write,1,s,n); }
static void put_u(const char *pfx, u64 v) {
    char b[96]; long n=0;
    while (pfx[n]) { b[n]=pfx[n]; n++; }
    char d[24]; int nd=0;
    do { d[nd++]=(char)('0'+(v%10)); v/=10; } while (v && nd<24);
    while (nd) b[n++]=d[--nd];
    b[n++]='\n';
    SC3(SYS_write,1,b,n);
}

extern void _start(void);
__asm__(".globl _start\n_start:\n  call main_c\n  mov %rax,%rdi\n"
        "  mov $231,%rax\n  syscall\n");

/* A minimal, well-formed virgl command stream: CREATE_SUB_CTX(1).
 * virgl command header = (len<<16) | (type<<8) | objtype, followed by
 * `len` dwords. VIRGL_CCMD_CREATE_SUB_CTX = 0x25 with one payload dword.
 * We only need the host to ACCEPT a stream -- proving the submission
 * path end-to-end -- not to draw anything yet. */
#define VIRGL_CCMD_CREATE_SUB_CTX 0x25
static u32 g_cmd[2];

static char g_name[32];

int main_c(void);
int main_c(void) {
    put("[drmtest] start\n");

    int fd = (int)SC3(SYS_open, "/dev/dri/renderD128", O_RDWR, 0);
    if (fd < 0) {
        fd = (int)SC3(SYS_open, "/dev/dri/card0", O_RDWR, 0);
        if (fd < 0) { put("[drmtest] open /dev/dri FAILED\n"); return 11; }
    }

    /* 1. VERSION -- Mesa matches its driver on this exact name. */
    struct drm_version ver;
    for (u64 i=0;i<sizeof(ver);i++) ((char*)&ver)[i]=0;
    ver.name_len = sizeof(g_name)-1;
    ver.name     = (u64)(unsigned long)g_name;
    if (SC3(SYS_ioctl, fd, DRM_IOCTL_VERSION, &ver) < 0) {
        put("[drmtest] VERSION FAILED\n"); return 12;
    }
    put("[drmtest] driver name: "); put(g_name); put("\n");
    if (!(g_name[0]=='v'&&g_name[1]=='i'&&g_name[2]=='r'&&g_name[3]=='t'&&
          g_name[4]=='i'&&g_name[5]=='o'&&g_name[6]=='_'&&g_name[7]=='g')) {
        put("[drmtest] driver name is not virtio_gpu\n"); return 12;
    }

    /* 2. GETPARAM(3D_FEATURES) -- is the host actually 3D-capable? */
    u64 val = 0;
    struct vg_getparam gp = { VIRTGPU_PARAM_3D_FEATURES,
                              (u64)(unsigned long)&val };
    if (SC3(SYS_ioctl, fd, VG_IOCTL_GETPARAM, &gp) < 0) {
        put("[drmtest] GETPARAM FAILED\n"); return 13;
    }
    put_u("[drmtest] 3D_FEATURES=", val);
    if (val != 1) { put("[drmtest] host reports no 3D\n"); return 13; }

    /* 3. GET_CAPS -- the real virglrenderer capability blob. */
    static unsigned char caps[512];
    struct vg_get_caps gc = { 0, 0, (u64)(unsigned long)caps, 308, 0 };
    if (SC3(SYS_ioctl, fd, VG_IOCTL_GET_CAPS, &gc) < 0) {
        put("[drmtest] GET_CAPS FAILED\n"); return 14;
    }
    u32 nz = 0;
    for (int i = 0; i < 308; i++) if (caps[i]) nz++;
    put_u("[drmtest] caps non-zero bytes=", nz);
    if (nz == 0) { put("[drmtest] caps blob is all zeros\n"); return 14; }

    /* 4. CONTEXT_INIT */
    struct vg_ctx_init ci = { 0, 0, 0 };
    if (SC3(SYS_ioctl, fd, VG_IOCTL_CTX_INIT, &ci) < 0) {
        put("[drmtest] CONTEXT_INIT FAILED\n"); return 15;
    }

    /* 5. RESOURCE_CREATE: a 256x256 BGRA texture (target 2 = TEXTURE_2D,
     *    format 1 = B8G8R8A8_UNORM, bind 2 = RENDER_TARGET). */
    struct vg_res_create rc;
    for (u64 i=0;i<sizeof(rc);i++) ((char*)&rc)[i]=0;
    rc.target = 2; rc.format = 1; rc.bind = 2;
    rc.width = 256; rc.height = 256; rc.depth = 1;
    rc.array_size = 1; rc.size = 256*256*4; rc.stride = 256*4;
    if (SC3(SYS_ioctl, fd, VG_IOCTL_RES_CREATE, &rc) < 0) {
        put("[drmtest] RESOURCE_CREATE FAILED\n"); return 16;
    }
    put_u("[drmtest] bo_handle=", rc.bo_handle);
    put_u("[drmtest] res_handle=", rc.res_handle);
    if (!rc.bo_handle || !rc.res_handle) return 16;

    /* 6. MAP + mmap: the buffer must be CPU-visible and coherent. */
    struct vg_map mp = { 0, rc.bo_handle, 0 };
    if (SC3(SYS_ioctl, fd, VG_IOCTL_MAP, &mp) < 0) {
        put("[drmtest] MAP FAILED\n"); return 17;
    }
    i64 va = SC6(SYS_mmap, 0, rc.size, 0x1|0x2 /*RW*/, 0x01 /*SHARED*/,
                 fd, mp.offset);
    if (va < 0 && va > -4096) { put("[drmtest] mmap FAILED\n"); return 17; }
    volatile u32 *px = (volatile u32 *)va;
    for (int i = 0; i < 1024; i++) px[i] = 0xff00ff00u + (u32)i;
    u32 rb = px[1023];
    if (rb != 0xff00ff00u + 1023u) {
        put("[drmtest] mapped buffer read-back MISMATCH\n"); return 17;
    }
    put("[drmtest] BO mapped + coherent\n");

    /* 7. TRANSFER_TO_HOST: push the pixels we just wrote to the GPU. */
    struct vg_transfer tr;
    for (u64 i=0;i<sizeof(tr);i++) ((char*)&tr)[i]=0;
    tr.bo_handle = rc.bo_handle;
    tr.box.w = 16; tr.box.h = 16; tr.box.d = 1;
    tr.stride = 256*4;
    if (SC3(SYS_ioctl, fd, VG_IOCTL_XFER_TO, &tr) < 0) {
        put("[drmtest] TRANSFER_TO_HOST FAILED\n"); return 18;
    }
    put("[drmtest] TRANSFER_TO_HOST ok\n");

    /* 8. EXECBUFFER: submit a real virgl command stream. */
    g_cmd[0] = (1u << 16) | (VIRGL_CCMD_CREATE_SUB_CTX << 8) | 0u;
    g_cmd[1] = 1;                       /* sub-context id */
    struct vg_execbuffer eb;
    for (u64 i=0;i<sizeof(eb);i++) ((char*)&eb)[i]=0;
    eb.size = sizeof(g_cmd);
    eb.command = (u64)(unsigned long)g_cmd;
    eb.fence_fd = -1;
    if (SC3(SYS_ioctl, fd, VG_IOCTL_EXECBUFFER, &eb) < 0) {
        put("[drmtest] EXECBUFFER FAILED\n"); return 19;
    }
    put("[drmtest] EXECBUFFER ok (virgl command stream accepted)\n");

    /* 9. GEM_CLOSE */
    struct drm_gem_close gcl = { rc.bo_handle, 0 };
    if (SC3(SYS_ioctl, fd, DRM_IOCTL_GEM_CLOSE, &gcl) < 0) {
        put("[drmtest] GEM_CLOSE FAILED\n"); return 20;
    }

    SC1(SYS_close, fd);
    put("[drmtest] PASS: DRM render node drives real virgl 3D\n");
    return 3;
}
