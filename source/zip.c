// SPDX-License-Identifier: MIT
#include "zip.h"

#include <dirent.h>
#include <errno.h>
#include <minizip/unzip.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define ZIP_MAX_FILE (1024ull * 1024ull * 1024ull)
#define ZIP_MAX_TOTAL (8ull * 1024ull * 1024ull * 1024ull)
#define ZIP_NAME_MAX 1024
#define ZIP_READ_SIZE (128 * 1024)

static void set_error(char* err, size_t err_size, const char* message) {
    if (err && err_size)
        snprintf(err, err_size, "%s", message);
}

static int make_one_directory(const char* path) {
    if (mkdir(path, 0777) == 0)
        return 0;
    if (errno != EEXIST)
        return -1;
    struct stat status;
    return stat(path, &status) == 0 && S_ISDIR(status.st_mode) ? 0 : -1;
}

int path_mkdirs(const char* path) {
    char copy[1024];
    if (strlen(path) >= sizeof(copy))
        return -1;
    strcpy(copy, path);
    for (char* slash = copy + 1; *slash; slash++) {
        if (*slash != '/')
            continue;
        *slash = '\0';
        if (strcmp(copy, "sdmc:") != 0 && make_one_directory(copy) != 0)
            return -1;
        *slash = '/';
    }
    return make_one_directory(copy);
}

