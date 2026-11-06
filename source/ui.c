#include <switch.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>

#include "ui.h"
#include "i18n.h"

#define UI_W 1280
#define UI_H 720
#define UI_FONT_PX 20
#define UI_LINE_H 22
#define UI_TEXT_X 24
#define UI_TEXT_Y 64
#define UI_TEXT_W (UI_W - UI_TEXT_X * 2)
#define UI_TEXT_ROWS ((UI_H - UI_TEXT_Y - 44) / UI_LINE_H)
#define UI_MAX_LINES 128
#define UI_MAX_SEGS 32
#define UI_SEG_BYTES 512

#define UMIN(a, b) ((a) < (b) ? (a) : (b))
#define UMAX(a, b) ((a) > (b) ? (a) : (b))

typedef struct {
    u8 r, g, b, a;
} Col;

typedef struct {
    char text[UI_SEG_BYTES];
    Col color;
    bool bold;
    bool dim;
    bool reverse;
} UiSeg;

typedef struct {
    UiSeg segs[UI_MAX_SEGS];
    int nseg;
} UiLine;

#define GLYPH_MAX 4096
typedef struct {
    u32 cp;
    int w, h, left, top, adv;
    u8 *bm;
    bool used;
} Glyph;

static Glyph g_glyphs[GLYPH_MAX];

static UiLine g_lines[UI_MAX_LINES];
static int g_row;
static Col g_cur;
static bool g_bold, g_dim, g_reverse;

static Framebuffer g_fb;
static bool g_fb_ok;
static u8 *g_buf;
static u32 g_stride;
static FT_Library g_ft;
static FT_Face g_face;

static bool g_in_frame;
static bool g_just_framed;
static const char *g_footer;
static const char *g_footer2;
static const char *g_page_title;

static const Col C_DEFAULT = {228, 228, 230, 255};
static const Col C_BLACK = {0, 0, 0, 255};
static const Col C_WHITE = {255, 255, 255, 255};

static Col style_color(UiStyle style)
{
    switch (style) {
        case UI_DIM: return (Col){150, 156, 170, 255};
        case UI_ACCENT: return (Col){76, 194, 255, 255};
        case UI_GOOD: return (Col){90, 208, 122, 255};
        case UI_BAD: return (Col){255, 107, 107, 255};
        case UI_TITLE: return (Col){255, 255, 255, 255};
        case UI_NORMAL:
        default: return C_DEFAULT;
    }
}

static Col style_fill(UiStyle style)
{
    switch (style) {
        case UI_PANEL: return (Col){22, 26, 34, 255};
        case UI_BORDER: return (Col){42, 48, 64, 255};
        case UI_SELECTED: return (Col){31, 58, 95, 255};
        case UI_ACCENT: return (Col){76, 194, 255, 255};
        case UI_GOOD: return (Col){90, 208, 122, 255};
        case UI_BAD: return (Col){255, 107, 107, 255};
        case UI_DIM: return (Col){60, 64, 74, 255};
        default: return (Col){30, 34, 44, 255};
    }
}

