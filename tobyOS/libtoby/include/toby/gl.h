/* toby/gl.h -- Userland 3D graphics API for tobyOS VirGL.
 *
 * This header provides two layers:
 *
 *   1. Low-level syscall wrappers (toby_gl_sys_*) that map directly to
 *      the kernel's ABI_SYS_GL_* syscalls (100-104).
 *
 *   2. High-level Gallium3D command stream encoder (toby_gl_*) that
 *      builds properly formatted VirGL command buffers and submits them
 *      via the low-level layer. This gives an OpenGL ES 2.0-like API.
 *
 * Applications should use the high-level API unless they need raw
 * command buffer access for advanced features.
 */

#ifndef TOBY_GL_H
#define TOBY_GL_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ====================================================================
 * VirGL protocol constants
 * ==================================================================== */

/* Command opcodes (VIRGL_CCMD_*) */
#define VIRGL_CCMD_NOP                          0
#define VIRGL_CCMD_CREATE_OBJECT                1
#define VIRGL_CCMD_BIND_OBJECT                  2
#define VIRGL_CCMD_DESTROY_OBJECT               3
#define VIRGL_CCMD_SET_VIEWPORT_STATE           4
#define VIRGL_CCMD_SET_FRAMEBUFFER_STATE        5
#define VIRGL_CCMD_SET_VERTEX_BUFFERS           6
#define VIRGL_CCMD_CLEAR                        7
#define VIRGL_CCMD_DRAW_VBO                     8
#define VIRGL_CCMD_RESOURCE_INLINE_WRITE        9
#define VIRGL_CCMD_SET_SAMPLER_VIEWS            10
#define VIRGL_CCMD_SET_INDEX_BUFFER             11
#define VIRGL_CCMD_SET_CONSTANT_BUFFER          12
#define VIRGL_CCMD_SET_STENCIL_REF              13
#define VIRGL_CCMD_SET_BLEND_COLOR              14
#define VIRGL_CCMD_SET_SCISSOR_STATE            15
#define VIRGL_CCMD_BLIT                         16
#define VIRGL_CCMD_RESOURCE_COPY_REGION         17
#define VIRGL_CCMD_BIND_SAMPLER_STATES          18
#define VIRGL_CCMD_BEGIN_QUERY                  19
#define VIRGL_CCMD_END_QUERY                    20
#define VIRGL_CCMD_GET_QUERY_RESULT             21
#define VIRGL_CCMD_SET_POLYGON_STIPPLE          22
#define VIRGL_CCMD_SET_CLIP_STATE               23
#define VIRGL_CCMD_SET_SAMPLE_MASK              24
#define VIRGL_CCMD_SET_STREAMOUT_TARGETS        25
#define VIRGL_CCMD_SET_RENDER_CONDITION         26
#define VIRGL_CCMD_SET_UNIFORM_BUFFER           27
#define VIRGL_CCMD_SET_SUB_CTX                  28
#define VIRGL_CCMD_CREATE_SUB_CTX               29
#define VIRGL_CCMD_DESTROY_SUB_CTX              30
#define VIRGL_CCMD_BIND_SHADER                  31
#define VIRGL_CCMD_SET_TESS_STATE               32
#define VIRGL_CCMD_SET_MIN_SAMPLES              33
#define VIRGL_CCMD_SET_SHADER_BUFFERS           34
#define VIRGL_CCMD_SET_SHADER_IMAGES            35
#define VIRGL_CCMD_MEMORY_BARRIER               36
#define VIRGL_CCMD_LAUNCH_GRID                  37
#define VIRGL_CCMD_SET_FRAMEBUFFER_STATE_NO_ATTACH 38
#define VIRGL_CCMD_TEXTURE_BARRIER              39
#define VIRGL_CCMD_SET_ATOMIC_BUFFERS           40
#define VIRGL_CCMD_SET_DEBUG_FLAGS              41
#define VIRGL_CCMD_GET_QUERY_RESULT_QBO         42
#define VIRGL_CCMD_TRANSFER3D                   43
#define VIRGL_CCMD_END_TRANSFERS                44
#define VIRGL_CCMD_COPY_TRANSFER3D              45
#define VIRGL_CCMD_SET_TWEAKS                   46
#define VIRGL_CCMD_CLEAR_TEXTURE                47
#define VIRGL_CCMD_PIPE_RESOURCE_CREATE         48
#define VIRGL_CCMD_PIPE_RESOURCE_SET_TYPE       49
#define VIRGL_CCMD_GET_MEMORY_INFO              50
#define VIRGL_CCMD_SEND_STRING_MARKER           51
#define VIRGL_CCMD_LINK_SHADER                  52

