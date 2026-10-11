/*
 * hle_d3d8_vertex.c -- vertex buffer draws and vertex shader constants,
 * forwarded to the shadow device.
 *
 * Part of shadow mode (hle_d3d8.c, RECOMP_HLE_D3D8=shadow). Every replacement
 * runs the title's own body first.
 *
 * A vertex buffer is a guest X_D3DVertexBuffer (Common, Data, Lock): Data is
 * physical, so the vertices are read at guest 0x80000000 + Data, like texels
 * (Cxbx-Reloaded, GetDataFromXboxResource). Rather than mirror buffers on the
 * host, each draw hands the vertices it needs to the host's UP draws, the same
 * path DrawVerticesUP takes (hle_d3d8.c).
 *
 * DrawIndexedVertices takes a pointer to 16-bit indices and adds the base
 * vertex index set with SetIndices (Cxbx-Reloaded, Direct3D9.cpp). Burnout 2's
 * XDK symbols do not name SetIndices, but every DrawIndexedVertices calls
 * CDevice_SetStateVB with it (7,188 of each in a profiled minute): the lifted
 * DrawIndexedVertices pushes the device's base index, and SetStateVB multiplies
 * its argument by the stride and adds the buffer's Data when it sets the
 * vertex array offsets. So the base is captured there. A title whose symbols
 * do not name that function -- or name only the stdcall CDevice_SetStateVB_8,
 * `this` on the stack -- never updates it; the first indexed buffer draw
 * without it says so once, and draws with base 0.
 *
 * The host binds one stream, stream 0. A vertex program that reads another
 * one -- Outrun 2's road takes v13 from stream 1 -- has those registers copied
 * in beside each stream 0 vertex (shadow_expand_vertices in hle_d3d8.c),
 * which finds them through hle_d3d8_stream_vertices below at the first
 * vertex each buffer draw passes along. FVF draws use only stream 0.
 *
 * Vertex shader constants arrive through fastcall setters: register in ecx,
 * data in edx, and for the NotInline pair a count of floats on the stack.
 * The register is already 0..191, the host's range; Cxbx-Reloaded subtracts
 * 96 on the way in only because its shared setter adds it back.
 *
 * XDK 3925 has only that shared setter, D3DDevice_SetVertexShaderConstant,
 * and it is the exception to the line above: its register is -96-based and
 * the XDK biases it itself, so the replacement at the bottom of this file
 * adds 96 to reach the same range the fastcall ones already arrive in.
 */
#include "platform/xbox_winnt.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "hle.h"

#include "d3d8_xbox.h"
#include "d3d8_vsh.h"
#include "hle_d3d8_record.h"

/* From hle_d3d8.c. */
IDirect3DDevice8 *hle_d3d8_shadow_device(void);
void hle_d3d8_shadow_draw(uint32_t xpt, uint32_t count, const void *verts,
                          uint32_t stride, int from_buffer);
void hle_d3d8_shadow_draw_indexed(uint32_t xpt, uint32_t count, const uint16_t *idx,
                                  const void *verts, uint32_t stride, int from_buffer);
void hle_d3d8_shadow_set_first_vertex(uint32_t first);

#define CONTIG_BASE 0x80000000u
#define CONTIG_SIZE (64u * 1024u * 1024u)    /* kernel.h XBOX_CONTIG_SIZE */
#define MAX_CONSTANT_REGISTERS 192

static uint32_t g_stream0_vb;        /* guest X_D3DVertexBuffer */
static uint32_t g_stream0_stride;
static uint32_t g_stream_vb[16], g_stream_stride[16];   /* every stream, 0 included */
static uint32_t g_base_vertex;
static int      g_base_vertex_seen;  /* CDevice_SetStateVB has run */
static int      g_in_notinline;      /* inside SetVertexShaderConstantNotInline */
static unsigned long g_skip_no_stream, g_skip_range;

/* Vertex arrays the title programs itself, through BeginPush and EndPush.
 *
 * NV097_SET_VERTEX_DATA_ARRAY_FORMAT (0x1760 + 4i) gives attribute i its
 * format and its own stride, NV097_SET_VERTEX_DATA_ARRAY_OFFSET (0x1720 + 4i)
 * its own physical address. The XDK writes both from its stream and
 * declaration state in CDevice_SetStateVB. A title may push its own instead
 * and then call DrawVertices or DrawIndexedVertices: TimeSplitters: Future
 * Perfect draws its models that way, every attribute in an array of its own,
 * after selecting a declaration that matches. Stream 0 then describes
 * whatever the last XDK-path draw used, and reading the draw's vertices from
 * it gave a 24-byte vertex buffer to 28- to 36-byte layouts -- every model in
 * the 2401 intro cutscene skipped as "stride", or drawn as shards.
 *
 * BeginPush runs CDevice_SetStateVB first, so the XDK's own arrays are
 * written before the title's and the title's are what the GPU uses until the
 * XDK has reason to write its own again: a new declaration
 * (hle_d3d8_push_arrays_off, from the vertex shader selection in hle_d3d8.c)
 * or a new stream. Until then each draw is gathered from the pushed arrays
 * (push_gather) into one vertex laid out as the selected declaration says. */
static uint32_t g_push_fmt[16], g_push_off[16];
static uint32_t g_push_known;          /* attributes pushed since the last reset */
static int      g_push_active;
static unsigned long g_push_draws, g_push_failed;

uint32_t hle_d3d8_shadow_program_object(void);   /* hle_d3d8.c */
static int guest_readable(uint32_t va, uint64_t bytes);

void hle_d3d8_push_arrays_off(void)
{
    g_push_active = 0;
    g_push_known = 0;
}

/* Bytes of one X_D3DVSDT element: the low nibble is the type, the high one
 * the component count (FLOAT 2, D3DCOLOR 0, NORMSHORT 1, PBYTE 4, SHORT 5,
 * NORMPACKED3 6). 0 for a format this cannot size. */
static UINT vsdt_bytes(uint32_t format)
{
    UINT count = (format >> 4) & 0xFu;

    if (format == 0x72u)                 /* FLOAT2H: three floats */
        return 12u;
    switch (format & 0xFu) {
    case 2:  return 4u * count;
    case 0:
    case 4:  return count;
    case 1:
    case 5:  return 2u * count;
    case 6:  return 4u;
    default: return 0u;
    }
}

/* The commands between BeginPush's pointer and EndPush's: only the vertex
 * array formats and offsets are kept. Stops at anything that is not a method
 * header (a jump has no business inside one push). */
static void push_scan(uint32_t start, uint32_t end)
{
    uint32_t va = start, pushed = 0;

    while (va + 4u <= end) {
        uint32_t w = HLE_MEM32(va);
        uint32_t count, method, i;
        int noninc;

        va += 4u;
        if ((w & 0xFFFF0003u) == 0x00020000u || (w & 3u) == 2u)
            continue;                    /* return, call */
        if ((w & 0x00030003u) != 0u || (w & 0xE0000000u) == 0x20000000u)
            break;                       /* a jump, or not a command */
        count = (w >> 18) & 0x7FFu;
        method = w & 0x1FFCu;
        noninc = (w & 0xE0000000u) == 0x40000000u;
        for (i = 0; i < count && va + 4u <= end; i++, va += 4u) {
            uint32_t m = noninc ? method : method + i * 4u;
            uint32_t value = HLE_MEM32(va);

            if (m >= 0x1720u && m < 0x1760u) {
                g_push_off[(m - 0x1720u) / 4u] = value;
                pushed |= 1u << ((m - 0x1720u) / 4u);
            } else if (m >= 0x1760u && m < 0x17A0u) {
                g_push_fmt[(m - 0x1760u) / 4u] = value;
                pushed |= 1u << ((m - 0x1760u) / 4u);
            }
        }
    }
    if (pushed) {
        static int said;
        g_push_known |= pushed;
        g_push_active = 1;
        if (!said++)
            fprintf(stderr, "[HLE-D3D8] the title pushes its own vertex arrays (BeginPush, "
                    "attributes 0x%04X); its draws are gathered from them until the next "
                    "declaration or stream\n", pushed);
    }
}

