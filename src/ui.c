// Framebuffer UI: 720p, shared font via stb_truetype, PNG icons via stb_image.
#include "s3gv.h"
#include "generated/gear_tables.h"
#include <switch.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

#define FB_W 1280
#define FB_H 720

static Framebuffer g_fb;
static bool g_ui_ok = false;
static u32 g_fbw = FB_W;

static stbtt_fontinfo g_font;
static bool g_font_ok = false;
static float g_font_h = 30.0f;

// icon cache: the 3 worn icons decoded once per loadout change
static struct {
    uint16_t id;
    uint8_t* rgba; // 128*128*4
} g_icons[3];

#define RGBA8_GET_R(c) ((c) & 0xFF)
#define RGBA8_GET_G(c) (((c) >> 8) & 0xFF)
#define RGBA8_GET_B(c) (((c) >> 16) & 0xFF)

static inline uint32_t blend(uint32_t dst, uint8_t r, uint8_t g_, uint8_t b, uint8_t a) {
    if (a == 255) return RGBA8_MAXALPHA(r, g_, b);
    if (a == 0) return dst;
    uint32_t dr = RGBA8_GET_R(dst), dg = RGBA8_GET_G(dst), db = RGBA8_GET_B(dst);
    uint32_t na = 255 - a;
    uint32_t rr = (r * a + dr * na) / 255;
    uint32_t gg = (g_ * a + dg * na) / 255;
    uint32_t bb = (b * a + db * na) / 255;
    return RGBA8_MAXALPHA(rr, gg, bb);
}

int ui_init(void) {
    Result rc = framebufferCreate(&g_fb, nwindowGetDefault(), FB_W, FB_H, PIXEL_FORMAT_RGBA_8888, 2);
    if (R_FAILED(rc)) return -1;
    framebufferMakeLinear(&g_fb);
    g_fbw = FB_W; // linear shadow buffer is FB_W wide; stride checked at fb_begin
    g_ui_ok = true;

    // shared font
    PlFontData font;
    rc = plInitialize(PlServiceType_User);
    if (R_SUCCEEDED(rc)) {
        rc = plGetSharedFontByType(&font, PlSharedFontType_Standard);
        if (R_SUCCEEDED(rc)) {
            int off = stbtt_GetFontOffsetForIndex(font.address, 0);
            if (off >= 0 && stbtt_InitFont(&g_font, font.address, off) != 0)
                g_font_ok = true;
        }
        // keep pl initialized (font address must stay valid)
    }
    return 0;
}

void ui_exit(void) {
    for (int i = 0; i < 3; i++) {
        if (g_icons[i].rgba) { stbi_image_free(g_icons[i].rgba); g_icons[i].rgba = NULL; }
        g_icons[i].id = 0;
    }
    if (g_ui_ok) {
        framebufferClose(&g_fb);
        g_ui_ok = false;
    }
    plExit();
}

static uint32_t* fb_begin(void) {
    if (!g_ui_ok) return NULL;
    u32 stride;
    return (uint32_t*)framebufferBegin(&g_fb, &stride);
}

static void fb_end(uint32_t* fb) {
    if (fb) framebufferEnd(&g_fb);
}

static void fill(uint32_t* fb, uint32_t color) {
    for (u32 i = 0; i < g_fbw * FB_H; i++) fb[i] = color;
}

static void blit_rgba(uint32_t* fb, int x, int y, const uint8_t* px, int w, int h) {
    for (int j = 0; j < h; j++) {
        int yy = y + j;
        if (yy < 0 || yy >= FB_H) continue;
        for (int i = 0; i < w; i++) {
            int xx = x + i;
            if (xx < 0 || xx >= (int)g_fbw) continue;
            const uint8_t* p = px + (j * w + i) * 4;
            uint32_t* d = &fb[yy * g_fbw + xx];
            *d = blend(*d, p[0], p[1], p[2], p[3]);
        }
    }
}

// glyph cache (one entry per unicode codepoint at current size)
#define GLYPH_CACHE 256
static struct { uint32_t cp; int w, h, xoff, yoff, adv; uint8_t* bits; } g_gcache[GLYPH_CACHE];
static int g_gcache_n = 0;

static const uint8_t* glyph_for(uint32_t cp, int* w, int* h, int* xoff, int* yoff, int* adv) {
    for (int i = 0; i < g_gcache_n; i++)
        if (g_gcache[i].cp == cp) {
            *w = g_gcache[i].w; *h = g_gcache[i].h;
            *xoff = g_gcache[i].xoff; *yoff = g_gcache[i].yoff; *adv = g_gcache[i].adv;
            return g_gcache[i].bits;
        }
    if (g_gcache_n >= GLYPH_CACHE) { // reset cache
        for (int i = 0; i < g_gcache_n; i++) free(g_gcache[i].bits);
        g_gcache_n = 0;
    }
    float scale = stbtt_ScaleForPixelHeight(&g_font, g_font_h);
    int gw = 0, gh = 0, gx = 0, gy = 0, ga = 0;
    uint8_t* bits = stbtt_GetCodepointBitmap(&g_font, 0, scale, (int)cp, &gw, &gh, &gx, &gy);
    stbtt_GetCodepointHMetrics(&g_font, (int)cp, &ga, 0);
    int idx = g_gcache_n++;
    g_gcache[idx].cp = cp;
    g_gcache[idx].w = gw; g_gcache[idx].h = gh;
    g_gcache[idx].xoff = gx; g_gcache[idx].yoff = gy;
    g_gcache[idx].adv = (int)(ga * scale);
    g_gcache[idx].bits = bits;
    *w = gw; *h = gh; *xoff = gx; *yoff = gy; *adv = g_gcache[idx].adv;
    return bits;
}

