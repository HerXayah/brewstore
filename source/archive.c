// SPDX-License-Identifier: MIT
#include "archive.h"
#include "path_safe.h"

#include <lzma.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/types.h>
#include <zlib.h>

#define ARCHIVE_MAX_FILE (1024ull * 1024ull * 1024ull)
#define ARCHIVE_MAX_TOTAL (8ull * 1024ull * 1024ull * 1024ull)
#define ARCHIVE_NAME_MAX 1024
#define ARCHIVE_READ_SIZE (128 * 1024)
#define TAR_BLOCK 512

typedef int (*TarEntry)(void* context, const char* name, char type,
                        unsigned long long size, FILE* file);

typedef struct {
    const char* prefix;
    int total;
    int done;
    unsigned long long bytes;
    const char* target;
    ZipProgress progress;
    void* user;
    char error[128];
} TarExtract;

typedef struct {
    char root[256];
    bool first;
    bool valid;
} TarRoot;

static void set_error(char* err, size_t err_size, const char* message) {
    if (err && err_size)
        snprintf(err, err_size, "%s", message);
}

static bool ends_with_ci(const char* text, const char* suffix) {
    const size_t text_length = strlen(text);
    const size_t suffix_length = strlen(suffix);
    return suffix_length <= text_length &&
           strcasecmp(text + text_length - suffix_length, suffix) == 0;
}

bool archive_is_archive(const char* name) {
    return name && (ends_with_ci(name, ".zip") || ends_with_ci(name, ".tar") ||
                    ends_with_ci(name, ".tar.gz") || ends_with_ci(name, ".tgz") ||
                    ends_with_ci(name, ".tar.xz") || ends_with_ci(name, ".txz"));
}

static bool parse_number(const unsigned char* field, size_t length,
                         unsigned long long* value) {
    if (length && (field[0] & 0x80)) {
        unsigned long long result = field[0] & 0x7f;
        for (size_t i = 1; i < length; i++)
            result = (result << 8) | field[i];
        *value = result;
        return true;
    }
    unsigned long long result = 0;
    bool seen = false;
    for (size_t i = 0; i < length; i++) {
        const unsigned char c = field[i];
        if (c == 0 || c == ' ')
            continue;
        if (c < '0' || c > '7')
            return false;
        result = result * 8 + (unsigned)(c - '0');
        seen = true;
    }
    if (!seen)
        return false;
    *value = result;
    return true;
}

static void copy_field(char* out, size_t out_size, const unsigned char* field,
                       size_t length) {
    size_t used = 0;
    while (used < length && used + 1 < out_size && field[used])
        used++;
    memcpy(out, field, used);
    out[used] = '\0';
}

static bool tar_header_valid(const unsigned char* header) {
    unsigned long long stored = 0;
    if (!parse_number(header + 148, 8, &stored))
        return false;
    unsigned long long sum = 0;
    for (size_t i = 0; i < TAR_BLOCK; i++)
        sum += (i >= 148 && i < 156) ? ' ' : header[i];
    return sum == stored;
}

static int skip_bytes(FILE* file, unsigned long long size) {
    while (size) {
        const size_t chunk = size > ARCHIVE_READ_SIZE ? ARCHIVE_READ_SIZE : (size_t)size;
        unsigned char buffer[ARCHIVE_READ_SIZE];
        if (fread(buffer, 1, chunk, file) != chunk)
            return -1;
        size -= chunk;
    }
    return 0;
}

static int skip_padding(FILE* file, unsigned long long size) {
    const size_t padding = (size_t)((TAR_BLOCK - (size % TAR_BLOCK)) % TAR_BLOCK);
    return padding ? skip_bytes(file, padding) : 0;
}

static void normalize_name(char* name) {
    while (name[0] == '.' && name[1] == '/')
        memmove(name, name + 2, strlen(name + 2) + 1);
}

static void trim_directory_slash(char* name) {
    const size_t length = strlen(name);
    if (length && name[length - 1] == '/')
        name[length - 1] = '\0';
}

