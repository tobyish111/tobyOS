/* libtoby/src/gl.c -- Gallium3D/VirGL command stream encoder.
 *
 * Builds properly formatted VirGL command buffers in userland and
 * submits them to the host via the kernel's VIRTIO_GPU_CMD_SUBMIT_3D
 * plumbing (syscall 103).
 *
 * Each toby_gl_* API call appends one or more VirGL commands to an
 * internal 4KB buffer. toby_gl_flush() submits the accumulated buffer
 * to the kernel and resets the write pointer.
 *
 * The command header format (per virglrenderer vrend_decode.c):
 *   bits  0-7  : opcode (VIRGL_CCMD_*)
 *   bits  8-15 : object type (for CREATE_OBJECT/BIND_OBJECT, else 0)
 *   bits 16-31 : payload size in uint32_t words (excluding header)
 */

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "libtoby_internal.h"

/* We include the public GL header for the constants and struct defs */
#include <toby/gl.h>

/* ---- internal helpers ------------------------------------------- */

static uint32_t virgl_header(uint32_t opcode, uint32_t obj_type,
                             uint32_t size_dwords) {
    return (size_dwords << 16) | (obj_type << 8) | opcode;
}

static int cmd_space(toby_gl_ctx *ctx, uint32_t words_needed) {
    return (ctx->cmd_offset + words_needed) <= TOBY_GL_CMD_BUF_WORDS;
}

static void cmd_emit(toby_gl_ctx *ctx, uint32_t word) {
    if (ctx->cmd_offset < TOBY_GL_CMD_BUF_WORDS)
        ctx->cmd_buf[ctx->cmd_offset++] = word;
}

static uint32_t alloc_handle(toby_gl_ctx *ctx) {
    return ctx->next_handle++;
}

/* Reinterpret float as uint32_t for embedding in command stream */
static uint32_t f2u(float f) {
    uint32_t u;
    memcpy(&u, &f, 4);
    return u;
}

/* ---- static context storage (single GL context per process) ----- */

static toby_gl_ctx g_ctx;
static int g_ctx_active;

/* ---- public API ------------------------------------------------- */

toby_gl_ctx *toby_gl_init(int window_fd) {
    if (g_ctx_active)
        return NULL;

    long rc = toby_gl_sys_create_ctx();
    if (rc <= 0)
        return NULL;

    memset(&g_ctx, 0, sizeof(g_ctx));
    g_ctx.ctx_id = (uint32_t)rc;
    g_ctx.window_fd = window_fd;
    g_ctx.cmd_offset = 0;
    g_ctx.next_handle = 1;
    g_ctx.vp_x = 0;
    g_ctx.vp_y = 0;
    g_ctx.vp_w = 320;
    g_ctx.vp_h = 240;
    g_ctx_active = 1;

    return &g_ctx;
}

void toby_gl_destroy(toby_gl_ctx *ctx) {
    if (!ctx || !g_ctx_active)
        return;
    toby_gl_sys_destroy_ctx(ctx->ctx_id);
    g_ctx_active = 0;
}

/* ---- Clear ------------------------------------------------------ */

void toby_gl_clear(toby_gl_ctx *ctx, float r, float g, float b, float a) {
    if (!ctx) return;
    /* VIRGL_CCMD_CLEAR: 8 dwords payload
     * [0] buffers mask (PIPE_CLEAR_COLOR = 0x4)
     * [1-4] color (as 4x uint32 float-bits)
     * [5] depth (float bits) -- 1.0 default
     * [6-7] stencil (uint64) -- 0 */
    if (!cmd_space(ctx, 9)) toby_gl_flush(ctx);

    cmd_emit(ctx, virgl_header(VIRGL_CCMD_CLEAR, 0, 8));
    cmd_emit(ctx, TOBY_GL_CLEAR_COLOR);
    cmd_emit(ctx, f2u(r));
    cmd_emit(ctx, f2u(g));
    cmd_emit(ctx, f2u(b));
    cmd_emit(ctx, f2u(a));
    cmd_emit(ctx, f2u(1.0f));  /* depth */
    cmd_emit(ctx, 0);          /* stencil low */
    cmd_emit(ctx, 0);          /* stencil high */
}

void toby_gl_clear_depth(toby_gl_ctx *ctx, float depth) {
    if (!ctx) return;
    if (!cmd_space(ctx, 9)) toby_gl_flush(ctx);

    cmd_emit(ctx, virgl_header(VIRGL_CCMD_CLEAR, 0, 8));
    cmd_emit(ctx, TOBY_GL_CLEAR_DEPTH);
    cmd_emit(ctx, 0);          /* color r */
    cmd_emit(ctx, 0);          /* color g */
    cmd_emit(ctx, 0);          /* color b */
    cmd_emit(ctx, 0);          /* color a */
    cmd_emit(ctx, f2u(depth));
    cmd_emit(ctx, 0);
    cmd_emit(ctx, 0);
}

/* ---- Viewport --------------------------------------------------- */

void toby_gl_viewport(toby_gl_ctx *ctx, int x, int y, int w, int h) {
    if (!ctx) return;
    ctx->vp_x = x;
    ctx->vp_y = y;
    ctx->vp_w = w;
    ctx->vp_h = h;

    /* VIRGL_CCMD_SET_VIEWPORT_STATE:
     * [0] start_slot (0)
     * [1-6] scale_x, scale_y, scale_z, translate_x, translate_y, translate_z */
    if (!cmd_space(ctx, 8)) toby_gl_flush(ctx);

    float scale_x = (float)w * 0.5f;
    float scale_y = (float)h * -0.5f;
    float scale_z = 0.5f;
    float trans_x = (float)x + scale_x;
    float trans_y = (float)y + (float)h * 0.5f;
    float trans_z = 0.5f;

    cmd_emit(ctx, virgl_header(VIRGL_CCMD_SET_VIEWPORT_STATE, 0, 7));
    cmd_emit(ctx, 0);  /* start_slot */
    cmd_emit(ctx, f2u(scale_x));
    cmd_emit(ctx, f2u(scale_y));
    cmd_emit(ctx, f2u(scale_z));
    cmd_emit(ctx, f2u(trans_x));
    cmd_emit(ctx, f2u(trans_y));
    cmd_emit(ctx, f2u(trans_z));
}

