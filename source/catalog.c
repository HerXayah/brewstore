// SPDX-License-Identifier: MIT
#include "catalog.h"

#include "json_util.h"
#include "path_safe.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define CATALOG_MAX_BYTES (1024 * 1024)
#define CATALOG_NAME_MAX 63
#define CATALOG_TITLE_MAX 95
#define CATALOG_AUTHOR_MAX 63
#define CATALOG_DESC_MAX 511
#define CATALOG_ICON_MAX 511
#define CATALOG_URL_MAX 511
#define CATALOG_VERSION_MAX 31
#define CATALOG_RELEASE_TAG_MAX 63
#define CATALOG_ASSET_MAX 127
#define CATALOG_DL_MAX 1023
#define CATALOG_TARGET_MAX 255
#define CATALOG_MARKER_MAX 511
#define CATALOG_STRIP_MAX 127

static int fail(char* err, size_t err_size, const char* message) {
    if (err && err_size)
        snprintf(err, err_size, "%s", message);
    return -1;
}

static int get_strict(const char* json, const jsmntok_t* tokens, int count, int object,
                      const char* key, char* out, size_t out_size) {
    /* json_object_get uses the truncating decoder; use a strict lookup below. */
    if (object < 0 || object >= count || tokens[object].type != JSMN_OBJECT)
        return -1;
    int pos = object + 1;
    for (int pair = 0; pair < tokens[object].size && pos < count; pair++) {
        const int value = pos + 1;
        char name[64];
        if (json_token_string(json, &tokens[pos], name, sizeof(name)) == 0 &&
            strcmp(name, key) == 0 && value < count)
            return json_token_string_strict(json, &tokens[value], out, out_size);
        pos = json_token_skip(tokens, value, count);
    }
    return -1;
}

static int get_optional(const char* json, const jsmntok_t* tokens, int count,
                        int object, const char* key, char* out, size_t out_size) {
    out[0] = '\0';
    if (!json_object_has(json, tokens, count, object, key))
        return 0;
    return get_strict(json, tokens, count, object, key, out, out_size);
}

static int safe_name(const char* name) {
    if (!name[0] || strlen(name) > CATALOG_NAME_MAX)
        return 0;
    if (!isalnum((unsigned char)name[0]))
        return 0;
    for (const char* p = name; *p; p++)
        if (!(isalnum((unsigned char)*p) || *p == '-' || *p == '_' || *p == '.'))
            return 0;
    return 1;
}

static int safe_relative(const char* path) {
    return path_safe_relative(path) ? 1 : 0;
}

static int valid_target(const char* target) {
    static const char prefix[] = "sdmc:/switch/";
    if (strcmp(target, "sdmc:/") == 0 || strcmp(target, "sdmc:/switch") == 0)
        return 1;
    if (strncmp(target, prefix, sizeof(prefix) - 1) != 0 || strlen(target) > CATALOG_TARGET_MAX)
        return 0;
    if (strstr(target, "..") || strchr(target, '\\'))
        return 0;
    return safe_relative(target + sizeof(prefix) - 1);
}

