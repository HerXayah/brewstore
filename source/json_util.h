// SPDX-License-Identifier: MIT
#pragma once

#include <stdbool.h>
#include <stddef.h>

#define JSMN_STRICT
#define JSMN_STATIC
#include "jsmn.h"

/// Decodes a JSON string token (including \u escapes), or copies a primitive.
int json_token_string(const char* json, const jsmntok_t* token, char* out, size_t out_size);

/// Strict variant: fails instead of silently truncating a string.
int json_token_string_strict(const char* json, const jsmntok_t* token, char* out, size_t out_size);

/// Returns the index just past a token and its children.
int json_token_skip(const jsmntok_t* tokens, int index, int count);

/// Returns non-zero when the object contains the key.
int json_object_has(const char* json, const jsmntok_t* tokens, int count,
                    int object, const char* key);

/// Finds a string value by key in an object.
int json_object_get(const char* json, const jsmntok_t* tokens, int count, int object,
                    const char* key, char* out, size_t out_size);

/// Finds a boolean value by key in an object.
int json_object_get_bool(const char* json, const jsmntok_t* tokens, int count, int object,
                         const char* key, bool* out);

/// Finds an array token by key in an object.
int json_array_get(const char* json, const jsmntok_t* tokens, int count, int object,
                   const char* key);

/// Finds an object token by key in an object.
int json_object_get_object(const char* json, const jsmntok_t* tokens, int count, int object,
                           const char* key);