/* ---- Shaders ---------------------------------------------------- */

int toby_gl_create_shader(toby_gl_ctx *ctx, int type, const char *tgsi_text) {
    if (!ctx || !tgsi_text) return -1;

    uint32_t handle = alloc_handle(ctx);
    size_t text_len = 0;
    const char *p = tgsi_text;
    while (*p++) text_len++;

    /* Payload for VIRGL_OBJECT_SHADER:
     * [0] handle
     * [1] shader type
     * [2] num_tokens (length of text including NUL, in bytes)
     * [3] offlen: (0 << 22) | (num_tokens & 0x3FFFFF) for single-packet
     * [4..] TGSI text packed as uint32_t words */
    uint32_t text_bytes = (uint32_t)(text_len + 1);
    uint32_t text_dwords = (text_bytes + 3) / 4;
    uint32_t payload_dwords = 4 + text_dwords;

    if (!cmd_space(ctx, 1 + payload_dwords)) toby_gl_flush(ctx);
    if (!cmd_space(ctx, 1 + payload_dwords)) return -1;

    cmd_emit(ctx, virgl_header(VIRGL_CCMD_CREATE_OBJECT,
                               VIRGL_OBJECT_SHADER, payload_dwords));
    cmd_emit(ctx, handle);
    cmd_emit(ctx, (uint32_t)type);
    cmd_emit(ctx, text_bytes);
    /* offlen: single-packet encoding, offset=0 in high bits */
    cmd_emit(ctx, text_bytes & 0x3FFFFFu);

    /* Pack TGSI text into dwords, padding with zeros */
    uint32_t i;
    for (i = 0; i + 4 <= text_bytes; i += 4) {
        uint32_t w = (uint32_t)(uint8_t)tgsi_text[i]
                   | ((uint32_t)(uint8_t)tgsi_text[i+1] << 8)
                   | ((uint32_t)(uint8_t)tgsi_text[i+2] << 16)
                   | ((uint32_t)(uint8_t)tgsi_text[i+3] << 24);
        cmd_emit(ctx, w);
    }
    if (i < text_bytes) {
        uint32_t w = 0;
        uint32_t shift = 0;
        while (i < text_bytes) {
            w |= ((uint32_t)(uint8_t)tgsi_text[i]) << shift;
            shift += 8;
            i++;
        }
        cmd_emit(ctx, w);
    }

    return (int)handle;
}

void toby_gl_bind_shader(toby_gl_ctx *ctx, int shader_handle, int type) {
    if (!ctx) return;
    /* VIRGL_CCMD_BIND_SHADER: 2 dwords
     * [0] handle
     * [1] type */
    if (!cmd_space(ctx, 3)) toby_gl_flush(ctx);

    cmd_emit(ctx, virgl_header(VIRGL_CCMD_BIND_SHADER, 0, 2));
    cmd_emit(ctx, (uint32_t)shader_handle);
    cmd_emit(ctx, (uint32_t)type);
}

/* ---- Vertex buffers --------------------------------------------- */

int toby_gl_create_vertex_buffer(toby_gl_ctx *ctx,
                                 const void *data, uint32_t size) {
    if (!ctx || !data || size == 0) return -1;

    uint32_t handle = alloc_handle(ctx);

    /* Use VIRGL_CCMD_RESOURCE_INLINE_WRITE to upload vertex data.
     * Payload:
     * [0] resource handle
     * [1] level (0)
     * [2] usage (0)
     * [3] stride (0 for raw buffer)
     * [4] layer_stride (0)
     * [5] x (offset = 0)
     * [6] y (0)
     * [7] z (0)
     * [8] w (width = size)
     * [9] h (1)
     * [10] d (1)
     * [11..] data */
    uint32_t data_dwords = (size + 3) / 4;
    uint32_t payload_dwords = 11 + data_dwords;

    /* First create the resource via PIPE_RESOURCE_CREATE */
    if (!cmd_space(ctx, 13)) toby_gl_flush(ctx);
    if (!cmd_space(ctx, 13)) return -1;

    /* VIRGL_CCMD_PIPE_RESOURCE_CREATE: 11 dwords payload
     * [0] handle
     * [1] target (PIPE_BUFFER = 0)
     * [2] format (PIPE_FORMAT_R8_UNORM = 0x1C)
     * [3] bind (PIPE_BIND_VERTEX_BUFFER = 0x4)
     * [4] width (size in bytes)
     * [5] height (1)
     * [6] depth (1)
     * [7] array_size (1)
     * [8] last_level (0)
     * [9] nr_samples (0)
     * [10] flags (0) */
    cmd_emit(ctx, virgl_header(VIRGL_CCMD_PIPE_RESOURCE_CREATE, 0, 11));
    cmd_emit(ctx, handle);
    cmd_emit(ctx, 0);          /* PIPE_BUFFER */
    cmd_emit(ctx, 0x1C);       /* PIPE_FORMAT_R8_UNORM */
    cmd_emit(ctx, 0x4);        /* PIPE_BIND_VERTEX_BUFFER */
    cmd_emit(ctx, size);
    cmd_emit(ctx, 1);
    cmd_emit(ctx, 1);
    cmd_emit(ctx, 1);
    cmd_emit(ctx, 0);
    cmd_emit(ctx, 0);
    cmd_emit(ctx, 0);

    /* Now upload the data inline */
    if (!cmd_space(ctx, 1 + payload_dwords)) toby_gl_flush(ctx);
    if (!cmd_space(ctx, 1 + payload_dwords)) return -1;

    cmd_emit(ctx, virgl_header(VIRGL_CCMD_RESOURCE_INLINE_WRITE, 0,
                               payload_dwords));
    cmd_emit(ctx, handle);     /* resource handle */
    cmd_emit(ctx, 0);          /* level */
    cmd_emit(ctx, 0);          /* usage */
    cmd_emit(ctx, 0);          /* stride */
    cmd_emit(ctx, 0);          /* layer_stride */
    cmd_emit(ctx, 0);          /* x */
    cmd_emit(ctx, 0);          /* y */
    cmd_emit(ctx, 0);          /* z */
    cmd_emit(ctx, size);       /* w */
    cmd_emit(ctx, 1);          /* h */
    cmd_emit(ctx, 1);          /* d */

    /* Copy vertex data word by word */
    const uint8_t *src = (const uint8_t *)data;
    uint32_t i;
    for (i = 0; i + 4 <= size; i += 4) {
        uint32_t w = (uint32_t)src[i]
                   | ((uint32_t)src[i+1] << 8)
                   | ((uint32_t)src[i+2] << 16)
                   | ((uint32_t)src[i+3] << 24);
        cmd_emit(ctx, w);
    }
    if (i < size) {
        uint32_t w = 0;
        uint32_t shift = 0;
        while (i < size) {
            w |= ((uint32_t)src[i]) << shift;
            shift += 8;
            i++;
        }
        cmd_emit(ctx, w);
    }

    return (int)handle;
}

