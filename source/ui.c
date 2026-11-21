// SPDX-License-Identifier: MIT
// Minimal framebuffer UI: shapes, shared-font text, and decoded PNG/JPEG icons.
#define STB_IMAGE_IMPLEMENTATION
#define STB_TRUETYPE_IMPLEMENTATION
#include "ui.h"

#include "stb_image.h"
#include "stb_truetype.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define GLYPH_CACHE_SIZE 256

typedef struct {
    unsigned codepoint;
    int pixel_height;
    int width;
    int height;
    int xoff;
    int yoff;
    unsigned char* bitmap;
    bool valid;
} GlyphCacheEntry;

static GlyphCacheEntry g_glyphs[GLYPH_CACHE_SIZE];
static int g_glyph_count;
static int g_glyph_next;

static void glyph_cache_clear(void) {
    for (int i = 0; i < GLYPH_CACHE_SIZE; i++) {
        if (g_glyphs[i].bitmap)
            stbtt_FreeBitmap(g_glyphs[i].bitmap, NULL);
        g_glyphs[i].bitmap = NULL;
        g_glyphs[i].valid = false;
    }
    g_glyph_count = 0;
    g_glyph_next = 0;
}

static const GlyphCacheEntry* glyph_for(Ui* ui, unsigned codepoint, int pixel_height,
                                        float scale) {
    for (int i = 0; i < g_glyph_count; i++) {
        if (g_glyphs[i].valid && g_glyphs[i].codepoint == codepoint &&
            g_glyphs[i].pixel_height == pixel_height)
            return &g_glyphs[i];
    }
    GlyphCacheEntry* slot;
    if (g_glyph_count < GLYPH_CACHE_SIZE) {
        slot = &g_glyphs[g_glyph_count++];
    } else {
        slot = &g_glyphs[g_glyph_next];
        g_glyph_next = (g_glyph_next + 1) % GLYPH_CACHE_SIZE;
        if (slot->bitmap)
            stbtt_FreeBitmap(slot->bitmap, NULL);
    }
    memset(slot, 0, sizeof(*slot));
    slot->codepoint = codepoint;
    slot->pixel_height = pixel_height;
    slot->bitmap = stbtt_GetCodepointBitmap(ui->font, scale, scale, (int)codepoint,
                                            &slot->width, &slot->height,
                                            &slot->xoff, &slot->yoff);
    slot->valid = true;
    return slot;
}

static unsigned utf8_next(const char** cursor) {
    const unsigned char* text = (const unsigned char*)*cursor;
    if (!*text)
        return 0;
    unsigned codepoint;
    int length;
    if (text[0] < 0x80) {
        codepoint = text[0];
        length = 1;
    } else if ((text[0] & 0xe0) == 0xc0) {
        codepoint = text[0] & 0x1f;
        length = 2;
    } else if ((text[0] & 0xf0) == 0xe0) {
        codepoint = text[0] & 0x0f;
        length = 3;
    } else if ((text[0] & 0xf8) == 0xf0) {
        codepoint = text[0] & 0x07;
        length = 4;
    } else {
        (*cursor)++;
        return '?';
    }
    for (int i = 1; i < length; i++) {
        if ((text[i] & 0xc0) != 0x80) {
            (*cursor)++;
            return '?';
        }
        codepoint = (codepoint << 6) | (text[i] & 0x3f);
    }
    *cursor += length;
    return codepoint;
}

static void put_pixel(Ui* ui, int x, int y, u32 color) {
    if (x < 0 || y < 0 || x >= UI_WIDTH || y >= UI_HEIGHT)
        return;
    const u32 alpha = color >> 24;
    if (alpha == 0)
        return;
    u32* destination = &ui->pixels[(u32)y * (ui->stride / 4) + (u32)x];
    if (alpha == 0xff) {
        *destination = color;
        return;
    }
    const u32 old = *destination;
    const u32 old_r = old & 0xff;
    const u32 old_g = (old >> 8) & 0xff;
    const u32 old_b = (old >> 16) & 0xff;
    const u32 new_r = color & 0xff;
    const u32 new_g = (color >> 8) & 0xff;
    const u32 new_b = (color >> 16) & 0xff;
    const u32 inverse = 255 - alpha;
    *destination = RGBA8((new_r * alpha + old_r * inverse) / 255,
                         (new_g * alpha + old_g * inverse) / 255,
                         (new_b * alpha + old_b * inverse) / 255,
                         0xff);
}