/* hle_d3d8.c's D3DDevice_EndPush hands every push here: the commands from
 * where the device's push pointer still points (what BeginPush handed out;
 * BeginStateBig does not advance it) to where EndPush takes it back. */
void hle_d3d8_push_arrays_scan(uint32_t start, uint32_t end)
{
    if (hle_d3d8_shadow_device() && start && end > start && end - start <= 0x10000u &&
        guest_readable(start, end - start))
        push_scan(start, end);
}

/* The vertices the GPU fetches for indices first .. first + vertices - 1 when
 * the title's pushed arrays are in force: every register of the selected
 * declaration read from its own array (address + index * stride) and laid
 * out at its declared offset, so the rest of the draw path sees an ordinary
 * stream 0 vertex. A register no push described stays zero. NULL (counted)
 * when there is no declaration to lay them out by. */
/* Inline vertices (SET_BEGIN_END ... INLINE_ARRAY ...) gathered the same way:
 * `inl` holds them packed, every enabled attribute in order. NULL for arrays
 * in guest memory. */
static const uint8_t *g_gather_inline;
static uint32_t       g_gather_inline_bytes;

static uint8_t *push_gather_from(const uint32_t off[16], const uint32_t fmt[16],
                                 uint32_t known, uint32_t first, uint32_t vertices,
                                 uint32_t *stride);

static uint8_t *push_gather(uint32_t first, uint32_t vertices, uint32_t *stride)
{
    return push_gather_from(g_push_off, g_push_fmt, g_push_known, first, vertices, stride);
}

static uint8_t *push_gather_from(const uint32_t off[16], const uint32_t fmt[16],
                                 uint32_t known, uint32_t first, uint32_t vertices,
                                 uint32_t *stride)
{
    uint32_t object = hle_d3d8_shadow_program_object(), extent = 0, i, v;
    uint32_t inl_off[16], inl_size = 0;
    uint8_t *out;

    if (!object || !vertices || !guest_readable(object, 20u + 16u * 16u))
        return NULL;
    for (i = 0; i < 16u; i++) {
        uint32_t attr = object + 20u + i * 16u, format = HLE_MEM32(attr + 8u);
        UINT size;

        if (format <= 0x02u)
            continue;
        if (!(size = vsdt_bytes(format)))
            return NULL;
        if (HLE_MEM32(attr + 4u) + size > extent)
            extent = HLE_MEM32(attr + 4u) + size;
    }
    if (!extent || extent > 256u)
        return NULL;
    if (g_gather_inline) {
        /* Inline: each enabled attribute packed after the previous one. */
        for (i = 0; i < 16u; i++) {
            inl_off[i] = inl_size;
            if ((known & (1u << i)) && (fmt[i] & 0xFFu) > 0x02u)
                inl_size += vsdt_bytes(fmt[i] & 0xFFu);
        }
        if (!inl_size || (uint64_t)(first + vertices) * inl_size > g_gather_inline_bytes)
            return NULL;
    }
    out = calloc((size_t)vertices, extent);
    if (!out)
        return NULL;
    for (i = 0; i < 16u; i++) {
        uint32_t attr = object + 20u + i * 16u, format = HLE_MEM32(attr + 8u);
        uint32_t offset = HLE_MEM32(attr + 4u), pushed = fmt[i];
        uint32_t pstride = pushed >> 8, va = (off[i] & 0x7FFFFFFFu) | CONTIG_BASE;
        UINT size, psize;

        if (format <= 0x02u || !(known & (1u << i)) || (pushed & 0xFFu) <= 0x02u)
            continue;
        size = vsdt_bytes(format);
        psize = vsdt_bytes(pushed & 0xFFu);
        if (psize && psize < size)
            size = psize;
        if ((pushed & 0xFFu) != format) {
            static int said;
            if (said++ < 4)
                fprintf(stderr, "[HLE-D3D8] pushed vertex array %u is format 0x%02X, the "
                        "declaration says 0x%02X; read as the declaration\n",
                        i, pushed & 0xFFu, format);
        }
        if (g_gather_inline) {
            for (v = 0; v < vertices; v++)
                memcpy(out + (size_t)v * extent + offset,
                       g_gather_inline + (size_t)(first + v) * inl_size + inl_off[i], size);
            continue;
        }
        if (!guest_readable(va, (uint64_t)(first + vertices - 1u) * pstride + size)) {
            g_push_failed++;
            continue;
        }
        for (v = 0; v < vertices; v++)
            memcpy(out + (size_t)v * extent + offset,
                   HLE_PTR(va + (first + v) * pstride), size);
    }
    *stride = extent;
    return out;
}

/* ------------------------------------------------------------------------
 * Vertex shader constants the title writes into the push buffer itself.
 *
 * Need for Speed Underground 2 never calls a SetVertexShaderConstant for its
 * object transforms: its renderer (0x000A2EA7 and the code around it) takes
 * the device's push pointer, writes SET_TRANSFORM_CONSTANT_LOAD (0x1EA4) and
 * SET_TRANSFORM_CONSTANT (0x0B80) with the matrices, and moves the pointer
 * on. Nothing executes the push buffer here, so every object was drawn with
 * whatever c96..c99 last held -- the front end's 2D identity -- and the race
 * came out as triangles stretched across the screen.
 *
 * So before each draw the push buffer is walked from where the last walk
 * stopped to the device's put pointer (the first word of the device, as
 * D3DDevice_BeginStateBig reads it), following jumps, one level of calls and
 * returns, and the constant loads in it are applied in order. That is what
 * the NV2A would have loaded by the time it reached the draw, whoever wrote
 * the commands -- the XDK's own SetVertexShaderConstant writes are in there
 * too, and replaying them again in order changes nothing. The viewport pair
 * (58, 59) is left to shadow mode, which keeps its own. Anything that is not
 * a command header ends the walk and resynchronises at the put pointer.
 * RECOMP_HLE_D3D8_PB_CONSTANTS=0 turns it off.
 * ------------------------------------------------------------------------ */
HLE_IMPORT_VAR(D3D_g_pDevice);
int xbox_EnvSwitch(const char *name, int default_on);   /* xbox_memory_layout.c */

#define PB_CONST_LOAD   0x1EA4u
#define PB_CONST_FIRST  0x0B80u
#define PB_CONST_END    0x0C00u
#define PB_MAX_WORDS    (4u * 1024u * 1024u)

static uint32_t g_pb_scan;              /* next unscanned word (VA); 0 = resync */
static uint32_t g_pb_load;              /* the NV2A's constant load pointer */
static float    g_pb_cur[4];
static float    g_pb_run[MAX_CONSTANT_REGISTERS][4];
static int      g_pb_run_first = -1, g_pb_run_count;
static unsigned long g_pb_constants, g_pb_resyncs, g_pb_walks;

static void pb_flush(void)
{
    if (g_pb_run_first >= 0 && g_pb_run_count > 0)
        host_vsh_set_constant(g_pb_run_first, &g_pb_run[0][0], g_pb_run_count);
    g_pb_run_first = -1;
    g_pb_run_count = 0;
}