void toby_gl_set_vertex_buffer(toby_gl_ctx *ctx, int buf_handle,
                               uint32_t stride, uint32_t offset) {
    if (!ctx) return;
    /* VIRGL_CCMD_SET_VERTEX_BUFFERS:
     * Repeated per buffer: stride, offset, handle
     * We set a single buffer at slot 0. */
    if (!cmd_space(ctx, 4)) toby_gl_flush(ctx);

    cmd_emit(ctx, virgl_header(VIRGL_CCMD_SET_VERTEX_BUFFERS, 0, 3));
    cmd_emit(ctx, stride);
    cmd_emit(ctx, offset);
    cmd_emit(ctx, (uint32_t)buf_handle);
}

int toby_gl_create_vertex_elements(toby_gl_ctx *ctx, int count,
                                   const struct toby_gl_vertex_element *elems) {
    if (!ctx || count <= 0 || !elems) return -1;

    uint32_t handle = alloc_handle(ctx);

    /* VIRGL_OBJECT_VERTEX_ELEMENTS:
     * [0] handle
     * per element (4 dwords each):
     *   [0] src_offset
     *   [1] instance_divisor (0)
     *   [2] vertex_buffer_index
     *   [3] src_format */
    uint32_t payload = 1 + (uint32_t)count * 4;
    if (!cmd_space(ctx, 1 + payload)) toby_gl_flush(ctx);
    if (!cmd_space(ctx, 1 + payload)) return -1;

    cmd_emit(ctx, virgl_header(VIRGL_CCMD_CREATE_OBJECT,
                               VIRGL_OBJECT_VERTEX_ELEMENTS, payload));
    cmd_emit(ctx, handle);
    for (int i = 0; i < count; i++) {
        cmd_emit(ctx, elems[i].src_offset);
        cmd_emit(ctx, 0);  /* instance_divisor */
        cmd_emit(ctx, elems[i].vertex_buffer_index);
        cmd_emit(ctx, elems[i].src_format);
    }

    return (int)handle;
}

void toby_gl_bind_vertex_elements(toby_gl_ctx *ctx, int ve_handle) {
    if (!ctx) return;
    /* VIRGL_CCMD_BIND_OBJECT with VIRGL_OBJECT_VERTEX_ELEMENTS */
    if (!cmd_space(ctx, 2)) toby_gl_flush(ctx);

    cmd_emit(ctx, virgl_header(VIRGL_CCMD_BIND_OBJECT,
                               VIRGL_OBJECT_VERTEX_ELEMENTS, 1));
    cmd_emit(ctx, (uint32_t)ve_handle);
}

/* ---- Drawing ---------------------------------------------------- */

void toby_gl_draw_arrays(toby_gl_ctx *ctx, int mode, int start, int count) {
    if (!ctx) return;
    /* VIRGL_CCMD_DRAW_VBO: 12 dwords payload */
    if (!cmd_space(ctx, 13)) toby_gl_flush(ctx);

    cmd_emit(ctx, virgl_header(VIRGL_CCMD_DRAW_VBO, 0, 12));
    cmd_emit(ctx, (uint32_t)start);
    cmd_emit(ctx, (uint32_t)count);
    cmd_emit(ctx, (uint32_t)mode);
    cmd_emit(ctx, 0);          /* indexed */
    cmd_emit(ctx, 1);          /* instance_count */
    cmd_emit(ctx, 0);          /* index_bias */
    cmd_emit(ctx, 0);          /* start_instance */
    cmd_emit(ctx, 0);          /* primitive_restart */
    cmd_emit(ctx, 0);          /* restart_index */
    cmd_emit(ctx, (uint32_t)start);
    cmd_emit(ctx, (uint32_t)(start + count - 1));
    cmd_emit(ctx, 0);          /* cso */
}

void toby_gl_draw_elements(toby_gl_ctx *ctx, int mode, int count,
                           int index_handle, int index_size) {
    if (!ctx) return;
    (void)index_handle;
    (void)index_size;
    if (!cmd_space(ctx, 13)) toby_gl_flush(ctx);

    cmd_emit(ctx, virgl_header(VIRGL_CCMD_DRAW_VBO, 0, 12));
    cmd_emit(ctx, 0);          /* start (offset handled by index buffer) */
    cmd_emit(ctx, (uint32_t)count);
    cmd_emit(ctx, (uint32_t)mode);
    cmd_emit(ctx, 1);          /* indexed = 1 */
    cmd_emit(ctx, 1);          /* instance_count */
    cmd_emit(ctx, 0);          /* index_bias */
    cmd_emit(ctx, 0);          /* start_instance */
    cmd_emit(ctx, 0);          /* primitive_restart */
    cmd_emit(ctx, 0);          /* restart_index */
    cmd_emit(ctx, 0);          /* min_index */
    cmd_emit(ctx, (uint32_t)(count - 1));  /* max_index */
    cmd_emit(ctx, 0);          /* cso */
}

