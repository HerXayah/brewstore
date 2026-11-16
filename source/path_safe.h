// SPDX-License-Identifier: MIT
#pragma once

#include <stdbool.h>

/// Validates a relative path component sequence for extraction/target use.
/// Rejects absolute paths, traversal, FAT-invalid characters, control bytes,
/// invalid UTF-8, empty components, and components longer than 255 bytes.
bool path_safe_relative(const char* path);
