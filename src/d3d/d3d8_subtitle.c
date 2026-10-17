/*
 * d3d8_subtitle.c -- subtitles for a title that has none, from a table.
 *
 * A title's speech is files it streams. The table, subtitles.txt in the
 * per-user settings folder (recomp_config_user_dir), holds one line per
 * subtitle:
 *
 *     <key>\t<start seconds>\t<end seconds>\t<text>
 *
 * <key> names a speech file. Which file is playing is told by its sound,
 * not its name: a title may stream speech out of an encrypted archive (Max
 * Payne does, and only probes the loose files at boot), so a second table,
 * subtitle_index.txt, holds a fingerprint per key --
 *
 *     <hex FNV-1a 64 of the first min(8192, size) bytes>\t<key>
 *
 * of the file's data and of the file itself -- and every packet a title
 * hands a DirectSound stream is fingerprinted the same way. A match starts
 * that key's lines against the clock, and each frame draws the current one
 * at the bottom of the picture: white, outlined in black, wrapped to three
 * lines. Starting another key replaces what is showing.
 *
 * subtitles = 0 in the title's settings, or RECOMP_SUBTITLES=0, turns it off.
 * Windows only (GDI draws the text); elsewhere the entry points do nothing.
 */
#include "d3d8_internal.h"
#include "d3d8_subtitle.h"
#include "recomp_config.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)

#define SUB_W  1600            /* the text bitmap: three lines at 52 px */
#define SUB_H   176
#define SUB_FONT_PX 52
#define SUB_LINGER  0.4        /* seconds a line stays past its end time */

typedef struct { char key[96]; float start, end; char *text; } SubLine;

static struct {
    int        loaded, enabled;
    SubLine   *lines;
    int        count;
    CRITICAL_SECTION lock;
    /* what is playing */
    int        first, last;     /* range of lines for the playing key, -1 = none */
    LARGE_INTEGER t0, qpf;
    /* fingerprint -> first line */
    struct { uint64_t hash; int line; } *index;
    int        index_count;
    /* drawing */
    int        tried, failed;
    RhiImage  *texture;
    RhiView   *srv;
    RhiBuffer *cb;
    RhiShader *vs, *ps;
    RhiBlendState  *blend;
    RhiDepthState  *depth;
    RhiRasterState *raster;
    HDC        dc;
    HBITMAP    bitmap;
    HFONT      font;
    void      *bits;
    int        shown;           /* index of the line in the bitmap, -1 none */
} s = { .first = -1, .last = -1, .shown = -1 };

static int cmp_key(const void *a, const void *b)
{
    const SubLine *x = a, *y = b;
    int c = strcmp(x->key, y->key);
    if (c) return c;
    return x->start < y->start ? -1 : x->start > y->start;
}

static int find(const char *key);
static void load_index(const char *dir);

static void load(void)
{
    char dir[600], path[700], buf[2048];
    FILE *f;
    int cap = 0;

    s.loaded = 1;
    InitializeCriticalSection(&s.lock);
    QueryPerformanceFrequency(&s.qpf);
    s.enabled = recomp_config_bool("RECOMP_SUBTITLES", "subtitles", 1);
    if (!s.enabled || !recomp_config_user_dir(dir, sizeof dir))
        return;
    snprintf(path, sizeof path, "%s\\subtitles.txt", dir);
    f = fopen(path, "rb");
    if (!f)
        return;
    while (fgets(buf, sizeof buf, f)) {
        char *k = buf, *a, *b, *t, *e;
        SubLine l;

        if (!(a = strchr(k, '\t')) || !(b = strchr(a + 1, '\t')) || !(t = strchr(b + 1, '\t')))
            continue;
        *a = *b = *t = '\0';
        t++;
        for (e = t + strlen(t); e > t && (e[-1] == '\n' || e[-1] == '\r'); e--)
            e[-1] = '\0';
        if (!*t || strlen(k) >= sizeof l.key)
            continue;
        memset(&l, 0, sizeof l);
        strcpy(l.key, k);
        l.start = (float)atof(a + 1);
        l.end = (float)atof(b + 1);
        l.text = _strdup(t);
        if (s.count == cap) {
            cap = cap ? cap * 2 : 1024;
            s.lines = realloc(s.lines, (size_t)cap * sizeof *s.lines);
        }
        s.lines[s.count++] = l;
    }
    fclose(f);
    qsort(s.lines, (size_t)s.count, sizeof *s.lines, cmp_key);
    load_index(dir);
    fprintf(stderr, "[SUBTITLE] %d lines, %d fingerprints from %s\n", s.count,
            s.index_count, dir);
    fflush(stderr);
}