int ui_init(Ui* ui, char* err, size_t err_size) {
    glyph_cache_clear();
    memset(ui, 0, sizeof(*ui));
    Result rc = plInitialize(PlServiceType_User);
    if (R_FAILED(rc)) {
        if (err && err_size)
            snprintf(err, err_size, "plInitialize failed 0x%x", rc);
        return -1;
    }
    ui->pl_open = true;
    rc = plGetSharedFontByType(&ui->font_data, PlSharedFontType_Standard);
    if (R_FAILED(rc)) {
        if (err && err_size)
            snprintf(err, err_size, "shared font failed 0x%x", rc);
        ui_exit(ui);
        return -1;
    }
    ui->font = calloc(1, sizeof(stbtt_fontinfo));
    if (!ui->font || !stbtt_InitFont(ui->font, ui->font_data.address, 0)) {
        if (err && err_size)
            snprintf(err, err_size, "cannot parse shared font");
        ui_exit(ui);
        return -1;
    }
    rc = framebufferCreate(&ui->framebuffer, nwindowGetDefault(), UI_WIDTH, UI_HEIGHT,
                           PIXEL_FORMAT_RGBA_8888, 2);
    if (R_FAILED(rc)) {
        if (err && err_size)
            snprintf(err, err_size, "framebufferCreate failed 0x%x", rc);
        ui_exit(ui);
        return -1;
    }
    framebufferMakeLinear(&ui->framebuffer);
    ui->framebuffer_open = true;
    return 0;
}

void ui_exit(Ui* ui) {
    if (ui->framebuffer_open) {
        framebufferClose(&ui->framebuffer);
        ui->framebuffer_open = false;
    }
    free(ui->font);
    ui->font = NULL;
    glyph_cache_clear();
    if (ui->pl_open) {
        plExit();
        ui->pl_open = false;
    }
}

void ui_begin(Ui* ui) {
    ui->pixels = framebufferBegin(&ui->framebuffer, &ui->stride);
    ui->active = ui->pixels != NULL;
}

void ui_end(Ui* ui) {
    if (ui->active) {
        framebufferEnd(&ui->framebuffer);
        ui->active = false;
    }
}

void ui_clear(Ui* ui, u32 color) {
    if (!ui->active)
        return;
    const u32 pitch = ui->stride / 4;
    for (u32 y = 0; y < UI_HEIGHT; y++) {
        for (u32 x = 0; x < UI_WIDTH; x++)
            ui->pixels[y * pitch + x] = color;
    }
}

void ui_rect(Ui* ui, int x, int y, int width, int height, u32 color) {
    for (int row = y; row < y + height; row++) {
        for (int column = x; column < x + width; column++)
            put_pixel(ui, column, row, color);
    }
}

static float font_scale(const Ui* ui, int pixel_height) {
    return stbtt_ScaleForPixelHeight(ui->font, (float)pixel_height);
}

int ui_text_width(Ui* ui, const char* text, int pixel_height) {
    if (!text || !*text)
        return 0;
    const float scale = font_scale(ui, pixel_height);
    const char* cursor = text;
    float width = 0;
    unsigned previous = 0;
    for (;;) {
        const unsigned codepoint = utf8_next(&cursor);
        if (!codepoint)
            break;
        int advance = 0;
        int bearing = 0;
        stbtt_GetCodepointHMetrics(ui->font, (int)codepoint, &advance, &bearing);
        width += advance * scale;
        if (previous)
            width += stbtt_GetCodepointKernAdvance(ui->font, (int)previous, (int)codepoint) * scale;
        previous = codepoint;
    }
    return (int)(width + 0.5f);
}

void ui_text(Ui* ui, int x, int y, const char* text, int pixel_height, u32 color) {
    if (!text || !*text)
        return;
    const float scale = font_scale(ui, pixel_height);
    int ascent = 0;
    int descent = 0;
    int line_gap = 0;
    stbtt_GetFontVMetrics(ui->font, &ascent, &descent, &line_gap);
    const int baseline = y + (int)(ascent * scale + 0.5f);
    const char* cursor = text;
    float pen = (float)x;
    unsigned previous = 0;
    for (;;) {
        const unsigned codepoint = utf8_next(&cursor);
        if (!codepoint)
            break;
        int advance = 0;
        int bearing = 0;
        stbtt_GetCodepointHMetrics(ui->font, (int)codepoint, &advance, &bearing);
        if (previous)
            pen += stbtt_GetCodepointKernAdvance(ui->font, (int)previous, (int)codepoint) * scale;
        const GlyphCacheEntry* glyph = glyph_for(ui, codepoint, pixel_height, scale);
        if (glyph && glyph->bitmap) {
            const int draw_x = (int)(pen + 0.5f) + glyph->xoff;
            const int draw_y = baseline + glyph->yoff;
            for (int row = 0; row < glyph->height; row++) {
                for (int column = 0; column < glyph->width; column++) {
                    const u32 coverage = glyph->bitmap[row * glyph->width + column];
                    if (!coverage)
                        continue;
                    const u32 alpha = (coverage * (color >> 24)) / 255;
                    put_pixel(ui, draw_x + column, draw_y + row,
                              (color & 0x00ffffff) | (alpha << 24));
                }
            }
        }
        pen += advance * scale;
        previous = codepoint;
    }
}