static int valid_sha(const char* sha) {
    if (strlen(sha) != 64)
        return 0;
    for (const char* p = sha; *p; p++)
        if (!((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f')))
            return 0;
    return 1;
}

static int plain_asset(const char* name) {
    if (!name[0] || strlen(name) > CATALOG_ASSET_MAX)
        return 0;
    if (strchr(name, '/') || strchr(name, '\\') || strchr(name, ':') || strstr(name, ".."))
        return 0;
    return 1;
}

static int parse_entry(const char* json, const jsmntok_t* tokens, int count, int object,
                       HbPackage* pkg, char* err, size_t err_size) {
    memset(pkg, 0, sizeof(*pkg));
    pkg->shortcut = true;
    strcpy(pkg->branch, "main");
    if (get_strict(json, tokens, count, object, "name", pkg->name, sizeof(pkg->name)) != 0 ||
        !safe_name(pkg->name))
        return fail(err, err_size, "invalid name");
    if (get_optional(json, tokens, count, object, "title", pkg->title, sizeof(pkg->title)) != 0)
        strcpy(pkg->title, pkg->name);
    if (get_optional(json, tokens, count, object, "author", pkg->author, sizeof(pkg->author)) != 0)
        strcpy(pkg->author, "Unknown");
    if (get_optional(json, tokens, count, object, "description", pkg->description, sizeof(pkg->description)) != 0 ||
        get_optional(json, tokens, count, object, "icon", pkg->icon, sizeof(pkg->icon)) != 0 ||
        get_optional(json, tokens, count, object, "version", pkg->version, sizeof(pkg->version)) != 0 ||
        get_optional(json, tokens, count, object, "stripPrefix", pkg->strip_prefix, sizeof(pkg->strip_prefix)) != 0 ||
        get_optional(json, tokens, count, object, "marker", pkg->marker, sizeof(pkg->marker)) != 0)
        return fail(err, err_size, "field is too long");
    if (get_strict(json, tokens, count, object, "url", pkg->url, sizeof(pkg->url)) != 0 ||
        get_strict(json, tokens, count, object, "release_tag", pkg->release_tag, sizeof(pkg->release_tag)) != 0 ||
        get_strict(json, tokens, count, object, "asset", pkg->asset, sizeof(pkg->asset)) != 0 ||
        get_strict(json, tokens, count, object, "download_url", pkg->download_url, sizeof(pkg->download_url)) != 0 ||
        get_strict(json, tokens, count, object, "sha256", pkg->sha256, sizeof(pkg->sha256)) != 0)
        return fail(err, err_size, "required field is missing or too long");
    char target[CATALOG_TARGET_MAX + 1];
    const int has_target = json_object_has(json, tokens, count, object, "target");
    if (has_target) {
        if (get_strict(json, tokens, count, object, "target", target, sizeof(target)) != 0)
            return fail(err, err_size, "target is too long");
        snprintf(pkg->target, sizeof(pkg->target), "%s", target);
        pkg->target_explicit = true;
    }
    if (!pkg->name[0] || !pkg->url[0] || !pkg->asset[0] || !pkg->download_url[0] ||
        !pkg->release_tag[0] || !valid_sha(pkg->sha256))
        return fail(err, err_size, "missing required field");
    if (strncmp(pkg->url, "https://", 8) != 0 ||
        strncmp(pkg->download_url, "https://", 8) != 0)
        return fail(err, err_size, "URL is not https");
    if (pkg->icon[0] && strncmp(pkg->icon, "https://", 8) != 0)
        return fail(err, err_size, "icon is not https");
    if (!plain_asset(pkg->asset))
        return fail(err, err_size, "invalid asset");
    if (has_target && !valid_target(pkg->target))
        return fail(err, err_size, "invalid target");
    if (pkg->marker[0] &&
        (strncmp(pkg->marker, "sdmc:/", 6) != 0 || pkg->marker[6] == '\0' ||
         strstr(pkg->marker, "..") || strchr(pkg->marker, '\\')))
        return fail(err, err_size, "invalid marker");
    if (pkg->strip_prefix[0]) {
        char* start = pkg->strip_prefix;
        while (*start == '/')
            start++;
        memmove(pkg->strip_prefix, start, strlen(start) + 1);
        size_t length = strlen(pkg->strip_prefix);
        while (length && pkg->strip_prefix[length - 1] == '/')
            pkg->strip_prefix[--length] = '\0';
        if (!safe_relative(pkg->strip_prefix))
            return fail(err, err_size, "invalid stripPrefix");
        if (length + 1 >= sizeof(pkg->strip_prefix))
            return fail(err, err_size, "invalid stripPrefix");
        pkg->strip_prefix[length] = '/';
        pkg->strip_prefix[length + 1] = '\0';
    }
    if (strlen(pkg->title) > CATALOG_TITLE_MAX || strlen(pkg->author) > CATALOG_AUTHOR_MAX ||
        strlen(pkg->description) > CATALOG_DESC_MAX || strlen(pkg->icon) > CATALOG_ICON_MAX ||
        strlen(pkg->url) > CATALOG_URL_MAX || strlen(pkg->version) > CATALOG_VERSION_MAX ||
        strlen(pkg->release_tag) > CATALOG_RELEASE_TAG_MAX ||
        strlen(pkg->download_url) > CATALOG_DL_MAX || strlen(pkg->target) > CATALOG_TARGET_MAX ||
        strlen(pkg->marker) > CATALOG_MARKER_MAX || strlen(pkg->strip_prefix) > CATALOG_STRIP_MAX)
        return fail(err, err_size, "field too long");

    char text[32] = "";
    if (json_object_has(json, tokens, count, object, "size")) {
        if (get_strict(json, tokens, count, object, "size", text, sizeof(text)) != 0)
            return fail(err, err_size, "invalid size");
        char* end = NULL;
        const unsigned long long size = strtoull(text, &end, 10);
        if (!text[0] || text[0] == '-' || !end || *end)
            return fail(err, err_size, "invalid size");
        pkg->size = size;
        pkg->size_known = true;
    }
    text[0] = '\0';
    if (json_object_has(json, tokens, count, object, "published")) {
        if (get_strict(json, tokens, count, object, "published", text, sizeof(text)) != 0 ||
            strlen(text) > 23 || text[0] == '-' ||
            strspn(text, "0123456789") != strlen(text))
            return fail(err, err_size, "invalid published value");
        snprintf(pkg->published, sizeof(pkg->published), "%.23s", text);
    }
    bool shortcut = true;
    if (json_object_get_bool(json, tokens, count, object, "shortcut", &shortcut) == 0)
        pkg->shortcut = shortcut;
    return 0;
}

int catalog_load(const char* path, HbPackageList* out, CatalogStats* stats,
                 char* err, size_t err_size) {
    if (stats)
        memset(stats, 0, sizeof(*stats));
    FILE* file = fopen(path, "rb");
    if (!file)
        return fail(err, err_size, "cannot open catalogue");
    char* text = malloc(CATALOG_MAX_BYTES + 1);
    if (!text) {
        fclose(file);
        return fail(err, err_size, "out of memory");
    }
    const size_t length = fread(text, 1, CATALOG_MAX_BYTES, file);
    fclose(file);
    if (length == 0 || length >= CATALOG_MAX_BYTES) {
        free(text);
        return fail(err, err_size, "catalogue is empty or too large");
    }
    text[length] = '\0';

    jsmn_parser parser;
    jsmn_init(&parser);
    const int needed = jsmn_parse(&parser, text, length, NULL, 0);
    if (needed <= 0) {
        free(text);
        return -2;
    }
    jsmntok_t* tokens = malloc(sizeof(*tokens) * (size_t)needed);
    if (!tokens) {
        free(text);
        return fail(err, err_size, "out of memory");
    }
    jsmn_init(&parser);
    const int count = jsmn_parse(&parser, text, length, tokens, (unsigned)needed);
    if (count != needed || tokens[0].type != JSMN_OBJECT) {
        free(tokens);
        free(text);
        return fail(err, err_size, "invalid catalogue JSON");
    }
    char schema[16];
    if (get_strict(text, tokens, count, 0, "schema_version", schema, sizeof(schema)) != 0) {
        free(tokens);
        free(text);
        return -2;
    }
    if (strcmp(schema, "1") != 0) {
        free(tokens);
        free(text);
        return fail(err, err_size, "unsupported catalogue schema");
    }
    const int packages = json_array_get(text, tokens, count, 0, "packages");
    if (packages < 0) {
        free(tokens);
        free(text);
        return fail(err, err_size, "missing packages array");
    }
    char count_text[32];
    if (json_object_has(text, tokens, count, 0, "count")) {
        if (get_strict(text, tokens, count, 0, "count", count_text, sizeof(count_text)) != 0)
            { free(tokens); free(text); return fail(err, err_size, "invalid count"); }
        char* end = NULL;
        const long declared = strtol(count_text, &end, 10);
        if (!count_text[0] || !end || *end || declared != tokens[packages].size) {
            free(tokens);
            free(text);
            return fail(err, err_size, "catalogue count mismatch");
        }
    }

    int pos = packages + 1;
    for (int index = 0; index < tokens[packages].size; index++) {
        const int object = pos;
        pos = json_token_skip(tokens, object, count);
        if (object >= count || tokens[object].type != JSMN_OBJECT) {
            if (stats) stats->skipped++;
            continue;
        }
        HbPackage pkg;
        char entry_error[128] = "";
        if (parse_entry(text, tokens, count, object, &pkg, entry_error,
                        sizeof(entry_error)) != 0) {
            if (stats) stats->skipped++;
            continue;
        }
        bool duplicate = false;
        for (int i = 0; i < out->count; i++) {
            if (strcasecmp(out->items[i].name, pkg.name) == 0) {
                duplicate = true;
                break;
            }
        }
        if (duplicate || hb_list_push(out, &pkg) != 0) {
            if (stats) stats->skipped++;
            continue;
        }
        if (stats) stats->packages++;
    }
    free(tokens);
    free(text);
    return 0;
}
