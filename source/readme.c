// SPDX-License-Identifier: MIT
#include "readme.h"

#include "github.h"
#include "http.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define README_MAX_BYTES 16384
#define DESCRIPTION_MAX 700

static int fail(char* err, size_t err_size, const char* message) {
    if (err && err_size)
        snprintf(err, err_size, "%s", message);
    return -1;
}

static int read_text(const char* path, char* out, size_t out_size) {
    FILE* file = fopen(path, "rb");
    if (!file)
        return -1;
    const size_t length = fread(out, 1, out_size - 1, file);
    fclose(file);
    out[length] = '\0';
    return length ? 0 : -1;
}

static void clean_markdown(const char* input, char* output, size_t output_size) {
    size_t used = 0;
    int line_start = 1;
    int in_fence = 0;
    for (const char* p = input; *p && used + 1 < output_size; p++) {
        if (line_start && p[0] == '`' && p[1] == '`' && p[2] == '`') {
            in_fence = !in_fence;
            while (*p && *p != '\n')
                p++;
            line_start = 1;
            continue;
        }
        if (in_fence) {
            if (*p == '\n')
                line_start = 1;
            else
                line_start = 0;
            continue;
        }
        if (line_start) {
            while (*p == '#' || *p == '>' || *p == '-' || *p == '*' || *p == ' ')
                p++;
            if (*p == '\n' || *p == '\0')
                continue;
            line_start = 0;
        }
        if (p[0] == '!' && p[1] == '[') {
            while (*p && *p != ')' && *p != '\n')
                p++;
            continue;
        }
        if (p[0] == '[' && p[1] == '!') {
            while (*p && *p != '\n')
                p++;
            continue;
        }
        if (p[0] == '[') {
            const char* close = strchr(p, ']');
            if (close && close[1] == '(') {
                for (const char* text = p + 1; text < close; text++)
                    output[used++] = *text;
                p = close;
                while (*p && *p != ')' && *p != '\n')
                    p++;
                continue;
            }
        }
        if (*p == '\n' || *p == '\r' || *p == '\t') {
            if (used && output[used - 1] != ' ')
                output[used++] = ' ';
            line_start = (*p == '\n');
            continue;
        }
        if (*p == '*' || *p == '`' || *p == '_' || *p == '|')
            continue;
        output[used++] = *p;
        line_start = 0;
    }
    while (used && output[used - 1] == ' ')
        used--;
    output[used] = '\0';
}

static int fetch_branch(const char* owner, const char* repo, const char* branch,
                        const char* cache_path, char* out, size_t out_size,
                        char* err, size_t err_size) {
    static const char* const names[] = { "README.md", "readme.md" };
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        char url[1024];
        snprintf(url, sizeof(url), "https://raw.githubusercontent.com/%s/%s/%s/%s",
                 owner, repo, branch, names[i]);
        char hash[65];
        if (http_download(url, cache_path, hash, err, err_size) == 0) {
            char raw[README_MAX_BYTES + 1];
            if (read_text(cache_path, raw, sizeof(raw)) != 0)
                return fail(err, err_size, "README is empty");
            clean_markdown(raw, out, out_size);
            if (!out[0]) {
                remove(cache_path);
                return fail(err, err_size, "README has no description text");
            }

            FILE* cached = fopen(cache_path, "wb");
            if (cached) {
                fwrite(out, 1, strlen(out), cached);
                fclose(cached);
            }
            return 0;
        }
    }
    return -1;
}

int readme_description(const char* repo_url, const char* branch,
                       const char* cache_path, char* out, size_t out_size,
                       char* err, size_t err_size) {
    out[0] = '\0';
    char cached[README_MAX_BYTES + 1];
    if (read_text(cache_path, cached, sizeof(cached)) == 0) {
        clean_markdown(cached, out, out_size);
        if (out[0])
            return 0;
    }

    char owner[128];
    char repo[128];
    if (github_repo_parts(repo_url, owner, sizeof(owner), repo, sizeof(repo)) != 0)
        return fail(err, err_size, "README source is not a GitHub repository");

    char branch_name[64];
    if (branch && branch[0] && strcmp(branch, "main") != 0) {
        snprintf(branch_name, sizeof(branch_name), "%s", branch);
        if (fetch_branch(owner, repo, branch_name, cache_path, out, out_size,
                         err, err_size) == 0)
            return 0;
    }
    if (fetch_branch(owner, repo, "main", cache_path, out, out_size, err, err_size) == 0)
        return 0;
    if (fetch_branch(owner, repo, "master", cache_path, out, out_size, err, err_size) == 0)
        return 0;

    FILE* miss = fopen(cache_path, "wb");
    if (miss) {
        fputc(' ', miss);
        fclose(miss);
    }
    return fail(err, err_size, "README.md was not found");
}