/* Object types for CREATE_OBJECT / BIND_OBJECT */
#define VIRGL_OBJECT_BLEND                      1
#define VIRGL_OBJECT_RASTERIZER                 2
#define VIRGL_OBJECT_DSA                        3
#define VIRGL_OBJECT_SHADER                     4
#define VIRGL_OBJECT_VERTEX_ELEMENTS            5
#define VIRGL_OBJECT_SAMPLER_VIEW               6
#define VIRGL_OBJECT_SAMPLER_STATE              7
#define VIRGL_OBJECT_SURFACE                    8
#define VIRGL_OBJECT_QUERY                      9
#define VIRGL_OBJECT_STREAMOUT_TARGET           10

/* Shader types */
#define VIRGL_SHADER_VERTEX                     0
#define VIRGL_SHADER_FRAGMENT                   1
#define VIRGL_SHADER_GEOMETRY                   2
#define VIRGL_SHADER_TESS_CTRL                  3
#define VIRGL_SHADER_TESS_EVAL                  4
#define VIRGL_SHADER_COMPUTE                    5

/* Primitive modes (Gallium PIPE_PRIM_*) */
#define TOBY_GL_POINTS                          0
#define TOBY_GL_LINES                           1
#define TOBY_GL_LINE_LOOP                       2
#define TOBY_GL_LINE_STRIP                      3
#define TOBY_GL_TRIANGLES                       4
#define TOBY_GL_TRIANGLE_STRIP                  5
#define TOBY_GL_TRIANGLE_FAN                    6
#define TOBY_GL_QUADS                           7
#define TOBY_GL_QUAD_STRIP                      8
#define TOBY_GL_POLYGON                         9

/* Blend factors (Gallium PIPE_BLENDFACTOR_*) */
#define TOBY_GL_BLEND_ZERO                      0x11
#define TOBY_GL_BLEND_ONE                       0x12
#define TOBY_GL_BLEND_SRC_COLOR                 0x13
#define TOBY_GL_BLEND_SRC_ALPHA                 0x15
#define TOBY_GL_BLEND_DST_ALPHA                 0x16
#define TOBY_GL_BLEND_DST_COLOR                 0x17
#define TOBY_GL_BLEND_INV_SRC_COLOR             0x19
#define TOBY_GL_BLEND_INV_SRC_ALPHA             0x1A
#define TOBY_GL_BLEND_INV_DST_ALPHA             0x1B
#define TOBY_GL_BLEND_INV_DST_COLOR             0x1C

/* Fill modes (Gallium PIPE_POLYGON_MODE_*) */
#define TOBY_GL_FILL_SOLID                      0
#define TOBY_GL_FILL_LINE                       1
#define TOBY_GL_FILL_POINT                      2

/* Cull modes (Gallium PIPE_FACE_*) */
#define TOBY_GL_CULL_NONE                       0
#define TOBY_GL_CULL_FRONT                      1
#define TOBY_GL_CULL_BACK                       2
#define TOBY_GL_CULL_FRONT_AND_BACK             3

