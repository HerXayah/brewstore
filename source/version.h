// SPDX-License-Identifier: MIT
#pragma once

/// Returns 1 when candidate is newer than installed, 0 when equal/older,
/// -1 when the strings cannot be ordered numerically.
int version_is_newer(const char* candidate, const char* installed);