static void draw_text(uint32_t* fb, int x, int y, uint32_t color, const char* str) {
    if (!g_font_ok) return;
    float scale = stbtt_ScaleForPixelHeight(&g_font, g_font_h);
    int ascent = 0, descent = 0, linegap = 0;
    stbtt_GetFontVMetrics(&g_font, &ascent, &descent, &linegap);
    int base = y + (int)(ascent * scale);
    while (*str) {
        uint32_t cp = 0;
        ssize_t n = decode_utf8(&cp, (const uint8_t*)str);
        if (n <= 0) break;
        str += n;
        if (cp == '\n') break;
        if (cp == ' ') { x += (int)(0.55f * g_font_h); continue; }
        int w, h, xo, yo, adv;
        const uint8_t* bits = glyph_for(cp, &w, &h, &xo, &yo, &adv);
        if (bits) {
            uint8_t cr = RGBA8_GET_R(color), cg = RGBA8_GET_G(color), cb = RGBA8_GET_B(color);
            for (int j = 0; j < h; j++) {
                int yy = base + yo + j;
                if (yy < 0 || yy >= FB_H) continue;
                for (int i = 0; i < w; i++) {
                    int xx = x + xo + i;
                    if (xx < 0 || xx >= (int)g_fbw) continue;
                    uint32_t* d = &fb[yy * g_fbw + xx];
                    *d = blend(*d, cr, cg, cb, bits[j * w + i]);
                }
            }
        }
        x += adv;
    }
}

static void load_icons(void) {
    for (int t = 0; t < 3; t++) {
        uint16_t id = g_loadout[t];
        if (g_icons[t].id == id && g_icons[t].rgba) continue;
        if (g_icons[t].rgba) { stbi_image_free(g_icons[t].rgba); g_icons[t].rgba = NULL; }
        g_icons[t].id = id;
        if (!id) continue;
        const GearDataEntry* e = gear_data_find(t, id);
        if (!e) continue;
        int w, h, ch;
        uint8_t* px = stbi_load(e->icon, &w, &h, &ch, 4);
        if (px) { g_icons[t].rgba = px; }
        else app_log("icon load failed: %s", e->icon);
    }
}

void ui_draw(const char* status) {
    load_icons();
    uint32_t* fb = fb_begin();
    if (!fb) return;
    fill(fb, RGBA8_MAXALPHA(0x14, 0x16, 0x1c));

    draw_text(fb, 40, 28, RGBA8_MAXALPHA(0x6e, 0xe7, 0x60), "Splat3GearView NX");

    // 3 worn cards
    static const char* LN[3] = { "Head", "Clothes", "Shoes" };
    int card_x[3] = { 60, 470, 880 };
    for (int t = 0; t < 3; t++) {
        int x = card_x[t], y = 140;
        const GearDataEntry* e = gear_data_find(t, g_loadout[t]);
        // card bg
        for (int j = -70; j < 260; j++)
            for (int i = -30; i < 300; i++) {
                int xx = x + i, yy = y + j;
                if (xx < 0 || xx >= (int)g_fbw || yy < 0 || yy >= FB_H) continue;
                fb[yy * g_fbw + xx] = RGBA8_MAXALPHA(0x1e, 0x22, 0x2c);
            }
        if (g_icons[t].rgba)
            blit_rgba(fb, x, y, g_icons[t].rgba, 128, 128);
        if (e) {
            char line[128];
            snprintf(line, sizeof(line), "%s: %s", LN[t], e->name);
            draw_text(fb, x - 20, y + 150, RGBA8_MAXALPHA(0xf2, 0xf5, 0xfa), line);
            draw_text(fb, x - 20, y + 186, RGBA8_MAXALPHA(0x9a, 0xa3, 0xb5), e->brand);
            const GearInfo* gi = &g_worn_info[t];
            if (gi->id) {
                // abilities
                char main_line[64], sub_line[96], meta_line[64];
                snprintf(main_line, sizeof(main_line), "Main: %s", ability_name(gi->primary));
                snprintf(sub_line, sizeof(sub_line), "Subs: %s / %s / %s",
                         ability_name(gi->subs[0]), ability_name(gi->subs[1]), ability_name(gi->subs[2]));
                snprintf(meta_line, sizeof(meta_line), "Rarity %u  Seed %08X  Exp %u",
                         gi->rarity, gi->seed, gi->exp);
                draw_text(fb, x - 20, y + 218, RGBA8_MAXALPHA(0xd8, 0xdc, 0xe4), main_line);
                draw_text(fb, x - 20, y + 248, RGBA8_MAXALPHA(0xb8, 0xc0, 0xcc), sub_line);
                draw_text(fb, x - 20, y + 278, RGBA8_MAXALPHA(0x8f, 0x98, 0xa8), meta_line);
            }
        } else {
            draw_text(fb, x, y + 150, RGBA8_MAXALPHA(0x9a, 0xa3, 0xb5), "- none -");
        }
    }

    draw_text(fb, 40, 660, RGBA8_MAXALPHA(0x55, 0x5d, 0x6b),
              "A: read worn   Y: raw dump   B: exit");

    fb_end(fb);
}
