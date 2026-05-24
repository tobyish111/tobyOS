/* gl_raster.c -- Pure software OpenGL ES 2.0 rasterizer for tobyOS.
 *
 * Provides a standard glXxx() API suitable for 2D compositing and basic 3D.
 * No hardware GPU required -- all rendering is done in software to an
 * internal XRGB8888 color buffer.
 *
 * Shader model: We don't implement a full GLSL compiler.  Instead, shader
 * source is stored but the rasterizer uses fixed-function paths selected
 * by which state is enabled (texture bound, blending on, etc.).
 */

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <toby/gl.h>

/* ======================================================================
 * Internal constants
 * ====================================================================== */

#define GL_MAX_TEXTURES   32
#define GL_MAX_BUFFERS    64
#define GL_MAX_SHADERS    32
#define GL_MAX_PROGRAMS   16
#define GL_MAX_ATTRIBS    8
#define GL_MAX_UNIFORMS   32
#define GL_MAX_TEX_SIZE   512
#define GL_MAX_DIRTY_RECTS 64
#define GL_RASTER_MAX_W   1280
#define GL_RASTER_MAX_H   1024
#define GL_BUF_POOL_SIZE  16384

/* ======================================================================
 * Internal types
 * ====================================================================== */

typedef struct {
    uint32_t *data;
    int width, height;
    int min_filter, mag_filter;
    int wrap_s, wrap_t;
    int valid;
} gl_texture_t;

typedef struct {
    void *data;
    int size;
    int valid;
} gl_buffer_t;

typedef struct {
    char source[512];
    GLenum type;
    int compiled;
    int valid;
} gl_shader_t;

typedef struct {
    int vs_id, fs_id;
    int linked;
    int valid;
    char uniform_names[GL_MAX_UNIFORMS][64];
    float uniform_values[GL_MAX_UNIFORMS][16];
    int uniform_count;
    char attrib_names[GL_MAX_ATTRIBS][64];
    int attrib_count;
} gl_program_t;

typedef struct {
    int enabled;
    GLint size;
    GLenum type;
    GLboolean normalized;
    GLsizei stride;
    const void *pointer;
} gl_attrib_t;

typedef struct {
    float x, y, z, w;
    float u, v;
    float r, g, b, a;
} gl_vertex_t;

/* ======================================================================
 * Global state
 * ====================================================================== */

static struct {
    uint32_t *color_buf;
    float *depth_buf;
    uint32_t *target_fb;
    int width, height;
    int vp_x, vp_y, vp_w, vp_h;

    float clear_r, clear_g, clear_b, clear_a;
    float clear_depth;

    int blend_enabled;
    GLenum blend_sfactor, blend_dfactor;
    int depth_test_enabled;

    gl_texture_t textures[GL_MAX_TEXTURES];
    int next_tex_id;
    int bound_texture;
    int active_texture_unit;

    gl_buffer_t buffers[GL_MAX_BUFFERS];
    int next_buf_id;
    int bound_array_buffer;

    gl_shader_t shaders[GL_MAX_SHADERS];
    int next_shader_id;

    gl_program_t programs[GL_MAX_PROGRAMS];
    int next_program_id;
    int current_program;

    gl_attrib_t attribs[GL_MAX_ATTRIBS];

    int initialized;
} g_gl;

/* ======================================================================
 * Math helpers
 * ====================================================================== */

static float gl_min(float a, float b) { return a < b ? a : b; }
static float gl_max(float a, float b) { return a > b ? a : b; }
static float gl_clamp(float v, float lo, float hi) {
    return gl_max(lo, gl_min(hi, v));
}
static int gl_imin(int a, int b) { return a < b ? a : b; }
static int gl_imax(int a, int b) { return a > b ? a : b; }

static float gl_abs(float x) { return x < 0.0f ? -x : x; }
static float gl_floor(float x) {
    int i = (int)x;
    return (float)(x < (float)i ? i - 1 : i);
}

static uint32_t gl_pack_color(float r, float g, float b, float a) {
    int ri = (int)(gl_clamp(r, 0.0f, 1.0f) * 255.0f + 0.5f);
    int gi = (int)(gl_clamp(g, 0.0f, 1.0f) * 255.0f + 0.5f);
    int bi = (int)(gl_clamp(b, 0.0f, 1.0f) * 255.0f + 0.5f);
    int ai = (int)(gl_clamp(a, 0.0f, 1.0f) * 255.0f + 0.5f);
    (void)ai;
    return ((uint32_t)ri << 16) | ((uint32_t)gi << 8) | (uint32_t)bi;
}

static uint32_t gl_pack_color_alpha(float r, float g, float b, float a) {
    int ri = (int)(gl_clamp(r, 0.0f, 1.0f) * 255.0f + 0.5f);
    int gi = (int)(gl_clamp(g, 0.0f, 1.0f) * 255.0f + 0.5f);
    int bi = (int)(gl_clamp(b, 0.0f, 1.0f) * 255.0f + 0.5f);
    int ai = (int)(gl_clamp(a, 0.0f, 1.0f) * 255.0f + 0.5f);
    return ((uint32_t)ai << 24) | ((uint32_t)ri << 16) |
           ((uint32_t)gi << 8) | (uint32_t)bi;
}

static void gl_unpack_color(uint32_t c, float *r, float *g, float *b, float *a) {
    *a = (float)((c >> 24) & 0xFF) / 255.0f;
    *r = (float)((c >> 16) & 0xFF) / 255.0f;
    *g = (float)((c >> 8) & 0xFF) / 255.0f;
    *b = (float)(c & 0xFF) / 255.0f;
}

/* 4x4 matrix-vector multiply */
static void mat4_mul_vec4(const float *m, const float *v, float *out) {
    for (int i = 0; i < 4; i++) {
        out[i] = m[i*4+0]*v[0] + m[i*4+1]*v[1] + m[i*4+2]*v[2] + m[i*4+3]*v[3];
    }
}

/* ======================================================================
 * Texture sampling
 * ====================================================================== */

static int gl_tex_wrap(int coord, int size, int mode) {
    if (mode == GL_REPEAT) {
        coord = coord % size;
        if (coord < 0) coord += size;
    } else {
        if (coord < 0) coord = 0;
        if (coord >= size) coord = size - 1;
    }
    return coord;
}

static uint32_t gl_tex_sample_nearest(gl_texture_t *tex, float u, float v) {
    if (!tex || !tex->data || !tex->valid) return 0xFFFFFFFF;

    int x = (int)(u * tex->width);
    int y = (int)(v * tex->height);
    x = gl_tex_wrap(x, tex->width, tex->wrap_s);
    y = gl_tex_wrap(y, tex->height, tex->wrap_t);
    return tex->data[y * tex->width + x];
}