/* ---- Index buffers ---------------------------------------------- */

int toby_gl_create_index_buffer(toby_gl_ctx *ctx,
                                const void *data, uint32_t size,
                                int index_size) {
    if (!ctx || !data || size == 0) return -1;
    (void)index_size;

    uint32_t handle = alloc_handle(ctx);
    uint32_t data_dwords = (size + 3) / 4;

    /* Create resource */
    if (!cmd_space(ctx, 12)) toby_gl_flush(ctx);
    if (!cmd_space(ctx, 12)) return -1;

    cmd_emit(ctx, virgl_header(VIRGL_CCMD_PIPE_RESOURCE_CREATE, 0, 11));
    cmd_emit(ctx, handle);
    cmd_emit(ctx, 0);          /* PIPE_BUFFER */
    cmd_emit(ctx, 0x1C);       /* PIPE_FORMAT_R8_UNORM */
    cmd_emit(ctx, 0x8);        /* PIPE_BIND_INDEX_BUFFER */
    cmd_emit(ctx, size);
    cmd_emit(ctx, 1);
    cmd_emit(ctx, 1);
    cmd_emit(ctx, 1);
    cmd_emit(ctx, 0);
    cmd_emit(ctx, 0);
    cmd_emit(ctx, 0);

    /* Upload inline */
    uint32_t payload_dwords = 11 + data_dwords;
    if (!cmd_space(ctx, 1 + payload_dwords)) toby_gl_flush(ctx);
    if (!cmd_space(ctx, 1 + payload_dwords)) return -1;

    cmd_emit(ctx, virgl_header(VIRGL_CCMD_RESOURCE_INLINE_WRITE, 0,
                               payload_dwords));
    cmd_emit(ctx, handle);
    cmd_emit(ctx, 0); cmd_emit(ctx, 0); cmd_emit(ctx, 0);
    cmd_emit(ctx, 0); cmd_emit(ctx, 0); cmd_emit(ctx, 0);
    cmd_emit(ctx, 0); cmd_emit(ctx, size); cmd_emit(ctx, 1);
    cmd_emit(ctx, 1);

    const uint8_t *src = (const uint8_t *)data;
    uint32_t i;
    for (i = 0; i + 4 <= size; i += 4) {
        uint32_t w = (uint32_t)src[i]
                   | ((uint32_t)src[i+1] << 8)
                   | ((uint32_t)src[i+2] << 16)
                   | ((uint32_t)src[i+3] << 24);
        cmd_emit(ctx, w);
    }
    if (i < size) {
        uint32_t w = 0, shift = 0;
        while (i < size) { w |= ((uint32_t)src[i]) << shift; shift += 8; i++; }
        cmd_emit(ctx, w);
    }

    return (int)handle;
}

void toby_gl_set_index_buffer(toby_gl_ctx *ctx, int buf_handle,
                              int index_size, uint32_t offset) {
    if (!ctx) return;
    /* VIRGL_CCMD_SET_INDEX_BUFFER: 3 dwords
     * [0] handle, [1] index_size, [2] offset */
    if (!cmd_space(ctx, 4)) toby_gl_flush(ctx);

    cmd_emit(ctx, virgl_header(VIRGL_CCMD_SET_INDEX_BUFFER, 0, 3));
    cmd_emit(ctx, (uint32_t)buf_handle);
    cmd_emit(ctx, (uint32_t)index_size);
    cmd_emit(ctx, offset);
}

/* ---- Uniform / constant buffers --------------------------------- */

int toby_gl_create_uniform_buffer(toby_gl_ctx *ctx,
                                  const void *data, uint32_t size) {
    if (!ctx || !data || size == 0) return -1;

    uint32_t handle = alloc_handle(ctx);
    uint32_t data_dwords = (size + 3) / 4;

    /* Create resource */
    if (!cmd_space(ctx, 12)) toby_gl_flush(ctx);
    if (!cmd_space(ctx, 12)) return -1;

    cmd_emit(ctx, virgl_header(VIRGL_CCMD_PIPE_RESOURCE_CREATE, 0, 11));
    cmd_emit(ctx, handle);
    cmd_emit(ctx, 0);          /* PIPE_BUFFER */
    cmd_emit(ctx, 0x1C);       /* PIPE_FORMAT_R8_UNORM */
    cmd_emit(ctx, 0x10);       /* PIPE_BIND_CONSTANT_BUFFER */
    cmd_emit(ctx, size);
    cmd_emit(ctx, 1);
    cmd_emit(ctx, 1);
    cmd_emit(ctx, 1);
    cmd_emit(ctx, 0);
    cmd_emit(ctx, 0);
    cmd_emit(ctx, 0);

    /* Upload data inline */
    uint32_t payload_dwords = 11 + data_dwords;
    if (!cmd_space(ctx, 1 + payload_dwords)) toby_gl_flush(ctx);
    if (!cmd_space(ctx, 1 + payload_dwords)) return -1;

    cmd_emit(ctx, virgl_header(VIRGL_CCMD_RESOURCE_INLINE_WRITE, 0,
                               payload_dwords));
    cmd_emit(ctx, handle);
    cmd_emit(ctx, 0); cmd_emit(ctx, 0); cmd_emit(ctx, 0);
    cmd_emit(ctx, 0); cmd_emit(ctx, 0); cmd_emit(ctx, 0);
    cmd_emit(ctx, 0); cmd_emit(ctx, size); cmd_emit(ctx, 1);
    cmd_emit(ctx, 1);

    const uint8_t *src = (const uint8_t *)data;
    uint32_t i;
    for (i = 0; i + 4 <= size; i += 4) {
        uint32_t w = (uint32_t)src[i]
                   | ((uint32_t)src[i+1] << 8)
                   | ((uint32_t)src[i+2] << 16)
                   | ((uint32_t)src[i+3] << 24);
        cmd_emit(ctx, w);
    }
    if (i < size) {
        uint32_t w = 0, shift = 0;
        while (i < size) { w |= ((uint32_t)src[i]) << shift; shift += 8; i++; }
        cmd_emit(ctx, w);
    }

    return (int)handle;
}