/* First line for key, or -1. */
static int find(const char *key)
{
    int lo = 0, hi = s.count - 1, at = -1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2, c = strcmp(s.lines[mid].key, key);
        if (c == 0) { at = mid; hi = mid - 1; }
        else if (c < 0) lo = mid + 1;
        else hi = mid - 1;
    }
    return at;
}

static int cmp_hash(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y;
}

static void load_index(const char *dir)
{
    char path[700], buf[512];
    FILE *f;
    int cap = 0;

    snprintf(path, sizeof path, "%s\\subtitle_index.txt", dir);
    if (!(f = fopen(path, "rb")))
        return;
    while (fgets(buf, sizeof buf, f)) {
        char *tab = strchr(buf, '\t'), *e;
        int line;
        if (!tab)
            continue;
        *tab++ = '\0';
        for (e = tab + strlen(tab); e > tab && (e[-1] == '\n' || e[-1] == '\r'); e--)
            e[-1] = '\0';
        if ((line = find(tab)) < 0)
            continue;                            /* no lines for it (silence) */
        if (s.index_count == cap) {
            cap = cap ? cap * 2 : 1024;
            s.index = realloc(s.index, (size_t)cap * sizeof *s.index);
        }
        s.index[s.index_count].hash = _strtoui64(buf, NULL, 16);
        s.index[s.index_count].line = line;
        s.index_count++;
    }
    fclose(f);
    qsort(s.index, (size_t)s.index_count, sizeof *s.index, cmp_hash);
}

static uint64_t fnv64(const uint8_t *p, uint32_t n)
{
    uint64_t h = 0xcbf29ce484222325ull;
    while (n--)
        h = (h ^ *p++) * 0x100000001b3ull;
    return h;
}

void d3d8_subtitle_audio_packet(const void *data, uint32_t size)
{
    uint64_t h;
    int lo, hi, line = -1;

    if (!s.loaded) load();
    if (!s.enabled || !s.index_count || !data || size < 256)
        return;
    /* Silence fingerprints alike whatever file it opens: a packet of one
     * repeated byte says nothing about which line is playing. */
    {
        uint32_t n = size < 8192u ? size : 8192u;
        if (memcmp(data, (const uint8_t *)data + 1, n - 1) == 0)
            return;
    }
    h = fnv64((const uint8_t *)data, size < 8192u ? size : 8192u);
    for (lo = 0, hi = s.index_count - 1; lo <= hi;) {
        int mid = (lo + hi) / 2;
        if (s.index[mid].hash == h) { line = s.index[mid].line; break; }
        if (s.index[mid].hash < h) lo = mid + 1; else hi = mid - 1;
    }
    /* RECOMP_SUBTITLE_TRACE=1: every packet's fingerprint, its first bytes
     * and what it matched, the first 400 -- to line a title's packets up
     * against its files. A match is always said. */
    {
        static int trace = -1, said;
        if (trace < 0) {
            const char *e = getenv("RECOMP_SUBTITLE_TRACE");
            trace = e && *e && *e != '0';
        }
        if ((trace && said < 400) || line >= 0) {
            const uint8_t *b = (const uint8_t *)data;
            said++;
            fprintf(stderr, "[SUBTITLE] packet %u bytes %016llx %02X%02X%02X%02X%02X%02X%02X%02X %s\n",
                    size, (unsigned long long)h, b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7],
                    line >= 0 ? s.lines[line].key : "-");
            fflush(stderr);
        }
    }
    if (line < 0)
        return;
    EnterCriticalSection(&s.lock);
    {
        int last = line;
        while (last + 1 < s.count && strcmp(s.lines[last + 1].key, s.lines[line].key) == 0)
            last++;
        s.first = line;
        s.last = last;
        QueryPerformanceCounter(&s.t0);
    }
    LeaveCriticalSection(&s.lock);
}

