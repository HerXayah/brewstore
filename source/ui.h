// SPDX-License-Identifier: MIT
#pragma once

#include <switch.h>

#include <stdbool.h>
#include <stddef.h>

#define UI_WIDTH 1280
#define UI_HEIGHT 720

typedef struct {
    Framebuffer framebuffer;
    PlFontData font_data;
    struct stbtt_fontinfo* font;
    u32* pixels;
    u32 stride;
    bool active;
    bool framebuffer_open;
    bool pl_open;
} Ui;

int ui_init(Ui* ui, char* err, size_t err_size);
void ui_exit(Ui* ui);

void ui_begin(Ui* ui);
void ui_end(Ui* ui);
void ui_clear(Ui* ui, u32 color);
void ui_rect(Ui* ui, int x, int y, int width, int height, u32 color);
void ui_text(Ui* ui, int x, int y, const char* text, int pixel_height, u32 color);
int ui_text_width(Ui* ui, const char* text, int pixel_height);
void ui_text_wrap(Ui* ui, int x, int y, const char* text, int pixel_height,
                  u32 color, int max_width, int max_lines, int line_height);
void ui_text_truncated(Ui* ui, int x, int y, const char* text, int pixel_height,
                       u32 color, int max_width);
void ui_image(Ui* ui, int x, int y, int width, int height,
              const u32* pixels, int image_width, int image_height);

/// Loads a PNG/JPEG from SD into an RGBA8 buffer. Caller frees pixels.
int ui_load_image(const char* path, u32** pixels, int* width, int* height);
