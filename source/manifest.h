// SPDX-License-Identifier: MIT
// brewstore package manifest: a small JSON file listing GitHub archives to install
// under sdmc:/switch. Deliberately no installer, no system paths, no NAND.
#pragma once

#include <stdbool.h>
#include <stddef.h>

#define HBPKG_MAX_PACKAGES 64
#define HBPKG_MAX_INCLUDES 64

typedef struct {
    char name[64];
    char title[96];
    char author[64];
    char description[512];
    char icon[512];
    char release[16];
    char asset[64];
    char version[32];
    char url[512];
    char branch[64];
    char strip_prefix[128];
    char target[256];
    char sha256[65];
    bool target_explicit;
    bool readme_fallback;
} HbPackage;

typedef struct {
    HbPackage items[HBPKG_MAX_PACKAGES];
    int count;
    char includes[HBPKG_MAX_INCLUDES][256];
    int include_count;
} HbManifest;

/// Reads and validates a manifest. Returns 0 on success, -1 on failure with a
/// human-readable message in err.
int manifest_load(const char* path, HbManifest* manifest, char* err, size_t err_size);

/// Fills target with a safe default if the manifest omitted it.
int manifest_default_target(const char* name, char* target, size_t target_size);