static uint32_t gl_tex_sample_bilinear(gl_texture_t *tex, float u, float v) {
    if (!tex || !tex->data || !tex->valid) return 0xFFFFFFFF;

    float fx = u * tex->width - 0.5f;
    float fy = v * tex->height - 0.5f;
    int x0 = (int)gl_floor(fx);
    int y0 = (int)gl_floor(fy);
    float dx = fx - (float)x0;
    float dy = fy - (float)y0;

    int x1 = x0 + 1;
    int y1 = y0 + 1;
    x0 = gl_tex_wrap(x0, tex->width, tex->wrap_s);
    x1 = gl_tex_wrap(x1, tex->width, tex->wrap_s);
    y0 = gl_tex_wrap(y0, tex->height, tex->wrap_t);
    y1 = gl_tex_wrap(y1, tex->height, tex->wrap_t);

    uint32_t c00 = tex->data[y0 * tex->width + x0];
    uint32_t c10 = tex->data[y0 * tex->width + x1];
    uint32_t c01 = tex->data[y1 * tex->width + x0];
    uint32_t c11 = tex->data[y1 * tex->width + x1];

    float r00, g00, b00, a00, r10, g10, b10, a10;
    float r01, g01, b01, a01, r11, g11, b11, a11;
    gl_unpack_color(c00, &r00, &g00, &b00, &a00);
    gl_unpack_color(c10, &r10, &g10, &b10, &a10);
    gl_unpack_color(c01, &r01, &g01, &b01, &a01);
    gl_unpack_color(c11, &r11, &g11, &b11, &a11);

    float inv_dx = 1.0f - dx;
    float inv_dy = 1.0f - dy;

    float r = (r00*inv_dx + r10*dx)*inv_dy + (r01*inv_dx + r11*dx)*dy;
    float g = (g00*inv_dx + g10*dx)*inv_dy + (g01*inv_dx + g11*dx)*dy;
    float b = (b00*inv_dx + b10*dx)*inv_dy + (b01*inv_dx + b11*dx)*dy;
    float a = (a00*inv_dx + a10*dx)*inv_dy + (a01*inv_dx + a11*dx)*dy;

    return gl_pack_color_alpha(r, g, b, a);
}

static uint32_t gl_tex_sample(gl_texture_t *tex, float u, float v) {
    if (!tex) return 0xFFFFFFFF;
    if (tex->mag_filter == GL_LINEAR)
        return gl_tex_sample_bilinear(tex, u, v);
    return gl_tex_sample_nearest(tex, u, v);
}

/* ======================================================================
 * Alpha blending
 * ====================================================================== */

static uint32_t gl_blend_pixel(uint32_t src_color, uint32_t dst_color,
                               GLenum sfactor, GLenum dfactor) {
    float sr, sg, sb, sa, dr, dg, db, da;
    gl_unpack_color(src_color, &sr, &sg, &sb, &sa);
    gl_unpack_color(dst_color, &dr, &dg, &db, &da);

    float sf = 1.0f, df = 0.0f;
    if (sfactor == GL_SRC_ALPHA) sf = sa;
    else if (sfactor == GL_ONE_MINUS_SRC_ALPHA) sf = 1.0f - sa;

    if (dfactor == GL_ONE_MINUS_SRC_ALPHA) df = 1.0f - sa;
    else if (dfactor == GL_SRC_ALPHA) df = sa;

    float or_ = sr * sf + dr * df;
    float og = sg * sf + dg * df;
    float ob = sb * sf + db * df;
    float oa = sa * sf + da * df;

    return gl_pack_color_alpha(or_, og, ob, oa);
}

/* ======================================================================
 * Fragment output
 * ====================================================================== */

static void gl_write_fragment(int x, int y, float depth, uint32_t color) {
    if (x < 0 || x >= g_gl.width || y < 0 || y >= g_gl.height)
        return;

    int idx = y * g_gl.width + x;

    if (g_gl.depth_test_enabled && g_gl.depth_buf) {
        if (depth >= g_gl.depth_buf[idx])
            return;
        g_gl.depth_buf[idx] = depth;
    }

    if (g_gl.blend_enabled) {
        uint32_t dst = g_gl.color_buf[idx];
        color = gl_blend_pixel(color, dst, g_gl.blend_sfactor, g_gl.blend_dfactor);
    }

    g_gl.color_buf[idx] = color;
}

/* ======================================================================
 * Triangle rasterization (edge function method)
 * ====================================================================== */

static float edge_function(float ax, float ay, float bx, float by,
                           float cx, float cy) {
    return (cx - ax) * (by - ay) - (cy - ay) * (bx - ax);
}