/* Vertex element source format (subset of PIPE_FORMAT_*) */
#define TOBY_GL_FORMAT_R32_FLOAT                0x1D
#define TOBY_GL_FORMAT_R32G32_FLOAT             0x1E
#define TOBY_GL_FORMAT_R32G32B32_FLOAT          0x1F
#define TOBY_GL_FORMAT_R32G32B32A32_FLOAT       0x20
#define TOBY_GL_FORMAT_R8G8B8A8_UNORM           0x24
#define TOBY_GL_FORMAT_B8G8R8A8_UNORM           0x1C
#define TOBY_GL_FORMAT_R16_UINT                 0x2F
#define TOBY_GL_FORMAT_R32_UINT                 0x30

/* Clear buffer bits */
#define TOBY_GL_CLEAR_COLOR                     (1 << 2)
#define TOBY_GL_CLEAR_DEPTH                     (1 << 0)
#define TOBY_GL_CLEAR_STENCIL                   (1 << 1)

/* Pipe resource targets */
#define TOBY_GL_TARGET_BUFFER                   0
#define TOBY_GL_TARGET_TEXTURE_1D               1
#define TOBY_GL_TARGET_TEXTURE_2D               2
#define TOBY_GL_TARGET_TEXTURE_3D               3
#define TOBY_GL_TARGET_TEXTURE_CUBE             4
#define TOBY_GL_TARGET_TEXTURE_RECT             5
#define TOBY_GL_TARGET_TEXTURE_1D_ARRAY         6
#define TOBY_GL_TARGET_TEXTURE_2D_ARRAY         7

/* Pipe resource bind flags */
#define TOBY_GL_BIND_DEPTH_STENCIL              0x01
#define TOBY_GL_BIND_RENDER_TARGET              0x02
#define TOBY_GL_BIND_VERTEX_BUFFER              0x04
#define TOBY_GL_BIND_INDEX_BUFFER               0x08
#define TOBY_GL_BIND_CONSTANT_BUFFER            0x10
#define TOBY_GL_BIND_SAMPLER_VIEW               0x20

/* Texture wrap modes (PIPE_TEX_WRAP_*) */
#define TOBY_GL_WRAP_REPEAT                     0
#define TOBY_GL_WRAP_CLAMP                      1
#define TOBY_GL_WRAP_CLAMP_TO_EDGE              2
#define TOBY_GL_WRAP_CLAMP_TO_BORDER            3
#define TOBY_GL_WRAP_MIRROR_REPEAT              4

/* Texture filter modes (PIPE_TEX_FILTER_*) */
#define TOBY_GL_FILTER_NEAREST                  0
#define TOBY_GL_FILTER_LINEAR                   1

/* Texture mip filter modes (PIPE_TEX_MIPFILTER_*) */
#define TOBY_GL_MIPFILTER_NEAREST               0
#define TOBY_GL_MIPFILTER_LINEAR                1
#define TOBY_GL_MIPFILTER_NONE                  2

/* ====================================================================
 * Syscall numbers (must match kernel ABI)
 * ==================================================================== */

#define ABI_SYS_GL_CREATE_CTX                   100
#define ABI_SYS_GL_DESTROY_CTX                  101
#define ABI_SYS_GL_CREATE_BUFFER                102
#define ABI_SYS_GL_SUBMIT                       103
#define ABI_SYS_GL_SWAP_BUFFERS                 104

/* ====================================================================
 * Low-level syscall wrappers (inline)
 * ==================================================================== */

static inline long toby_gl_sys_create_ctx(void) {
    long r;
    __asm__ volatile ("syscall"
        : "=a"(r) : "0"((long)ABI_SYS_GL_CREATE_CTX)
        : "rcx", "r11", "memory");
    return r;
}

static inline long toby_gl_sys_destroy_ctx(uint32_t ctx_id) {
    long r;
    __asm__ volatile ("syscall"
        : "=a"(r)
        : "0"((long)ABI_SYS_GL_DESTROY_CTX), "D"((long)ctx_id)
        : "rcx", "r11", "memory");
    return r;
}

