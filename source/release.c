// SPDX-License-Identifier: MIT
#include "release.h"

#define JSMN_STRICT
#define JSMN_STATIC
#include "jsmn.h"

#include "github.h"
#include "http.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define RELEASE_JSON_MAX (1024 * 1024)
#define RELEASE_TOKENS 8192

static int fail(char* err, size_t err_size, const char* message) {
    if (err && err_size)
        snprintf(err, err_size, "%s", message);
    return -1;
}

static int token_string(const char* json, const jsmntok_t* token, char* out, size_t out_size) {
    if (!token || token->end < token->start || !out_size)
        return -1;
    if (token->type != JSMN_STRING && token->type != JSMN_PRIMITIVE)
        return -1;
    const size_t length = (size_t)(token->end - token->start);
    if (length >= out_size)
        return -1;
    memcpy(out, json + token->start, length);
    out[length] = '\0';
    return 0;
}

static int token_skip(const jsmntok_t* tokens, int index, int count) {
    if (index < 0 || index >= count)
        return count;
    int position = index + 1;
    if (tokens[index].type == JSMN_OBJECT) {
        for (int pair = 0; pair < tokens[index].size; pair++) {
            position = token_skip(tokens, position, count);
            position = token_skip(tokens, position, count);
        }
    } else if (tokens[index].type == JSMN_ARRAY) {
        for (int child = 0; child < tokens[index].size; child++)
            position = token_skip(tokens, position, count);
    }
    return position;
}

static int object_get(const char* json, const jsmntok_t* tokens, int count, int object,
                      const char* key, char* out, size_t out_size) {
    if (object < 0 || object >= count || tokens[object].type != JSMN_OBJECT)
        return -1;
    int position = object + 1;
    for (int pair = 0; pair < tokens[object].size && position < count; pair++) {
        const int value = position + 1;
        char name[64];
        if (token_string(json, &tokens[position], name, sizeof(name)) == 0 &&
            strcmp(name, key) == 0)
            return token_string(json, &tokens[value], out, out_size);
        position = token_skip(tokens, value, count);
    }
    return -1;
}

static int array_get(const char* json, const jsmntok_t* tokens, int count, int object,
                     const char* key) {
    if (object < 0 || object >= count || tokens[object].type != JSMN_OBJECT)
        return -1;
    int position = object + 1;
    for (int pair = 0; pair < tokens[object].size && position < count; pair++) {
        const int value = position + 1;
        char name[64];
        if (token_string(json, &tokens[position], name, sizeof(name)) == 0 &&
            strcmp(name, key) == 0 && tokens[value].type == JSMN_ARRAY)
            return value;
        position = token_skip(tokens, value, count);
    }
    return -1;
}

static int safe_asset_name(const char* name) {
    if (!name[0] || strchr(name, '/') || strchr(name, '\\') ||
        strchr(name, ':') || strstr(name, ".."))
        return 0;
    return 1;
}

static int ends_with_ci(const char* text, const char* suffix) {
    const size_t text_length = strlen(text);
    const size_t suffix_length = strlen(suffix);
    if (suffix_length > text_length)
        return 0;
    return strcasecmp(text + text_length - suffix_length, suffix) == 0;
}

static int read_file(const char* path, char** out, size_t* out_size, char* err, size_t err_size) {
    FILE* file = fopen(path, "rb");
    if (!file)
        return fail(err, err_size, "cannot open GitHub release response");
    char* text = malloc(RELEASE_JSON_MAX + 1);
    if (!text) {
        fclose(file);
        return fail(err, err_size, "out of memory");
    }
    const size_t length = fread(text, 1, RELEASE_JSON_MAX, file);
    fclose(file);
    if (!length || length >= RELEASE_JSON_MAX) {
        free(text);
        return fail(err, err_size, "GitHub release response is empty or too large");
    }
    text[length] = '\0';
    *out = text;
    *out_size = length;
    return 0;
}

