// SPDX-License-Identifier: MIT
#include "shortcuts.h"

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#define SHORTCUT_LINE 1024

static void marker_for(const char* package, char* out, size_t out_size) {
    snprintf(out, out_size, "# brewstore:%s", package);
}

static void clean_title(const char* title, const char* package, char* out, size_t out_size) {
    const char* source = title && title[0] ? title : package;
    size_t used = 0;
    while (*source && used + 1 < out_size) {
        const unsigned char c = (unsigned char)*source++;
        out[used++] = (c == '\r' || c == '\n' || c == '=') ? ' ' : (char)c;
    }
    while (used && out[used - 1] == ' ')
        used--;
    out[used] = '\0';
    if (!used)
        snprintf(out, out_size, "%s", package);
}

int shortcuts_set(const char* path, const char* package, const char* title,
                  const char* nro) {
    if (!path || !package || !package[0])
        return -1;
    if (!nro || !nro[0])
        return shortcuts_remove(path, package);
    if (strncmp(nro, "sdmc:/", 6) != 0 || strlen(nro) >= 512 ||
        strstr(nro, "\r") || strstr(nro, "\n"))
        return -1;
    const size_t nro_length = strlen(nro);
    if (nro_length < 4 || strcasecmp(nro + nro_length - 4, ".nro") != 0)
        return -1;

    char marker[128];
    marker_for(package, marker, sizeof(marker));

    char temporary[512];
    snprintf(temporary, sizeof(temporary), "%s.tmp", path);
    FILE* output = fopen(temporary, "wb");
    if (!output)
        return -1;

    FILE* input = fopen(path, "rb");
    bool skip_next = false;
    if (input) {
        char line[SHORTCUT_LINE];
        while (fgets(line, sizeof(line), input)) {
            line[strcspn(line, "\r\n")] = '\0';
            if (skip_next) {
                skip_next = false;
                continue;
            }
            if (strcmp(line, marker) == 0) {
                skip_next = true;
                continue;
            }
            fprintf(output, "%s\n", line);
        }
        fclose(input);
    } else if (errno != ENOENT) {
        fclose(output);
        remove(temporary);
        return -1;
    }

    char name[128];
    clean_title(title, package, name, sizeof(name));
    fprintf(output, "%s\n%s=%s\n", marker, name, nro);
    if (fclose(output) != 0) {
        remove(temporary);
        return -1;
    }
    remove(path);
    if (rename(temporary, path) != 0) {
        remove(temporary);
        return -1;
    }
    return 0;
}

int shortcuts_remove(const char* path, const char* package) {
    if (!path || !package || !package[0])
        return -1;
    FILE* input = fopen(path, "rb");
    if (!input)
        return errno == ENOENT ? 0 : -1;

    char marker[128];
    marker_for(package, marker, sizeof(marker));
    char temporary[512];
    snprintf(temporary, sizeof(temporary), "%s.tmp", path);
    FILE* output = fopen(temporary, "wb");
    if (!output) {
        fclose(input);
        return -1;
    }

    bool skip_next = false;
    char line[SHORTCUT_LINE];
    while (fgets(line, sizeof(line), input)) {
        line[strcspn(line, "\r\n")] = '\0';
        if (skip_next) {
            skip_next = false;
            continue;
        }
        if (strcmp(line, marker) == 0) {
            skip_next = true;
            continue;
        }
        fprintf(output, "%s\n", line);
    }
    fclose(input);
    if (fclose(output) != 0) {
        remove(temporary);
        return -1;
    }
    remove(path);
    if (rename(temporary, path) != 0) {
        remove(temporary);
        return -1;
    }
    return 0;
}