void toby_gl_set_constant_buffer(toby_gl_ctx *ctx, int shader_type,
                                 int index, int buf_handle, uint32_t size) {
    if (!ctx) return;
    /* VIRGL_CCMD_SET_CONSTANT_BUFFER: 2 dwords + optional inline
     * When using a resource handle:
     * [0] shader_type
     * [1] index
     * [2] offset (0)
     * [3] size
     * [4] handle */
    if (!cmd_space(ctx, 6)) toby_gl_flush(ctx);

    cmd_emit(ctx, virgl_header(VIRGL_CCMD_SET_CONSTANT_BUFFER, 0, 5));
    cmd_emit(ctx, (uint32_t)shader_type);
    cmd_emit(ctx, (uint32_t)index);
    cmd_emit(ctx, 0);          /* offset */
    cmd_emit(ctx, size);
    cmd_emit(ctx, (uint32_t)buf_handle);
}

void toby_gl_set_constant_buffer_inline(toby_gl_ctx *ctx, int shader_type,
                                        int index, const void *data,
                                        uint32_t size) {
    if (!ctx || !data || size == 0) return;

    uint32_t data_dwords = (size + 3) / 4;
    /* Inline form: shader_type, index, then raw float data
     * handle = 0 signals inline mode */
    uint32_t payload = 2 + data_dwords;
    if (!cmd_space(ctx, 1 + payload)) toby_gl_flush(ctx);
    if (!cmd_space(ctx, 1 + payload)) return;

    cmd_emit(ctx, virgl_header(VIRGL_CCMD_SET_CONSTANT_BUFFER, 0, payload));
    cmd_emit(ctx, (uint32_t)shader_type);
    cmd_emit(ctx, (uint32_t)index);

    const uint8_t *src = (const uint8_t *)data;
    uint32_t i;
    for (i = 0; i + 4 <= size; i += 4) {
        uint32_t w = (uint32_t)src[i]
                   | ((uint32_t)src[i+1] << 8)
                   | ((uint32_t)src[i+2] << 16)
                   | ((uint32_t)src[i+3] << 24);
        cmd_emit(ctx, w);
    }
    if (i < size) {
        uint32_t w = 0, shift = 0;
        while (i < size) { w |= ((uint32_t)src[i]) << shift; shift += 8; i++; }
        cmd_emit(ctx, w);
    }
}

/* ---- Textures --------------------------------------------------- */

int toby_gl_create_texture_2d(toby_gl_ctx *ctx, uint32_t width,
                              uint32_t height, uint32_t format) {
    if (!ctx) return -1;

    uint32_t handle = alloc_handle(ctx);

    if (!cmd_space(ctx, 12)) toby_gl_flush(ctx);
    if (!cmd_space(ctx, 12)) return -1;

    cmd_emit(ctx, virgl_header(VIRGL_CCMD_PIPE_RESOURCE_CREATE, 0, 11));
    cmd_emit(ctx, handle);
    cmd_emit(ctx, 2);          /* PIPE_TEXTURE_2D */
    cmd_emit(ctx, format);
    cmd_emit(ctx, 0x20);       /* PIPE_BIND_SAMPLER_VIEW */
    cmd_emit(ctx, width);
    cmd_emit(ctx, height);
    cmd_emit(ctx, 1);          /* depth */
    cmd_emit(ctx, 1);          /* array_size */
    cmd_emit(ctx, 0);          /* last_level */
    cmd_emit(ctx, 0);          /* nr_samples */
    cmd_emit(ctx, 0);          /* flags */

    return (int)handle;
}

void toby_gl_upload_texture(toby_gl_ctx *ctx, int tex_handle,
                            uint32_t width, uint32_t height,
                            uint32_t format, const void *data,
                            uint32_t data_size) {
    if (!ctx || !data || data_size == 0) return;
    (void)format;

    uint32_t data_dwords = (data_size + 3) / 4;
    uint32_t payload_dwords = 11 + data_dwords;

    if (!cmd_space(ctx, 1 + payload_dwords)) toby_gl_flush(ctx);
    if (!cmd_space(ctx, 1 + payload_dwords)) return;

    cmd_emit(ctx, virgl_header(VIRGL_CCMD_RESOURCE_INLINE_WRITE, 0,
                               payload_dwords));
    cmd_emit(ctx, (uint32_t)tex_handle);
    cmd_emit(ctx, 0);          /* level */
    cmd_emit(ctx, 0);          /* usage */
    cmd_emit(ctx, width * 4);  /* stride (assume 4 BPP) */
    cmd_emit(ctx, 0);          /* layer_stride */
    cmd_emit(ctx, 0);          /* x */
    cmd_emit(ctx, 0);          /* y */
    cmd_emit(ctx, 0);          /* z */
    cmd_emit(ctx, width);      /* w */
    cmd_emit(ctx, height);     /* h */
    cmd_emit(ctx, 1);          /* d */

    const uint8_t *src = (const uint8_t *)data;
    uint32_t i;
    for (i = 0; i + 4 <= data_size; i += 4) {
        uint32_t w = (uint32_t)src[i]
                   | ((uint32_t)src[i+1] << 8)
                   | ((uint32_t)src[i+2] << 16)
                   | ((uint32_t)src[i+3] << 24);
        cmd_emit(ctx, w);
    }
    if (i < data_size) {
        uint32_t w = 0, shift = 0;
        while (i < data_size) { w |= ((uint32_t)src[i]) << shift; shift += 8; i++; }
        cmd_emit(ctx, w);
    }
}