static void gl_rasterize_triangle(gl_vertex_t *v0, gl_vertex_t *v1, gl_vertex_t *v2) {
    /* Convert from clip space to screen space */
    float inv_w0 = 1.0f / v0->w;
    float inv_w1 = 1.0f / v1->w;
    float inv_w2 = 1.0f / v2->w;

    float sx0 = (v0->x * inv_w0 * 0.5f + 0.5f) * g_gl.vp_w + g_gl.vp_x;
    float sy0 = (1.0f - (v0->y * inv_w0 * 0.5f + 0.5f)) * g_gl.vp_h + g_gl.vp_y;
    float sz0 = v0->z * inv_w0 * 0.5f + 0.5f;

    float sx1 = (v1->x * inv_w1 * 0.5f + 0.5f) * g_gl.vp_w + g_gl.vp_x;
    float sy1 = (1.0f - (v1->y * inv_w1 * 0.5f + 0.5f)) * g_gl.vp_h + g_gl.vp_y;
    float sz1 = v1->z * inv_w1 * 0.5f + 0.5f;

    float sx2 = (v2->x * inv_w2 * 0.5f + 0.5f) * g_gl.vp_w + g_gl.vp_x;
    float sy2 = (1.0f - (v2->y * inv_w2 * 0.5f + 0.5f)) * g_gl.vp_h + g_gl.vp_y;
    float sz2 = v2->z * inv_w2 * 0.5f + 0.5f;

    /* Bounding box */
    int min_x = gl_imax((int)gl_min(gl_min(sx0, sx1), sx2), 0);
    int min_y = gl_imax((int)gl_min(gl_min(sy0, sy1), sy2), 0);
    int max_x = gl_imin((int)(gl_max(gl_max(sx0, sx1), sx2) + 1.0f), g_gl.width - 1);
    int max_y = gl_imin((int)(gl_max(gl_max(sy0, sy1), sy2) + 1.0f), g_gl.height - 1);

    float area = edge_function(sx0, sy0, sx1, sy1, sx2, sy2);
    if (gl_abs(area) < 0.001f) return;
    float inv_area = 1.0f / area;

    /* Determine rendering mode based on state */
    int has_texture = (g_gl.bound_texture > 0 &&
                       g_gl.bound_texture <= GL_MAX_TEXTURES &&
                       g_gl.textures[g_gl.bound_texture - 1].valid);
    gl_texture_t *tex = has_texture ? &g_gl.textures[g_gl.bound_texture - 1] : NULL;

    /* Get uniform color from current program if available */
    float uni_r = 1.0f, uni_g = 1.0f, uni_b = 1.0f, uni_a = 1.0f;
    if (g_gl.current_program > 0 && g_gl.current_program <= GL_MAX_PROGRAMS) {
        gl_program_t *prog = &g_gl.programs[g_gl.current_program - 1];
        if (prog->valid && prog->uniform_count > 0) {
            uni_r = prog->uniform_values[0][0];
            uni_g = prog->uniform_values[0][1];
            uni_b = prog->uniform_values[0][2];
            uni_a = prog->uniform_values[0][3];
        }
    }

    /* Perspective-correct interpolation setup */
    float u0_over_w = v0->u * inv_w0;
    float u1_over_w = v1->u * inv_w1;
    float u2_over_w = v2->u * inv_w2;
    float v0_over_w = v0->v * inv_w0;
    float v1_over_w = v1->v * inv_w1;
    float v2_over_w = v2->v * inv_w2;

    float r0_over_w = v0->r * inv_w0;
    float r1_over_w = v1->r * inv_w1;
    float r2_over_w = v2->r * inv_w2;
    float g0_over_w = v0->g * inv_w0;
    float g1_over_w = v1->g * inv_w1;
    float g2_over_w = v2->g * inv_w2;
    float b0_over_w = v0->b * inv_w0;
    float b1_over_w = v1->b * inv_w1;
    float b2_over_w = v2->b * inv_w2;
    float a0_over_w = v0->a * inv_w0;
    float a1_over_w = v1->a * inv_w1;
    float a2_over_w = v2->a * inv_w2;

    for (int py = min_y; py <= max_y; py++) {
        for (int px = min_x; px <= max_x; px++) {
            float pcx = (float)px + 0.5f;
            float pcy = (float)py + 0.5f;

            float w0 = edge_function(sx1, sy1, sx2, sy2, pcx, pcy);
            float w1 = edge_function(sx2, sy2, sx0, sy0, pcx, pcy);
            float w2 = edge_function(sx0, sy0, sx1, sy1, pcx, pcy);

            if (area > 0) {
                if (w0 < 0 || w1 < 0 || w2 < 0) continue;
            } else {
                if (w0 > 0 || w1 > 0 || w2 > 0) continue;
            }

            w0 *= inv_area;
            w1 *= inv_area;
            w2 *= inv_area;

            /* Depth interpolation */
            float depth = sz0 * w0 + sz1 * w1 + sz2 * w2;

            /* Perspective-correct interpolation */
            float one_over_w = inv_w0 * w0 + inv_w1 * w1 + inv_w2 * w2;
            float persp_w = (one_over_w != 0.0f) ? 1.0f / one_over_w : 1.0f;

            float frag_r, frag_g, frag_b, frag_a;

            if (has_texture) {
                float u = (u0_over_w * w0 + u1_over_w * w1 + u2_over_w * w2) * persp_w;
                float v_coord = (v0_over_w * w0 + v1_over_w * w1 + v2_over_w * w2) * persp_w;

                uint32_t tex_color = gl_tex_sample(tex, u, v_coord);
                gl_unpack_color(tex_color, &frag_r, &frag_g, &frag_b, &frag_a);

                /* Modulate with vertex color */
                float vr = (r0_over_w * w0 + r1_over_w * w1 + r2_over_w * w2) * persp_w;
                float vg = (g0_over_w * w0 + g1_over_w * w1 + g2_over_w * w2) * persp_w;
                float vb = (b0_over_w * w0 + b1_over_w * w1 + b2_over_w * w2) * persp_w;
                float va = (a0_over_w * w0 + a1_over_w * w1 + a2_over_w * w2) * persp_w;

                frag_r *= vr;
                frag_g *= vg;
                frag_b *= vb;
                frag_a *= va;
            } else {
                frag_r = (r0_over_w * w0 + r1_over_w * w1 + r2_over_w * w2) * persp_w;
                frag_g = (g0_over_w * w0 + g1_over_w * w1 + g2_over_w * w2) * persp_w;
                frag_b = (b0_over_w * w0 + b1_over_w * w1 + b2_over_w * w2) * persp_w;
                frag_a = (a0_over_w * w0 + a1_over_w * w1 + a2_over_w * w2) * persp_w;

                frag_r *= uni_r;
                frag_g *= uni_g;
                frag_b *= uni_b;
                frag_a *= uni_a;
            }

            uint32_t final_color = gl_pack_color_alpha(frag_r, frag_g, frag_b, frag_a);
            gl_write_fragment(px, py, depth, final_color);
        }
    }
}

/* ======================================================================
 * Line rasterization (Bresenham)
 * ====================================================================== */

static void gl_rasterize_line(gl_vertex_t *v0, gl_vertex_t *v1) {
    float inv_w0 = 1.0f / v0->w;
    float inv_w1 = 1.0f / v1->w;

    int x0 = (int)((v0->x * inv_w0 * 0.5f + 0.5f) * g_gl.vp_w + g_gl.vp_x);
    int y0 = (int)((1.0f - (v0->y * inv_w0 * 0.5f + 0.5f)) * g_gl.vp_h + g_gl.vp_y);
    int x1 = (int)((v1->x * inv_w1 * 0.5f + 0.5f) * g_gl.vp_w + g_gl.vp_x);
    int y1 = (int)((1.0f - (v1->y * inv_w1 * 0.5f + 0.5f)) * g_gl.vp_h + g_gl.vp_y);

    uint32_t color = gl_pack_color_alpha(v0->r, v0->g, v0->b, v0->a);

    int dx = x1 - x0;
    int dy = y1 - y0;
    int sx = dx > 0 ? 1 : -1;
    int sy = dy > 0 ? 1 : -1;
    dx = dx < 0 ? -dx : dx;
    dy = dy < 0 ? -dy : dy;

    int err = dx - dy;
    int steps = dx > dy ? dx : dy;

    for (int i = 0; i <= steps; i++) {
        float t = steps > 0 ? (float)i / (float)steps : 0.0f;
        float depth = (v0->z * inv_w0) * (1.0f - t) + (v1->z * inv_w1) * t;
        depth = depth * 0.5f + 0.5f;
        gl_write_fragment(x0, y0, depth, color);

        int e2 = err * 2;
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 < dx) { err += dx; y0 += sy; }
    }
}

/* ======================================================================
 * Point rasterization
 * ====================================================================== */

static void gl_rasterize_point(gl_vertex_t *v) {
    float inv_w = 1.0f / v->w;
    int x = (int)((v->x * inv_w * 0.5f + 0.5f) * g_gl.vp_w + g_gl.vp_x);
    int y = (int)((1.0f - (v->y * inv_w * 0.5f + 0.5f)) * g_gl.vp_h + g_gl.vp_y);
    float depth = v->z * inv_w * 0.5f + 0.5f;
    uint32_t color = gl_pack_color_alpha(v->r, v->g, v->b, v->a);
    gl_write_fragment(x, y, depth, color);
}

/* ======================================================================
 * Vertex processing (apply MVP transform)
 * ====================================================================== */