/* The line to show now, or -1. */
static int current(void)
{
    LARGE_INTEGER now;
    double t;
    int i, at = -1;

    EnterCriticalSection(&s.lock);
    if (s.first >= 0) {
        QueryPerformanceCounter(&now);
        t = (double)(now.QuadPart - s.t0.QuadPart) / (double)s.qpf.QuadPart;
        for (i = s.first; i <= s.last; i++)
            if (t >= s.lines[i].start && t < s.lines[i].end + SUB_LINGER)
                at = i;
        if (t > s.lines[s.last].end + SUB_LINGER)
            s.first = s.last = -1;                 /* finished */
    }
    LeaveCriticalSection(&s.lock);
    return at;
}

/* --------------------------------------------------------------- drawing */

typedef struct { float x, y, w, h; float tex_w, tex_h, pad0, pad1; } SubConstants;

/* Red is the white text's coverage, green the black outline's (and the
 * text's): colour from the first, alpha from the larger. */
static const char kShader[] =
    "cbuffer Sub : register(b7) { float4 rect; float4 size; };\n"
    "struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };\n"
    "VSOut vs_main(uint id : SV_VertexID) {\n"
    "    float2 c = float2(id & 1, id >> 1);\n"
    "    VSOut o;\n"
    "    o.pos = float4(rect.x + c.x * rect.z, rect.y - c.y * rect.w, 0, 1);\n"
    "    o.uv  = c * size.xy;\n"
    "    return o;\n"
    "}\n"
    "Texture2D<float4> tex : register(t8);\n"
    "SamplerState smp : register(s8);\n"
    "float4 ps_main(VSOut i) : SV_Target {\n"
    "    float4 t = tex.Load(int3(int2(i.uv), 0));\n"
    "    return float4(t.rrr, max(t.r, t.g));\n"
    "}\n";

static void sub_fail(const char *what)
{
    s.failed = 1;
    fprintf(stderr, "[SUBTITLE] %s failed; subtitles are off for this run\n", what);
    fflush(stderr);
}

static RhiShader *compile(uint32_t stage, const char *entry, const char *target)
{
    RhiShaderSource src;
    char err[2048];
    RhiShader *sh;

    memset(&src, 0, sizeof src);
    src.hlsl = kShader;
    src.len = sizeof kShader - 1;
    src.name = "subtitle";
    src.entry = entry;
    src.target = target;
    sh = rhi_shader_create(stage, &src, err, sizeof err);
    if (!sh && err[0])
        fprintf(stderr, "[SUBTITLE] %s\n", err);
    return sh;
}