static void pax_path(const char* data, size_t length, char* out, size_t out_size) {
    size_t position = 0;
    while (position < length) {
        const size_t start = position;
        while (position < length && data[position] != ' ')
            position++;
        if (position >= length)
            break;
        char* end = NULL;
        const unsigned long record = strtoul(data + start, &end, 10);
        if (end != data + position || record == 0 || start + record > length)
            break;
        const size_t record_end = start + (size_t)record;
        const char* field = data + position + 1;
        const char* equals = memchr(field, '=', record_end - (size_t)(field - data));
        if (equals && (size_t)(equals - field) == 4 && memcmp(field, "path", 4) == 0) {
            const char* value = equals + 1;
            size_t value_length = record_end - (size_t)(value - data);
            while (value_length && (value[value_length - 1] == '\n' ||
                                    value[value_length - 1] == '\0'))
                value_length--;
            if (value_length >= out_size)
                value_length = out_size - 1;
            memcpy(out, value, value_length);
            out[value_length] = '\0';
        }
        position = record_end;
    }
}

static int read_text(FILE* file, unsigned long long size, char* out, size_t out_size) {
    const size_t wanted = size < out_size - 1 ? (size_t)size : out_size - 1;
    out[0] = '\0';
    if (wanted && fread(out, 1, wanted, file) != wanted)
        return -1;
    out[wanted] = '\0';
    if (size > wanted && skip_bytes(file, size - wanted) != 0)
        return -1;
    return skip_padding(file, size);
}

static int tar_walk(FILE* file, TarEntry entry, void* context, char* err,
                    size_t err_size) {
    unsigned char header[TAR_BLOCK];
    char long_name[ARCHIVE_NAME_MAX] = "";
    char pax_name[ARCHIVE_NAME_MAX] = "";
    bool have_long = false;
    bool have_pax = false;
    for (;;) {
        const size_t read = fread(header, 1, sizeof(header), file);
        if (!read)
            return 0;
        if (read != sizeof(header)) {
            set_error(err, err_size, "truncated tar header");
            return -1;
        }
        bool empty = true;
        for (size_t i = 0; i < sizeof(header); i++) {
            if (header[i]) {
                empty = false;
                break;
            }
        }
        if (empty)
            return 0;
        if (!tar_header_valid(header)) {
            set_error(err, err_size, "invalid tar header");
            return -1;
        }

        unsigned long long size = 0;
        if (!parse_number(header + 124, 12, &size)) {
            set_error(err, err_size, "invalid tar entry size");
            return -1;
        }
        if (size > ARCHIVE_MAX_TOTAL) {
            set_error(err, err_size, "tar entry is too large");
            return -1;
        }
        const char type = (char)header[156];

        if (type == 'L' || type == 'K') {
            if (type == 'L' && read_text(file, size, long_name, sizeof(long_name)) != 0) {
                set_error(err, err_size, "cannot read tar long name");
                return -1;
            }
            if (type == 'K') {
                if (skip_bytes(file, size) != 0 ||
                    skip_padding(file, size) != 0) {
                    set_error(err, err_size, "cannot skip tar link name");
                    return -1;
                }
            }
            have_long = type == 'L';
            continue;
        }
        if (type == 'x' || type == 'g') {
            if (type == 'x') {
                char pax[16384];
                if (read_text(file, size, pax, sizeof(pax)) != 0) {
                    set_error(err, err_size, "cannot read tar extended header");
                    return -1;
                }
                pax_path(pax, strlen(pax), pax_name, sizeof(pax_name));
                have_pax = pax_name[0] != '\0';
            } else {
                if (skip_bytes(file, size) != 0 ||
                    skip_padding(file, size) != 0) {
                    set_error(err, err_size, "cannot skip tar extended header");
                    return -1;
                }
            }
            continue;
        }

        char name[ARCHIVE_NAME_MAX];
        if (have_pax) {
            snprintf(name, sizeof(name), "%s", pax_name);
        } else if (have_long) {
            snprintf(name, sizeof(name), "%s", long_name);
        } else {
            char base[101];
            char prefix[156];
            copy_field(base, sizeof(base), header, 100);
            copy_field(prefix, sizeof(prefix), header + 345, 155);
            if (prefix[0])
                snprintf(name, sizeof(name), "%s/%s", prefix, base);
            else
                snprintf(name, sizeof(name), "%s", base);
        }
        have_long = false;
        have_pax = false;
        normalize_name(name);

        int consumed = 0;
        if (type == '0' || type == '\0' || type == '7' || type == '5' ||
            type == '1' || type == '2' || type == '3' || type == '4' ||
            type == '6') {
            consumed = entry(context, name, type, size, file);
            if (consumed < 0)
                return -1;
        }
        if (!consumed && skip_bytes(file, size) != 0) {
            set_error(err, err_size, "cannot skip tar entry data");
            return -1;
        }
        if (skip_padding(file, size) != 0) {
            set_error(err, err_size, "cannot skip tar padding");
            return -1;
        }
    }
}

