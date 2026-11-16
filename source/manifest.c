// SPDX-License-Identifier: MIT
#include "manifest.h"
#include "path_safe.h"

#define JSMN_STRICT
#define JSMN_STATIC
#include "jsmn.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MANIFEST_MAX_BYTES (256 * 1024)
#define MANIFEST_MAX_TOKENS 4096

static int fail(char* err, size_t err_size, const char* message) {
    if (err && err_size)
        snprintf(err, err_size, "%s", message);
    return -1;
}

static int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int read_hex4(const char* text, unsigned* value) {
    unsigned result = 0;
    for (int i = 0; i < 4; i++) {
        const int digit = hex_value(text[i]);
        if (digit < 0)
            return -1;
        result = (result << 4) | (unsigned)digit;
    }
    *value = result;
    return 0;
}

static int utf8_append(char* out, size_t out_size, size_t* used, unsigned codepoint) {
    if (codepoint == 0)
        return -1;
    unsigned char bytes[4];
    int count;
    if (codepoint <= 0x7f) {
        bytes[0] = (unsigned char)codepoint;
        count = 1;
    } else if (codepoint <= 0x7ff) {
        bytes[0] = (unsigned char)(0xc0 | (codepoint >> 6));
        bytes[1] = (unsigned char)(0x80 | (codepoint & 0x3f));
        count = 2;
    } else if (codepoint <= 0xffff) {
        bytes[0] = (unsigned char)(0xe0 | (codepoint >> 12));
        bytes[1] = (unsigned char)(0x80 | ((codepoint >> 6) & 0x3f));
        bytes[2] = (unsigned char)(0x80 | (codepoint & 0x3f));
        count = 3;
    } else if (codepoint <= 0x10ffff) {
        bytes[0] = (unsigned char)(0xf0 | (codepoint >> 18));
        bytes[1] = (unsigned char)(0x80 | ((codepoint >> 12) & 0x3f));
        bytes[2] = (unsigned char)(0x80 | ((codepoint >> 6) & 0x3f));
        bytes[3] = (unsigned char)(0x80 | (codepoint & 0x3f));
        count = 4;
    } else {
        return -1;
    }
    if (*used + (size_t)count >= out_size)
        return -1;
    memcpy(out + *used, bytes, (size_t)count);
    *used += (size_t)count;
    return 0;
}

static int token_string(const char* json, const jsmntok_t* token, char* out, size_t out_size) {
    if (!token || token->type != JSMN_STRING || token->end < token->start || !out_size)
        return -1;
    const char* input = json + token->start;
    const char* end = json + token->end;
    size_t used = 0;
    while (input < end && used + 1 < out_size) {
        if (*input != '\\') {
            out[used++] = *input++;
            continue;
        }
        input++;
        if (input >= end)
            return -1;
        switch (*input) {
            case '"': out[used++] = '"'; input++; break;
            case '\\': out[used++] = '\\'; input++; break;
            case '/': out[used++] = '/'; input++; break;
            case 'b': out[used++] = '\b'; input++; break;
            case 'f': out[used++] = '\f'; input++; break;
            case 'n': out[used++] = '\n'; input++; break;
            case 'r': out[used++] = '\r'; input++; break;
            case 't': out[used++] = '\t'; input++; break;
            case 'u': {
                unsigned codepoint = 0;
                if (input + 4 >= end || read_hex4(input + 1, &codepoint) != 0)
                    return -1;
                input += 5;
                if (codepoint >= 0xd800 && codepoint <= 0xdbff &&
                    input + 5 < end && input[0] == '\\' && input[1] == 'u') {
                    unsigned low = 0;
                    if (read_hex4(input + 2, &low) == 0 &&
                        low >= 0xdc00 && low <= 0xdfff) {
                        codepoint = 0x10000 + ((codepoint - 0xd800) << 10) + (low - 0xdc00);
                        input += 6;
                    }
                }
                if (utf8_append(out, out_size, &used, codepoint) != 0)
                    return -1;
                break;
            }
            default:
                return -1;
        }
    }
    out[used] = '\0';
    return 0;
}

static int token_skip(const jsmntok_t* tokens, int index, int count) {
    if (index < 0 || index >= count)
        return count;
    int pos = index + 1;
    if (tokens[index].type == JSMN_OBJECT) {
        for (int pair = 0; pair < tokens[index].size; pair++) {
            pos = token_skip(tokens, pos, count);  // key
            pos = token_skip(tokens, pos, count);  // value
        }
    } else if (tokens[index].type == JSMN_ARRAY) {
        for (int child = 0; child < tokens[index].size; child++)
            pos = token_skip(tokens, pos, count);
    }
    return pos;
}