static void gl_process_vertex(const float *pos, int pos_size,
                              const float *color, int color_size,
                              const float *texcoord, int texcoord_size,
                              const float *mvp, gl_vertex_t *out) {
    float in_pos[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    if (pos_size >= 1) in_pos[0] = pos[0];
    if (pos_size >= 2) in_pos[1] = pos[1];
    if (pos_size >= 3) in_pos[2] = pos[2];
    if (pos_size >= 4) in_pos[3] = pos[3];

    if (mvp) {
        float clip[4];
        mat4_mul_vec4(mvp, in_pos, clip);
        out->x = clip[0];
        out->y = clip[1];
        out->z = clip[2];
        out->w = clip[3];
    } else {
        out->x = in_pos[0];
        out->y = in_pos[1];
        out->z = in_pos[2];
        out->w = in_pos[3];
    }

    if (out->w == 0.0f) out->w = 1.0f;

    out->r = 1.0f; out->g = 1.0f; out->b = 1.0f; out->a = 1.0f;
    if (color) {
        if (color_size >= 1) out->r = color[0];
        if (color_size >= 2) out->g = color[1];
        if (color_size >= 3) out->b = color[2];
        if (color_size >= 4) out->a = color[3];
    }

    out->u = 0.0f; out->v = 0.0f;
    if (texcoord) {
        if (texcoord_size >= 1) out->u = texcoord[0];
        if (texcoord_size >= 2) out->v = texcoord[1];
    }
}

/* ======================================================================
 * Clipping (simple near-plane clip; trivial reject via Cohen-Sutherland)
 * ====================================================================== */

#define CLIP_INSIDE 0
#define CLIP_LEFT   1
#define CLIP_RIGHT  2
#define CLIP_BOTTOM 4
#define CLIP_TOP    8
#define CLIP_NEAR   16
#define CLIP_FAR    32

static int gl_outcode(gl_vertex_t *v) {
    int code = CLIP_INSIDE;
    float w = gl_abs(v->w);
    if (v->x < -w) code |= CLIP_LEFT;
    if (v->x > w)  code |= CLIP_RIGHT;
    if (v->y < -w) code |= CLIP_BOTTOM;
    if (v->y > w)  code |= CLIP_TOP;
    if (v->z < -w) code |= CLIP_NEAR;
    if (v->z > w)  code |= CLIP_FAR;
    return code;
}

static int gl_trivial_reject(gl_vertex_t *v0, gl_vertex_t *v1, gl_vertex_t *v2) {
    int c0 = gl_outcode(v0);
    int c1 = gl_outcode(v1);
    int c2 = gl_outcode(v2);
    return (c0 & c1 & c2) != 0;
}

static gl_vertex_t gl_lerp_vertex(gl_vertex_t *a, gl_vertex_t *b, float t) {
    gl_vertex_t out;
    float inv_t = 1.0f - t;
    out.x = a->x * inv_t + b->x * t;
    out.y = a->y * inv_t + b->y * t;
    out.z = a->z * inv_t + b->z * t;
    out.w = a->w * inv_t + b->w * t;
    out.u = a->u * inv_t + b->u * t;
    out.v = a->v * inv_t + b->v * t;
    out.r = a->r * inv_t + b->r * t;
    out.g = a->g * inv_t + b->g * t;
    out.b = a->b * inv_t + b->b * t;
    out.a = a->a * inv_t + b->a * t;
    return out;
}

/* Clip triangle against near plane (z = -w). Returns number of output
 * triangles (0, 1, or 2). Output vertices stored in out[] (up to 6). */
static int gl_clip_near_plane(gl_vertex_t *v0, gl_vertex_t *v1, gl_vertex_t *v2,
                              gl_vertex_t *out) {
    gl_vertex_t *verts[3] = {v0, v1, v2};
    gl_vertex_t clipped[4];
    int clipped_count = 0;

    for (int i = 0; i < 3; i++) {
        gl_vertex_t *cur = verts[i];
        gl_vertex_t *next = verts[(i + 1) % 3];

        float d_cur = cur->z + cur->w;
        float d_next = next->z + next->w;

        if (d_cur >= 0) {
            if (clipped_count < 4)
                clipped[clipped_count++] = *cur;
        }
        if ((d_cur >= 0) != (d_next >= 0)) {
            float t = d_cur / (d_cur - d_next);
            if (clipped_count < 4)
                clipped[clipped_count++] = gl_lerp_vertex(cur, next, t);
        }
    }

    if (clipped_count < 3) return 0;

    out[0] = clipped[0];
    out[1] = clipped[1];
    out[2] = clipped[2];
    if (clipped_count == 4) {
        out[3] = clipped[0];
        out[4] = clipped[2];
        out[5] = clipped[3];
        return 2;
    }
    return 1;
}

/* ======================================================================
 * Public API -- Initialization
 * ====================================================================== */

void gl_init(int width, int height, uint32_t *framebuffer) {
    memset(&g_gl, 0, sizeof(g_gl));

    g_gl.width = width;
    g_gl.height = height;
    g_gl.target_fb = framebuffer;

    int pixel_count = width * height;

    static uint32_t s_color_buf[GL_RASTER_MAX_W * GL_RASTER_MAX_H];
    static float s_depth_buf[GL_RASTER_MAX_W * GL_RASTER_MAX_H];

    if (pixel_count <= GL_RASTER_MAX_W * GL_RASTER_MAX_H) {
        g_gl.color_buf = s_color_buf;
        g_gl.depth_buf = s_depth_buf;
    } else {
        g_gl.color_buf = framebuffer;
        g_gl.depth_buf = NULL;
    }

    g_gl.vp_x = 0;
    g_gl.vp_y = 0;
    g_gl.vp_w = width;
    g_gl.vp_h = height;

    g_gl.clear_r = 0.0f;
    g_gl.clear_g = 0.0f;
    g_gl.clear_b = 0.0f;
    g_gl.clear_a = 1.0f;
    g_gl.clear_depth = 1.0f;

    g_gl.blend_sfactor = GL_SRC_ALPHA;
    g_gl.blend_dfactor = GL_ONE_MINUS_SRC_ALPHA;

    g_gl.next_tex_id = 1;
    g_gl.next_buf_id = 1;
    g_gl.next_shader_id = 1;
    g_gl.next_program_id = 1;

    g_gl.initialized = 1;
}

void gl_present(void) {
    if (!g_gl.initialized || !g_gl.target_fb || !g_gl.color_buf) return;
    if (g_gl.color_buf != g_gl.target_fb) {
        memcpy(g_gl.target_fb, g_gl.color_buf,
               (size_t)g_gl.width * g_gl.height * sizeof(uint32_t));
    }
}

/* ======================================================================
 * Public API -- State management
 * ====================================================================== */

void glClearColor(GLfloat r, GLfloat g, GLfloat b, GLfloat a) {
    g_gl.clear_r = r;
    g_gl.clear_g = g;
    g_gl.clear_b = b;
    g_gl.clear_a = a;
}

void glClear(GLbitfield mask) {
    if (!g_gl.initialized) return;

    int pixel_count = g_gl.width * g_gl.height;

    if (mask & GL_COLOR_BUFFER_BIT) {
        uint32_t c = gl_pack_color(g_gl.clear_r, g_gl.clear_g,
                                   g_gl.clear_b, g_gl.clear_a);
        for (int i = 0; i < pixel_count; i++)
            g_gl.color_buf[i] = c;
    }

    if ((mask & GL_DEPTH_BUFFER_BIT) && g_gl.depth_buf) {
        for (int i = 0; i < pixel_count; i++)
            g_gl.depth_buf[i] = g_gl.clear_depth;
    }
}

void glViewport(GLint x, GLint y, GLsizei w, GLsizei h) {
    g_gl.vp_x = x;
    g_gl.vp_y = y;
    g_gl.vp_w = w;
    g_gl.vp_h = h;
}

void glEnable(GLenum cap) {
    switch (cap) {
    case GL_BLEND: g_gl.blend_enabled = 1; break;
    case GL_DEPTH_TEST: g_gl.depth_test_enabled = 1; break;
    default: break;
    }
}

void glDisable(GLenum cap) {
    switch (cap) {
    case GL_BLEND: g_gl.blend_enabled = 0; break;
    case GL_DEPTH_TEST: g_gl.depth_test_enabled = 0; break;
    default: break;
    }
}

void glBlendFunc(GLenum sfactor, GLenum dfactor) {
    g_gl.blend_sfactor = sfactor;
    g_gl.blend_dfactor = dfactor;
}

/* ======================================================================
 * Public API -- Shaders
 * ====================================================================== */

GLuint glCreateShader(GLenum type) {
    if (g_gl.next_shader_id > GL_MAX_SHADERS) return 0;
    int id = g_gl.next_shader_id++;
    gl_shader_t *s = &g_gl.shaders[id - 1];
    memset(s, 0, sizeof(*s));
    s->type = type;
    s->valid = 1;
    return (GLuint)id;
}

void glShaderSource(GLuint shader, GLsizei count, const char **string,
                    const GLint *length) {
    if (shader == 0 || shader > GL_MAX_SHADERS) return;
    gl_shader_t *s = &g_gl.shaders[shader - 1];
    if (!s->valid) return;

    s->source[0] = '\0';
    int offset = 0;
    for (GLsizei i = 0; i < count && offset < 510; i++) {
        if (!string[i]) continue;
        int len = length ? length[i] : -1;
        if (len < 0) {
            const char *p = string[i];
            while (*p && offset < 510) s->source[offset++] = *p++;
        } else {
            for (int j = 0; j < len && offset < 510; j++)
                s->source[offset++] = string[i][j];
        }
    }
    s->source[offset] = '\0';
}

void glCompileShader(GLuint shader) {
    if (shader == 0 || shader > GL_MAX_SHADERS) return;
    gl_shader_t *s = &g_gl.shaders[shader - 1];
    if (!s->valid) return;
    s->compiled = 1;
}

void glGetShaderiv(GLuint shader, GLenum pname, GLint *params) {
    if (!params) return;
    if (shader == 0 || shader > GL_MAX_SHADERS) { *params = 0; return; }
    gl_shader_t *s = &g_gl.shaders[shader - 1];
    if (pname == GL_COMPILE_STATUS)
        *params = s->compiled ? GL_TRUE : GL_FALSE;
}

GLuint glCreateProgram(void) {
    if (g_gl.next_program_id > GL_MAX_PROGRAMS) return 0;
    int id = g_gl.next_program_id++;
    gl_program_t *p = &g_gl.programs[id - 1];
    memset(p, 0, sizeof(*p));
    p->valid = 1;
    return (GLuint)id;
}

void glAttachShader(GLuint program, GLuint shader) {
    if (program == 0 || program > GL_MAX_PROGRAMS) return;
    if (shader == 0 || shader > GL_MAX_SHADERS) return;
    gl_program_t *p = &g_gl.programs[program - 1];
    gl_shader_t *s = &g_gl.shaders[shader - 1];
    if (!p->valid || !s->valid) return;

    if (s->type == GL_VERTEX_SHADER)
        p->vs_id = (int)shader;
    else if (s->type == GL_FRAGMENT_SHADER)
        p->fs_id = (int)shader;
}

void glLinkProgram(GLuint program) {
    if (program == 0 || program > GL_MAX_PROGRAMS) return;
    gl_program_t *p = &g_gl.programs[program - 1];
    if (!p->valid) return;
    p->linked = 1;
}

void glUseProgram(GLuint program) {
    g_gl.current_program = (int)program;
}

void glDeleteShader(GLuint shader) {
    if (shader == 0 || shader > GL_MAX_SHADERS) return;
    g_gl.shaders[shader - 1].valid = 0;
}

static int gl_str_eq(const char *a, const char *b) {
    while (*a && *b) { if (*a++ != *b++) return 0; }
    return *a == *b;
}

static int gl_str_copy(char *dst, const char *src, int max) {
    int i = 0;
    while (src[i] && i < max - 1) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
    return i;
}

GLint glGetUniformLocation(GLuint program, const char *name) {
    if (program == 0 || program > GL_MAX_PROGRAMS || !name) return -1;
    gl_program_t *p = &g_gl.programs[program - 1];
    if (!p->valid) return -1;

    for (int i = 0; i < p->uniform_count; i++) {
        if (gl_str_eq(p->uniform_names[i], name))
            return i;
    }

    if (p->uniform_count >= GL_MAX_UNIFORMS) return -1;
    int idx = p->uniform_count++;
    gl_str_copy(p->uniform_names[idx], name, 64);
    memset(p->uniform_values[idx], 0, sizeof(p->uniform_values[idx]));
    return idx;
}

void glUniform1i(GLint location, GLint v0) {
    if (location < 0 || g_gl.current_program == 0) return;
    gl_program_t *p = &g_gl.programs[g_gl.current_program - 1];
    if (!p->valid || location >= p->uniform_count) return;
    p->uniform_values[location][0] = (float)v0;
}

void glUniform1f(GLint location, GLfloat v0) {
    if (location < 0 || g_gl.current_program == 0) return;
    gl_program_t *p = &g_gl.programs[g_gl.current_program - 1];
    if (!p->valid || location >= p->uniform_count) return;
    p->uniform_values[location][0] = v0;
}

void glUniform4f(GLint location, GLfloat v0, GLfloat v1, GLfloat v2, GLfloat v3) {
    if (location < 0 || g_gl.current_program == 0) return;
    gl_program_t *p = &g_gl.programs[g_gl.current_program - 1];
    if (!p->valid || location >= p->uniform_count) return;
    p->uniform_values[location][0] = v0;
    p->uniform_values[location][1] = v1;
    p->uniform_values[location][2] = v2;
    p->uniform_values[location][3] = v3;
}

void glUniformMatrix4fv(GLint location, GLsizei count, GLboolean transpose,
                        const GLfloat *value) {
    if (location < 0 || g_gl.current_program == 0 || !value) return;
    (void)count;
    gl_program_t *p = &g_gl.programs[g_gl.current_program - 1];
    if (!p->valid || location >= p->uniform_count) return;

    if (transpose) {
        for (int r = 0; r < 4; r++)
            for (int c = 0; c < 4; c++)
                p->uniform_values[location][r * 4 + c] = value[c * 4 + r];
    } else {
        memcpy(p->uniform_values[location], value, 16 * sizeof(float));
    }
}

GLint glGetAttribLocation(GLuint program, const char *name) {
    if (program == 0 || program > GL_MAX_PROGRAMS || !name) return -1;
    gl_program_t *p = &g_gl.programs[program - 1];
    if (!p->valid) return -1;

    for (int i = 0; i < p->attrib_count; i++) {
        if (gl_str_eq(p->attrib_names[i], name))
            return i;
    }

    if (p->attrib_count >= GL_MAX_ATTRIBS) return -1;
    int idx = p->attrib_count++;
    gl_str_copy(p->attrib_names[idx], name, 64);
    return idx;
}

/* ======================================================================
 * Public API -- Buffers
 * ====================================================================== */

void glGenBuffers(GLsizei n, GLuint *buffers) {
    if (!buffers) return;
    for (GLsizei i = 0; i < n; i++) {
        if (g_gl.next_buf_id > GL_MAX_BUFFERS) { buffers[i] = 0; continue; }
        int id = g_gl.next_buf_id++;
        g_gl.buffers[id - 1].valid = 1;
        g_gl.buffers[id - 1].data = NULL;
        g_gl.buffers[id - 1].size = 0;
        buffers[i] = (GLuint)id;
    }
}

void glBindBuffer(GLenum target, GLuint buffer) {
    if (target == GL_ARRAY_BUFFER)
        g_gl.bound_array_buffer = (int)buffer;
}

void glBufferData(GLenum target, GLsizei size, const GLvoid *data, GLenum usage) {
    (void)usage;
    if (target != GL_ARRAY_BUFFER) return;
    int id = g_gl.bound_array_buffer;
    if (id <= 0 || id > GL_MAX_BUFFERS) return;

    gl_buffer_t *buf = &g_gl.buffers[id - 1];

    static uint8_t s_buffer_pool[GL_MAX_BUFFERS][GL_BUF_POOL_SIZE];
    if (size > GL_BUF_POOL_SIZE) size = GL_BUF_POOL_SIZE;

    buf->data = s_buffer_pool[id - 1];
    buf->size = size;
    if (data)
        memcpy(buf->data, data, (size_t)size);
}

void glVertexAttribPointer(GLuint index, GLint size, GLenum type,
                           GLboolean normalized, GLsizei stride,
                           const GLvoid *pointer) {
    if (index >= GL_MAX_ATTRIBS) return;
    g_gl.attribs[index].size = size;
    g_gl.attribs[index].type = type;
    g_gl.attribs[index].normalized = normalized;
    g_gl.attribs[index].stride = stride;
    g_gl.attribs[index].pointer = pointer;
}

void glEnableVertexAttribArray(GLuint index) {
    if (index >= GL_MAX_ATTRIBS) return;
    g_gl.attribs[index].enabled = 1;
}

/* ======================================================================
 * Public API -- Drawing
 * ====================================================================== */

static const float *gl_get_attrib_ptr(int attrib_idx, int vertex_idx) {
    if (attrib_idx < 0 || attrib_idx >= GL_MAX_ATTRIBS) return NULL;
    gl_attrib_t *attr = &g_gl.attribs[attrib_idx];
    if (!attr->enabled) return NULL;

    int buf_id = g_gl.bound_array_buffer;
    const uint8_t *base = NULL;

    if (buf_id > 0 && buf_id <= GL_MAX_BUFFERS && g_gl.buffers[buf_id - 1].data) {
        base = (const uint8_t *)g_gl.buffers[buf_id - 1].data;
    }

    uintptr_t offset = (uintptr_t)attr->pointer;
    int stride = attr->stride;
    if (stride == 0)
        stride = attr->size * (int)sizeof(float);

    const uint8_t *ptr = base + offset + (uintptr_t)(vertex_idx * stride);
    return (const float *)ptr;
}

void glDrawArrays(GLenum mode, GLint first, GLsizei count) {
    if (!g_gl.initialized || count <= 0) return;

    /* Find the MVP matrix uniform (look for "u_mvp" or use first mat4 uniform) */
    const float *mvp = NULL;
    if (g_gl.current_program > 0 && g_gl.current_program <= GL_MAX_PROGRAMS) {
        gl_program_t *prog = &g_gl.programs[g_gl.current_program - 1];
        if (prog->valid) {
            for (int i = 0; i < prog->uniform_count; i++) {
                if (gl_str_eq(prog->uniform_names[i], "u_mvp") ||
                    gl_str_eq(prog->uniform_names[i], "mvp") ||
                    gl_str_eq(prog->uniform_names[i], "u_matrix")) {
                    mvp = prog->uniform_values[i];
                    break;
                }
            }
        }
    }

    /* Determine which attribs are position, color, texcoord */
    int pos_attr = 0;
    int color_attr = -1;
    int tex_attr = -1;

    if (g_gl.current_program > 0 && g_gl.current_program <= GL_MAX_PROGRAMS) {
        gl_program_t *prog = &g_gl.programs[g_gl.current_program - 1];
        if (prog->valid) {
            for (int i = 0; i < prog->attrib_count; i++) {
                if (gl_str_eq(prog->attrib_names[i], "a_position") ||
                    gl_str_eq(prog->attrib_names[i], "position") ||
                    gl_str_eq(prog->attrib_names[i], "a_pos"))
                    pos_attr = i;
                else if (gl_str_eq(prog->attrib_names[i], "a_color") ||
                         gl_str_eq(prog->attrib_names[i], "color"))
                    color_attr = i;
                else if (gl_str_eq(prog->attrib_names[i], "a_texcoord") ||
                         gl_str_eq(prog->attrib_names[i], "texcoord") ||
                         gl_str_eq(prog->attrib_names[i], "a_uv"))
                    tex_attr = i;
            }
        }
    }

    /* If no named attribs, use a heuristic based on enabled attribs */
    if (color_attr < 0 && g_gl.attribs[1].enabled) color_attr = 1;
    if (tex_attr < 0 && g_gl.attribs[2].enabled) tex_attr = 2;

    if (mode == GL_TRIANGLES) {
        for (GLsizei i = 0; i + 2 < count; i += 3) {
            int idx0 = first + i;
            int idx1 = first + i + 1;
            int idx2 = first + i + 2;

            const float *p0 = gl_get_attrib_ptr(pos_attr, idx0);
            const float *p1 = gl_get_attrib_ptr(pos_attr, idx1);
            const float *p2 = gl_get_attrib_ptr(pos_attr, idx2);
            if (!p0 || !p1 || !p2) continue;

            const float *c0 = color_attr >= 0 ? gl_get_attrib_ptr(color_attr, idx0) : NULL;
            const float *c1 = color_attr >= 0 ? gl_get_attrib_ptr(color_attr, idx1) : NULL;
            const float *c2 = color_attr >= 0 ? gl_get_attrib_ptr(color_attr, idx2) : NULL;

            const float *t0 = tex_attr >= 0 ? gl_get_attrib_ptr(tex_attr, idx0) : NULL;
            const float *t1 = tex_attr >= 0 ? gl_get_attrib_ptr(tex_attr, idx1) : NULL;
            const float *t2 = tex_attr >= 0 ? gl_get_attrib_ptr(tex_attr, idx2) : NULL;

            int pos_size = g_gl.attribs[pos_attr].size;
            int col_size = color_attr >= 0 ? g_gl.attribs[color_attr].size : 0;
            int tex_size = tex_attr >= 0 ? g_gl.attribs[tex_attr].size : 0;

            gl_vertex_t v0, v1, v2;
            gl_process_vertex(p0, pos_size, c0, col_size, t0, tex_size, mvp, &v0);
            gl_process_vertex(p1, pos_size, c1, col_size, t1, tex_size, mvp, &v1);
            gl_process_vertex(p2, pos_size, c2, col_size, t2, tex_size, mvp, &v2);

            if (gl_trivial_reject(&v0, &v1, &v2)) continue;

            gl_vertex_t clipped[6];
            int num_tris = gl_clip_near_plane(&v0, &v1, &v2, clipped);
            for (int t = 0; t < num_tris; t++) {
                gl_rasterize_triangle(&clipped[t*3], &clipped[t*3+1], &clipped[t*3+2]);
            }
        }
    } else if (mode == GL_TRIANGLE_STRIP) {
        for (GLsizei i = 0; i + 2 < count; i++) {
            int idx0, idx1, idx2;
            if (i % 2 == 0) {
                idx0 = first + i;
                idx1 = first + i + 1;
                idx2 = first + i + 2;
            } else {
                idx0 = first + i + 1;
                idx1 = first + i;
                idx2 = first + i + 2;
            }

            const float *p0 = gl_get_attrib_ptr(pos_attr, idx0);
            const float *p1 = gl_get_attrib_ptr(pos_attr, idx1);
            const float *p2 = gl_get_attrib_ptr(pos_attr, idx2);
            if (!p0 || !p1 || !p2) continue;

            const float *c0 = color_attr >= 0 ? gl_get_attrib_ptr(color_attr, idx0) : NULL;
            const float *c1 = color_attr >= 0 ? gl_get_attrib_ptr(color_attr, idx1) : NULL;
            const float *c2 = color_attr >= 0 ? gl_get_attrib_ptr(color_attr, idx2) : NULL;

            const float *t0 = tex_attr >= 0 ? gl_get_attrib_ptr(tex_attr, idx0) : NULL;
            const float *t1 = tex_attr >= 0 ? gl_get_attrib_ptr(tex_attr, idx1) : NULL;
            const float *t2 = tex_attr >= 0 ? gl_get_attrib_ptr(tex_attr, idx2) : NULL;

            int pos_size = g_gl.attribs[pos_attr].size;
            int col_size = color_attr >= 0 ? g_gl.attribs[color_attr].size : 0;
            int tex_size = tex_attr >= 0 ? g_gl.attribs[tex_attr].size : 0;

            gl_vertex_t v0, v1, v2;
            gl_process_vertex(p0, pos_size, c0, col_size, t0, tex_size, mvp, &v0);
            gl_process_vertex(p1, pos_size, c1, col_size, t1, tex_size, mvp, &v1);
            gl_process_vertex(p2, pos_size, c2, col_size, t2, tex_size, mvp, &v2);

            if (gl_trivial_reject(&v0, &v1, &v2)) continue;

            gl_vertex_t clipped[6];
            int num_tris = gl_clip_near_plane(&v0, &v1, &v2, clipped);
            for (int t = 0; t < num_tris; t++) {
                gl_rasterize_triangle(&clipped[t*3], &clipped[t*3+1], &clipped[t*3+2]);
            }
        }
    } else if (mode == GL_TRIANGLE_FAN) {
        for (GLsizei i = 1; i + 1 < count; i++) {
            int idx0 = first;
            int idx1 = first + i;
            int idx2 = first + i + 1;

            const float *p0 = gl_get_attrib_ptr(pos_attr, idx0);
            const float *p1 = gl_get_attrib_ptr(pos_attr, idx1);
            const float *p2 = gl_get_attrib_ptr(pos_attr, idx2);
            if (!p0 || !p1 || !p2) continue;

            const float *c0 = color_attr >= 0 ? gl_get_attrib_ptr(color_attr, idx0) : NULL;
            const float *c1 = color_attr >= 0 ? gl_get_attrib_ptr(color_attr, idx1) : NULL;
            const float *c2 = color_attr >= 0 ? gl_get_attrib_ptr(color_attr, idx2) : NULL;

            const float *t0 = tex_attr >= 0 ? gl_get_attrib_ptr(tex_attr, idx0) : NULL;
            const float *t1 = tex_attr >= 0 ? gl_get_attrib_ptr(tex_attr, idx1) : NULL;
            const float *t2 = tex_attr >= 0 ? gl_get_attrib_ptr(tex_attr, idx2) : NULL;

            int pos_size = g_gl.attribs[pos_attr].size;
            int col_size = color_attr >= 0 ? g_gl.attribs[color_attr].size : 0;
            int tex_size = tex_attr >= 0 ? g_gl.attribs[tex_attr].size : 0;

            gl_vertex_t v0, v1, v2;
            gl_process_vertex(p0, pos_size, c0, col_size, t0, tex_size, mvp, &v0);
            gl_process_vertex(p1, pos_size, c1, col_size, t1, tex_size, mvp, &v1);
            gl_process_vertex(p2, pos_size, c2, col_size, t2, tex_size, mvp, &v2);

            if (gl_trivial_reject(&v0, &v1, &v2)) continue;

            gl_vertex_t clipped[6];
            int num_tris = gl_clip_near_plane(&v0, &v1, &v2, clipped);
            for (int t = 0; t < num_tris; t++) {
                gl_rasterize_triangle(&clipped[t*3], &clipped[t*3+1], &clipped[t*3+2]);
            }
        }
    } else if (mode == GL_LINES) {
        for (GLsizei i = 0; i + 1 < count; i += 2) {
            int idx0 = first + i;
            int idx1 = first + i + 1;

            const float *p0 = gl_get_attrib_ptr(pos_attr, idx0);
            const float *p1 = gl_get_attrib_ptr(pos_attr, idx1);
            if (!p0 || !p1) continue;

            const float *c0 = color_attr >= 0 ? gl_get_attrib_ptr(color_attr, idx0) : NULL;
            const float *c1 = color_attr >= 0 ? gl_get_attrib_ptr(color_attr, idx1) : NULL;

            int pos_size = g_gl.attribs[pos_attr].size;
            int col_size = color_attr >= 0 ? g_gl.attribs[color_attr].size : 0;

            gl_vertex_t v0, v1;
            gl_process_vertex(p0, pos_size, c0, col_size, NULL, 0, mvp, &v0);
            gl_process_vertex(p1, pos_size, c1, col_size, NULL, 0, mvp, &v1);

            gl_rasterize_line(&v0, &v1);
        }
    } else if (mode == GL_LINE_STRIP) {
        for (GLsizei i = 0; i + 1 < count; i++) {
            int idx0 = first + i;
            int idx1 = first + i + 1;

            const float *p0 = gl_get_attrib_ptr(pos_attr, idx0);
            const float *p1 = gl_get_attrib_ptr(pos_attr, idx1);
            if (!p0 || !p1) continue;

            const float *c0 = color_attr >= 0 ? gl_get_attrib_ptr(color_attr, idx0) : NULL;
            const float *c1 = color_attr >= 0 ? gl_get_attrib_ptr(color_attr, idx1) : NULL;

            int pos_size = g_gl.attribs[pos_attr].size;
            int col_size = color_attr >= 0 ? g_gl.attribs[color_attr].size : 0;

            gl_vertex_t v0, v1;
            gl_process_vertex(p0, pos_size, c0, col_size, NULL, 0, mvp, &v0);
            gl_process_vertex(p1, pos_size, c1, col_size, NULL, 0, mvp, &v1);

            gl_rasterize_line(&v0, &v1);
        }
    } else if (mode == GL_POINTS) {
        for (GLsizei i = 0; i < count; i++) {
            int idx = first + i;

            const float *p = gl_get_attrib_ptr(pos_attr, idx);
            if (!p) continue;

            const float *c = color_attr >= 0 ? gl_get_attrib_ptr(color_attr, idx) : NULL;

            int pos_size = g_gl.attribs[pos_attr].size;
            int col_size = color_attr >= 0 ? g_gl.attribs[color_attr].size : 0;

            gl_vertex_t v;
            gl_process_vertex(p, pos_size, c, col_size, NULL, 0, mvp, &v);
            gl_rasterize_point(&v);
        }
    }
}

/* ======================================================================
 * Public API -- Textures
 * ====================================================================== */

void glGenTextures(GLsizei n, GLuint *textures) {
    if (!textures) return;
    for (GLsizei i = 0; i < n; i++) {
        if (g_gl.next_tex_id > GL_MAX_TEXTURES) { textures[i] = 0; continue; }
        int id = g_gl.next_tex_id++;
        gl_texture_t *t = &g_gl.textures[id - 1];
        memset(t, 0, sizeof(*t));
        t->min_filter = GL_NEAREST;
        t->mag_filter = GL_NEAREST;
        t->wrap_s = GL_REPEAT;
        t->wrap_t = GL_REPEAT;
        t->valid = 1;
        textures[i] = (GLuint)id;
    }
}

void glBindTexture(GLenum target, GLuint texture) {
    (void)target;
    g_gl.bound_texture = (int)texture;
}

void glTexImage2D(GLenum target, GLint level, GLint internalformat,
                  GLsizei width, GLsizei height, GLint border,
                  GLenum format, GLenum type, const GLvoid *pixels) {
    (void)target; (void)level; (void)internalformat; (void)border;

    if (g_gl.bound_texture <= 0 || g_gl.bound_texture > GL_MAX_TEXTURES) return;
    gl_texture_t *tex = &g_gl.textures[g_gl.bound_texture - 1];
    if (!tex->valid) return;

    if (width > GL_MAX_TEX_SIZE) width = GL_MAX_TEX_SIZE;
    if (height > GL_MAX_TEX_SIZE) height = GL_MAX_TEX_SIZE;

    /* Static texture storage pool */
    static uint32_t s_tex_pool[GL_MAX_TEXTURES][GL_MAX_TEX_SIZE * GL_MAX_TEX_SIZE / GL_MAX_TEXTURES];
    int pool_size = (GL_MAX_TEX_SIZE * GL_MAX_TEX_SIZE) / GL_MAX_TEXTURES;

    if (width * height > pool_size) return;

    tex->data = s_tex_pool[g_gl.bound_texture - 1];
    tex->width = width;
    tex->height = height;

    if (!pixels) {
        memset(tex->data, 0, (size_t)width * height * 4);
        return;
    }

    if (format == GL_RGBA && type == GL_UNSIGNED_BYTE) {
        const uint8_t *src = (const uint8_t *)pixels;
        for (int i = 0; i < width * height; i++) {
            uint8_t r = src[i * 4 + 0];
            uint8_t g = src[i * 4 + 1];
            uint8_t b = src[i * 4 + 2];
            uint8_t a = src[i * 4 + 3];
            tex->data[i] = ((uint32_t)a << 24) | ((uint32_t)r << 16) |
                           ((uint32_t)g << 8) | (uint32_t)b;
        }
    } else if (format == GL_RGB && type == GL_UNSIGNED_BYTE) {
        const uint8_t *src = (const uint8_t *)pixels;
        for (int i = 0; i < width * height; i++) {
            uint8_t r = src[i * 3 + 0];
            uint8_t g = src[i * 3 + 1];
            uint8_t b = src[i * 3 + 2];
            tex->data[i] = (0xFFu << 24) | ((uint32_t)r << 16) |
                           ((uint32_t)g << 8) | (uint32_t)b;
        }
    } else {
        memcpy(tex->data, pixels, (size_t)width * height * 4);
    }
}

void glTexParameteri(GLenum target, GLenum pname, GLint param) {
    (void)target;
    if (g_gl.bound_texture <= 0 || g_gl.bound_texture > GL_MAX_TEXTURES) return;
    gl_texture_t *tex = &g_gl.textures[g_gl.bound_texture - 1];
    if (!tex->valid) return;

    switch (pname) {
    case GL_TEXTURE_MIN_FILTER: tex->min_filter = param; break;
    case GL_TEXTURE_MAG_FILTER: tex->mag_filter = param; break;
    case GL_TEXTURE_WRAP_S: tex->wrap_s = param; break;
    case GL_TEXTURE_WRAP_T: tex->wrap_t = param; break;
    default: break;
    }
}

void glActiveTexture(GLenum texture) {
    g_gl.active_texture_unit = (int)(texture - GL_TEXTURE0);
}

/* ======================================================================
 * Public API -- Framebuffer readback
 * ====================================================================== */

void glReadPixels(GLint x, GLint y, GLsizei width, GLsizei height,
                  GLenum format, GLenum type, GLvoid *pixels) {
    if (!g_gl.initialized || !g_gl.color_buf || !pixels) return;
    if (format != GL_RGBA || type != GL_UNSIGNED_BYTE) return;

    uint8_t *dst = (uint8_t *)pixels;
    for (GLsizei row = 0; row < height; row++) {
        int sy = y + row;
        if (sy < 0 || sy >= g_gl.height) {
            memset(dst + row * width * 4, 0, (size_t)width * 4);
            continue;
        }
        for (GLsizei col = 0; col < width; col++) {
            int sx = x + col;
            uint32_t pixel = 0;
            if (sx >= 0 && sx < g_gl.width)
                pixel = g_gl.color_buf[sy * g_gl.width + sx];

            int offset = (row * width + col) * 4;
            dst[offset + 0] = (uint8_t)((pixel >> 16) & 0xFF);
            dst[offset + 1] = (uint8_t)((pixel >> 8) & 0xFF);
            dst[offset + 2] = (uint8_t)(pixel & 0xFF);
            dst[offset + 3] = (uint8_t)((pixel >> 24) & 0xFF);
        }
    }
}