static int root_entry(void* context, const char* name, char type,
                      unsigned long long size, FILE* file) {
    (void)type;
    (void)size;
    (void)file;
    TarRoot* root = context;
    if (!root->valid)
        return 0;
    const bool directory = type == '5' || (name[0] && name[strlen(name) - 1] == '/');
    char copy[ARCHIVE_NAME_MAX];
    snprintf(copy, sizeof(copy), "%s", name);
    trim_directory_slash(copy);
    if (!copy[0])
        return 0;
    if (!path_safe_relative(copy)) {
        root->valid = false;
        return 0;
    }
    const char* slash = strchr(copy, '/');
    if (!slash) {
        if (!directory)
            root->valid = false;
        return 0;
    }
    const size_t length = (size_t)(slash - copy);
    if (!length || length >= sizeof(root->root)) {
        root->valid = false;
        return 0;
    }
    if (root->first) {
        memcpy(root->root, copy, length);
        root->root[length] = '\0';
        root->first = false;
    } else if (strncmp(copy, root->root, length) != 0 || copy[length] != '/') {
        root->valid = false;
    }
    return 0;
}

static int relative_entry(TarExtract* extract, const char* name, char type,
                          char* relative, size_t relative_size) {
    char copy[ARCHIVE_NAME_MAX];
    snprintf(copy, sizeof(copy), "%s", name);
    char normalized[ARCHIVE_NAME_MAX];
    snprintf(normalized, sizeof(normalized), "%s", copy);
    normalize_name(normalized);
    trim_directory_slash(normalized);
    if (!normalized[0])
        return 0;
    if (!path_safe_relative(normalized)) {
        set_error(extract->error, sizeof(extract->error), "unsafe path in archive");
        return -1;
    }

    const char* prefix = extract->prefix;
    const size_t prefix_length = prefix ? strlen(prefix) : 0;
    if (prefix_length && strncmp(normalized, prefix, prefix_length) != 0)
        return 0;
    const char* selected = normalized + prefix_length;
    if (!selected[0])
        return 0;
    if (strncmp(selected, "__MACOSX/", 9) == 0 || strcmp(selected, "__MACOSX") == 0)
        return 0;
    if (type == '1' || type == '2')
        return 0;
    if (strlen(selected) >= relative_size)
        return 0;
    snprintf(relative, relative_size, "%s", selected);
    return 1;
}

static int count_entry(void* context, const char* name, char type,
                       unsigned long long size, FILE* file) {
    (void)file;
    TarExtract* extract = context;
    char relative[ARCHIVE_NAME_MAX];
    const int selected = relative_entry(extract, name, type, relative, sizeof(relative));
    if (selected < 0)
        return -1;
    if (!selected || type == '5')
        return 0;
    if (size > ARCHIVE_MAX_FILE || extract->bytes + size > ARCHIVE_MAX_TOTAL) {
        set_error(extract->error, sizeof(extract->error),
                  "archive expands beyond the size limit");
        return -1;
    }
    extract->bytes += size;
    extract->total++;
    return 0;
}

