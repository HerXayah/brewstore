// SPDX-License-Identifier: MIT
#pragma once

#include <stddef.h>

typedef struct {
    char default_install_path[256];
    char download_proxy[256];
} AppSettings;

/// Loads settings.json, creating the default file when absent.
int settings_load(const char* path, AppSettings* settings, char* err, size_t err_size);
int settings_save(const char* path, const AppSettings* settings, char* err, size_t err_size);
int settings_valid_path(const char* path);
int settings_valid_proxy(const char* proxy);
