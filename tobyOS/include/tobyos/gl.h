/* tobyos/gl.h -- Minimal userland 3D graphics API (kernel-side declarations).
 *
 * This header is for KERNEL code that needs to dispatch GL-related
 * syscalls. Userland apps should use <toby/gl.h> from the SDK instead.
 *
 * The GL API provides context management and command buffer submission
 * over the VirGL protocol implemented by virtio_gpu.c. When VirGL is
 * not available (most default QEMU configs), all calls return -1.
 */

#ifndef TOBYOS_GL_H
#define TOBYOS_GL_H

#include <tobyos/types.h>

/* Syscall numbers for 3D operations (reserved range 100-109). */
#define ABI_SYS_GL_CREATE_CTX    100
#define ABI_SYS_GL_DESTROY_CTX   101
#define ABI_SYS_GL_CREATE_BUFFER 102
#define ABI_SYS_GL_SUBMIT        103
#define ABI_SYS_GL_SWAP_BUFFERS  104

/* Kernel-side dispatch targets (implemented in syscall.c). */
long sys_gl_create_ctx(void);
long sys_gl_destroy_ctx(uint32_t ctx_id);
long sys_gl_create_buffer(uint32_t ctx_id, uint32_t size_bytes);
long sys_gl_submit(uint32_t ctx_id, const void *cmd_buf, uint32_t cmd_size);
long sys_gl_swap_buffers(uint32_t ctx_id, int win_fd);

#endif /* TOBYOS_GL_H */