static int extract_entry(void* context, const char* name, char type,
                         unsigned long long size, FILE* file) {
    TarExtract* extract = context;
    char relative[ARCHIVE_NAME_MAX];
    const int selected = relative_entry(extract, name, type, relative, sizeof(relative));
    if (selected < 0)
        return -1;
    if (!selected)
        return 0;
    if (!path_safe_relative(relative)) {
        set_error(extract->error, sizeof(extract->error), "unsafe path in archive");
        return -1;
    }

    char full[1024];
    const size_t target_length = strlen(extract->target);
    const bool target_slash = target_length && extract->target[target_length - 1] == '/';
    const int written = snprintf(full, sizeof(full), target_slash ? "%s%s" : "%s/%s",
                                 extract->target, relative);
    if (written <= 0 || (size_t)written >= sizeof(full)) {
        set_error(extract->error, sizeof(extract->error), "archive path is too long");
        return -1;
    }

    if (type == '5' || (relative[0] && relative[strlen(relative) - 1] == '/')) {
        if (path_mkdirs(full) != 0) {
            set_error(extract->error, sizeof(extract->error),
                      "cannot create target directory");
            return -1;
        }
        return 0;
    }
    if (type != '0' && type != '\0' && type != '7')
        return 0;
    if (size > ARCHIVE_MAX_FILE || extract->bytes + size > ARCHIVE_MAX_TOTAL) {
        set_error(extract->error, sizeof(extract->error),
                  "archive expands beyond the size limit");
        return -1;
    }

    char parent[1024];
    snprintf(parent, sizeof(parent), "%s", full);
    char* slash = strrchr(parent, '/');
    if (slash) {
        *slash = '\0';
        if (path_mkdirs(parent) != 0) {
            set_error(extract->error, sizeof(extract->error),
                      "cannot create target directory");
            return -1;
        }
    }
    FILE* output = fopen(full, "wb");
    if (!output) {
        set_error(extract->error, sizeof(extract->error), "cannot create target file");
        return -1;
    }
    unsigned long long remaining = size;
    bool failed = false;
    while (remaining) {
        const size_t chunk = remaining > ARCHIVE_READ_SIZE ? ARCHIVE_READ_SIZE
                                                           : (size_t)remaining;
        unsigned char buffer[ARCHIVE_READ_SIZE];
        if (fread(buffer, 1, chunk, file) != chunk ||
            fwrite(buffer, 1, chunk, output) != chunk) {
            failed = true;
            break;
        }
        remaining -= chunk;
    }
    if (fclose(output) != 0)
        failed = true;
    if (failed) {
        remove(full);
        set_error(extract->error, sizeof(extract->error), "cannot write target file");
        return -1;
    }
    extract->bytes += size;
    extract->done++;
    if (extract->progress)
        extract->progress(extract->done, extract->total, relative, extract->user);
    return 1;
}

static int tar_extract_file(const char* path, const char* target,
                            const char* strip_prefix, char* err, size_t err_size,
                            ZipProgress progress, void* user) {
    FILE* file = fopen(path, "rb");
    if (!file) {
        set_error(err, err_size, "cannot open archive");
        return -1;
    }

    char prefix[512] = "";
    if (strip_prefix && strip_prefix[0]) {
        if (strlen(strip_prefix) >= sizeof(prefix)) {
            fclose(file);
            set_error(err, err_size, "stripPrefix is too long");
            return -1;
        }
        snprintf(prefix, sizeof(prefix), "%s", strip_prefix);
        const size_t length = strlen(prefix);
        if (length && prefix[length - 1] != '/') {
            if (length + 1 >= sizeof(prefix)) {
                fclose(file);
                set_error(err, err_size, "stripPrefix is too long");
                return -1;
            }
            prefix[length] = '/';
            prefix[length + 1] = '\0';
        }
    } else {
        TarRoot root = {.first = true, .valid = true};
        if (tar_walk(file, root_entry, &root, err, err_size) != 0) {
            fclose(file);
            return -1;
        }
        if (root.valid && root.root[0]) {
            if (strlen(root.root) + 2 > sizeof(prefix)) {
                fclose(file);
                set_error(err, err_size, "archive root name is too long");
                return -1;
            }
            snprintf(prefix, sizeof(prefix), "%s/", root.root);
        }
        if (fseek(file, 0, SEEK_SET) != 0) {
            fclose(file);
            set_error(err, err_size, "cannot rewind archive");
            return -1;
        }
    }

    TarExtract count = {.prefix = prefix, .target = target,
                        .progress = progress, .user = user};
    if (tar_walk(file, count_entry, &count, err, err_size) != 0) {
        if (count.error[0])
            set_error(err, err_size, count.error);
        fclose(file);
        return -1;
    }
    if (prefix[0] && count.total == 0) {
        fclose(file);
        set_error(err, err_size, "stripPrefix matched no archive entries");
        return -1;
    }
    if (fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        set_error(err, err_size, "cannot rewind archive");
        return -1;
    }

    TarExtract extract = {.prefix = prefix, .total = count.total,
                          .target = target, .progress = progress, .user = user};
    const int result = tar_walk(file, extract_entry, &extract, err, err_size);
    fclose(file);
    if (result != 0) {
        if (extract.error[0])
            set_error(err, err_size, extract.error);
        return -1;
    }
    return 0;
}