static int object_get(const char* json, const jsmntok_t* tokens, int count, int object,
                      const char* key, char* out, size_t out_size) {
    if (object < 0 || object >= count || tokens[object].type != JSMN_OBJECT)
        return -1;
    int pos = object + 1;
    for (int pair = 0; pair < tokens[object].size && pos < count; pair++) {
        const int value = pos + 1;
        char name[64];
        if (token_string(json, &tokens[pos], name, sizeof(name)) == 0 &&
            strcmp(name, key) == 0 && value < count) {
            return token_string(json, &tokens[value], out, out_size);
        }
        pos = token_skip(tokens, value, count);
    }
    return -1;
}

static int object_get_bool(const char* json, const jsmntok_t* tokens, int count, int object,
                           const char* key, bool* out) {
    if (object < 0 || object >= count || tokens[object].type != JSMN_OBJECT)
        return -1;
    int pos = object + 1;
    for (int pair = 0; pair < tokens[object].size && pos < count; pair++) {
        const int value = pos + 1;
        char name[64];
        if (token_string(json, &tokens[pos], name, sizeof(name)) == 0 &&
            strcmp(name, key) == 0 && value < count &&
            tokens[value].type == JSMN_PRIMITIVE) {
            const size_t length = (size_t)(tokens[value].end - tokens[value].start);
            if (length == 4 && memcmp(json + tokens[value].start, "true", 4) == 0) {
                *out = true;
                return 0;
            }
            if (length == 5 && memcmp(json + tokens[value].start, "false", 5) == 0) {
                *out = false;
                return 0;
            }
            return -1;
        }
        pos = token_skip(tokens, value, count);
    }
    return -1;
}

static int array_get_json(const char* json, const jsmntok_t* tokens, int count, int object,
                          const char* key) {
    if (object < 0 || object >= count || tokens[object].type != JSMN_OBJECT)
        return -1;
    int pos = object + 1;
    for (int pair = 0; pair < tokens[object].size && pos < count; pair++) {
        const int value = pos + 1;
        char name[64];
        if (token_string(json, &tokens[pos], name, sizeof(name)) == 0 &&
            strcmp(name, key) == 0 && value < count && tokens[value].type == JSMN_ARRAY) {
            return value;
        }
        pos = token_skip(tokens, value, count);
    }
    return -1;
}

static int safe_name(const char* name) {
    if (!name[0])
        return 0;
    if ((name[0] == '.' && name[1] == '\0') ||
        (name[0] == '.' && name[1] == '.' && name[2] == '\0'))
        return 0;
    for (const char* p = name; *p; p++) {
        if (!(isalnum((unsigned char)*p) || *p == '-' || *p == '_' || *p == '.'))
            return 0;
    }
    return 1;
}

static int safe_relative(const char* path) {
    return path_safe_relative(path) ? 1 : 0;
}

static int normalize_target(char* target, size_t target_size) {
    char temporary[256];
    if (target[0] == '/') {
        const int written = snprintf(temporary, sizeof(temporary), "sdmc:%s", target);
        if (written <= 0 || (size_t)written >= sizeof(temporary))
            return -1;
        snprintf(target, target_size, "%s", temporary);
    } else if (strncmp(target, "switch/", 7) == 0) {
        const int written = snprintf(temporary, sizeof(temporary), "sdmc:/%s", target);
        if (written <= 0 || (size_t)written >= sizeof(temporary))
            return -1;
        snprintf(target, target_size, "%s", temporary);
    } else if (strncmp(target, "sdmc:switch/", 12) == 0) {
        const int written = snprintf(temporary, sizeof(temporary), "sdmc:/switch/%s", target + 12);
        if (written <= 0 || (size_t)written >= sizeof(temporary))
            return -1;
        snprintf(target, target_size, "%s", temporary);
    }
    return 0;
}

static int valid_target(const char* target) {
    static const char prefix[] = "sdmc:/switch/";
    if (strcmp(target, "sdmc:/") == 0 || strcmp(target, "sdmc:/switch") == 0)
        return 1;
    if (strncmp(target, prefix, sizeof(prefix) - 1) != 0)
        return 0;
    if (strstr(target, "..") || strchr(target, '\\'))
        return 0;
    return safe_relative(target + sizeof(prefix) - 1);
}

static int valid_sha(const char* sha) {
    if (!sha[0])
        return 1;
    if (strlen(sha) != 64)
        return 0;
    for (const char* p = sha; *p; p++) {
        if (!isxdigit((unsigned char)*p))
            return 0;
    }
    return 1;
}