static inline long toby_gl_sys_create_buffer(uint32_t ctx_id, uint32_t size) {
    long r;
    __asm__ volatile ("syscall"
        : "=a"(r)
        : "0"((long)ABI_SYS_GL_CREATE_BUFFER),
          "D"((long)ctx_id), "S"((long)size)
        : "rcx", "r11", "memory");
    return r;
}

static inline long toby_gl_sys_submit(uint32_t ctx_id,
                                      const void *cmd_buf,
                                      uint32_t cmd_size) {
    long r;
    __asm__ volatile ("syscall"
        : "=a"(r)
        : "0"((long)ABI_SYS_GL_SUBMIT),
          "D"((long)ctx_id), "S"((long)(uintptr_t)cmd_buf),
          "d"((long)cmd_size)
        : "rcx", "r11", "memory");
    return r;
}

static inline long toby_gl_sys_swap_buffers(uint32_t ctx_id, int win_fd) {
    long r;
    register long r10 __asm__("r10") = (long)win_fd;
    __asm__ volatile ("syscall"
        : "=a"(r)
        : "0"((long)ABI_SYS_GL_SWAP_BUFFERS),
          "D"((long)ctx_id), "r"(r10)
        : "rcx", "r11", "memory");
    return r;
}

/* ====================================================================
 * High-level command stream encoder API
 * ==================================================================== */

/* Command buffer capacity in uint32_t words (4 KB) */
#define TOBY_GL_CMD_BUF_WORDS  1024

/* Maximum number of tracked objects (shaders, VBOs, etc.) */
#define TOBY_GL_MAX_OBJECTS    64

/* Vertex element descriptor */
struct toby_gl_vertex_element {
    uint32_t src_offset;
    uint32_t src_format;    /* TOBY_GL_FORMAT_* */
    uint32_t vertex_buffer_index;
};

/* Opaque context -- allocated by toby_gl_init() */
typedef struct toby_gl_ctx {
    uint32_t ctx_id;
    int      window_fd;
    uint32_t cmd_buf[TOBY_GL_CMD_BUF_WORDS];
    uint32_t cmd_offset;
    uint32_t next_handle;
    int      vp_x, vp_y, vp_w, vp_h;
} toby_gl_ctx;

/* Lifecycle */
toby_gl_ctx *toby_gl_init(int window_fd);
void         toby_gl_destroy(toby_gl_ctx *ctx);

/* Clear operations */
void toby_gl_clear(toby_gl_ctx *ctx, float r, float g, float b, float a);
void toby_gl_clear_depth(toby_gl_ctx *ctx, float depth);

/* Viewport */
void toby_gl_viewport(toby_gl_ctx *ctx, int x, int y, int w, int h);

/* Shaders (TGSI text input) */
int  toby_gl_create_shader(toby_gl_ctx *ctx, int type, const char *tgsi_text);
void toby_gl_bind_shader(toby_gl_ctx *ctx, int shader_handle, int type);

/* Vertex data */
int  toby_gl_create_vertex_buffer(toby_gl_ctx *ctx,
                                  const void *data, uint32_t size);
void toby_gl_set_vertex_buffer(toby_gl_ctx *ctx, int buf_handle,
                               uint32_t stride, uint32_t offset);
int  toby_gl_create_vertex_elements(toby_gl_ctx *ctx, int count,
                                    const struct toby_gl_vertex_element *elems);
void toby_gl_bind_vertex_elements(toby_gl_ctx *ctx, int ve_handle);

/* Drawing */
void toby_gl_draw_arrays(toby_gl_ctx *ctx, int mode, int start, int count);
void toby_gl_draw_elements(toby_gl_ctx *ctx, int mode, int count,
                           int index_handle, int index_size);

/* Index buffers */
int  toby_gl_create_index_buffer(toby_gl_ctx *ctx,
                                 const void *data, uint32_t size,
                                 int index_size);
void toby_gl_set_index_buffer(toby_gl_ctx *ctx, int buf_handle,
                              int index_size, uint32_t offset);