int release_latest_asset(const char* repo_url, const char* asset_hint,
                         const char* cache_path, char* asset_url, size_t url_size,
                         char* asset_name, size_t name_size,
                         unsigned long long* asset_size,
                         char* err, size_t err_size) {
    char owner[128];
    char repo[128];
    if (github_repo_parts(repo_url, owner, sizeof(owner), repo, sizeof(repo)) != 0)
        return fail(err, err_size, "not a GitHub repository URL");

    char api[1024];
    snprintf(api, sizeof(api), "https://api.github.com/repos/%s/%s/releases?per_page=10",
             owner, repo);
    char hash[65];
    if (http_download(api, cache_path, hash, err, err_size) != 0)
        return -1;

    char* json = NULL;
    size_t length = 0;
    if (read_file(cache_path, &json, &length, err, err_size) != 0)
        return -1;

    jsmntok_t* tokens = malloc(sizeof(jsmntok_t) * RELEASE_TOKENS);
    if (!tokens) {
        free(json);
        return fail(err, err_size, "out of memory");
    }
    jsmn_parser parser;
    jsmn_init(&parser);
    const int count = jsmn_parse(&parser, json, length, tokens, RELEASE_TOKENS);
    if (count < 1 || (tokens[0].type != JSMN_OBJECT && tokens[0].type != JSMN_ARRAY)) {
        free(tokens);
        free(json);
        return fail(err, err_size, "GitHub release response is not JSON");
    }

    char fallback_url[1024] = "";
    char fallback_name[128] = "";
    unsigned long long fallback_size = 0;
    const int releases = tokens[0].type == JSMN_ARRAY ? tokens[0].size : 1;
    int release_position = tokens[0].type == JSMN_ARRAY ? 1 : 0;
    for (int release = 0; release < releases; release++) {
        const int release_object = release_position;
        release_position = token_skip(tokens, release_object, count);
        if (release_object >= count || tokens[release_object].type != JSMN_OBJECT)
            continue;
        const int assets = array_get(json, tokens, count, release_object, "assets");
        if (assets < 0)
            continue;

        int position = assets + 1;
        for (int index = 0; index < tokens[assets].size; index++) {
            const int object = position;
            position = token_skip(tokens, object, count);
            if (object >= count || tokens[object].type != JSMN_OBJECT)
                continue;
            char name[128];
            char url[1024];
            char size_text[32] = "0";
            if (object_get(json, tokens, count, object, "name", name, sizeof(name)) != 0 ||
                object_get(json, tokens, count, object, "browser_download_url", url, sizeof(url)) != 0)
                continue;
            object_get(json, tokens, count, object, "size", size_text, sizeof(size_text));
            const unsigned long long size = strtoull(size_text, NULL, 10);
            if (!safe_asset_name(name) || strncmp(url, "https://", 8) != 0)
                continue;
            if (asset_hint && asset_hint[0]) {
                if (ends_with_ci(name, asset_hint)) {
                    snprintf(asset_url, url_size, "%s", url);
                    snprintf(asset_name, name_size, "%s", name);
                    if (asset_size)
                        *asset_size = size;
                    free(tokens);
                    free(json);
                    return 0;
                }
            } else if (ends_with_ci(name, ".nro")) {
                snprintf(asset_url, url_size, "%s", url);
                snprintf(asset_name, name_size, "%s", name);
                if (asset_size)
                    *asset_size = size;
                free(tokens);
                free(json);
                return 0;
            } else if (!fallback_name[0] && ends_with_ci(name, ".zip")) {
                snprintf(fallback_url, sizeof(fallback_url), "%s", url);
                snprintf(fallback_name, sizeof(fallback_name), "%s", name);
                fallback_size = size;
            }
        }
    }

    if (fallback_name[0]) {
        snprintf(asset_url, url_size, "%s", fallback_url);
        snprintf(asset_name, name_size, "%s", fallback_name);
        if (asset_size)
            *asset_size = fallback_size;
        free(tokens);
        free(json);
        return 0;
    }

    free(tokens);
    free(json);
    return fail(err, err_size, "release has no .nro or .zip asset");
}
