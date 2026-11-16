// SPDX-License-Identifier: MIT
#include "version.h"

#include <ctype.h>
#include <stdbool.h>
#include <string.h>
#include <strings.h>

#define VERSION_PARTS 8

typedef struct {
    int core[VERSION_PARTS];
    int core_count;
    const char* prerelease;
    size_t prerelease_length;
} ParsedVersion;

static int parse_version(const char* text, ParsedVersion* out) {
    memset(out, 0, sizeof(*out));
    if (!text || !text[0])
        return -1;
    if (*text == 'v' || *text == 'V')
        text++;
    if (!isdigit((unsigned char)*text))
        return -1;

    const char* cursor = text;
    while (*cursor && out->core_count < VERSION_PARTS) {
        if (!isdigit((unsigned char)*cursor))
            return -1;
        int value = 0;
        while (isdigit((unsigned char)*cursor)) {
            if (value < 100000)
                value = value * 10 + (*cursor - '0');
            cursor++;
        }
        out->core[out->core_count++] = value;
        if (*cursor == '.') {
            cursor++;
            if (!isdigit((unsigned char)*cursor))
                return -1;
            continue;
        }
        break;
    }
    if (!out->core_count)
        return -1;
    if (*cursor == '-') {
        cursor++;
        out->prerelease = cursor;
        while (*cursor && *cursor != '+')
            cursor++;
        out->prerelease_length = (size_t)(cursor - out->prerelease);
        if (!out->prerelease_length)
            return -1;
    }
    if (*cursor == '+') {
        cursor++;
        while (*cursor && (isalnum((unsigned char)*cursor) || *cursor == '.' || *cursor == '-'))
            cursor++;
    }
    return *cursor ? -1 : 0;
}

static int compare_prerelease(const char* left, size_t left_length,
                              const char* right, size_t right_length) {
    const char* left_cursor = left;
    const char* right_cursor = right;
    const char* left_end = left + left_length;
    const char* right_end = right + right_length;
    while (left_cursor < left_end && right_cursor < right_end) {
        const char* left_part_end = memchr(left_cursor, '.', (size_t)(left_end - left_cursor));
        const char* right_part_end = memchr(right_cursor, '.', (size_t)(right_end - right_cursor));
        if (!left_part_end)
            left_part_end = left_end;
        if (!right_part_end)
            right_part_end = right_end;
        size_t left_part_length = (size_t)(left_part_end - left_cursor);
        size_t right_part_length = (size_t)(right_part_end - right_cursor);

        bool left_numeric = left_part_length > 0;
        bool right_numeric = right_part_length > 0;
        for (size_t i = 0; i < left_part_length; i++) {
            if (!isdigit((unsigned char)left_cursor[i]))
                left_numeric = false;
        }
        for (size_t i = 0; i < right_part_length; i++) {
            if (!isdigit((unsigned char)right_cursor[i]))
                right_numeric = false;
        }

        int result = 0;
        if (left_numeric && right_numeric) {
            while (left_part_length > 1 && *left_cursor == '0') {
                left_cursor++;
                left_part_length--;
            }
            while (right_part_length > 1 && *right_cursor == '0') {
                right_cursor++;
                right_part_length--;
            }
            if (left_part_length != right_part_length)
                result = left_part_length > right_part_length ? 1 : -1;
            else
                result = strncmp(left_cursor, right_cursor, left_part_length);
        } else if (left_numeric != right_numeric) {
            result = left_numeric ? -1 : 1;
        } else {
            const size_t length = left_part_length < right_part_length
                                      ? left_part_length : right_part_length;
            result = strncasecmp(left_cursor, right_cursor, length);
            if (!result && left_part_length != right_part_length)
                result = left_part_length > right_part_length ? 1 : -1;
        }
        if (result)
            return result > 0 ? 1 : -1;

        left_cursor = left_part_end < left_end ? left_part_end + 1 : left_end;
        right_cursor = right_part_end < right_end ? right_part_end + 1 : right_end;
    }
    if (left_cursor < left_end)
        return 1;
    if (right_cursor < right_end)
        return -1;
    return 0;
}

static int floating_version(const char* version) {
    return !version || !version[0] ||
           strcasecmp(version, "latest") == 0 ||
           strcasecmp(version, "test") == 0 ||
           strcasecmp(version, "unknown") == 0 ||
           strcasecmp(version, "main") == 0 ||
           strcasecmp(version, "master") == 0;
}

int version_is_newer(const char* candidate, const char* installed) {
    if (!candidate || !candidate[0] || !installed || !installed[0])
        return 0;
    if (strcasecmp(candidate, installed) == 0)
        return 0;
    if (floating_version(candidate) || floating_version(installed))
        return 0;

    ParsedVersion left;
    ParsedVersion right;
    if (parse_version(candidate, &left) != 0 || parse_version(installed, &right) != 0)
        return 0;

    const int count = left.core_count > right.core_count ? left.core_count : right.core_count;
    for (int i = 0; i < count; i++) {
        const int a = i < left.core_count ? left.core[i] : 0;
        const int b = i < right.core_count ? right.core[i] : 0;
        if (a != b)
            return a > b ? 1 : 0;
    }

    if (!left.prerelease && right.prerelease)
        return 1;
    if (left.prerelease && !right.prerelease)
        return 0;
    if (!left.prerelease && !right.prerelease)
        return 0;
    return compare_prerelease(left.prerelease, left.prerelease_length,
                              right.prerelease, right.prerelease_length) > 0;
}