static void pb_constant(uint32_t reg, const float v[4])
{
    if (reg >= MAX_CONSTANT_REGISTERS || reg == 58u || reg == 59u) {
        pb_flush();
        return;
    }
    if (g_pb_run_first < 0 || (uint32_t)(g_pb_run_first + g_pb_run_count) != reg ||
        g_pb_run_count >= MAX_CONSTANT_REGISTERS) {
        pb_flush();
        g_pb_run_first = (int)reg;
    }
    memcpy(g_pb_run[g_pb_run_count++], v, sizeof g_pb_run[0]);
    g_pb_constants++;
}

static void pb_method(uint32_t method, uint32_t value)
{
    if (method == PB_CONST_LOAD) {
        g_pb_load = value;
    } else if (method >= PB_CONST_FIRST && method < PB_CONST_END) {
        uint32_t comp = ((method - PB_CONST_FIRST) / 4u) & 3u;

        memcpy(&g_pb_cur[comp], &value, sizeof value);
        if (comp == 3u)
            pb_constant(g_pb_load++, g_pb_cur);
    }
}

static int pb_on(void)
{
    static int on = -1;
    if (on < 0)
        on = xbox_EnvSwitch("RECOMP_HLE_D3D8_PB_CONSTANTS", 1);
    return on;
}

/* Immediate-mode vertices the title writes into the push buffer itself
 * (hle_d3d8.c, hle_d3d8_pb_inline_method): SET_BEGIN_END and the
 * SET_VERTEX_DATA* family, 0x1880-0x1AFF. Halo 2's LTCG build inlines
 * D3DDevice_Begin, SetVertexData2f and End at every use, so its loading screen
 * and movie quads exist nowhere but here. */
#define PB_BEGIN_END        0x17FCu
#define PB_VDATA_FIRST      0x1880u
#define PB_VDATA_END        0x1B00u
int  hle_d3d8_pb_inline_on(void);
void hle_d3d8_pb_inline_method(uint32_t method, uint32_t value);

/* ...and a stage's texture, SET_TEXTURE_OFFSET / _FORMAT (0x1B00 and 0x1B04,
 * 64 bytes a stage, four stages), which the same build writes inline too. */
#define PB_TEX_FIRST        0x1B00u
#define PB_TEX_END          0x1C00u

/* ...and array draws: SET_VERTEX_DATA_ARRAY_OFFSET / _FORMAT (0x1720, 0x1760,
 * 16 each) and, inside a begin/end pair, ARRAY_ELEMENT16 (0x1800, two indices a
 * word), ARRAY_ELEMENT32 (0x1808), DRAW_ARRAYS (0x1810: count-1 in the top
 * byte, the first vertex below) and INLINE_ARRAY (0x1818, packed vertices).
 * Halo 2's menu draws its 3D scene that way: ~150 million index words in
 * 100 s, none of them from a call this file replaces. */
#define PB_ARRAYS_FIRST     0x1720u
#define PB_ARRAYS_END       0x17A0u
#define PB_ELEM16           0x1800u
#define PB_ELEM32           0x1808u
#define PB_DRAW_ARRAYS      0x1810u
#define PB_INLINE_ARRAY     0x1818u

static int pb_array_method(uint32_t method)
{
    return (method >= PB_ARRAYS_FIRST && method < PB_ARRAYS_END) ||
           method == PB_ELEM16 || method == PB_ELEM32 ||
           method == PB_DRAW_ARRAYS || method == PB_INLINE_ARRAY;
}

/* ...and the simple render states (depth, blend, colour mask, stencil:
 * 0x0300-0x0388 and SWATHWIDTH 0x09F8), stored in the render-state array as
 * the replaced SetRenderState_Simple stores them (hle_d3d8_state.c) -- for an
 * LTCG title only, which writes them inline at every use. Elsewhere the same
 * methods also carry the XDK's own transient changes (a colour mask, depth
 * mask or polygon offset set and put back around an internal operation),
 * which are hardware state, not the title's: stored in the array the XDK
 * itself reads, they broke TimeSplitters 2's front end. */
int hle_d3d8_state_store_simple(uint32_t method, uint32_t value);
int  hle_d3d8_title_ltcg(void);
/* ...and the title's own clears: SET_ZSTENCIL_CLEAR_VALUE (0x1D8C),
 * SET_COLOR_CLEAR_VALUE (0x1D90), CLEAR_SURFACE (0x1D94, which clears) and
 * SET_CLEAR_RECT_HORIZONTAL / _VERTICAL (0x1D98, 0x1D9C). */
void hle_d3d8_pb_clear(uint32_t surface, uint32_t color, uint32_t zstencil,
                       uint32_t horizontal, uint32_t vertical);
#define PB_CLEAR_FIRST      0x1D8Cu
#define PB_CLEAR_END        0x1DA0u
#define PB_CLEAR_SURFACE    0x1D94u
static uint32_t g_clear_reg[5];

/* ...and the render-target surface, SET_SURFACE_COLOR_OFFSET (0x0210) and
 * SET_SURFACE_ZETA_OFFSET (0x0214), for a depth switch the title made
 * without a call this file replaces (hle_d3d8_pb_surface). */
void hle_d3d8_pb_surface(uint32_t color, uint32_t zeta);
#define PB_SURFACE_COLOR    0x0210u
#define PB_SURFACE_ZETA     0x0214u
static uint32_t g_surface_color;

/* RECOMP_PB_SURFACE_TRACE=<first>-<last>: the surface (0x0200-0x021C) and
 * clear methods the walk passes over in those swaps, in order. For telling
 * which depth surface a clear reached on the console. */
unsigned long hle_d3d8_shadow_swaps(void);
static int pb_surface_trace(void)
{
    static int parsed;
    static unsigned long lo = 1, hi = 0;
    unsigned long now;

    if (!parsed) {
        const char *e = getenv("RECOMP_PB_SURFACE_TRACE");
        parsed = 1;
        if (e && sscanf(e, "%lu-%lu", &lo, &hi) < 2)
            hi = lo;
    }
    if (hi < lo)
        return 0;
    now = hle_d3d8_shadow_swaps();
    return now >= lo && now <= hi;
}

#define PB_SIMPLE_FIRST     0x0300u
#define PB_SIMPLE_END       0x038Cu
#define PB_SWATH            0x09F8u

static int pb_inline_method(uint32_t method)
{
    return method == PB_BEGIN_END || (method >= PB_VDATA_FIRST && method < PB_VDATA_END) ||
           (method >= PB_TEX_FIRST && method < PB_TEX_END && (method & 0x3Cu) <= 0x04u);
}

/* The walk's own copy of the vertex arrays. Not g_push_*: the XDK's own draw
 * code writes these methods for every title, and the replaced draws decide
 * by g_push_active how to read their vertices -- which must not change
 * because a walk went past. */
static uint32_t g_w_off[16], g_w_fmt[16], g_w_known;
static uint32_t g_w_prim, *g_w_idx, g_w_nidx, g_w_cidx, *g_w_inl, g_w_ninl, g_w_cinl;
static int      g_w_group;
static unsigned long g_w_draws, g_w_failed;
/* Set while hle_d3d8_push_skip() walks a replaced call's own writes: state is
 * applied, but its vertices and draws are left to the replacement. */
static int      g_w_nodraw;

static int w_grow(uint32_t **buf, uint32_t *cap, uint32_t need)
{
    if (need <= *cap)
        return 1;
    {
        uint32_t want = need * 2u < 4096u ? 4096u : need * 2u;
        uint32_t *g = (uint32_t *)realloc(*buf, (size_t)want * 4u);
        if (!g)
            return 0;
        *buf = g;
        *cap = want;
    }
    return 1;
}