void ui_text_truncated(Ui* ui, int x, int y, const char* text, int pixel_height,
                       u32 color, int max_width) {
    if (!text)
        return;
    const float scale = font_scale(ui, pixel_height);
    const char* cursor = text;
    char output[256];
    size_t used = 0;
    float width = 0;
    unsigned previous = 0;
    for (;;) {
        const char* start = cursor;
        const unsigned codepoint = utf8_next(&cursor);
        if (!codepoint)
            break;
        int advance = 0;
        int bearing = 0;
        stbtt_GetCodepointHMetrics(ui->font, (int)codepoint, &advance, &bearing);
        float add = advance * scale;
        if (previous)
            add += stbtt_GetCodepointKernAdvance(ui->font, (int)previous, (int)codepoint) * scale;
        if (width + add > (float)max_width && used)
            break;
        const size_t bytes = (size_t)(cursor - start);
        if (used + bytes >= sizeof(output))
            break;
        memcpy(output + used, start, bytes);
        used += bytes;
        width += add;
        previous = codepoint;
    }
    output[used] = '\0';
    ui_text(ui, x, y, output, pixel_height, color);
}

void ui_text_wrap(Ui* ui, int x, int y, const char* text, int pixel_height,
                  u32 color, int max_width, int max_lines, int line_height) {
    if (!text)
        return;
    const float scale = font_scale(ui, pixel_height);
    char line[256];
    size_t used = 0;
    float width = 0;
    unsigned previous = 0;
    int lines = 0;
    const char* cursor = text;
    while (*cursor && lines < max_lines) {
        const char* start = cursor;
        const unsigned codepoint = utf8_next(&cursor);
        if (codepoint == '\n') {
            line[used] = '\0';
            ui_text(ui, x, y + lines * line_height, line, pixel_height, color);
            used = 0;
            width = 0;
            previous = 0;
            lines++;
            continue;
        }
        int advance = 0;
        int bearing = 0;
        stbtt_GetCodepointHMetrics(ui->font, (int)codepoint, &advance, &bearing);
        float add = advance * scale;
        if (previous)
            add += stbtt_GetCodepointKernAdvance(ui->font, (int)previous, (int)codepoint) * scale;
        if (width + add > (float)max_width && used) {
            line[used] = '\0';
            ui_text(ui, x, y + lines * line_height, line, pixel_height, color);
            lines++;
            used = 0;
            width = 0;
            previous = 0;
            if (lines >= max_lines)
                break;
        }
        const size_t bytes = (size_t)(cursor - start);
        if (used + bytes >= sizeof(line))
            break;
        memcpy(line + used, start, bytes);
        used += bytes;
        width += add;
        previous = codepoint;
    }
    if (used && lines < max_lines) {
        line[used] = '\0';
        ui_text(ui, x, y + lines * line_height, line, pixel_height, color);
    }
}

void ui_markdown_wrap(Ui* ui, int x, int y, const char* text, int pixel_height,
                      u32 color, int max_width, int max_lines, int line_height) {
    if (!text)
        return;
    char plain[1024];
    size_t used = 0;
    int line_start = 1;
    for (const char* p = text; *p && used + 1 < sizeof(plain); p++) {
        if (*p == '\n') {
            plain[used++] = '\n';
            line_start = 1;
            continue;
        }
        if (line_start) {
            while (*p == ' ' || *p == '\t' || *p == '#' || *p == '>')
                p++;
            if (*p == '-' || *p == '*') {
                while (*p == '-' || *p == '*')
                    p++;
                if (*p == ' ') {
                    plain[used++] = '\xE2';
                    if (used + 2 < sizeof(plain)) {
                        plain[used++] = '\x80';
                        plain[used++] = '\xA2';
                    }
                }
            }
            line_start = 0;
        }
        if ((p[0] == '*' && p[1] == '*') || (p[0] == '_' && p[1] == '_')) {
            p++;
            continue;
        }
        if (*p == '`')
            continue;
        if (p[0] == '!' && p[1] == '[') {
            while (*p && *p != ')' && *p != '\n')
                p++;
            continue;
        }
        if (*p == '[') {
            const char* close = strchr(p, ']');
            if (close && close[1] == '(') {
                for (const char* label = p + 1; label < close && used + 1 < sizeof(plain); label++)
                    plain[used++] = *label;
                p = close;
                while (*p && *p != ')' && *p != '\n')
                    p++;
                continue;
            }
        }
        if (*p == '<') {
            const char* close = strchr(p, '>');
            if (close) {
                if (strncmp(p + 1, "http://", 7) == 0 || strncmp(p + 1, "https://", 8) == 0) {
                    for (const char* label = p + 1; label < close && used + 1 < sizeof(plain); label++)
                        plain[used++] = *label;
                }
                p = close;
                continue;
            }
        }
        plain[used++] = *p;
    }
    plain[used] = '\0';
    ui_text_wrap(ui, x, y, plain, pixel_height, color, max_width, max_lines, line_height);
}