int toby_gl_create_sampler(toby_gl_ctx *ctx, int min_filter,
                           int mag_filter, int mip_filter,
                           int wrap_s, int wrap_t) {
    if (!ctx) return -1;

    uint32_t handle = alloc_handle(ctx);

    /* VIRGL_OBJECT_SAMPLER_STATE:
     * [0] handle
     * [1] S0: wrap_s | (wrap_t<<3) | (wrap_r<<6) | (min_img_filter<<9) |
     *         (min_mip_filter<<12) | (mag_img_filter<<15) |
     *         (compare_mode<<18) | (compare_func<<19)
     * [2] lod_bias (float)
     * [3] min_lod (float)
     * [4] max_lod (float)
     * [5-8] border_color (4x float) */
    if (!cmd_space(ctx, 10)) toby_gl_flush(ctx);

    uint32_t s0 = (uint32_t)(wrap_s & 7)
                | ((uint32_t)(wrap_t & 7) << 3)
                | ((uint32_t)(wrap_s & 7) << 6)   /* wrap_r = wrap_s */
                | ((uint32_t)(min_filter & 7) << 9)
                | ((uint32_t)(mip_filter & 7) << 12)
                | ((uint32_t)(mag_filter & 7) << 15);

    cmd_emit(ctx, virgl_header(VIRGL_CCMD_CREATE_OBJECT,
                               VIRGL_OBJECT_SAMPLER_STATE, 9));
    cmd_emit(ctx, handle);
    cmd_emit(ctx, s0);
    cmd_emit(ctx, 0);          /* lod_bias = 0.0 */
    cmd_emit(ctx, 0);          /* min_lod = 0.0 */
    cmd_emit(ctx, f2u(1000.0f)); /* max_lod */
    cmd_emit(ctx, 0);          /* border r */
    cmd_emit(ctx, 0);          /* border g */
    cmd_emit(ctx, 0);          /* border b */
    cmd_emit(ctx, 0);          /* border a */

    return (int)handle;
}

void toby_gl_bind_sampler(toby_gl_ctx *ctx, int shader_type,
                          int slot, int sampler_handle) {
    if (!ctx) return;
    /* VIRGL_CCMD_BIND_SAMPLER_STATES:
     * [0] shader_type
     * [1] start_slot
     * [2] num_samplers (1)
     * [3] handle */
    if (!cmd_space(ctx, 5)) toby_gl_flush(ctx);

    cmd_emit(ctx, virgl_header(VIRGL_CCMD_BIND_SAMPLER_STATES, 0, 4));
    cmd_emit(ctx, (uint32_t)shader_type);
    cmd_emit(ctx, (uint32_t)slot);
    cmd_emit(ctx, 1);
    cmd_emit(ctx, (uint32_t)sampler_handle);
}

int toby_gl_create_sampler_view(toby_gl_ctx *ctx, int tex_handle,
                                uint32_t format, int target) {
    if (!ctx) return -1;

    uint32_t handle = alloc_handle(ctx);

    /* VIRGL_OBJECT_SAMPLER_VIEW:
     * [0] handle
     * [1] res_handle
     * [2] format
     * [3] val0: first_level | (last_level << 8)
     * [4] val1: first_layer | (last_layer << 16)
     * [5] swizzle: r|(g<<3)|(b<<6)|(a<<9) -- identity = 0|1<<3|2<<6|3<<9 */
    if (!cmd_space(ctx, 7)) toby_gl_flush(ctx);
    (void)target;

    uint32_t swizzle = 0 | (1u << 3) | (2u << 6) | (3u << 9);

    cmd_emit(ctx, virgl_header(VIRGL_CCMD_CREATE_OBJECT,
                               VIRGL_OBJECT_SAMPLER_VIEW, 6));
    cmd_emit(ctx, handle);
    cmd_emit(ctx, (uint32_t)tex_handle);
    cmd_emit(ctx, format);
    cmd_emit(ctx, 0);          /* first_level=0, last_level=0 */
    cmd_emit(ctx, 0);          /* first_layer=0, last_layer=0 */
    cmd_emit(ctx, swizzle);

    return (int)handle;
}

void toby_gl_set_sampler_view(toby_gl_ctx *ctx, int shader_type,
                              int slot, int view_handle) {
    if (!ctx) return;
    /* VIRGL_CCMD_SET_SAMPLER_VIEWS:
     * [0] shader_type
     * [1] start_slot
     * [2] num_views (1)
     * [3..] view handles */
    if (!cmd_space(ctx, 4)) toby_gl_flush(ctx);

    cmd_emit(ctx, virgl_header(VIRGL_CCMD_SET_SAMPLER_VIEWS, 0, 3));
    cmd_emit(ctx, (uint32_t)shader_type);
    cmd_emit(ctx, (uint32_t)slot);
    cmd_emit(ctx, (uint32_t)view_handle);
}

/* ---- Framebuffer / surfaces ------------------------------------- */

int toby_gl_create_surface(toby_gl_ctx *ctx, int tex_handle,
                           uint32_t format, int level) {
    if (!ctx) return -1;

    uint32_t handle = alloc_handle(ctx);

    /* VIRGL_OBJECT_SURFACE:
     * [0] handle
     * [1] res_handle
     * [2] format
     * [3] val0: level | (first_layer << 8)
     * [4] val1: last_layer */
    if (!cmd_space(ctx, 6)) toby_gl_flush(ctx);

    cmd_emit(ctx, virgl_header(VIRGL_CCMD_CREATE_OBJECT,
                               VIRGL_OBJECT_SURFACE, 5));
    cmd_emit(ctx, handle);
    cmd_emit(ctx, (uint32_t)tex_handle);
    cmd_emit(ctx, format);
    cmd_emit(ctx, (uint32_t)level);  /* level, first_layer=0 */
    cmd_emit(ctx, 0);                /* last_layer=0 */

    return (int)handle;
}