static void w_index(uint32_t i)
{
    if (g_w_nidx < (1u << 22) && w_grow(&g_w_idx, &g_w_cidx, g_w_nidx + 1u))
        g_w_idx[g_w_nidx++] = i;
}

int  hle_d3d8_draw_registers(uint32_t xpt, uint32_t nverts, const float (*verts)[16][4],
                             const uint16_t *idx, uint32_t nidx);
void hle_d3d8_current_registers(float out[16][4]);

/* One attribute as the NV2A reads it into a float4 input register, from its
 * SET_VERTEX_DATA_ARRAY_FORMAT: type in bits 0-3, component count in 4-7.
 * Components it does not supply keep (0, 0, 0, 1). */
static void w_convert(uint32_t fmt, const uint8_t *src, float out[4])
{
    uint32_t type = fmt & 0xFu, n = (fmt >> 4) & 0xFu, k;
    float v[4] = { 0.0f, 0.0f, 0.0f, 1.0f };

    switch (type) {
    case 2:                                          /* F */
        for (k = 0; k < n && k < 4u; k++)
            memcpy(&v[k], src + 4u * k, 4u);
        break;
    case 0:                                          /* UB_D3D: B, G, R, A */
        if (n == 4u) {
            v[0] = src[2] / 255.0f; v[1] = src[1] / 255.0f;
            v[2] = src[0] / 255.0f; v[3] = src[3] / 255.0f;
            break;
        }
        /* fall through: fewer than four, in order */
    case 4:                                          /* UB_OGL */
        for (k = 0; k < n && k < 4u; k++)
            v[k] = src[k] / 255.0f;
        break;
    case 1:                                          /* S1: normalised short */
        for (k = 0; k < n && k < 4u; k++) {
            int16_t x;
            memcpy(&x, src + 2u * k, 2u);
            v[k] = x < 0 ? x / 32768.0f : x / 32767.0f;
        }
        break;
    case 5:                                          /* S32K: short as is */
        for (k = 0; k < n && k < 4u; k++) {
            int16_t x;
            memcpy(&x, src + 2u * k, 2u);
            v[k] = (float)x;
        }
        break;
    case 6: {                                        /* CMP: 11:11:10 signed */
        uint32_t w;
        memcpy(&w, src, 4u);
        v[0] = (float)((int32_t)(w << 21) >> 21) / 1023.0f;
        v[1] = (float)((int32_t)(w << 10) >> 21) / 1023.0f;
        v[2] = (float)((int32_t)w >> 22) / 511.0f;
        break;
    }
    default:
        return;
    }
    memcpy(out, v, sizeof v);
}

/* `vertices` vertices from `first` as sixteen input registers, from the walk's
 * arrays (or the packed inline vertices when `inl`). NULL when an array cannot
 * be read. */
static float (*w_gather_regs(uint32_t first, uint32_t vertices, const uint8_t *inl,
                             uint32_t inl_bytes))[16][4]
{
    float (*out)[16][4], cur[16][4];
    uint32_t i, v, packed = 0, at[16];

    if (!vertices)
        return NULL;
    for (i = 0; i < 16u; i++) {
        at[i] = packed;
        if ((g_w_known & (1u << i)) && (g_w_fmt[i] & 0xF0u))
            packed += vsdt_bytes(g_w_fmt[i] & 0xFFu);
    }
    if (inl && (!packed || (uint64_t)(first + vertices) * packed > inl_bytes))
        return NULL;
    out = malloc((size_t)vertices * sizeof *out);
    if (!out)
        return NULL;
    hle_d3d8_current_registers(cur);
    for (v = 0; v < vertices; v++)
        memcpy(out[v], cur, sizeof cur);
    for (i = 0; i < 16u; i++) {
        uint32_t fmt = g_w_fmt[i], stride = fmt >> 8, size;
        uint32_t va = (g_w_off[i] & 0x7FFFFFFFu) | CONTIG_BASE;

        if (!(g_w_known & (1u << i)) || !(fmt & 0xF0u) || !(size = vsdt_bytes(fmt & 0xFFu)))
            continue;
        if (inl) {
            for (v = 0; v < vertices; v++)
                w_convert(fmt, inl + (size_t)(first + v) * packed + at[i], out[v][i]);
            continue;
        }
        if (!guest_readable(va, (uint64_t)(first + vertices - 1u) * stride + size)) {
            free(out);
            return NULL;
        }
        for (v = 0; v < vertices; v++)
            w_convert(fmt, (const uint8_t *)HLE_PTR(va + (first + v) * stride), out[v][i]);
    }
    return out;
}

static void w_group_end(void)
{
    uint32_t stride, i, lo = 0xFFFFFFFFu, hi = 0;
    uint8_t *gathered;

    if (g_w_nidx) {
        uint16_t *rebased;
        for (i = 0; i < g_w_nidx; i++) {
            if (g_w_idx[i] < lo) lo = g_w_idx[i];
            if (g_w_idx[i] > hi) hi = g_w_idx[i];
        }
        if (hi - lo >= 0xFFFFu) {
            g_w_failed++;
            return;
        }
        {   /* under a vertex program: the registers, as the NV2A reads them */
            float (*regs)[16][4] = w_gather_regs(lo, hi - lo + 1u, NULL, 0);
            uint16_t *rb = regs ? (uint16_t *)malloc((size_t)g_w_nidx * sizeof *rb) : NULL;
            int drawn = 0;
            if (rb) {
                for (i = 0; i < g_w_nidx; i++)
                    rb[i] = (uint16_t)(g_w_idx[i] - lo);
                drawn = hle_d3d8_draw_registers(g_w_prim, hi - lo + 1u,
                                                (const float (*)[16][4])regs, rb, g_w_nidx);
            }
            free(rb);
            free(regs);
            if (drawn) {
                g_w_draws++;
                return;
            }
        }
        gathered = push_gather_from(g_w_off, g_w_fmt, g_w_known, lo, hi - lo + 1u, &stride);
        rebased = (uint16_t *)malloc((size_t)g_w_nidx * sizeof *rebased);
        if (gathered && rebased) {
            for (i = 0; i < g_w_nidx; i++)
                rebased[i] = (uint16_t)(g_w_idx[i] - lo);
            hle_d3d8_shadow_draw_indexed(g_w_prim, g_w_nidx, rebased, gathered, stride, 0);
            g_w_draws++;
        } else {
            g_w_failed++;
        }
        free(gathered);
        free(rebased);
    } else if (g_w_ninl) {
        uint32_t packed = 0;
        for (i = 0; i < 16u; i++)
            if ((g_w_known & (1u << i)) && (g_w_fmt[i] & 0xFFu) > 0x02u)
                packed += vsdt_bytes(g_w_fmt[i] & 0xFFu);
        if (!packed || (g_w_ninl * 4u) % packed) {
            g_w_failed++;
            return;
        }
        {
            float (*regs)[16][4] = w_gather_regs(0, g_w_ninl * 4u / packed,
                                                 (const uint8_t *)g_w_inl, g_w_ninl * 4u);
            int drawn = regs && hle_d3d8_draw_registers(g_w_prim, g_w_ninl * 4u / packed,
                                                        (const float (*)[16][4])regs, NULL, 0);
            free(regs);
            if (drawn) {
                g_w_draws++;
                return;
            }
        }
        g_gather_inline = (const uint8_t *)g_w_inl;
        g_gather_inline_bytes = g_w_ninl * 4u;
        gathered = push_gather_from(g_w_off, g_w_fmt, g_w_known, 0,
                                    g_w_ninl * 4u / packed, &stride);
        g_gather_inline = NULL;
        if (gathered) {
            hle_d3d8_shadow_draw(g_w_prim, g_w_ninl * 4u / packed, gathered, stride, 0);
            free(gathered);
            g_w_draws++;
        } else {
            g_w_failed++;
        }
    }
}