void ui_image(Ui* ui, int x, int y, int width, int height,
              const u32* pixels, int image_width, int image_height) {
    if (!pixels || image_width <= 0 || image_height <= 0 || width <= 0 || height <= 0)
        return;
    if (width == image_width && height == image_height) {
        for (int row = 0; row < height; row++)
            for (int column = 0; column < width; column++)
                put_pixel(ui, x + column, y + row,
                          pixels[row * image_width + column]);
        return;
    }
    const float scale_x = (float)width / (float)image_width;
    const float scale_y = (float)height / (float)image_height;
    const float scale = scale_x < scale_y ? scale_x : scale_y;
    const int draw_width = (int)(image_width * scale);
    const int draw_height = (int)(image_height * scale);
    const int offset_x = x + (width - draw_width) / 2;
    const int offset_y = y + (height - draw_height) / 2;
    for (int row = 0; row < draw_height; row++) {
        const int source_y = row * image_height / draw_height;
        for (int column = 0; column < draw_width; column++) {
            const int source_x = column * image_width / draw_width;
            put_pixel(ui, offset_x + column, offset_y + row,
                      pixels[source_y * image_width + source_x]);
        }
    }
}

int ui_gif_load_memory(const unsigned char* data, size_t size, UiGif* out) {
    memset(out, 0, sizeof(*out));
    if (!data || !size || size > 16 * 1024 * 1024)
        return -1;
    int* delays = NULL;
    int width = 0;
    int height = 0;
    int frames = 0;
    int channels = 0;
    unsigned char* decoded = stbi_load_gif_from_memory(
        data, (int)size, &delays, &width, &height, &frames, &channels, 4);
    if (!decoded || !delays || width <= 0 || height <= 0 || frames <= 0 ||
        width > 640 || height > 640 || frames > 512) {
        stbi_image_free(decoded);
        free(delays);
        return -1;
    }
    const size_t count = (size_t)width * (size_t)height * (size_t)frames;
    u32* pixels = malloc(count * sizeof(u32));
    if (!pixels) {
        stbi_image_free(decoded);
        free(delays);
        return -1;
    }
    for (size_t i = 0; i < count; i++) {
        pixels[i] = RGBA8(decoded[i * 4 + 0], decoded[i * 4 + 1],
                          decoded[i * 4 + 2], decoded[i * 4 + 3]);
    }
    stbi_image_free(decoded);
    out->pixels = pixels;
    out->delays = delays;
    out->width = width;
    out->height = height;
    out->frame_count = frames;
    return 0;
}

void ui_gif_free(UiGif* gif) {
    if (!gif)
        return;
    free(gif->pixels);
    free(gif->delays);
    memset(gif, 0, sizeof(*gif));
}

int ui_load_image(const char* path, u32** pixels, int* width, int* height) {
    *pixels = NULL;
    *width = 0;
    *height = 0;
    struct stat status;
    if (stat(path, &status) != 0 || status.st_size > 8 * 1024 * 1024)
        return -1;
    int image_width = 0;
    int image_height = 0;
    int channels = 0;
    if (!stbi_info(path, &image_width, &image_height, &channels))
        return -1;
    if (image_width <= 0 || image_height <= 0 ||
        image_width > 2048 || image_height > 2048)
        return -1;
    unsigned char* decoded = stbi_load(path, &image_width, &image_height, &channels, 4);
    if (!decoded)
        return -1;
    u32* output = malloc((size_t)image_width * (size_t)image_height * sizeof(u32));
    if (!output) {
        stbi_image_free(decoded);
        return -1;
    }
    for (int i = 0; i < image_width * image_height; i++) {
        output[i] = RGBA8(decoded[i * 4 + 0], decoded[i * 4 + 1],
                          decoded[i * 4 + 2], decoded[i * 4 + 3]);
    }
    stbi_image_free(decoded);
    *pixels = output;
    *width = image_width;
    *height = image_height;
    return 0;
}