/* Uniform/constant buffers */
int  toby_gl_create_uniform_buffer(toby_gl_ctx *ctx,
                                   const void *data, uint32_t size);
void toby_gl_set_constant_buffer(toby_gl_ctx *ctx, int shader_type,
                                 int index, int buf_handle, uint32_t size);
void toby_gl_set_constant_buffer_inline(toby_gl_ctx *ctx, int shader_type,
                                        int index, const void *data,
                                        uint32_t size);

/* Texture & sampler */
int  toby_gl_create_texture_2d(toby_gl_ctx *ctx, uint32_t width,
                               uint32_t height, uint32_t format);
void toby_gl_upload_texture(toby_gl_ctx *ctx, int tex_handle,
                            uint32_t width, uint32_t height,
                            uint32_t format, const void *data,
                            uint32_t data_size);
int  toby_gl_create_sampler(toby_gl_ctx *ctx, int min_filter,
                            int mag_filter, int mip_filter,
                            int wrap_s, int wrap_t);
void toby_gl_bind_sampler(toby_gl_ctx *ctx, int shader_type,
                          int slot, int sampler_handle);
int  toby_gl_create_sampler_view(toby_gl_ctx *ctx, int tex_handle,
                                 uint32_t format, int target);
void toby_gl_set_sampler_view(toby_gl_ctx *ctx, int shader_type,
                              int slot, int view_handle);

/* Framebuffer (render-to-texture) */
int  toby_gl_create_surface(toby_gl_ctx *ctx, int tex_handle,
                            uint32_t format, int level);
void toby_gl_set_framebuffer(toby_gl_ctx *ctx, int depth_surface,
                             int nr_cbufs, const int *color_surfaces,
                             uint32_t width, uint32_t height);

/* Scissor */
void toby_gl_set_scissor(toby_gl_ctx *ctx, int x0, int y0, int x1, int y1);

/* Compute dispatch */
void toby_gl_dispatch_compute(toby_gl_ctx *ctx,
                              uint32_t grid_x, uint32_t grid_y,
                              uint32_t grid_z,
                              uint32_t block_x, uint32_t block_y,
                              uint32_t block_z);

/* Shader storage / image buffers (compute) */
void toby_gl_set_shader_buffer(toby_gl_ctx *ctx, int shader_type,
                               int slot, int buf_handle,
                               uint32_t offset, uint32_t size);

/* Resource destruction */
void toby_gl_destroy_object(toby_gl_ctx *ctx, int handle);

/* Pipeline state objects */
int  toby_gl_create_blend(toby_gl_ctx *ctx, int enable, int src, int dst);
void toby_gl_bind_blend(toby_gl_ctx *ctx, int handle);
int  toby_gl_create_rasterizer(toby_gl_ctx *ctx, int fill_mode, int cull_mode);
void toby_gl_bind_rasterizer(toby_gl_ctx *ctx, int handle);
int  toby_gl_create_dsa(toby_gl_ctx *ctx, int depth_test, int depth_write);
void toby_gl_bind_dsa(toby_gl_ctx *ctx, int handle);

/* Submission */
void toby_gl_flush(toby_gl_ctx *ctx);
void toby_gl_swap_buffers(toby_gl_ctx *ctx);

/* ====================================================================
 * Standard OpenGL ES 2.0 API (software rasterizer -- gl_raster.c)
 *
 * These functions provide a drop-in GLES2-like interface backed by a
 * pure software triangle rasterizer. No GPU hardware required.
 * ==================================================================== */

typedef unsigned int GLuint;
typedef int GLint;
typedef float GLfloat;
typedef unsigned int GLenum;
typedef int GLsizei;
typedef unsigned char GLboolean;
typedef unsigned int GLbitfield;
typedef void GLvoid;

#define GL_TRUE  1
#define GL_FALSE 0

#define GL_COLOR_BUFFER_BIT   0x4000
#define GL_DEPTH_BUFFER_BIT   0x0100