static void pb_array_apply(uint32_t m, uint32_t v)
{
    if (m >= PB_ARRAYS_FIRST && m < PB_ARRAYS_FIRST + 0x40u) {
        g_w_off[(m - PB_ARRAYS_FIRST) / 4u] = v;
        g_w_known |= 1u << ((m - PB_ARRAYS_FIRST) / 4u);
    } else if (m >= PB_ARRAYS_FIRST + 0x40u && m < PB_ARRAYS_END) {
        g_w_fmt[(m - PB_ARRAYS_FIRST - 0x40u) / 4u] = v;
        g_w_known |= 1u << ((m - PB_ARRAYS_FIRST - 0x40u) / 4u);
    } else if (!g_w_group) {
        return;                          /* elements outside begin/end: nothing */
    } else if (m == PB_ELEM16) {
        w_index(v & 0xFFFFu);
        w_index(v >> 16);
    } else if (m == PB_ELEM32) {
        w_index(v);
    } else if (m == PB_DRAW_ARRAYS) {
        uint32_t first = v & 0x00FFFFFFu, n = (v >> 24) + 1u, k;
        for (k = 0; k < n; k++)
            w_index(first + k);
    } else if (m == PB_INLINE_ARRAY) {
        if (g_w_ninl < (1u << 22) && w_grow(&g_w_inl, &g_w_cinl, g_w_ninl + 1u))
            g_w_inl[g_w_ninl++] = v;
    }
}

static void pb_array_begin_end(uint32_t v)
{
    if (v) {
        g_w_prim = v;
        g_w_nidx = g_w_ninl = 0;
        g_w_group = 1;
    } else if (g_w_group) {
        w_group_end();
        g_w_group = 0;
        g_w_nidx = g_w_ninl = 0;
        if (g_w_draws && (g_w_draws == 1 || (g_w_draws % 50000u) == 0))
            fprintf(stderr, "[HLE-D3D8] push buffer array draws: %lu drawn, %lu not "
                    "(no declaration or layout to read them by)\n", g_w_draws, g_w_failed);
    }
}

void hle_d3d8_push_constants_sync(void)
{
    uint32_t device, put, va, ret = 0, words = 0;
    int jumps = 0;
    int inl = hle_d3d8_pb_inline_on();
    /* Not re-entrant: an immediate-mode draw from the walk goes through
     * hle_d3d8_shadow_draw, which syncs before drawing. The walk in progress
     * is already applying everything in order, so the nested call has
     * nothing to do -- and starting again from g_pb_scan, which only moves
     * when a walk ends, would replay the same span and recurse until the
     * stack ran out. Per thread, as the walk is. */
    static RECOMP_TLS int walking;

    if (walking)
        return;

    if (!pb_on() || !hle_var_D3D_g_pDevice || !hle_d3d8_shadow_device())
        return;
    device = HLE_MEM32(hle_var_D3D_g_pDevice);
    if (!device || !guest_readable(device, 4u))
        return;
    put = HLE_MEM32(device);
    if (!put || !guest_readable(put, 4u))
        return;
    if (!g_pb_scan) {
        g_pb_scan = put;
        return;
    }
    g_pb_walks++;
    walking = 1;
    va = g_pb_scan;
    while (va != put) {
        uint32_t w, count, method, i;

        if (!guest_readable(va, 4u) || ++words > PB_MAX_WORDS)
            goto resync;
        w = HLE_MEM32(va);
        if ((w & 0xE0000003u) == 0x20000000u || (w & 3u) == 1u) {
            /* A jump: the buffer wrapping, or a segment change. */
            uint32_t target = (w & 3u) == 1u ? (w & 0xFFFFFFFCu) : (w & 0x1FFFFFFCu);
            if (++jumps > 8)
                goto resync;
            va = CONTIG_BASE | (target & 0x0FFFFFFFu);
            continue;
        }
        if ((w & 3u) == 2u) {             /* a call, one level deep */
            if (ret)
                goto resync;
            ret = va + 4u;
            va = CONTIG_BASE | ((w & 0xFFFFFFFCu) & 0x0FFFFFFFu);
            continue;
        }
        if (w == 0x00020000u) {           /* return */
            if (!ret)
                goto resync;
            va = ret;
            ret = 0;
            continue;
        }
        if ((w & 0xA0030003u) != 0u)      /* not a method header */
            goto resync;
        count = (w >> 18) & 0x7FFu;
        method = w & 0x1FFCu;
        if (inl && method + 4u * count > PB_SURFACE_COLOR && method <= PB_SURFACE_ZETA &&
            guest_readable(va + 4u, 4ull * count)) {
            uint32_t k;
            for (k = 0; k < count; k++) {
                uint32_t m = (w & 0x40000000u) ? method : method + 4u * k;
                if (m == PB_SURFACE_COLOR)
                    g_surface_color = HLE_MEM32(va + 4u + 4u * k);
                else if (m == PB_SURFACE_ZETA && !g_w_nodraw) {
                    pb_flush();
                    hle_d3d8_pb_surface(g_surface_color, HLE_MEM32(va + 4u + 4u * k));
                }
            }
        }
        if (pb_surface_trace() && method + 4u * count > 0x0200u && method < 0x0220u) {
            uint32_t k;
            for (k = 0; k < count && guest_readable(va + 4u + 4u * k, 4u); k++)
                fprintf(stderr, "[PB-SURF swap %lu] method 0x%04X = 0x%08X%s\n",
                        hle_d3d8_shadow_swaps(), (w & 0x40000000u) ? method : method + 4u * k,
                        HLE_MEM32(va + 4u + 4u * k), g_w_nodraw ? " (replaced call)" : "");
        }
        if (method == PB_CONST_LOAD || (method + 4u * count > PB_CONST_FIRST &&
                                        method < PB_CONST_END) ||
            (inl && (method == PB_BEGIN_END || (method + 4u * count > PB_VDATA_FIRST &&
                                                method < PB_TEX_END) ||
                     (method + 4u * count > PB_ARRAYS_FIRST && method < PB_ARRAYS_END) ||
                     (method >= PB_ELEM16 && method <= PB_INLINE_ARRAY) ||
                     (method + 4u * count > PB_SIMPLE_FIRST && method < PB_SIMPLE_END) ||
                     (method + 4u * count > PB_CLEAR_FIRST && method < PB_CLEAR_END) ||
                     method == PB_SWATH))) {
            int noninc = (w & 0x40000000u) != 0u;

            if (!guest_readable(va + 4u, 4ull * count))
                goto resync;
            for (i = 0; i < count; i++) {
                uint32_t m = noninc ? method : method + 4u * i;
                uint32_t v = HLE_MEM32(va + 4u + 4u * i);
                if (inl && ((m >= PB_SIMPLE_FIRST && m < PB_SIMPLE_END) || m == PB_SWATH)) {
                    if (!g_w_nodraw && hle_d3d8_title_ltcg())
                        (void)hle_d3d8_state_store_simple(m, v);
                } else if (inl && m >= PB_CLEAR_FIRST && m < PB_CLEAR_END) {
                    g_clear_reg[(m - PB_CLEAR_FIRST) / 4u] = v;
                    if (pb_surface_trace())
                        fprintf(stderr, "[PB-SURF swap %lu] method 0x%04X = 0x%08X%s\n",
                                hle_d3d8_shadow_swaps(), m, v,
                                g_w_nodraw ? " (replaced call)" : "");
                    if (m == PB_CLEAR_SURFACE && !g_w_nodraw) {
                        pb_flush();
                        hle_d3d8_pb_clear(v, g_clear_reg[1], g_clear_reg[0],
                                          g_clear_reg[3], g_clear_reg[4]);
                    }
                } else if (inl && g_w_nodraw && (pb_inline_method(m) ||
                                                 m == PB_ELEM16 || m == PB_ELEM32 ||
                                                 m == PB_DRAW_ARRAYS || m == PB_INLINE_ARRAY)) {
                    /* a replaced call's own draw: it draws, not the walk */
                } else if (inl && pb_array_method(m)) {
                    pb_flush();
                    pb_array_apply(m, v);
                } else if (inl && pb_inline_method(m)) {
                    pb_flush();     /* constants first: they precede it */
                    if (m == PB_BEGIN_END)
                        pb_array_begin_end(v);
                    hle_d3d8_pb_inline_method(m, v);
                } else {
                    pb_method(m, v);
                }
            }
        }
        va += 4u + 4u * count;
        words += count;
    }
    pb_flush();
    g_pb_scan = put;
    walking = 0;
    return;
resync:
    walking = 0;
    pb_flush();
    g_pb_resyncs++;
    {
        static int said;
        if (said++ < 3)
            fprintf(stderr, "[HLE-D3D8] push buffer constants: walk from 0x%08X lost its "
                    "way at 0x%08X (word 0x%08X); resynchronised at 0x%08X "
                    "(%lu constants applied over %lu walks so far)\n", g_pb_scan, va,
                    guest_readable(va, 4u) ? HLE_MEM32(va) : 0u, put, g_pb_constants,
                    g_pb_walks);
    }
    g_pb_scan = put;
}

