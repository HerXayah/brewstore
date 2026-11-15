// SPDX-License-Identifier: MIT
#pragma once

#include <stddef.h>

/// Loads a cached README-derived description, or downloads README.md from a
/// GitHub repository and caches the cleaned text. Returns 0 when text is
/// available.
int readme_description(const char* repo_url, const char* branch,
                       const char* cache_path, char* out, size_t out_size,
                       char* err, size_t err_size);