static int create(void)
{
    RhiImageDesc td;
    RhiBufferDesc bd;
    RhiBlendDesc bl;
    RhiDepthDesc ds;
    RhiRasterDesc rd;
    BITMAPINFO bi;

    if (s.failed) return 0;
    if (s.tried) return s.texture != NULL;
    s.tried = 1;
    if (!rhi_device_ready()) { s.failed = 1; return 0; }

    memset(&td, 0, sizeof td);
    td.type = RHI_IMAGE_2D;
    td.width = SUB_W; td.height = SUB_H; td.depth = 1; td.mip_levels = 1;
    td.format = RHI_FORMAT_B8G8R8A8_UNORM;
    td.samples = 1;
    td.usage = RHI_USAGE_DYNAMIC;
    td.bind = RHI_BIND_SAMPLED;
    td.cpu_access = RHI_CPU_WRITE;
    if (!(s.texture = rhi_image_create(&td, NULL))) { sub_fail("CreateTexture2D"); return 0; }
    if (!(s.srv = rhi_view_create(s.texture, RHI_VIEW_SAMPLED, NULL))) { sub_fail("the view"); return 0; }
    memset(&bd, 0, sizeof bd);
    bd.size = sizeof(SubConstants);
    bd.usage = RHI_USAGE_DYNAMIC;
    bd.bind = RHI_BIND_UNIFORM;
    bd.cpu_access = RHI_CPU_WRITE;
    if (!(s.cb = rhi_buffer_create(&bd, NULL))) { sub_fail("CreateBuffer"); return 0; }
    if (!(s.vs = compile(RHI_STAGE_VERTEX, "vs_main", "vs_4_0"))) { sub_fail("the vertex shader"); return 0; }
    if (!(s.ps = compile(RHI_STAGE_PIXEL, "ps_main", "ps_4_0"))) { sub_fail("the pixel shader"); return 0; }
    memset(&bl, 0, sizeof bl);
    bl.enable = 1;
    bl.src = RHI_BLEND_SRC_ALPHA; bl.dst = RHI_BLEND_INV_SRC_ALPHA; bl.op = RHI_BLEND_OP_ADD;
    bl.src_alpha = RHI_BLEND_ONE; bl.dst_alpha = RHI_BLEND_INV_SRC_ALPHA; bl.op_alpha = RHI_BLEND_OP_ADD;
    bl.write_mask = RHI_WRITE_ALL;
    if (!(s.blend = rhi_blend_state_create(&bl))) { sub_fail("CreateBlendState"); return 0; }
    memset(&ds, 0, sizeof ds);
    if (!(s.depth = rhi_depth_state_create(&ds))) { sub_fail("CreateDepthStencilState"); return 0; }
    memset(&rd, 0, sizeof rd);
    rd.fill = RHI_FILL_SOLID; rd.cull = RHI_CULL_NONE; rd.depth_clip = 1;
    if (!(s.raster = rhi_raster_state_create(&rd))) { sub_fail("CreateRasterizerState"); return 0; }

    s.dc = CreateCompatibleDC(NULL);
    if (!s.dc) { sub_fail("CreateCompatibleDC"); return 0; }
    memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = SUB_W;
    bi.bmiHeader.biHeight = -SUB_H;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    s.bitmap = CreateDIBSection(s.dc, &bi, DIB_RGB_COLORS, &s.bits, NULL, 0);
    if (!s.bitmap) { sub_fail("CreateDIBSection"); return 0; }
    SelectObject(s.dc, s.bitmap);
    /* Grey-scale antialiasing: ClearType's coloured fringes would read as
     * outline in the green channel. */
    s.font = CreateFontW(-SUB_FONT_PX, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                         OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                         VARIABLE_PITCH | FF_SWISS, L"Arial");
    if (s.font) SelectObject(s.dc, s.font);
    SetBkMode(s.dc, TRANSPARENT);
    return 1;
}