static void ui_logf(const char *fmt, ...)
{
    FILE *f = fopen("sdmc:/PlayLog/ui.log", "a");
    if (!f)
        return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

static inline void put_px(int x, int y, Col c)
{
    if (!g_buf || x < 0 || y < 0 || x >= UI_W || y >= UI_H)
        return;
    u32 *p = (u32 *)(g_buf + (size_t)y * g_stride + (size_t)x * 4);
    *p = (u32)c.r | ((u32)c.g << 8) | ((u32)c.b << 16) | ((u32)c.a << 24);
}

static void fill_rect(int x, int y, int w, int h, Col c)
{
    if (!g_buf || w <= 0 || h <= 0)
        return;
    int x0 = UMAX(0, x), y0 = UMAX(0, y);
    int x1 = UMIN(UI_W, x + w), y1 = UMIN(UI_H, y + h);
    u32 v = (u32)c.r | ((u32)c.g << 8) | ((u32)c.b << 16) | ((u32)c.a << 24);
    for (int yy = y0; yy < y1; yy++) {
        u32 *p = (u32 *)(g_buf + (size_t)yy * g_stride);
        for (int xx = x0; xx < x1; xx++)
            p[xx] = v;
    }
}

static int utf8_decode(const unsigned char *p, u32 *cp)
{
    unsigned char c = p[0];
    if (c < 0x80) { *cp = c; return 1; }
    if ((c & 0xE0) == 0xC0 && p[1]) { *cp = ((c & 0x1F) << 6) | (p[1] & 0x3F); return 2; }
    if ((c & 0xF0) == 0xE0 && p[1] && p[2]) {
        *cp = ((c & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F);
        return 3;
    }
    if ((c & 0xF8) == 0xF0 && p[1] && p[2] && p[3]) {
        *cp = ((c & 0x07) << 18) | ((p[1] & 0x3F) << 12) | ((p[2] & 0x3F) << 6) | (p[3] & 0x3F);
        return 4;
    }
    *cp = c;
    return 1;
}

static Glyph *glyph_get(u32 cp)
{
    int base = (int)(cp % GLYPH_MAX);
    for (int i = 0; i < GLYPH_MAX; i++) {
        int k = (base + i) % GLYPH_MAX;
        if (g_glyphs[k].used) {
            if (g_glyphs[k].cp == cp)
                return &g_glyphs[k];
            continue;
        }
        if (FT_Load_Char(g_face, cp, FT_LOAD_RENDER) != 0)
            return NULL;
        FT_GlyphSlot g = g_face->glyph;
        Glyph *gl = &g_glyphs[k];
        memset(gl, 0, sizeof(*gl));
        gl->cp = cp;
        gl->w = (int)g->bitmap.width;
        gl->h = (int)g->bitmap.rows;
        gl->left = g->bitmap_left;
        gl->top = g->bitmap_top;
        gl->adv = g->advance.x >> 6;
        if (gl->w > 0 && gl->h > 0) {
            gl->bm = malloc((size_t)gl->w * gl->h);
            if (gl->bm) {
                for (int r = 0; r < gl->h; r++)
                    memcpy(gl->bm + (size_t)r * gl->w,
                           g->bitmap.buffer + (size_t)r * g->bitmap.pitch, (size_t)gl->w);
            }
        }
        gl->used = true;
        return gl;
    }
    return NULL;
}

static void draw_text_fb(int x, int y, int max_w, const char *text, Col col)
{
    if (!g_fb_ok || !g_face || !text || !text[0])
        return;

    const unsigned char *p = (const unsigned char *)text;
    int start = x;
    while (*p) {
        u32 cp;
        int n = utf8_decode(p, &cp);
        if (cp == '\n' || cp == '\r') { p += n; continue; }
        if (cp == '\t') {
            for (int i = 0; i < 4; i++) {
                if (FT_Load_Char(g_face, ' ', FT_LOAD_RENDER) == 0)
                    x += g_face->glyph->advance.x >> 6;
            }
            p += n;
            continue;
        }
        Glyph *gl = glyph_get(cp);
        if (!gl) { p += n; continue; }

        int adv = gl->adv;
        if (max_w > 0 && (x - start) + adv > max_w)
            break;

        int bx = x + gl->left;
        int by = y + (UI_FONT_PX - gl->top);
        for (int row = 0; row < gl->h; row++) {
            for (int cx = 0; cx < gl->w; cx++) {
                u8 a = gl->bm ? gl->bm[(size_t)row * gl->w + cx] : 0;
                if (!a)
                    continue;
                int dx = bx + (int)cx, dy = by + (int)row;
                if (dx < 0 || dy < 0 || dx >= UI_W || dy >= UI_H)
                    continue;
                u32 *dstp = (u32 *)(g_buf + (size_t)dy * g_stride + (size_t)dx * 4);
                u32 d = *dstp;
                int dr = d & 0xFF, dg = (d >> 8) & 0xFF, db = (d >> 16) & 0xFF;
                int r = (col.r * a + dr * (255 - a)) / 255;
                int gg = (col.g * a + dg * (255 - a)) / 255;
                int bb = (col.b * a + db * (255 - a)) / 255;
                *dstp = (u32)r | ((u32)gg << 8) | ((u32)bb << 16) | (0xFFu << 24);
            }
        }
        x += adv;
        p += n;
    }
}

static void reset_attrs(void)
{
    g_cur = C_DEFAULT;
    g_bold = false;
    g_dim = false;
    g_reverse = false;
}

static void apply_sgr(const char *params)
{
    if (!params || !params[0]) {
        reset_attrs();
        return;
    }
    const char *p = params;
    while (*p) {
        int v = atoi(p);
        while (*p && *p != ';') p++;
        if (*p == ';') p++;
        switch (v) {
            case 0: reset_attrs(); break;
            case 1: g_bold = true; break;
            case 2: g_dim = true; break;
            case 7: g_reverse = true; break;
            case 22: g_bold = false; break;
            case 30: g_cur = C_BLACK; break;
            case 31: g_cur = (Col){220, 80, 80, 255}; break;
            case 32: g_cur = (Col){90, 200, 110, 255}; break;
            case 33: g_cur = (Col){225, 190, 80, 255}; break;
            case 34: g_cur = (Col){90, 140, 230, 255}; break;
            case 35: g_cur = (Col){200, 110, 200, 255}; break;
            case 36: g_cur = (Col){80, 200, 210, 255}; break;
            case 37: g_cur = (Col){210, 210, 210, 255}; break;
            case 90: g_cur = (Col){120, 120, 120, 255}; break;
            case 91: g_cur = (Col){255, 110, 110, 255}; break;
            case 92: g_cur = (Col){120, 230, 140, 255}; break;
            case 93: g_cur = (Col){250, 220, 110, 255}; break;
            case 94: g_cur = (Col){120, 170, 255, 255}; break;
            case 95: g_cur = (Col){230, 140, 230, 255}; break;
            case 96: g_cur = (Col){110, 230, 235, 255}; break;
            case 97: g_cur = (Col){245, 245, 245, 255}; break;
            default: break;
        }
    }
}

static void new_line(void)
{
    g_row++;
    if (g_row >= UI_MAX_LINES) {
        memmove(&g_lines[0], &g_lines[1], sizeof(UiLine) * (UI_MAX_LINES - 1));
        memset(&g_lines[UI_MAX_LINES - 1], 0, sizeof(UiLine));
        g_row = UI_MAX_LINES - 1;
    }
}

static void append_bytes(const char *s, size_t n)
{
    if (n == 0)
        return;
    UiLine *line = &g_lines[g_row];
    UiSeg *seg = (line->nseg > 0) ? &line->segs[line->nseg - 1] : NULL;
    if (!seg || seg->color.r != g_cur.r || seg->color.g != g_cur.g ||
        seg->color.b != g_cur.b || seg->bold != g_bold || seg->dim != g_dim ||
        seg->reverse != g_reverse || strlen(seg->text) + 8 >= UI_SEG_BYTES) {
        if (line->nseg >= UI_MAX_SEGS)
            return;
        seg = &line->segs[line->nseg++];
        memset(seg, 0, sizeof(*seg));
        seg->color = g_cur;
        seg->bold = g_bold;
        seg->dim = g_dim;
        seg->reverse = g_reverse;
    }
    size_t cur = strlen(seg->text);
    size_t can = UI_SEG_BYTES - 1 - cur;
    if (n > can) n = can;
    memcpy(seg->text + cur, s, n);
    seg->text[cur + n] = '\0';
}

static void append_text(const char *s)
{
    size_t i = 0;
    while (s[i]) {
        unsigned char c = (unsigned char)s[i];
        if (c == 0x1b) {
            if (s[i + 1] == '[') {
                size_t j = i + 2;
                while (s[j] && !(s[j] >= '@' && s[j] <= '~')) j++;
                if (s[j] == 'm') {
                    char params[64];
                    size_t len = j - (i + 2);
                    if (len >= sizeof(params)) len = sizeof(params) - 1;
                    memcpy(params, s + i + 2, len);
                    params[len] = '\0';
                    apply_sgr(params);
                }
                i = s[j] ? j + 1 : j;
                continue;
            }
            i++;
            continue;
        }
        if (c == '\n') { new_line(); i++; continue; }
        if (c == '\r') { i++; continue; }
        if (c == '\t') { append_bytes("    ", 4); i++; continue; }

        size_t n = 1;
        if ((c & 0xE0) == 0xC0) n = 2;
        else if ((c & 0xF0) == 0xE0) n = 3;
        else if ((c & 0xF8) == 0xF0) n = 4;
        size_t avail = 1;
        while (avail < n && s[i + avail]) avail++;
        if (avail < n) n = 1;
        append_bytes(s + i, n);
        i += n;
    }
}

void uiFill(int x, int y, int w, int h, UiStyle style)
{
    fill_rect(x, y, w, h, style_fill(style));
}

void uiPanel(int x, int y, int w, int h)
{
    uiFill(x, y, w, h, UI_PANEL);
    uiFill(x, y, w, 1, UI_BORDER);
    uiFill(x, y + h - 1, w, 1, UI_BORDER);
    uiFill(x, y, 1, h, UI_BORDER);
    uiFill(x + w - 1, y, 1, h, UI_BORDER);
}

void uiTextAt(int x, int y, int max_w, const char *text, UiStyle style)
{
    draw_text_fb(x, y, max_w, text, style_color(style));
}

int uiTextW(const char *text, UiStyle style)
{
    if (!g_face || !text || !text[0])
        return 0;
    int w = 0;
    const unsigned char *p = (const unsigned char *)text;
    while (*p) {
        u32 cp;
        int n = utf8_decode(p, &cp);
        if (FT_Load_Char(g_face, cp, FT_LOAD_DEFAULT) == 0)
            w += g_face->glyph->advance.x >> 6;
        p += n;
    }
    return w;
}

void uiSetFooter(const char *text) { g_footer = text; }
void uiSetFooter2(const char *text) { g_footer2 = text; }
void uiSetPageTitle(const char *title) { g_page_title = title; }

void uiFrameBegin(const char *title, const char *subtitle)
{
    if (!g_fb_ok)
        return;
    g_buf = framebufferBegin(&g_fb, &g_stride);
    g_in_frame = true;
    g_just_framed = false;

    fill_rect(0, 0, UI_W, UI_H, (Col){13, 15, 20, 255});
    fill_rect(0, 0, UI_W, 48, style_fill(UI_PANEL));
    fill_rect(0, 47, UI_W, 1, style_fill(UI_BORDER));
    uiTextAt(20, 12, 900, title ? title : "PlayLog", UI_TITLE);
    if (subtitle && subtitle[0]) {
        int w = uiTextW(subtitle, UI_DIM);
        int x = UI_W - 20 - w;
        if (x > 20)
            uiTextAt(x, 15, w + 4, subtitle, UI_DIM);
    }
}

void uiFrameEnd(void)
{
    if (!g_fb_ok)
        return;
    if (!g_in_frame)
        return;

    fill_rect(0, UI_H - 40, UI_W, 40, style_fill(UI_PANEL));
    fill_rect(0, UI_H - 41, UI_W, 1, style_fill(UI_BORDER));
    if (g_footer && g_footer[0])
        uiTextAt(20, UI_H - 40 + 10, UI_W - 40, g_footer, UI_DIM);

    framebufferEnd(&g_fb);
    g_in_frame = false;
    g_just_framed = true;
}

void uiInit(void)
{
    reset_attrs();
    g_row = 0;
    memset(g_lines, 0, sizeof(g_lines));
    g_footer = NULL;
    g_page_title = NULL;

    ui_logf("uiInit start");

    Result rc = framebufferCreate(&g_fb, nwindowGetDefault(), UI_W, UI_H,
                                  PIXEL_FORMAT_RGBA_8888, 2);
    if (R_FAILED(rc)) {
        ui_logf("framebufferCreate failed 0x%x", rc);
        return;
    }
    rc = framebufferMakeLinear(&g_fb);
    if (R_FAILED(rc)) {
        ui_logf("framebufferMakeLinear failed 0x%x", rc);
        framebufferClose(&g_fb);
        return;
    }
    g_fb_ok = true;
    ui_logf("framebuffer ok");

    if (FT_Init_FreeType(&g_ft) != 0) {
        ui_logf("FT_Init_FreeType failed");
        return;
    }

    static const char *paths[] = {
        "sdmc:/PlayLog/font.otf",
        "sdmc:/PlayLog/font.ttf",
        "sdmc:/PlayLog/PlayLogFont.otf",
        "sdmc:/switch/PlayLogFont.otf",
    };
    for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
        if (FT_New_Face(g_ft, paths[i], 0, &g_face) == 0) {
            FT_Set_Pixel_Sizes(g_face, 0, UI_FONT_PX);
            ui_logf("font loaded: %s", paths[i]);
            break;
        }
    }
    if (!g_face)
        ui_logf("font load failed");
}

void uiExit(void)
{
    for (int i = 0; i < GLYPH_MAX; i++) {
        if (g_glyphs[i].bm) {
            free(g_glyphs[i].bm);
            g_glyphs[i].bm = NULL;
        }
        g_glyphs[i].used = false;
    }
    if (g_face) {
        FT_Done_Face(g_face);
        g_face = NULL;
    }
    if (g_ft) {
        FT_Done_FreeType(g_ft);
        g_ft = NULL;
    }
    if (g_fb_ok) {
        framebufferClose(&g_fb);
        g_fb_ok = false;
    }
}

void uiClear(void)
{
    memset(g_lines, 0, sizeof(g_lines));
    g_row = 0;
    reset_attrs();
    g_footer = NULL;
    g_footer2 = NULL;
    g_just_framed = false;
}

void uiPrintf(const char *fmt, ...)
{
    char buf[4096];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    append_text(buf);
}

static void draw_text_lines(void)
{
    int total = g_row + 1;
    int first = 0;
    if (total > UI_TEXT_ROWS)
        first = total - UI_TEXT_ROWS;

    for (int r = first; r < total; r++) {
        int y = UI_TEXT_Y + (r - first) * UI_LINE_H;
        if (y + UI_LINE_H > UI_H - 42)
            break;
        int x = UI_TEXT_X;
        UiLine *line = &g_lines[r];
        for (int si = 0; si < line->nseg; si++) {
            UiSeg *seg = &line->segs[si];
            if (!seg->text[0])
                continue;
            Col col = seg->color;
            if (seg->dim) {
                col.r = (u8)(col.r * 55 / 100);
                col.g = (u8)(col.g * 55 / 100);
                col.b = (u8)(col.b * 55 / 100);
            }
            int tw = uiTextW(seg->text, UI_NORMAL);
            if (seg->reverse) {
                fill_rect(x, y - 2, tw, UI_LINE_H, style_fill(UI_SELECTED));
                col = C_WHITE;
            }
            draw_text_fb(x, y, UI_TEXT_X + UI_TEXT_W - x, seg->text, col);
            x += tw;
            if (x >= UI_TEXT_X + UI_TEXT_W)
                break;
        }
    }
}

void uiUpdate(void)
{
    if (!g_fb_ok)
        return;
    if (g_just_framed) {
        g_just_framed = false;
        return;
    }
    if (g_in_frame)
        return;

    uiFrameBegin(g_page_title ? g_page_title : "PlayLogTool", NULL);
    draw_text_lines();
    if (!g_footer)
        uiSetFooter(tr("B 返回"));
    uiFrameEnd();
}

bool uiMainLoop(void)
{
    svcSleepThread(16000000ULL);
    return appletMainLoop();
}
