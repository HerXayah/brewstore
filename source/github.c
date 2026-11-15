// SPDX-License-Identifier: MIT
#include "github.h"

#include <stdio.h>
#include <string.h>

static int starts_with(const char* text, const char* prefix) {
    return strncmp(text, prefix, strlen(prefix)) == 0;
}

int github_archive_url(const char* url, const char* branch,
                       char* out, size_t out_size) {
    static const char prefix[] = "https://github.com/";
    if (!starts_with(url, prefix)) {
        if (strlen(url) >= out_size)
            return -1;
        strcpy(out, url);
        return 0;
    }
    if (strstr(url, "/archive/") || strstr(url, "/releases/") || strstr(url, "codeload.")) {
        if (strlen(url) >= out_size)
            return -1;
        strcpy(out, url);
        return 0;
    }

    const char* owner = url + sizeof(prefix) - 1;
    const char* slash = strchr(owner, '/');
    if (!slash)
        return -1;
    const char* repo = slash + 1;
    const char* end = strchr(repo, '/');
    const size_t owner_length = (size_t)(slash - owner);
    size_t repo_length = end ? (size_t)(end - repo) : strlen(repo);
    if (owner_length == 0 || repo_length == 0)
        return -1;
    if (repo_length >= 5 && strcmp(repo + repo_length - 4, ".git") == 0)
        repo_length -= 4;

    char selected[64];
    if (branch && branch[0])
        snprintf(selected, sizeof(selected), "%s", branch);
    else
        strcpy(selected, "main");
    if (end && strncmp(end, "/tree/", 6) == 0) {
        if (strlen(end + 6) >= sizeof(selected))
            return -1;
        strcpy(selected, end + 6);
        char* query = strchr(selected, '?');
        if (query)
            *query = '\0';
    }

    const int written = snprintf(out, out_size,
                                 "https://codeload.github.com/%.*s/%.*s/zip/refs/heads/%s",
                                 (int)owner_length, owner, (int)repo_length, repo, selected);
    return written > 0 && (size_t)written < out_size ? 0 : -1;
}

int github_repo_name(const char* url, char* out, size_t out_size) {
    static const char prefix[] = "https://github.com/";
    if (!starts_with(url, prefix))
        return -1;
    const char* owner = url + sizeof(prefix) - 1;
    const char* slash = strchr(owner, '/');
    if (!slash)
        return -1;
    const char* repo = slash + 1;
    const char* end = strchr(repo, '/');
    size_t length = end ? (size_t)(end - repo) : strlen(repo);
    if (length >= 4 && strcmp(repo + length - 4, ".git") == 0)
        length -= 4;
    if (!length || length + 1 >= out_size)
        return -1;
    size_t used = 0;
    for (size_t i = 0; i < length; i++) {
        const unsigned char c = (unsigned char)repo[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-')
            out[used++] = (char)c;
        else
            out[used++] = '-';
    }
    out[used] = '\0';
    if (strcmp(out, ".") == 0 || strcmp(out, "..") == 0)
        return -1;
    return used ? 0 : -1;
}

int github_repo_parts(const char* url, char* owner, size_t owner_size,
                      char* repo, size_t repo_size) {
    static const char prefix[] = "https://github.com/";
    if (!starts_with(url, prefix))
        return -1;
    const char* start = url + sizeof(prefix) - 1;
    const char* slash = strchr(start, '/');
    if (!slash)
        return -1;
    const char* repo_start = slash + 1;
    const char* end = strchr(repo_start, '/');
    size_t owner_length = (size_t)(slash - start);
    size_t repo_length = end ? (size_t)(end - repo_start) : strlen(repo_start);
    if (repo_length >= 4 && strcmp(repo_start + repo_length - 4, ".git") == 0)
        repo_length -= 4;
    if (!owner_length || !repo_length || owner_length + 1 > owner_size ||
        repo_length + 1 > repo_size)
        return -1;
    memcpy(owner, start, owner_length);
    owner[owner_length] = '\0';
    memcpy(repo, repo_start, repo_length);
    repo[repo_length] = '\0';
    return 0;
}
