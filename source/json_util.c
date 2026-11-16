// SPDX-License-Identifier: MIT
#include "json_util.h"

#include <string.h>

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

int json_token_string(const char* json, const jsmntok_t* token, char* out, size_t out_size) {
    if (!token || token->end < token->start || !out_size)
        return -1;
    if (token->type == JSMN_PRIMITIVE) {
        const size_t length = (size_t)(token->end - token->start);
        if (length >= out_size)
            return -1;
        memcpy(out, json + token->start, length);
        out[length] = '\0';
        return 0;
    }
    if (token->type != JSMN_STRING)
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

int json_token_skip(const jsmntok_t* tokens, int index, int count) {
    if (index < 0 || index >= count)
        return count;
    int pos = index + 1;
    if (tokens[index].type == JSMN_OBJECT) {
        for (int pair = 0; pair < tokens[index].size; pair++) {
            pos = json_token_skip(tokens, pos, count);
            pos = json_token_skip(tokens, pos, count);
        }
    } else if (tokens[index].type == JSMN_ARRAY) {
        for (int child = 0; child < tokens[index].size; child++)
            pos = json_token_skip(tokens, pos, count);
    }
    return pos;
}

int json_object_get(const char* json, const jsmntok_t* tokens, int count, int object,
                    const char* key, char* out, size_t out_size) {
    if (object < 0 || object >= count || tokens[object].type != JSMN_OBJECT)
        return -1;
    int pos = object + 1;
    for (int pair = 0; pair < tokens[object].size && pos < count; pair++) {
        const int value = pos + 1;
        char name[64];
        if (json_token_string(json, &tokens[pos], name, sizeof(name)) == 0 &&
            strcmp(name, key) == 0 && value < count)
            return json_token_string(json, &tokens[value], out, out_size);
        pos = json_token_skip(tokens, value, count);
    }
    return -1;
}

int json_object_get_bool(const char* json, const jsmntok_t* tokens, int count, int object,
                         const char* key, bool* out) {
    if (object < 0 || object >= count || tokens[object].type != JSMN_OBJECT)
        return -1;
    int pos = object + 1;
    for (int pair = 0; pair < tokens[object].size && pos < count; pair++) {
        const int value = pos + 1;
        char name[64];
        if (json_token_string(json, &tokens[pos], name, sizeof(name)) == 0 &&
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
        pos = json_token_skip(tokens, value, count);
    }
    return -1;
}

int json_array_get(const char* json, const jsmntok_t* tokens, int count, int object,
                   const char* key) {
    if (object < 0 || object >= count || tokens[object].type != JSMN_OBJECT)
        return -1;
    int pos = object + 1;
    for (int pair = 0; pair < tokens[object].size && pos < count; pair++) {
        const int value = pos + 1;
        char name[64];
        if (json_token_string(json, &tokens[pos], name, sizeof(name)) == 0 &&
            strcmp(name, key) == 0 && value < count && tokens[value].type == JSMN_ARRAY)
            return value;
        pos = json_token_skip(tokens, value, count);
    }
    return -1;
}

int json_object_get_object(const char* json, const jsmntok_t* tokens, int count, int object,
                           const char* key) {
    if (object < 0 || object >= count || tokens[object].type != JSMN_OBJECT)
        return -1;
    int pos = object + 1;
    for (int pair = 0; pair < tokens[object].size && pos < count; pair++) {
        const int value = pos + 1;
        char name[64];
        if (json_token_string(json, &tokens[pos], name, sizeof(name)) == 0 &&
            strcmp(name, key) == 0 && value < count && tokens[value].type == JSMN_OBJECT)
            return value;
        pos = json_token_skip(tokens, value, count);
    }
    return -1;
}
