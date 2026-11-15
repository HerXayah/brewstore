// SPDX-License-Identifier: MIT
#pragma once

#include <stddef.h>

/// Convert a GitHub repository URL to its codeload archive URL. Other URLs are
/// copied unchanged. Returns 0 on success.
int github_archive_url(const char* url, const char* branch,
                       char* out, size_t out_size);

/// Extracts a safe package name from a GitHub repository URL.
int github_repo_name(const char* url, char* out, size_t out_size);

/// Splits a GitHub repository URL into owner and repo.
int github_repo_parts(const char* url, char* owner, size_t owner_size,
                      char* repo, size_t repo_size);
