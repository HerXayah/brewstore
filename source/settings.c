// SPDX-License-Identifier: MIT
#include "settings.h"

#define JSMN_STRICT
#define JSMN_STATIC
#include "jsmn.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SETTINGS_MAX 8192

static int fail(char* err, size_t err_size, const char* message) {
    if (err && err_size)
        snprintf(err, err_size, "%s", message);
    return -1;
}

int settings_valid_path(const char* path) {
    if (!path || strncmp(path, "sdmc:/switch", 12) != 0)
        return 0;
    if (path[12] && path[12] != '/')
        return 0;
    if (strstr(path, "..") || strchr(path, '\\'))
        return 0;
    for (const char* p = path + 5; *p; p++) {
        if (*p == ':')
            return 0;
    }
    return 1;
}

int settings_valid_proxy(const char* proxy) {
    if (!proxy || !proxy[0])
        return 1;
    if (strncmp(proxy, "https://", 8) != 0)
        return 0;
    return !strchr(proxy, '"') && !strchr(proxy, '\\') &&
           !strchr(proxy, ' ') && !strchr(proxy, '\n') && !strchr(proxy, '\r');
}

int settings_save(const char* path, const AppSettings* settings, char* err, size_t err_size) {
    char temporary[320];
    snprintf(temporary, sizeof(temporary), "%s.tmp", path);
    FILE* file = fopen(temporary, "wb");
    if (!file)
        return fail(err, err_size, "cannot create settings.json");
    fprintf(file,
            "{\n  \"defaultInstallPath\": \"%s\",\n  \"downloadProxy\": \"%s\"\n}\n",
            settings->default_install_path, settings->download_proxy);
    if (fclose(file) != 0) {
        remove(temporary);
        return fail(err, err_size, "cannot write settings.json");
    }
    remove(path);
    if (rename(temporary, path) != 0) {
        remove(temporary);
        return fail(err, err_size, "cannot replace settings.json");
    }
    return 0;
}

int settings_load(const char* path, AppSettings* settings, char* err, size_t err_size) {
    memset(settings, 0, sizeof(*settings));
    strcpy(settings->default_install_path, "sdmc:/switch");
    settings->download_proxy[0] = '\0';

    FILE* file = fopen(path, "rb");
    if (!file) {
        if (errno == ENOENT)
            return settings_save(path, settings, err, err_size);
        return fail(err, err_size, "cannot open settings.json");
    }
    char text[SETTINGS_MAX + 1];
    const size_t length = fread(text, 1, SETTINGS_MAX, file);
    fclose(file);
    if (!length || length >= SETTINGS_MAX)
        return fail(err, err_size, "settings.json is empty or too large");
    text[length] = '\0';

    jsmntok_t tokens[256];
    jsmn_parser parser;
    jsmn_init(&parser);
    const int count = jsmn_parse(&parser, text, length, tokens, 256);
    if (count < 1 || tokens[0].type != JSMN_OBJECT)
        return fail(err, err_size, "settings.json is invalid");

    int position = 1;
    for (int pair = 0; pair < tokens[0].size && position + 1 < count; pair++) {
        const int key = position;
        const int value = position + 1;
        if (tokens[key].type == JSMN_STRING && tokens[value].type == JSMN_STRING &&
            (size_t)(tokens[key].end - tokens[key].start) == 18 &&
            memcmp(text + tokens[key].start, "defaultInstallPath", 18) == 0) {
            const size_t value_length = (size_t)(tokens[value].end - tokens[value].start);
            if (value_length >= sizeof(settings->default_install_path))
                return fail(err, err_size, "defaultInstallPath is too long");
            memcpy(settings->default_install_path, text + tokens[value].start, value_length);
            settings->default_install_path[value_length] = '\0';
            size_t path_length = strlen(settings->default_install_path);
            while (path_length > 12 && settings->default_install_path[path_length - 1] == '/')
                settings->default_install_path[--path_length] = '\0';
            if (!settings_valid_path(settings->default_install_path))
                return fail(err, err_size, "defaultInstallPath must be under sdmc:/switch");
            position = value + 1;
            continue;
        }
        if (tokens[key].type == JSMN_STRING && tokens[value].type == JSMN_STRING &&
            (size_t)(tokens[key].end - tokens[key].start) == 13 &&
            memcmp(text + tokens[key].start, "downloadProxy", 13) == 0) {
            const size_t proxy_length = (size_t)(tokens[value].end - tokens[value].start);
            if (proxy_length >= sizeof(settings->download_proxy))
                return fail(err, err_size, "downloadProxy is too long");
            memcpy(settings->download_proxy, text + tokens[value].start, proxy_length);
            settings->download_proxy[proxy_length] = '\0';
            if (!settings_valid_proxy(settings->download_proxy))
                return fail(err, err_size, "downloadProxy must be an https URL");
        }
        position = value + 1;
    }
    return 0;
}