void toby_gl_set_framebuffer(toby_gl_ctx *ctx, int depth_surface,
                             int nr_cbufs, const int *color_surfaces,
                             uint32_t width, uint32_t height) {
    if (!ctx) return;
    (void)width;
    (void)height;

    /* VIRGL_CCMD_SET_FRAMEBUFFER_STATE:
     * [0] nr_cbufs
     * [1] zsurf_handle (0 if none)
     * [2..] cbuf_handles */
    uint32_t payload = 2 + (uint32_t)(nr_cbufs > 0 ? nr_cbufs : 0);
    if (!cmd_space(ctx, 1 + payload)) toby_gl_flush(ctx);

    cmd_emit(ctx, virgl_header(VIRGL_CCMD_SET_FRAMEBUFFER_STATE, 0, payload));
    cmd_emit(ctx, (uint32_t)nr_cbufs);
    cmd_emit(ctx, (uint32_t)(depth_surface >= 0 ? depth_surface : 0));
    for (int i = 0; i < nr_cbufs; i++)
        cmd_emit(ctx, (uint32_t)color_surfaces[i]);
}

/* ---- Scissor ---------------------------------------------------- */

void toby_gl_set_scissor(toby_gl_ctx *ctx, int x0, int y0, int x1, int y1) {
    if (!ctx) return;
    /* VIRGL_CCMD_SET_SCISSOR_STATE:
     * [0] start_slot (0)
     * [1] minx | (miny << 16)
     * [2] maxx | (maxy << 16) */
    if (!cmd_space(ctx, 4)) toby_gl_flush(ctx);

    cmd_emit(ctx, virgl_header(VIRGL_CCMD_SET_SCISSOR_STATE, 0, 3));
    cmd_emit(ctx, 0);  /* start_slot */
    cmd_emit(ctx, ((uint32_t)x0 & 0xFFFF) | (((uint32_t)y0 & 0xFFFF) << 16));
    cmd_emit(ctx, ((uint32_t)x1 & 0xFFFF) | (((uint32_t)y1 & 0xFFFF) << 16));
}

/* ---- Compute dispatch ------------------------------------------- */

void toby_gl_dispatch_compute(toby_gl_ctx *ctx,
                              uint32_t grid_x, uint32_t grid_y,
                              uint32_t grid_z,
                              uint32_t block_x, uint32_t block_y,
                              uint32_t block_z) {
    if (!ctx) return;
    /* VIRGL_CCMD_LAUNCH_GRID: 7 dwords
     * [0] block_x, [1] block_y, [2] block_z
     * [3] grid_x,  [4] grid_y,  [5] grid_z
     * [6] indirect_handle (0 = direct dispatch) */
    if (!cmd_space(ctx, 8)) toby_gl_flush(ctx);

    cmd_emit(ctx, virgl_header(VIRGL_CCMD_LAUNCH_GRID, 0, 7));
    cmd_emit(ctx, block_x);
    cmd_emit(ctx, block_y);
    cmd_emit(ctx, block_z);
    cmd_emit(ctx, grid_x);
    cmd_emit(ctx, grid_y);
    cmd_emit(ctx, grid_z);
    cmd_emit(ctx, 0);  /* indirect_handle */
}

/* ---- Shader buffers (compute) ----------------------------------- */

void toby_gl_set_shader_buffer(toby_gl_ctx *ctx, int shader_type,
                               int slot, int buf_handle,
                               uint32_t offset, uint32_t size) {
    if (!ctx) return;
    /* VIRGL_CCMD_SET_SHADER_BUFFERS:
     * [0] shader_type
     * [1] start_slot
     * Per-buffer (3 dwords): offset, size, handle */
    if (!cmd_space(ctx, 6)) toby_gl_flush(ctx);

    cmd_emit(ctx, virgl_header(VIRGL_CCMD_SET_SHADER_BUFFERS, 0, 5));
    cmd_emit(ctx, (uint32_t)shader_type);
    cmd_emit(ctx, (uint32_t)slot);
    cmd_emit(ctx, offset);
    cmd_emit(ctx, size);
    cmd_emit(ctx, (uint32_t)buf_handle);
}

/* ---- Resource destruction --------------------------------------- */

void toby_gl_destroy_object(toby_gl_ctx *ctx, int handle) {
    if (!ctx || handle <= 0) return;
    /* VIRGL_CCMD_DESTROY_OBJECT:
     * [0] handle */
    if (!cmd_space(ctx, 2)) toby_gl_flush(ctx);

    cmd_emit(ctx, virgl_header(VIRGL_CCMD_DESTROY_OBJECT, 0, 1));
    cmd_emit(ctx, (uint32_t)handle);
}

/* ---- Blend state ------------------------------------------------ */

int toby_gl_create_blend(toby_gl_ctx *ctx, int enable, int src, int dst) {
    if (!ctx) return -1;

    uint32_t handle = alloc_handle(ctx);

    /* VIRGL_OBJECT_BLEND:
     * [0] handle
     * [1] S0 (logicop_enable=0, dither=0, alpha_to_coverage=0, etc.)
     * [2] S1 (logicop_func = 0)
     * Per-RT (we encode RT0 only):
     * [3] blend_enable | (colormask << 8)
     * [4] rgb_func | (rgb_src_factor << 8) | (rgb_dst_factor << 16)
     * [5] alpha_func | (alpha_src_factor << 8) | (alpha_dst_factor << 16) */
    if (!cmd_space(ctx, 7)) toby_gl_flush(ctx);

    uint32_t rt_blend = (uint32_t)(enable ? 1 : 0) | (0xFu << 8);
    uint32_t rgb_word = 0;
    uint32_t alpha_word = 0;
    if (enable) {
        /* func = ADD (1) for both */
        rgb_word = 1 | ((uint32_t)src << 8) | ((uint32_t)dst << 16);
        alpha_word = 1 | ((uint32_t)src << 8) | ((uint32_t)dst << 16);
    }

    cmd_emit(ctx, virgl_header(VIRGL_CCMD_CREATE_OBJECT,
                               VIRGL_OBJECT_BLEND, 6));
    cmd_emit(ctx, handle);
    cmd_emit(ctx, 0);          /* S0 */
    cmd_emit(ctx, 0);          /* S1 */
    cmd_emit(ctx, rt_blend);
    cmd_emit(ctx, rgb_word);
    cmd_emit(ctx, alpha_word);

    return (int)handle;
}

