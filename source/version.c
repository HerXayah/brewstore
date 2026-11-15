// SPDX-License-Identifier: MIT
#include "version.h"

#include <ctype.h>
#include <strings.h>

#define VERSION_PARTS 8

static int parse_numeric(const char* text, int* parts, int* count) {
    if (*text == 'v' || *text == 'V')
        text++;
    int used = 0;
    while (*text && used < VERSION_PARTS) {
        if (!isdigit((unsigned char)*text))
            return -1;
        int value = 0;
        while (isdigit((unsigned char)*text)) {
            if (value < 100000)
                value = value * 10 + (*text - '0');
            text++;
        }
        parts[used++] = value;
        if (!*text)
            break;
        if (*text != '.')
            return -1;
        text++;
        if (!*text)
            return -1;
    }
    *count = used;
    return used ? 0 : -1;
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

    int candidate_parts[VERSION_PARTS];
    int installed_parts[VERSION_PARTS];
    int candidate_count = 0;
    int installed_count = 0;
    const int candidate_numeric = parse_numeric(candidate, candidate_parts, &candidate_count);
    const int installed_numeric = parse_numeric(installed, installed_parts, &installed_count);
    if (candidate_numeric == 0 && installed_numeric == 0) {
        const int count = candidate_count > installed_count ? candidate_count : installed_count;
        for (int i = 0; i < count; i++) {
            const int left = i < candidate_count ? candidate_parts[i] : 0;
            const int right = i < installed_count ? installed_parts[i] : 0;
            if (left != right)
                return left > right ? 1 : 0;
        }
        return 0;
    }
    return 1;
}
