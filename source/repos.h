// SPDX-License-Identifier: MIT
#pragma once

#include <stdbool.h>
#include <stddef.h>

#define HBPKG_MAX_REPOS 8

typedef struct {
    char urls[HBPKG_MAX_REPOS][512];
    bool enabled[HBPKG_MAX_REPOS];
    int count;
} RepoList;

/// Loads repos.json. If the file does not exist, creates it with an empty
/// repository list. Returns 0 on success.
int repos_load(const char* path, RepoList* repos, char* err, size_t err_size);

/// Saves repos.json atomically through repos.json.tmp.
int repos_save(const char* path, const RepoList* repos, char* err, size_t err_size);

/// Adds a validated HTTPS catalog URL. Returns 0 on success, 1 if already
/// present, -1 on invalid input or a full list.
int repos_add(RepoList* repos, const char* url);

/// Removes the repository at index. Returns 0 on success.
int repos_remove(RepoList* repos, int index);