/* Title RAM or the contiguous window. */
static int guest_readable(uint32_t va, uint64_t bytes)
{
    if (va >= 0x00010000u && (uint64_t)va + bytes <= g_xbox_total_ram)
        return 1;
    return va >= CONTIG_BASE && (uint64_t)va + bytes <= (uint64_t)CONTIG_BASE + CONTIG_SIZE;
}

/* From hle_d3d8.c: the stream the selected program's vertices come from. */
uint32_t hle_d3d8_shadow_base_stream(void);

/* Host pointer to vertex `first` of the draw's own stream -- stream 0, or
 * the one stream the selected program reads (hle_d3d8_shadow_base_stream) --
 * for `vertices` vertices, with that stream's stride in *stride; NULL
 * (counted) if there is no buffer or it would read outside the contiguous
 * window. */
static const void *stream0_vertices(uint32_t first, uint32_t vertices, uint32_t *stride)
{
    uint32_t base = hle_d3d8_shadow_base_stream();
    uint32_t vb = base ? g_stream_vb[base] : g_stream0_vb;
    uint32_t data, va;
    uint64_t start, bytes;

    *stride = base ? g_stream_stride[base] : g_stream0_stride;
    if (!vb || !*stride || !guest_readable(vb, 12u)) {
        g_skip_no_stream++;
        return NULL;
    }
    data = HLE_MEM32(vb + 4u);
    /* Where the title's own D3DVertexBuffer_Lock2 hands out the vertices. */
    va = data | CONTIG_BASE;
    start = (uint64_t)(va - CONTIG_BASE) + (uint64_t)first * *stride;
    bytes = (uint64_t)vertices * *stride;
    if (!data || start + bytes > CONTIG_SIZE) {
        g_skip_range++;
        return NULL;
    }
    return HLE_PTR(CONTIG_BASE + (uint32_t)start);
}

/* Host pointer to vertex `first` of any stream's buffer, for `vertices`
 * vertices, with its stride; NULL if there is none or it would read outside
 * the contiguous window. For the registers a program reads from streams other
 * than 0, found at the same index as the stream 0 vertex they go with. */
const void *hle_d3d8_stream_vertices(uint32_t stream, uint32_t first, uint32_t vertices,
                                     uint32_t *stride)
{
    uint32_t vb, data;
    uint64_t start, bytes;

    if (stream >= 16u)
        return NULL;
    vb = g_stream_vb[stream];
    *stride = g_stream_stride[stream];
    if (!vb || !*stride || !guest_readable(vb, 12u))
        return NULL;
    data = HLE_MEM32(vb + 4u);
    start = (uint64_t)((data | CONTIG_BASE) - CONTIG_BASE) + (uint64_t)first * *stride;
    bytes = (uint64_t)vertices * *stride;
    if (!data || start + bytes > CONTIG_SIZE)
        return NULL;
    return HLE_PTR(CONTIG_BASE + (uint32_t)start);
}

static void report(void)
{
    static DWORD last;
    DWORD now = GetTickCount();

    if (!last) {
        last = now;
    } else if (now - last >= 5000) {
        fprintf(stderr, "[HLE-D3D8] shadow buffers: skipped %lu with no stream 0 buffer, "
                "%lu out of range; %lu drawn from title-pushed arrays (%lu not)\n",
                g_skip_no_stream, g_skip_range, g_push_draws, g_push_failed);
        if (g_pb_walks)
            fprintf(stderr, "[HLE-D3D8] push buffer constants: %lu applied over %lu walks, "
                    "%lu resync(s)\n", g_pb_constants, g_pb_walks, g_pb_resyncs);
        last = now;
    }
}

HLE_ORIGINAL(D3DDevice_SetStreamSource);
HLE_ORIGINAL(CDevice_SetStateVB);
HLE_ORIGINAL(D3DDevice_DrawVertices);
HLE_ORIGINAL(D3DDevice_DrawIndexedVertices);
/* The one generic setter, on XDKs that predate the specialised forms. */
HLE_ORIGINAL(D3DDevice_SetVertexShaderConstant);
HLE_ORIGINAL(D3DDevice_SetVertexShaderConstant1);
HLE_ORIGINAL(D3DDevice_SetVertexShaderConstant4);
HLE_ORIGINAL(D3DDevice_SetVertexShaderConstantNotInline);
HLE_ORIGINAL(D3DDevice_SetVertexShaderConstantNotInlineFast);

static void first_call(int *seen, const char *name, uint32_t arg)
{
    if (!*seen) {
        *seen = 1;
        fprintf(stderr, "[HLE] %s(0x%X) replaced by name\n", name, arg);
        fflush(stderr);
    }
}

static int original_missing(void (*fn)(void), const char *name)
{
    if (fn)
        return 0;
    fprintf(stderr, "[HLE] %s: original body missing -- regenerate the lift\n", name);
    return 1;
}

/* void D3DDevice_SetStreamSource(UINT StreamNumber,
 *     D3DVertexBuffer *pStreamData, UINT Stride)                            */
HLE_EXPORT(D3DDevice_SetStreamSource)
{
    static int seen;
    uint32_t stream = HLE_ARG(0);
    uint32_t vb = HLE_ARG(1), stride = HLE_ARG(2);

    first_call(&seen, "D3DDevice_SetStreamSource", stream);
    if (original_missing(hle_original_D3DDevice_SetStreamSource, "D3DDevice_SetStreamSource"))
        HLE_RETURN(0x80004005u);
    HLE_CALL_ORIGINAL(D3DDevice_SetStreamSource);
    hle_d3d8_push_arrays_off();          /* the XDK writes its own arrays again */
    if (stream == 0u) {
        g_stream0_vb = vb;
        g_stream0_stride = stride;
    }
    if (stream < 16u) {
        g_stream_vb[stream] = vb;
        g_stream_stride[stream] = stride;
    }
}

