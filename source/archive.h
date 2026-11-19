// SPDX-License-Identifier: MIT
#pragma once

#include <stdbool.h>

#include "zip.h"

/// True for archive assets this build can extract.
bool archive_is_archive(const char* name);

/// Extracts a zip, tar, tar.gz or tar.xz archive into target. asset_name
/// selects the format because downloaded cache files have no suffix. The rules
/// match zip_extract: an explicit strip_prefix wins, otherwise one common
/// top-level directory is removed. Unsafe paths, symlinks and oversized entries
/// are rejected or skipped.
int archive_extract(const char* path, const char* asset_name, const char* target,
                    const char* strip_prefix,
                    char* err, size_t err_size, ZipProgress progress, void* user);