void toby_gl_bind_blend(toby_gl_ctx *ctx, int handle) {
    if (!ctx) return;
    if (!cmd_space(ctx, 2)) toby_gl_flush(ctx);

    cmd_emit(ctx, virgl_header(VIRGL_CCMD_BIND_OBJECT,
                               VIRGL_OBJECT_BLEND, 1));
    cmd_emit(ctx, (uint32_t)handle);
}

/* ---- Rasterizer state ------------------------------------------- */

int toby_gl_create_rasterizer(toby_gl_ctx *ctx, int fill_mode, int cull_mode) {
    if (!ctx) return -1;

    uint32_t handle = alloc_handle(ctx);

    /* VIRGL_OBJECT_RASTERIZER:
     * [0] handle
     * [1] S0: flatshade(0) | depth_clip(1<<16) | clip_halfz(0) |
     *         fill_front(fill<<2) | fill_back(fill<<5) | cull_face(cull<<8)
     *         ... many packed bits
     * [2] point_size (float)
     * [3] sprite_coord_mode (0)
     * [4] S4: line_width (float)
     * [5] offset_units (float)
     * [6] offset_scale (float)
     * [7] offset_clamp (float) */
    if (!cmd_space(ctx, 9)) toby_gl_flush(ctx);

    uint32_t s0 = ((uint32_t)fill_mode << 2)
                | ((uint32_t)fill_mode << 5)
                | ((uint32_t)cull_mode << 8)
                | (1u << 16);  /* depth_clip_near */

    cmd_emit(ctx, virgl_header(VIRGL_CCMD_CREATE_OBJECT,
                               VIRGL_OBJECT_RASTERIZER, 8));
    cmd_emit(ctx, handle);
    cmd_emit(ctx, s0);
    cmd_emit(ctx, f2u(1.0f));  /* point_size */
    cmd_emit(ctx, 0);          /* sprite_coord_mode */
    cmd_emit(ctx, f2u(1.0f));  /* line_width */
    cmd_emit(ctx, 0);          /* offset_units */
    cmd_emit(ctx, 0);          /* offset_scale */
    cmd_emit(ctx, 0);          /* offset_clamp */

    return (int)handle;
}

void toby_gl_bind_rasterizer(toby_gl_ctx *ctx, int handle) {
    if (!ctx) return;
    if (!cmd_space(ctx, 2)) toby_gl_flush(ctx);

    cmd_emit(ctx, virgl_header(VIRGL_CCMD_BIND_OBJECT,
                               VIRGL_OBJECT_RASTERIZER, 1));
    cmd_emit(ctx, (uint32_t)handle);
}

/* ---- Depth/stencil/alpha state ---------------------------------- */

int toby_gl_create_dsa(toby_gl_ctx *ctx, int depth_test, int depth_write) {
    if (!ctx) return -1;

    uint32_t handle = alloc_handle(ctx);

    /* VIRGL_OBJECT_DSA:
     * [0] handle
     * [1] S0: depth_enabled | (depth_writemask<<1) | (depth_func<<2)
     * [2] S1: stencil[0] (disabled = 0)
     * [3] S2: stencil[1] (disabled = 0)
     * [4] alpha_ref (float) */
    if (!cmd_space(ctx, 6)) toby_gl_flush(ctx);

    /* depth_func = LESS (1) when enabled, ALWAYS (7) otherwise */
    uint32_t s0 = 0;
    if (depth_test) {
        s0 |= 1;              /* depth enable */
        s0 |= (depth_write ? 1u : 0u) << 1;
        s0 |= 1u << 2;       /* PIPE_FUNC_LESS */
    }

    cmd_emit(ctx, virgl_header(VIRGL_CCMD_CREATE_OBJECT,
                               VIRGL_OBJECT_DSA, 5));
    cmd_emit(ctx, handle);
    cmd_emit(ctx, s0);
    cmd_emit(ctx, 0);          /* stencil[0] */
    cmd_emit(ctx, 0);          /* stencil[1] */
    cmd_emit(ctx, 0);          /* alpha_ref */

    return (int)handle;
}

void toby_gl_bind_dsa(toby_gl_ctx *ctx, int handle) {
    if (!ctx) return;
    if (!cmd_space(ctx, 2)) toby_gl_flush(ctx);

    cmd_emit(ctx, virgl_header(VIRGL_CCMD_BIND_OBJECT,
                               VIRGL_OBJECT_DSA, 1));
    cmd_emit(ctx, (uint32_t)handle);
}

/* ---- Submission ------------------------------------------------- */

void toby_gl_flush(toby_gl_ctx *ctx) {
    if (!ctx || ctx->cmd_offset == 0) return;

    toby_gl_sys_submit(ctx->ctx_id, ctx->cmd_buf,
                       ctx->cmd_offset * sizeof(uint32_t));
    ctx->cmd_offset = 0;
}

void toby_gl_swap_buffers(toby_gl_ctx *ctx) {
    if (!ctx) return;
    toby_gl_flush(ctx);
    toby_gl_sys_swap_buffers(ctx->ctx_id, ctx->window_fd);
}