/* void CDevice::SetStateVB(DWORD BaseVertexIndex) -- thiscall, D3D internal,
 * called by every DrawIndexedVertices. */
HLE_EXPORT(CDevice_SetStateVB)
{
    static int seen;
    uint32_t base = HLE_ARG(0);

    first_call(&seen, "CDevice_SetStateVB", base);
    if (original_missing(hle_original_CDevice_SetStateVB, "CDevice_SetStateVB"))
        HLE_RETURN(0u);
    HLE_CALL_ORIGINAL(CDevice_SetStateVB);
    g_base_vertex = base;
    g_base_vertex_seen = 1;
}

void hle_d3d8_push_skip(void);

/* A replaced draw with no stream buffer to read: its stream setup was
 * inlined (Halo 2 writes SET_VERTEX_DATA_ARRAY_OFFSET itself), so the
 * replacement has nothing to draw from, but the body has just written the
 * arrays, the begin/end and its indices into the push buffer. The walk draws
 * it from there, as it draws the title's inlined draws. 1 when it did. */
static unsigned long g_walk_drawn_calls;

static int walk_draws_it(void)
{
    uint32_t base, vb, stride;

    if (g_push_active || !hle_d3d8_shadow_device() || !hle_d3d8_pb_inline_on())
        return 0;
    base = hle_d3d8_shadow_base_stream();
    vb = base ? g_stream_vb[base] : g_stream0_vb;
    stride = base ? g_stream_stride[base] : g_stream0_stride;
    if (vb && stride && guest_readable(vb, 12u) && HLE_MEM32(vb + 4u))
        return 0;
    hle_d3d8_push_constants_sync();
    if (++g_walk_drawn_calls == 1)
        fprintf(stderr, "[HLE-D3D8] shadow buffers: a replaced draw with no stream "
                "buffer is drawn from its push-buffer writes\n");
    return 1;
}

/* void D3DDevice_DrawVertices(D3DPRIMITIVETYPE PrimitiveType,
 *     UINT StartVertex, UINT VertexCount)                                   */
HLE_EXPORT(D3DDevice_DrawVertices)
{
    static int seen;
    uint32_t xpt = HLE_ARG(0);
    uint32_t start = HLE_ARG(1), count = HLE_ARG(2);

    first_call(&seen, "D3DDevice_DrawVertices", xpt);
    if (original_missing(hle_original_D3DDevice_DrawVertices, "D3DDevice_DrawVertices"))
        HLE_RETURN(0u);
    HLE_CALL_ORIGINAL(D3DDevice_DrawVertices);
    if (walk_draws_it())
        return;
    hle_d3d8_push_skip();   /* its own draw methods: drawn here, not by the walk */
    if (hle_d3d8_shadow_device() && count && g_push_active) {
        uint32_t stride;
        uint8_t *gathered = push_gather(start, count, &stride);
        if (gathered) {
            hle_d3d8_shadow_draw(xpt, count, gathered, stride, 0);
            free(gathered);
            g_push_draws++;
        } else {
            g_push_failed++;
        }
        report();
    } else if (hle_d3d8_shadow_device() && count) {
        uint32_t stride;
        const void *verts = stream0_vertices(start, count, &stride);
        if (verts) {
            hle_d3d8_shadow_set_first_vertex(start);
            hle_d3d8_shadow_draw(xpt, count, verts, stride, 1);
        }
        report();
    }
}

/* void D3DDevice_DrawIndexedVertices(D3DPRIMITIVETYPE PrimitiveType,
 *     UINT VertexCount, const WORD *pIndexData)                             */
HLE_EXPORT(D3DDevice_DrawIndexedVertices)
{
    static int seen;
    uint32_t xpt = HLE_ARG(0);
    uint32_t count = HLE_ARG(1), index_va = HLE_ARG(2);

    first_call(&seen, "D3DDevice_DrawIndexedVertices", xpt);
    if (original_missing(hle_original_D3DDevice_DrawIndexedVertices,
                         "D3DDevice_DrawIndexedVertices"))
        HLE_RETURN(0u);
    HLE_CALL_ORIGINAL(D3DDevice_DrawIndexedVertices);
    if (walk_draws_it())
        return;
    hle_d3d8_push_skip();   /* its own draw methods: drawn here, not by the walk */
    if (hle_d3d8_shadow_device() && count && index_va &&
        guest_readable(index_va, (uint64_t)count * 2u)) {
        const uint16_t *idx = (const uint16_t *)HLE_PTR(index_va);
        uint32_t i, vertices = 0, vstride;
        const void *verts;

        for (i = 0; i < count; i++)
            if ((uint32_t)idx[i] + 1u > vertices)
                vertices = (uint32_t)idx[i] + 1u;
        if (g_push_active) {
            /* The pushed arrays carry no base vertex: the XDK applies one by
             * rewriting its own offsets, which it has not done. Only the
             * range the indices use is gathered, rebased to zero. */
            uint32_t lo = 0xFFFFu, stride;
            uint16_t *rebased = malloc((size_t)count * sizeof *rebased);
            uint8_t *gathered = NULL;

            for (i = 0; i < count; i++)
                if (idx[i] < lo)
                    lo = idx[i];
            if (rebased)
                gathered = push_gather(lo, vertices - lo, &stride);
            if (gathered) {
                for (i = 0; i < count; i++)
                    rebased[i] = (uint16_t)(idx[i] - lo);
                hle_d3d8_shadow_draw_indexed(xpt, count, rebased, gathered, stride, 0);
                g_push_draws++;
            } else {
                g_push_failed++;
            }
            free(gathered);
            free(rebased);
            report();
            return;
        }
        if (!g_base_vertex_seen) {
            static int said;
            if (!said++)
                fprintf(stderr, "[HLE-D3D8] shadow buffers: CDevice_SetStateVB never "
                        "ran; indexed buffer draws use base vertex 0\n");
        }
        verts = stream0_vertices(g_base_vertex, vertices, &vstride);
        if (verts) {
            hle_d3d8_shadow_set_first_vertex(g_base_vertex);
            hle_d3d8_shadow_draw_indexed(xpt, count, idx, verts, vstride, 1);
        }
        report();
    }
}

static void forward_constants(uint32_t reg, uint32_t data, uint32_t count)
{
    if (!hle_d3d8_shadow_device() || !data || !count || reg >= MAX_CONSTANT_REGISTERS)
        return;
    if (count > MAX_CONSTANT_REGISTERS - reg)
        count = MAX_CONSTANT_REGISTERS - reg;
    if (!guest_readable(data, (uint64_t)count * 16u))
        return;
    host_vsh_set_constant((int)reg, (const float *)HLE_PTR(data), (int)count);
}

/* void __fastcall D3DDevice_SetVertexShaderConstant1(int Register,
 *     const void *pConstantData) -- one register.                           */
HLE_EXPORT(D3DDevice_SetVertexShaderConstant1)
{
    static int seen;
    uint32_t reg = g_ecx;
    uint32_t data = g_edx;

    first_call(&seen, "D3DDevice_SetVertexShaderConstant1", reg);
    if (original_missing(hle_original_D3DDevice_SetVertexShaderConstant1,
                         "D3DDevice_SetVertexShaderConstant1"))
        HLE_RETURN(0u);
    HLE_CALL_ORIGINAL(D3DDevice_SetVertexShaderConstant1);
    forward_constants(reg, data, 1u);
}

