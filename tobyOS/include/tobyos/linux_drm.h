/* linux_drm.h -- /dev/dri: the Linux DRM ABI over virtio-gpu (tier 3 Phase 1b).
 * See src/linux_drm.c for scope and method. */
#ifndef TOBYOS_LINUX_DRM_H
#define TOBYOS_LINUX_DRM_H

#include <tobyos/types.h>

struct file;

/* True once a virgl-capable virtio-gpu device is bound (slice 97's
 * 3D-only or a future 3D-capable display device). When false, opening
 * /dev/dri/* returns ENOENT -- exactly like a Linux box with no GPU. */
bool lxdrm_available(void);

/* open("/dev/dri/card0" | "/dev/dri/renderD128") -> FILE_KIND_DRM. */
struct file *lxdrm_open(const char *node);
void lxdrm_close(struct file *f);

long lxdrm_ioctl(struct file *f, unsigned long req, unsigned long arg);
/* mmap of a BO, keyed by the cookie DRM_IOCTL_VIRTGPU_MAP returned. */
long lxdrm_mmap(uint64_t offset, uint64_t len);

void lxdrm_dump_stats(void);

#endif /* TOBYOS_LINUX_DRM_H */
