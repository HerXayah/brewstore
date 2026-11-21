// SPDX-License-Identifier: MIT
// brewstore package manifest: a small JSON file listing GitHub archives to install
// under sdmc:/switch or the configured app root. Deliberately no installer, no NAND.
#pragma once

#include <stdbool.h>
#include <stddef.h>

#define HBPKG_LEGACY_MAX_PACKAGES 1024
#define HBPKG_MAX_INCLUDES 1024
#define HB_PACKAGES_SANITY_MAX 20000

typedef struct {
    char name[64];
    char title[96];
    char author[64];
    char description[512];
    char icon[512];
    char release[16];
    char release_tag[64];
    char asset[128];
    char download_url[1024];
    char version[32];
    char published[24];
    char url[512];
    char branch[64];
    char strip_prefix[128];
    char target[256];
    char marker[512];
    char sha256[65];
    bool target_explicit;
    bool readme_fallback;
    bool shortcut;
    bool size_known;
    unsigned long long size;
} HbPackage;

typedef struct {
    HbPackage items[HBPKG_LEGACY_MAX_PACKAGES];
    int count;
    char includes[HBPKG_MAX_INCLUDES][256];
    int include_count;
} HbManifest;

typedef struct {
    HbPackage* items;
    int count;
    int capacity;
} HbPackageList;

/// Ensures room for at least `capacity` packages. Returns 0 on success.
int hb_list_reserve(HbPackageList* list, int capacity);
/// Appends a package. Returns 0 on success, -1 on allocation failure.
int hb_list_push(HbPackageList* list, const HbPackage* package);
/// Releases the list and resets it.
void hb_list_free(HbPackageList* list);

/// Reads and validates a manifest. Returns 0 on success, -1 on failure with a
/// human-readable message in err.
int manifest_load(const char* path, HbManifest* manifest, char* err, size_t err_size);

/// Fills target with a safe default if the manifest omitted it.
int manifest_default_target(const char* name, char* target, size_t target_size);
