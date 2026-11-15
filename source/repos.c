// SPDX-License-Identifier: MIT
#include "repos.h"

#define JSMN_STRICT
#define JSMN_STATIC
#include "jsmn.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REPOS_MAX_BYTES (64 * 1024)
#define REPOS_MAX_TOKENS 512

static int fail(char* err, size_t err_size, const char* message) {
    if (err && err_size)
        snprintf(err, err_size, "%s", message);
    return -1;
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

static int token_string(const char* json, const jsmntok_t* token, char* out, size_t out_size) {
    if (!token || token->type != JSMN_STRING || token->end < token->start || !out_size)
        return -1;
    const size_t length = (size_t)(token->end - token->start);
    if (length >= out_size)
        return -1;
    memcpy(out, json + token->start, length);
    out[length] = '\0';
    return 0;
}

static int valid_url(const char* url) {
    if (strncmp(url, "https://", 8) != 0)
        return 0;
    if (!url[8] || strlen(url) >= 512)
        return 0;
    if (strchr(url, '"') || strchr(url, '\\') || strchr(url, '\n') || strchr(url, '\r'))
        return 0;
    return strstr(url, "..") == NULL;
}

int repos_save(const char* path, const RepoList* repos, char* err, size_t err_size) {
    char temporary[640];
    const int written = snprintf(temporary, sizeof(temporary), "%s.tmp", path);
    if (written <= 0 || (size_t)written >= sizeof(temporary))
        return fail(err, err_size, "repository path is too long");

    FILE* file = fopen(temporary, "wb");
    if (!file)
        return fail(err, err_size, "cannot create repos.json");
    fprintf(file, "{\n  \"repositories\": [\n");
    for (int i = 0; i < repos->count; i++) {
        fprintf(file,
                "    {\"url\": \"%s\", \"enabled\": %s}%s\n",
                repos->urls[i], repos->enabled[i] ? "true" : "false",
                i + 1 < repos->count ? "," : "");
    }
    fprintf(file, "  ]\n}\n");
    if (fclose(file) != 0) {
        remove(temporary);
        return fail(err, err_size, "cannot write repos.json");
    }
    remove(path);
    if (rename(temporary, path) != 0) {
        remove(temporary);
        return fail(err, err_size, "cannot replace repos.json");
    }
    return 0;
}

int repos_load(const char* path, RepoList* repos, char* err, size_t err_size) {
    memset(repos, 0, sizeof(*repos));
    FILE* file = fopen(path, "rb");
    if (!file) {
        if (errno == ENOENT)
            return repos_save(path, repos, err, err_size);
        return fail(err, err_size, "cannot open repos.json");
    }

    char* text = malloc(REPOS_MAX_BYTES + 1);
    if (!text) {
        fclose(file);
        return fail(err, err_size, "out of memory");
    }
    const size_t length = fread(text, 1, REPOS_MAX_BYTES, file);
    fclose(file);
    if (length == 0 || length >= REPOS_MAX_BYTES) {
        free(text);
        return fail(err, err_size, "repos.json is empty or too large");
    }
    text[length] = '\0';

    jsmn_parser parser;
    jsmntok_t* tokens = malloc(sizeof(jsmntok_t) * REPOS_MAX_TOKENS);
    if (!tokens) {
        free(text);
        return fail(err, err_size, "out of memory");
    }
    jsmn_init(&parser);
    const int count = jsmn_parse(&parser, text, length, tokens, REPOS_MAX_TOKENS);
    if (count < 1 || tokens[0].type != JSMN_OBJECT) {
        free(tokens);
        free(text);
        return fail(err, err_size, "repos.json must contain an object");
    }

    int position = 1;
    int found = -1;
    for (int pair = 0; pair < tokens[0].size && position < count; pair++) {
        const int value = position + 1;
        if (tokens[position].type == JSMN_STRING &&
            (size_t)(tokens[position].end - tokens[position].start) == 12 &&
            memcmp(text + tokens[position].start, "repositories", 12) == 0) {
            found = value;
            break;
        }
        position = token_skip(tokens, value, count);
    }

    if (found < 0 || found >= count || tokens[found].type != JSMN_ARRAY) {
        free(tokens);
        free(text);
        return fail(err, err_size, "repos.json is missing repositories");
    }

    int item = found + 1;
    for (int index = 0; index < tokens[found].size; index++) {
        char url[512] = "";
        bool enabled = true;
        if (item >= count) {
            free(tokens);
            free(text);
            return fail(err, err_size, "repos.json has a malformed entry");
        }
        if (tokens[item].type == JSMN_STRING) {
            if (token_string(text, &tokens[item], url, sizeof(url)) != 0) {
                free(tokens);
                free(text);
                return fail(err, err_size, "repository URL is too long");
            }
        } else if (tokens[item].type == JSMN_OBJECT) {
            int position = item + 1;
            for (int pair = 0; pair < tokens[item].size && position + 1 < count; pair++) {
                const int key = position;
                const int value = position + 1;
                char name[32];
                if (token_string(text, &tokens[key], name, sizeof(name)) == 0) {
                    if (strcmp(name, "url") == 0)
                        token_string(text, &tokens[value], url, sizeof(url));
                    else if (strcmp(name, "enabled") == 0 &&
                             tokens[value].type == JSMN_PRIMITIVE)
                        enabled = !(tokens[value].end - tokens[value].start == 5 &&
                                    memcmp(text + tokens[value].start, "false", 5) == 0);
                }
                position = token_skip(tokens, value, count);
            }
        } else {
            free(tokens);
            free(text);
            return fail(err, err_size, "repos.json has a malformed entry");
        }
        if (!valid_url(url)) {
            free(tokens);
            free(text);
            return fail(err, err_size, "repository URL must use https");
        }
        snprintf(repos->urls[repos->count], sizeof(repos->urls[0]), "%s", url);
        repos->enabled[repos->count] = enabled;
        repos->count++;
        if (repos->count >= HBPKG_MAX_REPOS)
            break;
        item = token_skip(tokens, item, count);
    }

    free(tokens);
    free(text);
    return 0;
}

int repos_add(RepoList* repos, const char* url) {
    if (!valid_url(url))
        return -1;
    for (int i = 0; i < repos->count; i++) {
        if (strcmp(repos->urls[i], url) == 0)
            return 1;
    }
    if (repos->count >= HBPKG_MAX_REPOS)
        return -1;
    strcpy(repos->urls[repos->count], url);
    repos->enabled[repos->count] = true;
    repos->count++;
    return 0;
}

int repos_remove(RepoList* repos, int index) {
    if (index < 0 || index >= repos->count)
        return -1;
    for (int i = index; i + 1 < repos->count; i++) {
        strcpy(repos->urls[i], repos->urls[i + 1]);
        repos->enabled[i] = repos->enabled[i + 1];
    }
    repos->count--;
    return 0;
}