/* void __fastcall D3DDevice_SetVertexShaderConstant1Fast(int Register,
 *     const void *pConstantData) -- one register, without the checks.
 *
 * Some builds have only this form of the single-register setter, beside
 * NotInlineFast: XGRA, Doom 3 and Breakdown name no other, and Outrun 2 has
 * both. Unreplaced, every constant a title set this way stayed zero on the
 * host, and a program that transforms its position by one drew nothing --
 * XGRA's movie quad reached the host every frame and came out black. */
HLE_ORIGINAL(D3DDevice_SetVertexShaderConstant1Fast);
HLE_EXPORT(D3DDevice_SetVertexShaderConstant1Fast)
{
    static int seen;
    uint32_t reg = g_ecx;
    uint32_t data = g_edx;

    first_call(&seen, "D3DDevice_SetVertexShaderConstant1Fast", reg);
    if (original_missing(hle_original_D3DDevice_SetVertexShaderConstant1Fast,
                         "D3DDevice_SetVertexShaderConstant1Fast"))
        HLE_RETURN(0u);
    HLE_CALL_ORIGINAL(D3DDevice_SetVertexShaderConstant1Fast);
    forward_constants(reg, data, 1u);
}

/* void __fastcall D3DDevice_SetVertexShaderConstant4(int Register,
 *     const void *pConstantData) -- four registers.                         */
HLE_EXPORT(D3DDevice_SetVertexShaderConstant4)
{
    static int seen;
    uint32_t reg = g_ecx;
    uint32_t data = g_edx;

    first_call(&seen, "D3DDevice_SetVertexShaderConstant4", reg);
    if (original_missing(hle_original_D3DDevice_SetVertexShaderConstant4,
                         "D3DDevice_SetVertexShaderConstant4"))
        HLE_RETURN(0u);
    HLE_CALL_ORIGINAL(D3DDevice_SetVertexShaderConstant4);
    forward_constants(reg, data, 4u);
}

/* void __fastcall D3DDevice_SetVertexShaderConstantNotInline(int Register,
 *     const void *pConstantData, DWORD ConstantCount) -- ConstantCount counts
 * floats (Cxbx-Reloaded divides it by 4). A count that is not a multiple of
 * four drops the last, partial register here; the XDK passes whole ones.
 *
 * Burnout 2's NotInline calls NotInlineFast (through its thunk), so the inner
 * call does not forward and this one does, once. */
HLE_EXPORT(D3DDevice_SetVertexShaderConstantNotInline)
{
    static int seen;
    uint32_t reg = g_ecx;
    uint32_t data = g_edx, floats = HLE_ARG(0);

    first_call(&seen, "D3DDevice_SetVertexShaderConstantNotInline", reg);
    if (original_missing(hle_original_D3DDevice_SetVertexShaderConstantNotInline,
                         "D3DDevice_SetVertexShaderConstantNotInline"))
        HLE_RETURN(0u);
    g_in_notinline++;
    HLE_CALL_ORIGINAL(D3DDevice_SetVertexShaderConstantNotInline);
    g_in_notinline--;
    forward_constants(reg, data, floats / 4u);
}

/* The same, the XDK's faster variant.                                        */
HLE_EXPORT(D3DDevice_SetVertexShaderConstantNotInlineFast)
{
    static int seen;
    uint32_t reg = g_ecx;
    uint32_t data = g_edx, floats = HLE_ARG(0);

    first_call(&seen, "D3DDevice_SetVertexShaderConstantNotInlineFast", reg);
    if (original_missing(hle_original_D3DDevice_SetVertexShaderConstantNotInlineFast,
                         "D3DDevice_SetVertexShaderConstantNotInlineFast"))
        HLE_RETURN(0u);
    HLE_CALL_ORIGINAL(D3DDevice_SetVertexShaderConstantNotInlineFast);
    if (!g_in_notinline)
        forward_constants(reg, data, floats / 4u);
}

/* void __stdcall D3DDevice_SetVertexShaderConstant(INT Register,
 *     const void *pConstantData, DWORD ConstantCount)
 *
 * The one generic setter, on an XDK that predates the specialised forms.
 * Max Payne is XDK 3925: its D3D8 exports this and none of Constant1,
 * Constant4, NotInline or NotInlineFast, so a title there sets every vertex
 * shader constant through a function nothing replaced, and the host saw none
 * of them.
 *
 * ConstantCount counts registers here, not floats -- it is the plain D3D8
 * signature. NotInline's float count is the odd one out, not this.
 *
 * Register is -96-based and biased here, which the specialised forms are not.
 * 3925 starts `mov edx,[ebp+8]; add edx,0x60`, applying the bias itself, while
 * 4721's Constant4 writes its ecx straight into the push buffer and indexes
 * its shadow array with it unchanged. host_vsh_set_constant wants the same
 * 0-based index the specialised forms hand it, so the bias has to be added
 * here or every constant lands 96 registers low -- silently, because the
 * range check below would still pass for most of them.
 *
 * The arguments are logged for the first few calls: this has not yet been
 * seen to run (Max Payne reaches no vertex shader in the frames it renders),
 * so the log is the evidence that the reading above is right.
 */
#define XBOX_VSH_CONSTANT_BIAS 96u

HLE_EXPORT(D3DDevice_SetVertexShaderConstant)
{
    static int seen;
    uint32_t reg   = HLE_ARG(0) + XBOX_VSH_CONSTANT_BIAS;
    uint32_t data  = HLE_ARG(1);
    uint32_t count = HLE_ARG(2);

    first_call(&seen, "D3DDevice_SetVertexShaderConstant", reg);
    {
        static int notes;
        if (notes < 4) {
            notes++;
            fprintf(stderr, "[HLE-D3D8] SetVertexShaderConstant(reg=%d, data=0x%08X,"
                    " count=%u) -> host register %u; ecx=0x%08X edx=0x%08X\n",
                    (int)HLE_ARG(0), data, count, reg, g_ecx, g_edx);
            fflush(stderr);
        }
    }
    if (original_missing(hle_original_D3DDevice_SetVertexShaderConstant,
                         "D3DDevice_SetVertexShaderConstant"))
        HLE_RETURN(0u);
    HLE_CALL_ORIGINAL(D3DDevice_SetVertexShaderConstant);
    forward_constants(reg, data, count);
}

/* Walk what a replaced call's own body just wrote, without drawing it. For
 * the replacements that draw themselves (Begin, End, SetVertexData*,
 * DrawVertices, DrawIndexedVertices, ...): the walk must not draw their
 * vertices a second time, but the state the body writes on the way -- the
 * vertex array offsets and formats the XDK's SetStateVB flushes, constants,
 * simple render states -- is what the next inlined draw reads. Skipping the
 * span outright, as this did, lost that state: Halo 2 inlines its stream
 * setup, and every array draw after a replaced draw read stale arrays.
 * RECOMP_HLE_D3D8_PB_SKIP_WALK=0 skips the span as before. */
void hle_d3d8_push_skip(void)
{
    uint32_t device, put;
    static int walk = -1;

    if (!g_pb_scan || !hle_var_D3D_g_pDevice)
        return;
    if (walk < 0)
        walk = xbox_EnvSwitch("RECOMP_HLE_D3D8_PB_SKIP_WALK", 1);
    if (walk && !g_w_nodraw) {
        g_w_nodraw = 1;
        hle_d3d8_push_constants_sync();
        g_w_nodraw = 0;
        return;
    }
    device = HLE_MEM32(hle_var_D3D_g_pDevice);
    if (!device || !guest_readable(device, 4u))
        return;
    put = HLE_MEM32(device);
    if (put && guest_readable(put, 4u))
        g_pb_scan = put;
}
