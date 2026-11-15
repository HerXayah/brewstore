// SPDX-License-Identifier: MIT
#pragma once

#include <stddef.h>

/// Recursively creates a directory and its parents. Returns 0 on success.
int path_mkdirs(const char* path);

/// Recursively removes a directory tree. Missing paths are not an error.
int path_remove_tree(const char* path);

/// Copies a regular file. The destination is replaced.
int path_copy_file(const char* source, const char* destination);

/// Reports extraction progress: files written so far, total files, current
/// relative path.
typedef void (*ZipProgress)(int current, int total, const char* name, void* user);

/// Extracts archive into target. When strip_prefix is empty, one common
/// top-level archive directory is removed automatically. Entries that escape
/// target, symlinks, and oversized entries are rejected or skipped.
int zip_extract(const char* archive, const char* target, const char* strip_prefix,
                char* err, size_t err_size, ZipProgress progress, void* user);
