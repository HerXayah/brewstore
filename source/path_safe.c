// SPDX-License-Identifier: MIT
#include "path_safe.h"

#include <stddef.h>
#include <string.h>

static bool valid_utf8(const char* text) {
    const unsigned char* p = (const unsigned char*)text;
    while (*p) {
        if (*p < 0x80) {
            p++;
            continue;
        }
        if (*p >= 0xc2 && *p <= 0xdf) {
            if ((p[1] & 0xc0) != 0x80)
                return false;
            p += 2;
        } else if (*p == 0xe0) {
            if ((p[1] & 0xe0) != 0xa0 || (p[2] & 0xc0) != 0x80)
                return false;
            p += 3;
        } else if ((*p >= 0xe1 && *p <= 0xec) || *p == 0xee || *p == 0xef) {
            if ((p[1] & 0xc0) != 0x80 || (p[2] & 0xc0) != 0x80)
                return false;
            p += 3;
        } else if (*p == 0xed) {
            if ((p[1] & 0xe0) != 0x80 || (p[2] & 0xc0) != 0x80)
                return false;
            p += 3;
        } else if (*p == 0xf0) {
            if ((p[1] & 0xf0) != 0x90 || (p[2] & 0xc0) != 0x80 ||
                (p[3] & 0xc0) != 0x80)
                return false;
            p += 4;
        } else if (*p >= 0xf1 && *p <= 0xf3) {
            if ((p[1] & 0xc0) != 0x80 || (p[2] & 0xc0) != 0x80 ||
                (p[3] & 0xc0) != 0x80)
                return false;
            p += 4;
        } else if (*p == 0xf4) {
            if ((p[1] & 0xf0) != 0x80 || (p[2] & 0xc0) != 0x80 ||
                (p[3] & 0xc0) != 0x80)
                return false;
            p += 4;
        } else {
            return false;
        }
    }
    return true;
}

bool path_safe_relative(const char* path) {
    if (!path || !path[0] || path[0] == '/' || strchr(path, '\\') ||
        strchr(path, ':') || !valid_utf8(path))
        return false;

    const char* part = path;
    while (*part) {
        const char* end = strchr(part, '/');
        const size_t length = end ? (size_t)(end - part) : strlen(part);
        if (length == 0 || length > 255 ||
            (length == 2 && part[0] == '.' && part[1] == '.'))
            return false;
        for (size_t i = 0; i < length; i++) {
            const unsigned char c = (unsigned char)part[i];
            if (c < 0x20 || c == 0x7f || strchr("*?\"<>|", c))
                return false;
        }
        if (!end)
            break;
        part = end + 1;
    }
    return true;
}
