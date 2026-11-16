// SPDX-License-Identifier: MIT
#pragma once

#include <stddef.h>

/// Resolves the newest GitHub release asset for a repository URL. The API
/// response is cached at cache_path. An empty asset_hint prefers .nro, then
/// .zip; a non-empty hint matches an asset name suffix case-insensitively.
int release_latest_asset(const char* repo_url, const char* asset_hint,
                         const char* cache_path, char* asset_url, size_t url_size,
                         char* asset_name, size_t name_size,
                         unsigned long long* asset_size,
                         char* digest, size_t digest_size,
                         char* err, size_t err_size);
