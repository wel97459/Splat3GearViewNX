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

// icon cache: the 3 worn icons decoded once per loadout change
static struct {
    uint16_t id;
    uint8_t* rgba; // 128*128*4
} g_icons[3];

// skill icon cache (abilities 0..26), drawn downscaled
#define SKILL_CACHE 27
typedef struct { int id; uint8_t* rgba; } SkillIcon;
static SkillIcon g_skill_icons[SKILL_CACHE];

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
    g_fbw = FB_W;
    g_ui_ok = true;

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
    for (int i = 0; i < SKILL_CACHE; i++) {
        if (g_skill_icons[i].rgba) { stbi_image_free(g_skill_icons[i].rgba); g_skill_icons[i].rgba = NULL; }
        g_skill_icons[i].id = -1;
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

// glyph cache: key = (codepoint, pixel size)
#define GLYPH_CACHE 512
typedef struct { uint32_t cp; int size; int w, h, xoff, yoff, adv; uint8_t* bits; } Glyph;
static Glyph g_gcache[GLYPH_CACHE];
static int g_gcache_n = 0;

static const uint8_t* glyph_for(uint32_t cp, int px, int* w, int* h, int* xoff, int* yoff, int* adv) {
    for (int i = 0; i < g_gcache_n; i++)
        if (g_gcache[i].cp == cp && g_gcache[i].size == px) {
            *w = g_gcache[i].w; *h = g_gcache[i].h;
            *xoff = g_gcache[i].xoff; *yoff = g_gcache[i].yoff; *adv = g_gcache[i].adv;
            return g_gcache[i].bits;
        }
    if (g_gcache_n >= GLYPH_CACHE) {
        for (int i = 0; i < g_gcache_n; i++) free(g_gcache[i].bits);
        g_gcache_n = 0;
    }
    float scale = stbtt_ScaleForPixelHeight(&g_font, (float)px);
    int gw = 0, gh = 0, gx = 0, gy = 0, ga = 0;
    uint8_t* bits = stbtt_GetCodepointBitmap(&g_font, 0, scale, (int)cp, &gw, &gh, &gx, &gy);
    stbtt_GetCodepointHMetrics(&g_font, (int)cp, &ga, 0);
    int idx = g_gcache_n++;
    g_gcache[idx].cp = cp; g_gcache[idx].size = px;
    g_gcache[idx].w = gw; g_gcache[idx].h = gh;
    g_gcache[idx].xoff = gx; g_gcache[idx].yoff = gy;
    g_gcache[idx].adv = (int)(ga * scale);
    g_gcache[idx].bits = bits;
    *w = gw; *h = gh; *xoff = gx; *yoff = gy; *adv = g_gcache[idx].adv;
    return bits;
}

static void draw_text_sized(uint32_t* fb, int x, int y, uint32_t color, int px, const char* str) {
    if (!g_font_ok) return;
    float scale = stbtt_ScaleForPixelHeight(&g_font, (float)px);
    int ascent = 0, descent = 0, linegap = 0;
    stbtt_GetFontVMetrics(&g_font, &ascent, &descent, &linegap);
    int base = y + (int)(ascent * scale);
    while (*str) {
        uint32_t cp = 0;
        ssize_t n = decode_utf8(&cp, (const uint8_t*)str);
        if (n <= 0) break;
        str += n;
        if (cp == '\n') break;
        if (cp == ' ') { x += (int)(0.55f * px); continue; }
        int w, h, xo, yo, adv;
        const uint8_t* bits = glyph_for(cp, px, &w, &h, &xo, &yo, &adv);
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

static int text_width_sized(const char* str, int px) {
    if (!g_font_ok) return 0;
    float scale = stbtt_ScaleForPixelHeight(&g_font, (float)px);
    int x = 0;
    while (*str) {
        uint32_t cp = 0;
        ssize_t n = decode_utf8(&cp, (const uint8_t*)str);
        if (n <= 0) break;
        str += n;
        if (cp == ' ') { x += (int)(0.55f * px); continue; }
        int ga = 0;
        stbtt_GetCodepointHMetrics(&g_font, (int)cp, &ga, 0);
        x += (int)(ga * scale);
    }
    return x;
}

static void draw_skill(uint32_t* fb, int x, int y, int ability_id, int size) {
    if (ability_id < 0 || ability_id >= SKILL_CACHE) return;
    SkillIcon* si = &g_skill_icons[ability_id];
    if (!si->rgba) {
        char path[64];
        snprintf(path, sizeof(path), "romfs:/skill/%d.png", ability_id);
        int w, h, ch;
        si->rgba = stbi_load(path, &w, &h, &ch, 4);
        si->id = ability_id;
        if (!si->rgba) app_log("skill icon load failed: %s", path);
    }
    if (!si->rgba) return;
    // downscale 126x126 -> size x size, nearest sampling
    const int src = 126;
    for (int j = 0; j < size; j++) {
        int yy = y + j;
        if (yy < 0 || yy >= FB_H) continue;
        int sj = j * src / size;
        for (int i = 0; i < size; i++) {
            int xx = x + i;
            if (xx < 0 || xx >= (int)g_fbw) continue;
            int s_i = i * src / size;
            const uint8_t* p = si->rgba + (sj * src + s_i) * 4;
            uint32_t* d = &fb[yy * g_fbw + xx];
            *d = blend(*d, p[0], p[1], p[2], p[3]);
        }
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

// clip text to max pixels with ellipsis
static void clip_text(char* out, size_t outsz, const char* str, int px, int maxw) {
    if (text_width_sized(str, px) <= maxw) {
        snprintf(out, outsz, "%s", str);
        return;
    }
    snprintf(out, outsz, "%s", str);
    size_t len = strlen(out);
    while (len > 1 && text_width_sized(out, px) + text_width_sized("...", px) > maxw) {
        out[--len] = '\0';
    }
    snprintf(out + len, outsz - len, "...");
}

void ui_draw(const char* status) {
    load_icons();
    uint32_t* fb = fb_begin();
    if (!fb) return;
    fill(fb, RGBA8_MAXALPHA(0x14, 0x16, 0x1c));

    draw_text_sized(fb, 40, 24, RGBA8_MAXALPHA(0x6e, 0xe7, 0x60), 34, "Splat3GearView NX");
    if (status && status[0])
        draw_text_sized(fb, 480, 32, RGBA8_MAXALPHA(0x9a, 0xa3, 0xb5), 22, status);

    // 3 worn cards
    static const char* LN[3] = { "Head", "Clothes", "Shoes" };
    int card_x[3] = { 55, 455, 855 };
    int card_w = 370, card_h = 460;
    for (int t = 0; t < 3; t++) {
        int x = card_x[t], y = 110;
        const GearDataEntry* e = gear_data_find(t, g_loadout[t]);
        const GearInfo* gi = &g_worn_info[t];
        for (int j = 0; j < card_h; j++)
            for (int i = 0; i < card_w; i++) {
                int xx = x + i, yy = y + j;
                if (xx < 0 || xx >= (int)g_fbw || yy < 0 || yy >= FB_H) continue;
                fb[yy * g_fbw + xx] = RGBA8_MAXALPHA(0x1e, 0x22, 0x2c);
            }
        int ix = x + (card_w - 128) / 2;
        if (g_icons[t].rgba)
            blit_rgba(fb, ix, y + 14, g_icons[t].rgba, 128, 128);
        int tx = x + 14;
        int maxw = card_w - 28;
        char line[160];
        if (e) {
            draw_text_sized(fb, tx, y + 150, RGBA8_MAXALPHA(0x8f, 0x98, 0xa8), 19, LN[t]);
            clip_text(line, sizeof(line), e->name, 25, maxw);
            draw_text_sized(fb, tx, y + 172, RGBA8_MAXALPHA(0xf2, 0xf5, 0xfa), 25, line);
            clip_text(line, sizeof(line), e->brand, 19, maxw);
            draw_text_sized(fb, tx, y + 202, RGBA8_MAXALPHA(0x9a, 0xa3, 0xb5), 19, line);
        } else {
            draw_text_sized(fb, tx, y + 150, RGBA8_MAXALPHA(0x9a, 0xa3, 0xb5), 24, "- none -");
        }
        if (gi->id) {
            // main ability: icon + name
            draw_skill(fb, tx, y + 232, gi->primary, 34);
            clip_text(line, sizeof(line), ability_name(gi->primary), 19, maxw - 42);
            draw_text_sized(fb, tx + 42, y + 240, RGBA8_MAXALPHA(0xd8, 0xdc, 0xe4), 19, line);
            // subs: icon + name per row
            for (int s = 0; s < 3; s++) {
                int sy = y + 272 + s * 42;
                draw_skill(fb, tx, sy, gi->subs[s], 30);
                clip_text(line, sizeof(line), ability_name(gi->subs[s]), 19, maxw - 40);
                draw_text_sized(fb, tx + 40, sy + 6, RGBA8_MAXALPHA(0xb8, 0xc0, 0xcc), 19, line);
            }
            // meta row
            snprintf(line, sizeof(line), "Seed %08X   Rarity %u", gi->seed, gi->rarity);
            draw_text_sized(fb, tx, y + 410, RGBA8_MAXALPHA(0x8f, 0x98, 0xa8), 19, line);
        }
    }

    draw_text_sized(fb, 40, 655, RGBA8_MAXALPHA(0x55, 0x5d, 0x6b), 24,
              "A: read worn   Y: raw dump   B: exit");

    fb_end(fb);
}