/* Render line i into the bitmap: outline in green, text in white. */
static void render(int i)
{
    wchar_t wide[1024];
    RECT r, m;
    int n, dx, dy, h;

    n = MultiByteToWideChar(CP_UTF8, 0, s.lines[i].text, -1, wide, 1024);
    if (n <= 0) n = 1;
    memset(s.bits, 0, (size_t)SUB_W * SUB_H * 4);
    SetRect(&m, 0, 0, SUB_W - 16, SUB_H);
    h = DrawTextW(s.dc, wide, -1, &m, DT_CENTER | DT_WORDBREAK | DT_CALCRECT | DT_NOPREFIX);
    if (h > SUB_H - 8) h = SUB_H - 8;
    SetTextColor(s.dc, RGB(0, 255, 0));
    for (dy = -3; dy <= 3; dy++)
        for (dx = -3; dx <= 3; dx++) {
            if (dx * dx + dy * dy > 10 || (!dx && !dy)) continue;
            SetRect(&r, 8 + dx, SUB_H - h - 4 + dy, SUB_W - 8 + dx, SUB_H - 4 + dy);
            DrawTextW(s.dc, wide, -1, &r, DT_CENTER | DT_WORDBREAK | DT_NOPREFIX);
        }
    SetTextColor(s.dc, RGB(255, 255, 255));
    SetRect(&r, 8, SUB_H - h - 4, SUB_W - 8, SUB_H - 4);
    DrawTextW(s.dc, wide, -1, &r, DT_CENTER | DT_WORDBREAK | DT_NOPREFIX);
    GdiFlush();
    rhi_image_update(s.texture, 0, NULL, s.bits, SUB_W * 4, 0);
    s.shown = i;
}

void d3d8_subtitle_draw(void)
{
    RhiOutputState saved;
    RhiView *rtv = d3d8_GetDefaultTargetView();
    RhiViewport vp;
    SubConstants c;
    UINT width = d3d8_GetBackBufferWidth(), height = d3d8_GetBackBufferHeight();
    float blend_factor[4] = { 1, 1, 1, 1 }, w, hh;
    void *mapped;
    int i;

    if (!s.loaded) load();
    if (!s.enabled || !s.count || !rtv || !width || !height)
        return;
    if ((i = current()) < 0 || !create())
        return;
    if (i != s.shown)
        render(i);

    /* 70% of the picture's width, a twentieth up from the bottom, and the
     * aspect kept as the picture is shown: a widescreen frame is this 4:3
     * buffer stretched to 16:9 (d3d8_display_wide_now), which would widen
     * the text with it, so its height is worked out at 16:9. */
    {
        extern int d3d8_display_wide_now(void);
        float shown_aspect = d3d8_display_wide_now() ? 16.0f / 9.0f
                                                     : (float)width / (float)height;
        w = 0.70f * 2.0f;
        hh = w * ((float)SUB_H / (float)SUB_W) * shown_aspect;
    }
    rhi_output_save(&saved);
    if (!(mapped = rhi_buffer_map(s.cb, RHI_MAP_WRITE_DISCARD)))
        goto restore;
    c.x = -w / 2.0f;
    c.y = -1.0f + 0.10f + hh;
    c.w = w;
    c.h = hh;
    c.tex_w = (float)SUB_W;
    c.tex_h = (float)SUB_H;
    c.pad0 = c.pad1 = 0.0f;
    memcpy(mapped, &c, sizeof c);
    rhi_buffer_unmap(s.cb);

    vp.x = vp.y = 0.0f;
    vp.width = (float)width;
    vp.height = (float)height;
    vp.min_depth = 0.0f;
    vp.max_depth = 1.0f;
    rhi_set_render_target(rtv, NULL);
    rhi_set_viewports(1, &vp);
    rhi_set_vertex_layout(NULL);
    rhi_set_topology(RHI_TOPOLOGY_TRIANGLE_STRIP);
    rhi_set_shader(RHI_STAGE_VERTEX, s.vs);
    rhi_set_shader(RHI_STAGE_PIXEL, s.ps);
    rhi_set_uniform_buffers(RHI_STAGE_VERTEX, 7, 1, &s.cb);
    rhi_set_uniform_buffers(RHI_STAGE_PIXEL, 7, 1, &s.cb);
    rhi_set_textures(8, 1, &s.srv);
    rhi_set_blend_state(s.blend, blend_factor, 0xFFFFFFFF);
    rhi_set_depth_state(s.depth, 0);
    rhi_set_raster_state(s.raster);
    rhi_draw(4, 0);
restore:
    rhi_output_restore(&saved);
}

#else  /* not Windows */
void d3d8_subtitle_audio_packet(const void *data, uint32_t size) { (void)data; (void)size; }
void d3d8_subtitle_draw(void) {}
#endif
