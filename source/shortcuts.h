// SPDX-License-Identifier: MIT
#pragma once

#include <stddef.h>

/// Adds or replaces the brewstore-owned shortcut block for package.
/// An empty nro removes the block. Manual shortcut lines are preserved.
int shortcuts_set(const char* path, const char* package, const char* title,
                  const char* nro);
int shortcuts_remove(const char* path, const char* package);