static int decompress_gzip(const char* source, const char* destination,
                           char* err, size_t err_size) {
    gzFile input = gzopen(source, "rb");
    if (!input) {
        set_error(err, err_size, "cannot open gzip archive");
        return -1;
    }
    FILE* output = fopen(destination, "wb");
    if (!output) {
        gzclose(input);
        set_error(err, err_size, "cannot create temporary archive");
        return -1;
    }
    unsigned char buffer[ARCHIVE_READ_SIZE];
    int read;
    int failed = 0;
    while ((read = gzread(input, buffer, sizeof(buffer))) > 0) {
        if (fwrite(buffer, 1, (size_t)read, output) != (size_t)read) {
            failed = 1;
            break;
        }
    }
    if (read < 0)
        failed = 1;
    if (fclose(output) != 0)
        failed = 1;
    if (gzclose(input) != Z_OK)
        failed = 1;
    if (failed) {
        remove(destination);
        set_error(err, err_size, "cannot decompress gzip archive");
        return -1;
    }
    return 0;
}

static int decompress_xz(const char* source, const char* destination,
                         char* err, size_t err_size) {
    FILE* input = fopen(source, "rb");
    if (!input) {
        set_error(err, err_size, "cannot open xz archive");
        return -1;
    }
    FILE* output = fopen(destination, "wb");
    if (!output) {
        fclose(input);
        set_error(err, err_size, "cannot create temporary archive");
        return -1;
    }
    lzma_stream stream = LZMA_STREAM_INIT;
    if (lzma_stream_decoder(&stream, UINT64_MAX, LZMA_CONCATENATED) != LZMA_OK) {
        fclose(input);
        fclose(output);
        remove(destination);
        set_error(err, err_size, "cannot start xz decoder");
        return -1;
    }
    unsigned char input_buffer[ARCHIVE_READ_SIZE];
    unsigned char output_buffer[ARCHIVE_READ_SIZE];
    bool end_of_input = false;
    int failed = 0;
    for (;;) {
        if (!stream.avail_in && !end_of_input) {
            const size_t read = fread(input_buffer, 1, sizeof(input_buffer), input);
            if (!read) {
                if (ferror(input)) {
                    failed = 1;
                    break;
                }
                end_of_input = true;
            } else {
                stream.next_in = input_buffer;
                stream.avail_in = read;
            }
        }
        stream.next_out = output_buffer;
        stream.avail_out = sizeof(output_buffer);
        const lzma_ret result = lzma_code(&stream, end_of_input ? LZMA_FINISH : LZMA_RUN);
        const size_t produced = sizeof(output_buffer) - stream.avail_out;
        if (produced && fwrite(output_buffer, 1, produced, output) != produced) {
            failed = 1;
            break;
        }
        if (result == LZMA_STREAM_END)
            break;
        if (result != LZMA_OK) {
            failed = 1;
            break;
        }
        if (end_of_input && !stream.avail_in && !produced) {
            failed = 1;
            break;
        }
    }
    lzma_end(&stream);
    if (fclose(output) != 0)
        failed = 1;
    fclose(input);
    if (failed) {
        remove(destination);
        set_error(err, err_size, "cannot decompress xz archive");
        return -1;
    }
    return 0;
}

int archive_extract(const char* path, const char* asset_name, const char* target,
                    const char* strip_prefix, char* err, size_t err_size,
                    ZipProgress progress, void* user) {
    const char* format = asset_name && asset_name[0] ? asset_name : path;
    if (ends_with_ci(format, ".zip"))
        return zip_extract(path, target, strip_prefix, err, err_size, progress, user);
    if (ends_with_ci(format, ".tar"))
        return tar_extract_file(path, target, strip_prefix, err, err_size, progress, user);

    char temporary[1024];
    const int written = snprintf(temporary, sizeof(temporary), "%s.tar", path);
    if (written <= 0 || (size_t)written >= sizeof(temporary)) {
        set_error(err, err_size, "archive path is too long");
        return -1;
    }
    int result;
    if (ends_with_ci(format, ".tar.gz") || ends_with_ci(format, ".tgz"))
        result = decompress_gzip(path, temporary, err, err_size);
    else if (ends_with_ci(format, ".tar.xz") || ends_with_ci(format, ".txz"))
        result = decompress_xz(path, temporary, err, err_size);
    else {
        set_error(err, err_size, "unsupported archive format");
        return -1;
    }
    if (result == 0)
        result = tar_extract_file(temporary, target, strip_prefix, err, err_size,
                                  progress, user);
    remove(temporary);
    return result;
}