#define GL_TRIANGLES      0x0004
#define GL_TRIANGLE_STRIP 0x0005
#define GL_TRIANGLE_FAN   0x0006
#define GL_LINES          0x0001
#define GL_LINE_STRIP     0x0003
#define GL_POINTS         0x0000

#define GL_FLOAT          0x1406
#define GL_UNSIGNED_BYTE  0x1401
#define GL_UNSIGNED_SHORT 0x1403

#define GL_TEXTURE_2D     0x0DE1
#define GL_TEXTURE0       0x84C0
#define GL_RGBA           0x1908
#define GL_RGB            0x1907
#define GL_NEAREST        0x2600
#define GL_LINEAR         0x2601
#define GL_TEXTURE_MIN_FILTER 0x2801
#define GL_TEXTURE_MAG_FILTER 0x2800
#define GL_TEXTURE_WRAP_S 0x2802
#define GL_TEXTURE_WRAP_T 0x2803
#define GL_CLAMP_TO_EDGE  0x812F
#define GL_REPEAT         0x2901

#define GL_BLEND          0x0BE2
#define GL_SRC_ALPHA      0x0302
#define GL_ONE_MINUS_SRC_ALPHA 0x0303
#define GL_DEPTH_TEST     0x0B71

#define GL_VERTEX_SHADER   0x8B31
#define GL_FRAGMENT_SHADER 0x8B30
#define GL_COMPILE_STATUS  0x8B81
#define GL_LINK_STATUS     0x8B82

#define GL_ARRAY_BUFFER   0x8892
#define GL_STATIC_DRAW    0x88E4
#define GL_DYNAMIC_DRAW   0x88E8

void glClearColor(GLfloat r, GLfloat g, GLfloat b, GLfloat a);
void glClear(GLbitfield mask);
void glViewport(GLint x, GLint y, GLsizei w, GLsizei h);
void glEnable(GLenum cap);
void glDisable(GLenum cap);
void glBlendFunc(GLenum sfactor, GLenum dfactor);

GLuint glCreateShader(GLenum type);
void glShaderSource(GLuint shader, GLsizei count, const char **string, const GLint *length);
void glCompileShader(GLuint shader);
void glGetShaderiv(GLuint shader, GLenum pname, GLint *params);
GLuint glCreateProgram(void);
void glAttachShader(GLuint program, GLuint shader);
void glLinkProgram(GLuint program);
void glUseProgram(GLuint program);
void glDeleteShader(GLuint shader);
GLint glGetUniformLocation(GLuint program, const char *name);
void glUniform1i(GLint location, GLint v0);
void glUniform1f(GLint location, GLfloat v0);
void glUniform4f(GLint location, GLfloat v0, GLfloat v1, GLfloat v2, GLfloat v3);
void glUniformMatrix4fv(GLint location, GLsizei count, GLboolean transpose, const GLfloat *value);
GLint glGetAttribLocation(GLuint program, const char *name);

void glGenBuffers(GLsizei n, GLuint *buffers);
void glBindBuffer(GLenum target, GLuint buffer);
void glBufferData(GLenum target, GLsizei size, const GLvoid *data, GLenum usage);
void glVertexAttribPointer(GLuint index, GLint size, GLenum type, GLboolean normalized, GLsizei stride, const GLvoid *pointer);
void glEnableVertexAttribArray(GLuint index);
void glDrawArrays(GLenum mode, GLint first, GLsizei count);

void glGenTextures(GLsizei n, GLuint *textures);
void glBindTexture(GLenum target, GLuint texture);
void glTexImage2D(GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height, GLint border, GLenum format, GLenum type, const GLvoid *pixels);
void glTexParameteri(GLenum target, GLenum pname, GLint param);
void glActiveTexture(GLenum texture);

void glReadPixels(GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type, GLvoid *pixels);

void gl_init(int width, int height, uint32_t *framebuffer);
void gl_present(void);

#ifdef __cplusplus
}
#endif

#endif /* TOBY_GL_H */
