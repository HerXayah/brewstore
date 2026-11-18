// SPDX-License-Identifier: MIT
#pragma once

#include <stddef.h>

enum {
    BUTTON_THEME_LIGHT_BLUE = 0,
    BUTTON_THEME_LESBIAN,
    BUTTON_THEME_TRANS,
    BUTTON_THEME_AROMANTIC,
    BUTTON_THEME_GAY,
    BUTTON_THEME_BISEXUAL,
    BUTTON_THEME_PANSEXUAL,
    BUTTON_THEME_NONBINARY,
    BUTTON_THEME_ASEXUAL,
    BUTTON_THEME_COUNT
};

#define DEFAULT_BUTTON_THEME BUTTON_THEME_TRANS

typedef struct {
    char default_install_path[256];
    char download_proxy[256];
    int button_theme;
    int list_view;
} AppSettings;

/// Loads settings.json, creating the default file when absent.
int settings_load(const char* path, AppSettings* settings, char* err, size_t err_size);
int settings_save(const char* path, const AppSettings* settings, char* err, size_t err_size);
int settings_valid_path(const char* path);
int settings_valid_proxy(const char* proxy);