int path_remove_tree(const char* path) {
    struct stat root_status;
    if (lstat(path, &root_status) != 0)
        return errno == ENOENT ? 0 : -1;
    if (S_ISLNK(root_status.st_mode))
        return unlink(path);
    if (!S_ISDIR(root_status.st_mode))
        return unlink(path);

    DIR* directory = opendir(path);
    if (!directory)
        return -1;

    struct dirent* entry;
    while ((entry = readdir(directory)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;
        char child[1024];
        const int written = snprintf(child, sizeof(child), "%s/%s", path, entry->d_name);
        if (written <= 0 || (size_t)written >= sizeof(child)) {
            closedir(directory);
            return -1;
        }
        struct stat status;
        if (lstat(child, &status) != 0)
            continue;
        if (S_ISDIR(status.st_mode)) {
            if (path_remove_tree(child) != 0) {
                closedir(directory);
                return -1;
            }
        } else if (unlink(child) != 0) {
            closedir(directory);
            return -1;
        }
    }
    closedir(directory);
    return rmdir(path);
}

static int safe_relative(const char* path) {
    if (!path[0] || path[0] == '/' || strchr(path, '\\') || strchr(path, ':'))
        return 0;
    const char* part = path;
    while (*part) {
        const char* end = strchr(part, '/');
        const size_t length = end ? (size_t)(end - part) : strlen(part);
        if (length == 0 || (length == 2 && part[0] == '.' && part[1] == '.'))
            return 0;
        part = end ? end + 1 : part + length;
    }
    return 1;
}

static int entry_name(unzFile archive, char* name, size_t name_size) {
    unz_file_info info;
    if (unzGetCurrentFileInfo(archive, &info, name, (uLong)name_size, NULL, 0, NULL, 0) != UNZ_OK)
        return -1;
    return info.size_filename < name_size ? 0 : -1;
}

static int detect_root(unzFile archive, char* root, size_t root_size) {
    root[0] = '\0';
    if (unzGoToFirstFile(archive) != UNZ_OK)
        return -1;
    bool first = true;
    for (;;) {
        char name[ZIP_NAME_MAX];
        if (entry_name(archive, name, sizeof(name)) != 0)
            return -1;
        const char* slash = strchr(name, '/');
        if (!slash)
            return 0;
        const size_t length = (size_t)(slash - name);
        if (length == 0 || length >= root_size)
            return 0;
        if (first) {
            if ((length == 1 && name[0] == '.') ||
                (length == 2 && name[0] == '.' && name[1] == '.'))
                return 0;
            memcpy(root, name, length);
            root[length] = '\0';
            first = false;
        } else if (strncmp(name, root, length) != 0 || name[length] != '/') {
            root[0] = '\0';
            return 0;
        }
        const int next = unzGoToNextFile(archive);
        if (next == UNZ_END_OF_LIST_OF_FILE)
            break;
        if (next != UNZ_OK)
            return -1;
    }
    return root[0] ? 1 : 0;
}

static int count_extractable(unzFile archive, const char* prefix) {
    if (unzGoToFirstFile(archive) != UNZ_OK)
        return -1;
    const size_t prefix_length = strlen(prefix);
    int count = 0;
    for (;;) {
        char name[ZIP_NAME_MAX];
        unz_file_info info;
        if (unzGetCurrentFileInfo(archive, &info, name, sizeof(name), NULL, 0, NULL, 0) != UNZ_OK ||
            info.size_filename >= sizeof(name))
            return -1;
        if (strncmp(name, prefix, prefix_length) != 0)
            return -1;
        const char* relative = name + prefix_length;
        const bool directory = relative[0] && relative[strlen(relative) - 1] == '/';
        const mode_t type = (mode_t)((info.external_fa >> 16) & S_IFMT);
        if (!directory && type != S_IFLNK &&
            strncmp(relative, "__MACOSX/", 9) != 0 && strcmp(relative, "__MACOSX") != 0)
            count++;
        const int next = unzGoToNextFile(archive);
        if (next == UNZ_END_OF_LIST_OF_FILE)
            break;
        if (next != UNZ_OK)
            return -1;
    }
    return count;
}

int zip_extract(const char* archive_path, const char* target, const char* strip_prefix,
                char* err, size_t err_size, ZipProgress progress, void* user) {
    unzFile archive = unzOpen(archive_path);
    if (!archive) {
        set_error(err, err_size, "cannot open downloaded archive");
        return -1;
    }

    char prefix[128];
    prefix[0] = '\0';
    if (strip_prefix && strip_prefix[0]) {
        if (strlen(strip_prefix) >= sizeof(prefix)) {
            unzClose(archive);
            set_error(err, err_size, "stripPrefix is too long");
            return -1;
        }
        strcpy(prefix, strip_prefix);
    } else {
        char root[128];
        const int detected = detect_root(archive, root, sizeof(root));
        if (detected < 0) {
            unzClose(archive);
            set_error(err, err_size, "cannot list archive");
            return -1;
        }
        if (detected > 0) {
            const size_t root_length = strlen(root);
            if (root_length + 2 > sizeof(prefix)) {
                unzClose(archive);
                set_error(err, err_size, "archive root name is too long");
                return -1;
            }
            memcpy(prefix, root, root_length);
            prefix[root_length] = '/';
            prefix[root_length + 1] = '\0';
        }
    }
    const size_t prefix_length = strlen(prefix);
    const int extract_total = count_extractable(archive, prefix);
    if (extract_total < 0) {
        unzClose(archive);
        set_error(err, err_size, "cannot count archive entries");
        return -1;
    }
    int extract_done = 0;
    char last_parent[1024] = "";

    if (unzGoToFirstFile(archive) != UNZ_OK) {
        unzClose(archive);
        set_error(err, err_size, "archive is empty");
        return -1;
    }

    unsigned long long total = 0;
    for (;;) {
        char name[ZIP_NAME_MAX];
        unz_file_info info;
        if (unzGetCurrentFileInfo(archive, &info, name, sizeof(name), NULL, 0, NULL, 0) != UNZ_OK ||
            info.size_filename >= sizeof(name)) {
            unzClose(archive);
            set_error(err, err_size, "cannot read archive entry");
            return -1;
        }
        if (strncmp(name, prefix, prefix_length) != 0) {
            unzClose(archive);
            set_error(err, err_size, "archive entry does not match stripPrefix");
            return -1;
        }

        const char* relative = name + prefix_length;
        const bool directory = relative[0] && relative[strlen(relative) - 1] == '/';
        char relative_trimmed[ZIP_NAME_MAX];
        if (strlen(relative) >= sizeof(relative_trimmed)) {
            unzClose(archive);
            set_error(err, err_size, "archive path is too long");
            return -1;
        }
        strcpy(relative_trimmed, relative);
        if (directory)
            relative_trimmed[strlen(relative_trimmed) - 1] = '\0';

        if (relative_trimmed[0] &&
            strncmp(relative_trimmed, "__MACOSX/", 9) != 0 &&
            strcmp(relative_trimmed, "__MACOSX") != 0) {
            if (!safe_relative(relative_trimmed)) {
                unzClose(archive);
                set_error(err, err_size, "unsafe path in archive");
                return -1;
            }
            char full[1024];
            const size_t target_length = strlen(target);
            const bool target_slash = target_length && target[target_length - 1] == '/';
            const int written = snprintf(full, sizeof(full), target_slash ? "%s%s" : "%s/%s",
                                         target, relative_trimmed);
            if (written <= 0 || (size_t)written >= sizeof(full)) {
                unzClose(archive);
                set_error(err, err_size, "archive path is too long");
                return -1;
            }

            if (directory) {
                if (path_mkdirs(full) != 0) {
                    unzClose(archive);
                    set_error(err, err_size, "cannot create target directory");
                    return -1;
                }
                snprintf(last_parent, sizeof(last_parent), "%s", full);
            } else {
                const mode_t type = (mode_t)((info.external_fa >> 16) & S_IFMT);
                if (type == S_IFLNK) {
                    /* Symlinks are never materialised. */
                } else if (info.uncompressed_size > ZIP_MAX_FILE ||
                           total + info.uncompressed_size > ZIP_MAX_TOTAL) {
                    unzClose(archive);
                    set_error(err, err_size, "archive expands beyond the size limit");
                    return -1;
                } else {
                    char parent[1024];
                    snprintf(parent, sizeof(parent), "%s", full);
                    char* slash = strrchr(parent, '/');
                    if (slash) {
                        *slash = '\0';
                        if (strcmp(parent, last_parent) != 0) {
                            if (path_mkdirs(parent) != 0) {
                                unzClose(archive);
                                set_error(err, err_size, "cannot create target directory");
                                return -1;
                            }
                            snprintf(last_parent, sizeof(last_parent), "%s", parent);
                        }
                    }
                    if (unzOpenCurrentFile(archive) != UNZ_OK) {
                        unzClose(archive);
                        set_error(err, err_size, "cannot open archive member");
                        return -1;
                    }
                    FILE* file = fopen(full, "wb");
                    if (!file) {
                        unzCloseCurrentFile(archive);
                        unzClose(archive);
                        set_error(err, err_size, "cannot create target file");
                        return -1;
                    }
                    static unsigned char buffer[ZIP_READ_SIZE];
                    int read;
                    int failed = 0;
                    while ((read = unzReadCurrentFile(archive, buffer, sizeof(buffer))) > 0) {
                        if (fwrite(buffer, 1, (size_t)read, file) != (size_t)read) {
                            failed = 1;
                            break;
                        }
                    }
                    if (read < 0)
                        failed = 1;
                    if (fclose(file) != 0)
                        failed = 1;
                    unzCloseCurrentFile(archive);
                    if (failed) {
                        unzClose(archive);
                        set_error(err, err_size, "cannot write target file");
                        return -1;
                    }
                    total += info.uncompressed_size;
                    extract_done++;
                    if (progress)
                        progress(extract_done, extract_total, relative_trimmed, user);
                }
            }
        }

        const int next = unzGoToNextFile(archive);
        if (next == UNZ_END_OF_LIST_OF_FILE)
            break;
        if (next != UNZ_OK) {
            unzClose(archive);
            set_error(err, err_size, "cannot advance archive");
            return -1;
        }
    }

    unzClose(archive);
    return 0;
}

int path_copy_file(const char* source, const char* destination) {
    FILE* input = fopen(source, "rb");
    if (!input)
        return -1;
    FILE* output = fopen(destination, "wb");
    if (!output) {
        fclose(input);
        return -1;
    }
    unsigned char buffer[32768];
    int result = 0;
    size_t read;
    while ((read = fread(buffer, 1, sizeof(buffer), input)) > 0) {
        if (fwrite(buffer, 1, read, output) != read) {
            result = -1;
            break;
        }
    }
    if (ferror(input))
        result = -1;
    if (fclose(output) != 0)
        result = -1;
    fclose(input);
    if (result != 0)
        remove(destination);
    return result;
}