int manifest_default_target(const char* name, char* target, size_t target_size) {
    if (!safe_name(name))
        return -1;
    static const char prefix[] = "sdmc:/switch/";
    const size_t length = strlen(name);
    if (length + sizeof(prefix) > target_size)
        return -1;
    memcpy(target, prefix, sizeof(prefix) - 1);
    memcpy(target + sizeof(prefix) - 1, name, length + 1);
    return 0;
}

int manifest_load(const char* path, HbManifest* manifest, char* err, size_t err_size) {
    memset(manifest, 0, sizeof(*manifest));
    FILE* file = fopen(path, "rb");
    if (!file)
        return fail(err, err_size, "cannot open packages.json");

    char* text = malloc(MANIFEST_MAX_BYTES + 1);
    if (!text) {
        fclose(file);
        return fail(err, err_size, "out of memory");
    }
    const size_t length = fread(text, 1, MANIFEST_MAX_BYTES, file);
    fclose(file);
    if (length == 0 || length >= MANIFEST_MAX_BYTES) {
        free(text);
        return fail(err, err_size, "packages.json is empty or too large");
    }
    text[length] = '\0';

    jsmn_parser parser;
    jsmntok_t* tokens = malloc(sizeof(jsmntok_t) * MANIFEST_MAX_TOKENS);
    if (!tokens) {
        free(text);
        return fail(err, err_size, "out of memory");
    }
    jsmn_init(&parser);
    const int count = jsmn_parse(&parser, text, length, tokens, MANIFEST_MAX_TOKENS);
    if (count < 0) {
        free(tokens);
        free(text);
        if (err && err_size)
            snprintf(err, err_size, "JSON parse error %d", count);
        return -1;
    }
    if (count < 1 || tokens[0].type != JSMN_OBJECT) {
        free(tokens);
        free(text);
        return fail(err, err_size, "packages.json must contain an object");
    }

    const int includes = array_get_json(text, tokens, count, 0, "include");
    if (includes >= 0 && tokens[includes].type == JSMN_ARRAY) {
        int include_pos = includes + 1;
        for (int i = 0; i < tokens[includes].size; i++) {
            if (include_pos >= count || tokens[include_pos].type != JSMN_STRING) {
                free(tokens);
                free(text);
                return fail(err, err_size, "include must be a string");
            }
            const size_t include_length = (size_t)(tokens[include_pos].end -
                                                   tokens[include_pos].start);
            if (include_length == 0 || include_length >= sizeof(manifest->includes[0]) ||
                manifest->include_count >= HBPKG_MAX_INCLUDES) {
                free(tokens);
                free(text);
                return fail(err, err_size, "too many or too long includes");
            }
            memcpy(manifest->includes[manifest->include_count],
                   text + tokens[include_pos].start, include_length);
            manifest->includes[manifest->include_count][include_length] = '\0';
            if (strstr(manifest->includes[manifest->include_count], "..") ||
                strchr(manifest->includes[manifest->include_count], '\\')) {
                free(tokens);
                free(text);
                return fail(err, err_size, "include path is unsafe");
            }
            manifest->include_count++;
            include_pos++;
        }
    }

    const int packages = array_get_json(text, tokens, count, 0, "packages");
    if (packages < 0 && manifest->include_count == 0) {
        free(tokens);
        free(text);
        return fail(err, err_size, "missing packages or include array");
    }

    if (packages >= 0) {
    int pos = packages + 1;
    for (int index = 0; index < tokens[packages].size; index++) {
        const int object = pos;
        pos = token_skip(tokens, object, count);
        if (object >= count || tokens[object].type != JSMN_OBJECT)
            continue;
        if (manifest->count >= HBPKG_MAX_PACKAGES) {
            free(tokens);
            free(text);
            return fail(err, err_size, "too many packages in manifest");
        }

        HbPackage* pkg = &manifest->items[manifest->count];
        memset(pkg, 0, sizeof(*pkg));
        pkg->target_explicit = false;
        pkg->shortcut = true;
        strcpy(pkg->branch, "main");
        if (object_get(text, tokens, count, object, "name", pkg->name, sizeof(pkg->name)) != 0 ||
            !safe_name(pkg->name)) {
            free(tokens);
            free(text);
            return fail(err, err_size, "package has no safe name");
        }
        if (object_get(text, tokens, count, object, "title", pkg->title,
                       sizeof(pkg->title)) != 0)
            strcpy(pkg->title, pkg->name);
        if (object_get(text, tokens, count, object, "author", pkg->author,
                       sizeof(pkg->author)) != 0)
            strcpy(pkg->author, "Unknown");
        object_get(text, tokens, count, object, "description", pkg->description,
                   sizeof(pkg->description));
        object_get_bool(text, tokens, count, object, "shortcut", &pkg->shortcut);
        object_get(text, tokens, count, object, "icon", pkg->icon, sizeof(pkg->icon));
        object_get(text, tokens, count, object, "release", pkg->release, sizeof(pkg->release));
        object_get(text, tokens, count, object, "asset", pkg->asset, sizeof(pkg->asset));
        if (pkg->icon[0] && strncmp(pkg->icon, "https://", 8) != 0) {
            free(tokens);
            free(text);
            return fail(err, err_size, "package icon must use https");
        }
        if (object_get(text, tokens, count, object, "version", pkg->version,
                       sizeof(pkg->version)) != 0)
            strcpy(pkg->version, "unknown");
        if (object_get(text, tokens, count, object, "url", pkg->url, sizeof(pkg->url)) != 0 &&
            object_get(text, tokens, count, object, "repo", pkg->url, sizeof(pkg->url)) != 0) {
            free(tokens);
            free(text);
            return fail(err, err_size, "package has no url");
        }
        if (strncmp(pkg->url, "https://", 8) != 0) {
            free(tokens);
            free(text);
            return fail(err, err_size, "package url must use https");
        }
        object_get(text, tokens, count, object, "branch", pkg->branch, sizeof(pkg->branch));
        object_get(text, tokens, count, object, "stripPrefix", pkg->strip_prefix,
                   sizeof(pkg->strip_prefix));
        object_get(text, tokens, count, object, "sha256", pkg->sha256, sizeof(pkg->sha256));
        if (!valid_sha(pkg->sha256)) {
            free(tokens);
            free(text);
            return fail(err, err_size, "sha256 must be 64 hex characters");
        }
        if (pkg->strip_prefix[0]) {
            char* start = pkg->strip_prefix;
            while (*start == '/')
                start++;
            memmove(pkg->strip_prefix, start, strlen(start) + 1);
            const size_t plen = strlen(pkg->strip_prefix);
            if (plen && pkg->strip_prefix[plen - 1] != '/') {
                if (plen + 1 >= sizeof(pkg->strip_prefix)) {
                    free(tokens);
                    free(text);
                    return fail(err, err_size, "stripPrefix is too long");
                }
                pkg->strip_prefix[plen] = '/';
                pkg->strip_prefix[plen + 1] = '\0';
            }
            if (strstr(pkg->strip_prefix, "..")) {
                free(tokens);
                free(text);
                return fail(err, err_size, "stripPrefix is unsafe");
            }
        }
        if (object_get(text, tokens, count, object, "target", pkg->target,
                       sizeof(pkg->target)) != 0 &&
            object_get(text, tokens, count, object, "installDir", pkg->target,
                       sizeof(pkg->target)) != 0 &&
            object_get(text, tokens, count, object, "install_dir", pkg->target,
                       sizeof(pkg->target)) != 0 &&
            object_get(text, tokens, count, object, "destination", pkg->target,
                       sizeof(pkg->target)) != 0) {
            if (manifest_default_target(pkg->name, pkg->target, sizeof(pkg->target)) != 0) {
                free(tokens);
                free(text);
                return fail(err, err_size, "cannot derive target");
            }
        } else {
            pkg->target_explicit = true;
        }
        if (normalize_target(pkg->target, sizeof(pkg->target)) != 0) {
            free(tokens);
            free(text);
            return fail(err, err_size, "install target is too long");
        }
        if (strcmp(pkg->target, "sdmc:/") != 0)
            while (strlen(pkg->target) > 1 && pkg->target[strlen(pkg->target) - 1] == '/')
                pkg->target[strlen(pkg->target) - 1] = '\0';
        object_get(text, tokens, count, object, "marker", pkg->marker, sizeof(pkg->marker));
        if (pkg->marker[0] &&
            (strncmp(pkg->marker, "sdmc:/", 6) != 0 || pkg->marker[6] == '\0' ||
             strstr(pkg->marker, "..") || strchr(pkg->marker, '\\'))) {
            free(tokens);
            free(text);
            return fail(err, err_size, "marker must be a safe sdmc path");
        }
        if (!valid_target(pkg->target)) {
            free(tokens);
            free(text);
            return fail(err, err_size, "target must be under sdmc:/switch/");
        }
        manifest->count++;
    }
    }

    free(tokens);
    free(text);
    return 0;
}
