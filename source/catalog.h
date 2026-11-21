// SPDX-License-Identifier: MIT
#pragma once

#include "manifest.h"

#include <stddef.h>

typedef struct {
    int packages;
    int skipped;
} CatalogStats;

/// Loads catalog.json. Returns 0 for a catalogue, -2 when the file is not a
/// catalogue (legacy manifest), and -1 for an invalid catalogue.
int catalog_load(const char* path, HbPackageList* out, CatalogStats* stats,
                 char* err, size_t err_size);
