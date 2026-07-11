/* sys/sysctl.h -- freestanding shim for the vendored openh264 thread
 * layer on tobyOS. WelsQueryLogicalProcessInfo() calls sysctlbyname()
 * to count CPUs; returning -1 makes it fall back to ProcessorCount = 1,
 * which is exactly what the single-threaded decode path wants. */
#ifndef TOBYOS_OPENH264_SYS_SYSCTL_SHIM_H
#define TOBYOS_OPENH264_SYS_SYSCTL_SHIM_H

#include <stddef.h>

static inline int sysctl(int *name, unsigned namelen, void *oldp,
                         size_t *oldlenp, void *newp, size_t newlen) {
    (void)name; (void)namelen; (void)oldp; (void)oldlenp;
    (void)newp; (void)newlen;
    return -1;
}
static inline int sysctlbyname(const char *name, void *oldp, size_t *oldlenp,
                               void *newp, size_t newlen) {
    (void)name; (void)oldp; (void)oldlenp; (void)newp; (void)newlen;
    return -1;
}

#endif /* TOBYOS_OPENH264_SYS_SYSCTL_SHIM_H */
