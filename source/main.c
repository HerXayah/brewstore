// SPDX-License-Identifier: MIT
// brewstore: a homebrew package browser for SD-card NROs and assets.
// It downloads GitHub archives, verifies optional SHA-256, and extracts them
// under sdmc:/switch or sdmc:/brewstore/apps. It never handles NSP/NAND content.
#include <switch.h>

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>

#include "archive.h"
#include "catalog.h"
#include "github.h"
#include "http.h"
#include "manifest.h"
#include "release.h"
#include "readme.h"
#include "repos.h"
#include "settings.h"
#include "shortcuts.h"
#include "ui.h"
#include "version.h"
#include "zip.h"

#define BREWSTORE_ROOT "sdmc:/brewstore"
#define LEGACY_BREWSTORE_ROOT "sdmc:/switch/brewstore"
#define MANIFEST_PATH BREWSTORE_ROOT "/packages.json"
#define CACHE_DIR BREWSTORE_ROOT "/cache"
#define ICON_DIR CACHE_DIR "/icons"
#define REPO_CACHE_DIR CACHE_DIR "/repos"
#define REPOS_PATH BREWSTORE_ROOT "/repos.json"
#define TEST_REPO_PATH BREWSTORE_ROOT "/test-repo.json"
#define INSTALLED_DIR BREWSTORE_ROOT "/installed"
#define README_DIR CACHE_DIR "/readme"
#define SETTINGS_PATH BREWSTORE_ROOT "/settings.json"
#define SHORTCUTS_PATH "sdmc:/switch/shortcuts.txt"
#define DEFAULT_REPO_URL "https://raw.githubusercontent.com/HerXayah/brewstore-repo/main/catalog.json"
#define LEGACY_DEFAULT_REPO_URL "https://raw.githubusercontent.com/HerXayah/brewstore-repo/main/packages.json"
#define VISIBLE_COLUMNS 3
#define VISIBLE_ROWS 2
#define VISIBLE_COUNT (VISIBLE_COLUMNS * VISIBLE_ROWS)
#define LIST_VISIBLE_COUNT 8
#define REPO_VISIBLE_ROWS 10
#define VERSION_VISIBLE_ROWS 8

#define COLOR_BACKGROUND RGBA8(12, 14, 18, 255)
#define COLOR_PANEL RGBA8(21, 24, 31, 255)
#define COLOR_CARD RGBA8(28, 32, 41, 255)
#define COLOR_CARD_HOVER RGBA8(37, 43, 55, 255)
#define COLOR_ACCENT RGBA8(95, 170, 255, 255)
#define COLOR_ACCENT_DIM RGBA8(46, 86, 132, 255)
#define COLOR_TEXT RGBA8(234, 238, 245, 255)
#define COLOR_MUTED RGBA8(143, 152, 166, 255)
#define COLOR_GREEN RGBA8(128, 201, 153, 255)
#define COLOR_RED RGBA8(230, 120, 120, 255)
#define CARD_ICON_SIZE 96

typedef struct {
    char name[64];
    u32* pixels;
    int width;
    int height;
} CachedIcon;

static HbPackageList g_manifest;
static RepoList g_repos;
static int g_selected;
enum { FILTER_ALL = 0, FILTER_UPDATES, FILTER_INSTALLED, FILTER_NOT_INSTALLED };
static int g_filter;
static HbPackage** g_visible;
static int g_visible_count;
static int g_visible_capacity;
static bool g_visible_dirty = true;
static char (*g_marked)[64];
static int g_marked_count;
static int g_marked_capacity;
enum { SORT_NAME = 0, SORT_AUTHOR, SORT_UPDATED };
static int g_sort;
static bool g_sort_reverse;
static bool g_context_menu;
static int g_context_selected;
static PadState* g_pad;
static bool g_return_to_repos;
static bool g_batch_active;
static bool g_batch_stop;
static int g_batch_index;
static int g_batch_total;
static int g_batch_completed;
static int g_scroll_page;
static bool g_list_view;
static bool g_repo_manager;
static bool g_versions_screen;
static int g_version_selected;
static int g_version_scroll;
static int g_version_count;
static ReleaseChoice g_version_choices[RELEASE_CHOICES_MAX];
static HbPackage* g_version_package;
static bool g_settings_screen;
static int g_setting_row;
static char g_search[96];
enum {
    CONFIRM_NONE = 0,
    CONFIRM_DISABLE_REPO,
    CONFIRM_ENABLE_REPO,
    CONFIRM_REMOVE_TEST,
    CONFIRM_DELETE_LOG
};
static int g_confirm_action;
static int g_confirm_index;
static bool g_confirm_yes;
static AppSettings g_settings;
static FILE* g_catalog_log;
static bool g_quit;
static bool g_ignore_b;
static int g_repo_selected;
static int g_repo_scroll;
static bool g_network_ready;
static char g_status[256];
static char g_manifest_error[256];
static bool g_cache_corrupt;
static CachedIcon g_icons[24];
static int g_icon_count;
static Ui* g_ui;
static u64 g_previous_keys[4];

static int view_page_size(void) {
    return g_list_view ? LIST_VISIBLE_COUNT : VISIBLE_COUNT;
}

static void refresh_catalogs(bool fetch);
static bool ends_with_ci(const char* text, const char* suffix);
static bool prompt_text(const char* header, const char* guide, const char* initial,
                        char* out, size_t out_size);
static void draw_confirm(Ui* ui);
static void add_test_repo(void);
static void add_repository(void);
static void repo_cache_path(const char* url, char* out, size_t out_size);
static void prune_empty_parents(char* path);

static void set_status(const char* format, ...) {
    va_list args;
    va_start(args, format);
    vsnprintf(g_status, sizeof(g_status), format, args);
    va_end(args);
}

static int state_path(const HbPackage* package, char* out, size_t out_size) {
    const int written = snprintf(out, out_size, INSTALLED_DIR "/%s", package->name);
    return written > 0 && (size_t)written < out_size ? 0 : -1;
}

static void trim_newline(char* text) {
    text[strcspn(text, "\r\n")] = '\0';
}

static void write_state(const HbPackage* package, const char* hash,
                        const char* installed_file, unsigned long long installed_at,
                        unsigned long long updated_at) {
    char path[512];
    if (state_path(package, path, sizeof(path)) != 0)
        return;
    FILE* file = fopen(path, "wb");
    if (!file)
        return;
    fprintf(file, "%s\nsha256=%s\n", package->version, hash ? hash : "");
    if (installed_file && installed_file[0])
        fprintf(file, "file=%s\n", installed_file);
    fprintf(file, "installed=%llu\nupdated=%llu\n", installed_at, updated_at);
    fclose(file);
}

static bool read_state(const HbPackage* package, char* version, size_t version_size,
                       char* hash, size_t hash_size, char* installed_file,
                       size_t installed_file_size, unsigned long long* installed_at,
                       unsigned long long* updated_at) {
    char path[512];
    if (state_path(package, path, sizeof(path)) != 0)
        return false;
    FILE* file = fopen(path, "rb");
    if (!file)
        return false;
    version[0] = '\0';
    hash[0] = '\0';
    if (installed_file && installed_file_size)
        installed_file[0] = '\0';
    if (installed_at)
        *installed_at = 0;
    if (updated_at)
        *updated_at = 0;
    if (!fgets(version, (int)version_size, file)) {
        fclose(file);
        return false;
    }
    trim_newline(version);
    char line[256];
    while (fgets(line, sizeof(line), file)) {
        trim_newline(line);
        if (strncmp(line, "sha256=", 7) == 0) {
            if (strlen(line + 7) >= hash_size) {
                fclose(file);
                return false;
            }
            strcpy(hash, line + 7);
            break;
        }
        if (strncmp(line, "file=", 5) == 0 && installed_file && installed_file_size) {
            if (strlen(line + 5) >= installed_file_size) {
                fclose(file);
                return false;
            }
            strcpy(installed_file, line + 5);
        }
        if (strncmp(line, "installed=", 10) == 0 && installed_at)
            *installed_at = strtoull(line + 10, NULL, 10);
        if (strncmp(line, "updated=", 8) == 0 && updated_at)
            *updated_at = strtoull(line + 8, NULL, 10);
    }
    fclose(file);
    return true;
}

static bool directory_nonempty(const char* path) {
    DIR* dir = opendir(path);
    if (!dir)
        return false;
    struct dirent* entry;
    bool found = false;
    while ((entry = readdir(dir))) {
        if (strcmp(entry->d_name, ".") != 0 && strcmp(entry->d_name, "..") != 0) {
            found = true;
            break;
        }
    }
    closedir(dir);
    return found;
}

static bool file_list_has_existing(const HbPackage* package) {
    char list_path[512];
    snprintf(list_path, sizeof(list_path), INSTALLED_DIR "/%s.files", package->name);
    FILE* list = fopen(list_path, "rb");
    if (!list)
        return 0;
    char line[1024];
    bool found = false;
    while (fgets(line, sizeof(line), list)) {
        line[strcspn(line, "\r\n")] = '\0';
        if (strncmp(line, "sdmc:/", 6) == 0 && !strstr(line, "..") &&
            access(line, F_OK) == 0) {
            found = true;
            break;
        }
    }
    fclose(list);
    return found;
}

static bool is_installed(const HbPackage* package) {
    char path[512];
    const bool has_state =
        state_path(package, path, sizeof(path)) == 0 && access(path, F_OK) == 0;
    if (has_state) {
        char version[64];
        char hash[65];
        char file[512];
        if (read_state(package, version, sizeof(version), hash, sizeof(hash),
                       file, sizeof(file), NULL, NULL)) {
            if (file[0] && access(file, F_OK) == 0)
                return true;
            if (package->marker[0] && access(package->marker, F_OK) == 0)
                return true;
            if (strcmp(package->target, "sdmc:/switch") != 0 &&
                strcmp(package->target, "sdmc:/") != 0 &&
                directory_nonempty(package->target))
                return true;
        }
    }
    if (package->marker[0] && access(package->marker, F_OK) == 0)
        return true;
    return file_list_has_existing(package);
}

static void format_installed_time(unsigned long long timestamp, char* out, size_t out_size) {
    out[0] = '\0';
    if (!timestamp)
        return;
    const time_t value = (time_t)timestamp;
    struct tm parts;
    if (localtime_r(&value, &parts))
        strftime(out, out_size, "%Y-%m-%d %H:%M", &parts);
}

typedef struct {
    bool checked;
    bool installed;
    bool update;
    bool times_known;
    unsigned long long installed_at;
    unsigned long long updated_at;
} InstalledState;

static InstalledState* g_installed;
static int g_installed_capacity;

static void invalidate_visible(void) {
    g_visible_dirty = true;
    if (g_installed && g_installed_capacity > 0)
        memset(g_installed, 0, sizeof(*g_installed) * (size_t)g_installed_capacity);
}

static unsigned long long package_published_timestamp(const HbPackage* package) {
    if (!package->published[0])
        return 0;
    char* end = NULL;
    const unsigned long long published = strtoull(package->published, &end, 10);
    return published && end && *end == '\0' ? published : 0;
}

static const InstalledState* package_state(const HbPackage* package) {
    const int index = (int)(package - g_manifest.items);
    if (index < 0 || index >= g_manifest.count)
        return NULL;
    if (!g_installed[index].checked) {
        InstalledState* state = &g_installed[index];
        state->installed = is_installed(package);
        if (state->installed) {
            char installed_version[64];
            char installed_hash[65];
            if (read_state(package, installed_version, sizeof(installed_version),
                           installed_hash, sizeof(installed_hash), NULL, 0,
                           &state->installed_at, &state->updated_at)) {
                state->times_known = true;
                if (version_is_newer(package->version, installed_version) ||
                    (package->sha256[0] && installed_hash[0] &&
                     strcasecmp(package->sha256, installed_hash) != 0))
                    state->update = true;
            } else {
                const unsigned long long published = package_published_timestamp(package);
                struct stat installed_status;
                if (published && package->marker[0] &&
                    stat(package->marker, &installed_status) == 0 &&
                    installed_status.st_mtime > 0 &&
                    (unsigned long long)installed_status.st_mtime < published)
                    state->update = true;
            }
        } else {
            state->times_known = true;
        }
        state->checked = true;
    }
    return &g_installed[index];
}

static int compare_packages(const void* left, const void* right);

static bool package_matches_search(const HbPackage* package) {
    if (!g_search[0])
        return true;
    return strcasestr(package->name, g_search) || strcasestr(package->title, g_search);
}

static int ensure_visible_capacity(int needed) {
    if (needed <= g_visible_capacity)
        return 0;
    HbPackage** items = realloc(g_visible, sizeof(*items) * (size_t)needed);
    if (!items)
        return -1;
    g_visible = items;
    g_visible_capacity = needed;
    return 0;
}

static int ensure_installed_capacity(int needed) {
    if (needed <= g_installed_capacity)
        return 0;
    InstalledState* items = realloc(g_installed, sizeof(*items) * (size_t)needed);
    if (!items)
        return -1;
    memset(items + g_installed_capacity, 0,
           sizeof(*items) * (size_t)(needed - g_installed_capacity));
    g_installed = items;
    g_installed_capacity = needed;
    return 0;
}

static int ensure_marked_capacity(int needed) {
    if (needed <= g_marked_capacity)
        return 0;
    char (*items)[64] = realloc(g_marked, sizeof(*items) * (size_t)needed);
    if (!items)
        return -1;
    g_marked = items;
    g_marked_capacity = needed;
    return 0;
}

static void rebuild_visible(void) {
    if (!g_visible_dirty)
        return;
    g_visible_count = 0;
    if (ensure_visible_capacity(g_manifest.count) != 0) {
        g_visible_dirty = false;
        return;
    }
    for (int i = 0; i < g_manifest.count; i++) {
        const InstalledState* state = package_state(&g_manifest.items[i]);
        bool match = true;
        switch (g_filter) {
            case FILTER_UPDATES: match = state->update; break;
            case FILTER_INSTALLED: match = state->installed; break;
            case FILTER_NOT_INSTALLED: match = !state->installed; break;
            default: break;
        }
        if (match && package_matches_search(&g_manifest.items[i]))
            g_visible[g_visible_count++] = &g_manifest.items[i];
    }
    qsort(g_visible, (size_t)g_visible_count, sizeof(g_visible[0]), compare_packages);
    g_visible_dirty = false;
}

static const char* filter_name(void) {
    switch (g_filter) {
        case FILTER_UPDATES: return "Updates";
        case FILTER_INSTALLED: return "Installed";
        case FILTER_NOT_INSTALLED: return "Not installed";
        default: return "All";
    }
}

static bool is_marked(const HbPackage* package) {
    for (int i = 0; i < g_marked_count; i++) {
        if (strcmp(g_marked[i], package->name) == 0)
            return true;
    }
    return false;
}

static void toggle_mark(const HbPackage* package) {
    for (int i = 0; i < g_marked_count; i++) {
        if (strcmp(g_marked[i], package->name) == 0) {
            memmove(&g_marked[i], &g_marked[i + 1],
                    sizeof(g_marked[0]) * (size_t)(g_marked_count - i - 1));
            g_marked_count--;
            return;
        }
    }
    if (ensure_marked_capacity(g_marked_count + 1) == 0)
        snprintf(g_marked[g_marked_count++], sizeof(g_marked[0]), "%s", package->name);
}

static void clear_marks(void) {
    g_marked_count = 0;
}

static int visible_count(void) {
    rebuild_visible();
    return g_visible_count;
}

static unsigned long long package_timestamp(const HbPackage* package, bool installed) {
    if (!installed) {
        const unsigned long long published = package_published_timestamp(package);
        if (published)
            return published;
    }
    const InstalledState* state = package_state(package);
    if (state && state->times_known)
        return installed ? state->installed_at : state->updated_at;
    char version[64];
    char hash[65];
    char file[512];
    unsigned long long installed_at = 0;
    unsigned long long updated_at = 0;
    if (!read_state(package, version, sizeof(version), hash, sizeof(hash),
                    file, sizeof(file), &installed_at, &updated_at))
        return 0;
    return installed ? installed_at : updated_at;
}

static int compare_packages(const void* left, const void* right) {
    const HbPackage* a = *(const HbPackage* const*)left;
    const HbPackage* b = *(const HbPackage* const*)right;
    int result = 0;
    if (g_sort == SORT_NAME) {
        result = strcasecmp(a->title, b->title);
    } else if (g_sort == SORT_AUTHOR) {
        result = strcasecmp(a->author, b->author);
    } else {
        const unsigned long long a_time = package_timestamp(a, false);
        const unsigned long long b_time = package_timestamp(b, false);
        if (a_time != b_time)
            result = a_time > b_time ? 1 : -1;
    }
    if (result == 0)
        result = strcasecmp(a->title, b->title);
    return g_sort_reverse ? -result : result;
}

static HbPackage* visible_package(int index) {
    rebuild_visible();
    return index >= 0 && index < g_visible_count ? g_visible[index] : NULL;
}

static const char* sort_name(void) {
    if (g_sort == SORT_NAME)
        return g_sort_reverse ? "Name Z-A" : "Name A-Z";
    if (g_sort == SORT_AUTHOR)
        return g_sort_reverse ? "Author Z-A" : "Author A-Z";
    return g_sort_reverse ? "Updated newest" : "Updated oldest";
}

static void clamp_selection(void) {
    const int count = visible_count();
    if (g_selected >= count)
        g_selected = count ? count - 1 : 0;
    if (g_selected < 0)
        g_selected = 0;
    g_scroll_page = count ? g_selected / view_page_size() : 0;
}

static void free_icons(void) {
    for (int i = 0; i < g_icon_count; i++)
        free(g_icons[i].pixels);
    g_icon_count = 0;
}

static u32* scale_icon(const u32* pixels, int width, int height) {
    if (!pixels || width <= 0 || height <= 0)
        return NULL;
    u32* scaled = calloc((size_t)CARD_ICON_SIZE * CARD_ICON_SIZE, sizeof(u32));
    if (!scaled)
        return NULL;
    int draw_width;
    int draw_height;
    if (width >= height) {
        draw_width = CARD_ICON_SIZE;
        draw_height = height * CARD_ICON_SIZE / width;
    } else {
        draw_height = CARD_ICON_SIZE;
        draw_width = width * CARD_ICON_SIZE / height;
    }
    if (draw_width < 1)
        draw_width = 1;
    if (draw_height < 1)
        draw_height = 1;
    const int offset_x = (CARD_ICON_SIZE - draw_width) / 2;
    const int offset_y = (CARD_ICON_SIZE - draw_height) / 2;
    for (int row = 0; row < draw_height; row++) {
        const int source_y = row * height / draw_height;
        for (int column = 0; column < draw_width; column++) {
            const int source_x = column * width / draw_width;
            scaled[(offset_y + row) * CARD_ICON_SIZE + offset_x + column] =
                pixels[source_y * width + source_x];
        }
    }
    return scaled;
}

static const CachedIcon* cache_icon(const HbPackage* package, u32* pixels,
                                    int width, int height) {
    if (g_icon_count >= (int)(sizeof(g_icons) / sizeof(g_icons[0]))) {
        free(g_icons[0].pixels);
        memmove(&g_icons[0], &g_icons[1], sizeof(g_icons[0]) * (g_icon_count - 1));
        g_icon_count--;
    }
    u32* scaled = scale_icon(pixels, width, height);
    if (scaled) {
        free(pixels);
        pixels = scaled;
        width = CARD_ICON_SIZE;
        height = CARD_ICON_SIZE;
    }
    CachedIcon* slot = &g_icons[g_icon_count++];
    snprintf(slot->name, sizeof(slot->name), "%s", package->name);
    slot->pixels = pixels;
    slot->width = width;
    slot->height = height;
    return slot;
}

typedef struct {
    Thread thread;
    bool running;
    volatile bool stop;
} IconPrefetch;

static IconPrefetch g_icon_prefetch;

static bool icon_prefetch_cancelled(void* user) {
    (void)user;
    return g_icon_prefetch.stop;
}

static void icon_prefetch_worker(void* user) {
    (void)user;
    for (int i = 0; i < g_manifest.count && !g_icon_prefetch.stop; i++) {
        const HbPackage* package = &g_manifest.items[i];
        if (!package->icon[0])
            continue;
        char path[512];
        snprintf(path, sizeof(path), ICON_DIR "/%s.img", package->name);
        if (access(path, F_OK) == 0)
            continue;
        char hash[65];
        char error[128];
        http_download_cancel(package->icon, path, hash, error, sizeof(error),
                             NULL, NULL, icon_prefetch_cancelled, NULL);
    }
}

static void icon_prefetch_stop(void) {
    if (!g_icon_prefetch.running)
        return;
    g_icon_prefetch.stop = true;
    threadWaitForExit(&g_icon_prefetch.thread);
    threadClose(&g_icon_prefetch.thread);
    g_icon_prefetch.running = false;
}

static void icon_prefetch_ensure(void) {
    if (g_icon_prefetch.running)
        return;
    if (!g_network_ready || !g_manifest.count)
        return;
    g_icon_prefetch.stop = false;
    if (R_FAILED(threadCreate(&g_icon_prefetch.thread, icon_prefetch_worker, NULL,
                              NULL, 0x40000, 0x2C, -2)))
        return;
    if (R_FAILED(threadStart(&g_icon_prefetch.thread))) {
        threadClose(&g_icon_prefetch.thread);
        return;
    }
    g_icon_prefetch.running = true;
}

static const CachedIcon* icon_for(const HbPackage* package) {
    for (int i = 0; i < g_icon_count; i++) {
        if (strcmp(g_icons[i].name, package->name) == 0)
            return g_icons[i].pixels ? &g_icons[i] : NULL;
    }

    if (!package->icon[0]) {
        cache_icon(package, NULL, 0, 0);
        return NULL;
    }
    if (!g_network_ready)
        return NULL;

    char path[512];
    snprintf(path, sizeof(path), ICON_DIR "/%s.img", package->name);
    if (access(path, F_OK) != 0) {
        char hash[65];
        char error[128];
        if (http_download(package->icon, path, hash, error, sizeof(error)) != 0) {
            cache_icon(package, NULL, 0, 0);
            return NULL;
        }
    }

    u32* pixels = NULL;
    int width = 0;
    int height = 0;
    if (ui_load_image(path, &pixels, &width, &height) != 0) {
        cache_icon(package, NULL, 0, 0);
        return NULL;
    }
    return cache_icon(package, pixels, width, height);
}

static const char* display_version(const HbPackage* package) {
    if (strcmp(package->version, "latest") == 0 ||
        strcmp(package->version, "test") == 0 ||
        strcmp(package->version, "unknown") == 0)
        return "Latest release";
    return package->version;
}

static void draw_loading(Ui* ui, const char* message) {
    if (!ui)
        return;
    ui_begin(ui);
    ui_clear(ui, COLOR_BACKGROUND);
    ui_rect(ui, 0, 0, UI_WIDTH, 56, COLOR_PANEL);
    ui_text(ui, 24, 14, "brewstore", 24, COLOR_TEXT);
    ui_text(ui, 24 + ui_text_width(ui, "brewstore", 24) + 10, 27,
            "Codename YukinaMinato", 9, COLOR_MUTED);
    ui_text(ui, 24, 300, message, 18, COLOR_MUTED);
    ui_end(ui);
}

static void draw_card(Ui* ui, const HbPackage* package, int x, int y, bool selected,
                      bool marked) {
    const int width = 276;
    const int height = 248;
    if (selected) {
        ui_rect(ui, x - 2, y - 2, width + 4, height + 4, COLOR_ACCENT);
        ui_rect(ui, x, y, width, height, COLOR_CARD_HOVER);
    } else {
        ui_rect(ui, x, y, width, height, COLOR_CARD);
    }

    if (marked) {
        ui_rect(ui, x + width - 30, y + 10, 20, 20, COLOR_ACCENT);
        ui_text(ui, x + width - 25, y + 11, "*", 16, COLOR_BACKGROUND);
    }

    const int icon_size = CARD_ICON_SIZE;
    const int icon_x = x + (width - icon_size) / 2;
    const int icon_y = y + 18;
    ui_rect(ui, icon_x, icon_y, icon_size, icon_size, COLOR_PANEL);
    const CachedIcon* icon = icon_for(package);
    if (icon) {
        ui_image(ui, icon_x, icon_y, icon_size, icon_size,
                 icon->pixels, icon->width, icon->height);
    } else {
        char initial[2] = { package->title[0] ? package->title[0] : '?', '\0' };
        const int initial_width = ui_text_width(ui, initial, 44);
        ui_text(ui, icon_x + (icon_size - initial_width) / 2, icon_y + 24,
                initial, 44, COLOR_MUTED);
    }

    ui_text_truncated(ui, x + 14, y + 130, package->title, 19, COLOR_TEXT, width - 28);
    ui_text_truncated(ui, x + 14, y + 157, package->author, 13, COLOR_MUTED, width - 28);
    ui_text_truncated(ui, x + 14, y + 181, display_version(package), 12, COLOR_MUTED,
                      width - 28);

    {
        const InstalledState* state = package_state(package);
        if (state && state->installed) {
            const char* label = state->update ? "Update" : "Installed";
            const int label_width = ui_text_width(ui, label, 13);
            ui_text(ui, x + width - 14 - label_width, y + 216, label, 13,
                    state->update ? COLOR_ACCENT : COLOR_GREEN);
        }
    }
}

static void draw_scrollbar(Ui* ui, int first, int visible, int total) {
    if (total <= visible)
        return;
    const int x = 912, y = 76, width = 8, height = 568;
    int thumb_height = height * visible / total;
    if (thumb_height < 24)
        thumb_height = 24;
    int max_first = total - visible;
    if (first > max_first)
        first = max_first;
    if (first < 0)
        first = 0;
    ui_rect(ui, x, y, width, height, COLOR_PANEL);
    const int thumb_y = y + (height - thumb_height) * first / max_first;
    ui_rect(ui, x, thumb_y, width, thumb_height, COLOR_ACCENT);
}

static void draw_list_row(Ui* ui, const HbPackage* package, int x, int y, int width,
                          bool selected, bool marked) {
    const int height = 64;
    if (selected) {
        ui_rect(ui, x - 2, y - 2, width + 4, height + 4, COLOR_ACCENT);
        ui_rect(ui, x, y, width, height, COLOR_CARD_HOVER);
    } else {
        ui_rect(ui, x, y, width, height, COLOR_CARD);
    }

    const int icon_size = 44;
    const int icon_x = x + 10;
    const int icon_y = y + 10;
    ui_rect(ui, icon_x, icon_y, icon_size, icon_size, COLOR_PANEL);
    const CachedIcon* icon = icon_for(package);
    if (icon) {
        ui_image(ui, icon_x, icon_y, icon_size, icon_size,
                 icon->pixels, icon->width, icon->height);
    } else {
        char initial[2] = { package->title[0] ? package->title[0] : '?', '\0' };
        const int initial_width = ui_text_width(ui, initial, 20);
        ui_text(ui, icon_x + (icon_size - initial_width) / 2, icon_y + 12,
                initial, 20, COLOR_MUTED);
    }

    const int text_x = x + 66;
    const int text_width = width - 78;
    ui_text_truncated(ui, text_x, y + 8, package->title, 16, COLOR_TEXT, text_width - 100);
    ui_text_truncated(ui, text_x, y + 31, package->author, 11, COLOR_MUTED, text_width - 100);
    const int version_width = ui_text_width(ui, display_version(package), 11);
    ui_text(ui, x + width - 12 - version_width, y + 42, display_version(package), 11,
            COLOR_MUTED);

    const InstalledState* state = package_state(package);
    if (state && state->installed) {
        const char* label = state->update ? "Update" : "Installed";
        const int label_width = ui_text_width(ui, label, 12);
        ui_text(ui, x + width - 12 - label_width, y + 10, label, 12,
                state->update ? COLOR_ACCENT : COLOR_GREEN);
    }
    if (marked)
        ui_text(ui, x + width - 14, y + 28, "*", 14, COLOR_ACCENT);
}

typedef struct {
    const char* name;
    int count;
    u32 colors[6];
} ButtonTheme;

static const ButtonTheme kButtonThemes[BUTTON_THEME_COUNT] = {
    {"Light blue", 1, {RGBA8(74, 144, 184, 255)}},
    {"Lesbian", 5, {RGBA8(213, 45, 0, 255), RGBA8(239, 118, 39, 255),
                    RGBA8(255, 154, 86, 255), RGBA8(209, 98, 164, 255),
                    RGBA8(181, 86, 144, 255)}},
    {"Trans", 5, {RGBA8(91, 206, 250, 255), RGBA8(245, 169, 184, 255),
                  RGBA8(255, 255, 255, 255), RGBA8(245, 169, 184, 255),
                  RGBA8(91, 206, 250, 255)}},
    {"Aromantic", 5, {RGBA8(61, 165, 66, 255), RGBA8(167, 211, 121, 255),
                      RGBA8(255, 255, 255, 255), RGBA8(169, 169, 169, 255),
                      RGBA8(0, 0, 0, 255)}},
    {"Gay", 6, {RGBA8(228, 3, 3, 255), RGBA8(255, 140, 0, 255),
                RGBA8(255, 237, 0, 255), RGBA8(0, 128, 38, 255),
                RGBA8(0, 77, 255, 255), RGBA8(117, 7, 135, 255)}},
    {"Bisexual", 3, {RGBA8(214, 2, 112, 255), RGBA8(155, 79, 150, 255),
                     RGBA8(0, 56, 168, 255)}},
    {"Pansexual", 3, {RGBA8(255, 33, 140, 255), RGBA8(255, 216, 0, 255),
                      RGBA8(33, 177, 255, 255)}},
    {"Nonbinary", 4, {RGBA8(255, 244, 48, 255), RGBA8(255, 255, 255, 255),
                      RGBA8(156, 89, 209, 255), RGBA8(0, 0, 0, 255)}},
    {"Asexual", 4, {RGBA8(0, 0, 0, 255), RGBA8(163, 163, 163, 255),
                    RGBA8(255, 255, 255, 255), RGBA8(128, 0, 128, 255)}},
};

static const char* button_theme_name(int theme) {
    if (theme < 0 || theme >= BUTTON_THEME_COUNT)
        theme = DEFAULT_BUTTON_THEME;
    return kButtonThemes[theme].name;
}

static void draw_primary_button(Ui* ui, int x, int y, int width, int height,
                               const char* label) {
    int theme = g_settings.button_theme;
    if (theme < 0 || theme >= BUTTON_THEME_COUNT)
        theme = DEFAULT_BUTTON_THEME;
    const ButtonTheme* style = &kButtonThemes[theme];
    for (int i = 0; i < style->count; i++) {
        const int top = y + height * i / style->count;
        const int bottom = y + height * (i + 1) / style->count;
        ui_rect(ui, x, top, width, bottom - top, style->colors[i]);
    }
    const int text_width = ui_text_width(ui, label, 15);
    const int text_x = x + (width - text_width) / 2;
    const int text_y = y + 13;
    unsigned brightest = 0;
    for (int i = 0; i < style->count; i++) {
        const u32 c = style->colors[i];
        const unsigned r = c & 0xff, g = (c >> 8) & 0xff, b = (c >> 16) & 0xff;
        const unsigned luma = 299 * r + 587 * g + 114 * b;
        if (luma > brightest)
            brightest = luma;
    }
    const u32 text_color = brightest > 170000
                               ? RGBA8(12, 14, 18, 255)
                               : RGBA8(255, 255, 255, 255);
    ui_text(ui, text_x, text_y, label, 15, text_color);
}

static bool readme_for(HbPackage* package) {
    if (!package->readme_fallback)
        return package->description[0] != '\0';
    const bool placeholder = strstr(package->description, "added for testing.") != NULL;
    if (package->description[0] && !placeholder)
        return true;
    char cache[512];
    snprintf(cache, sizeof(cache), README_DIR "/%s.txt", package->name);
    struct stat cached_status;
    if (stat(cache, &cached_status) == 0) {
        if (placeholder) {
            remove(cache);
        } else if (cached_status.st_size <= 1) {
            return false;
        }
    }
    char description[sizeof(package->description)];
    char error[128];
    if (readme_description(package->url, package->branch, cache, description,
                           sizeof(description), error, sizeof(error)) == 0) {
        snprintf(package->description, sizeof(package->description), "%s", description);
        return true;
    }
    if (placeholder)
        snprintf(package->description, sizeof(package->description), "No README description.");
    return false;
}

static void draw_shop_content(Ui* ui);

static void draw_shop(Ui* ui) {
    if (!ui)
        return;
    ui_begin(ui);
    ui_clear(ui, COLOR_BACKGROUND);
    draw_shop_content(ui);
    ui_end(ui);
    icon_prefetch_ensure();
}

static void draw_shop_content(Ui* ui) {
    const int visible = visible_count();
    HbPackage* selected = visible ? visible_package(g_selected) : NULL;
    if (selected)
        readme_for(selected);
    const int page_size = view_page_size();
    const int first = g_scroll_page * page_size;
    if (visible) {
        for (int slot = 0; slot < page_size; slot++) {
            const int index = first + slot;
            HbPackage* package = index < visible ? visible_package(index) : NULL;
            if (package)
                icon_for(package);
        }
    }
    ui_rect(ui, 0, 0, UI_WIDTH, 56, COLOR_PANEL);
    ui_text(ui, 24, 14, "brewstore", 24, COLOR_TEXT);
    ui_text(ui, 24 + ui_text_width(ui, "brewstore", 24) + 10, 27,
            "Codename YukinaMinato", 9, COLOR_MUTED);
    char filter_label[192];
    snprintf(filter_label, sizeof(filter_label), "%s · %s", filter_name(), sort_name());
    const int filter_width = ui_text_width(ui, filter_label, 13);
    int filter_x = UI_WIDTH - 32 - filter_width;
    if (filter_x < 360)
        filter_x = 360;
    ui_text_truncated(ui, filter_x, 25, filter_label, 13, COLOR_ACCENT,
                      UI_WIDTH - 24 - filter_x);
    if (g_search[0]) {
        char search_label[192];
        snprintf(search_label, sizeof(search_label), "Search: %s", g_search);
        const int min_search_x = 300;
        const int available = filter_x - 16 - min_search_x;
        if (available >= 40) {
            int search_width = ui_text_width(ui, search_label, 13);
            if (search_width > available)
                search_width = available;
            ui_text_truncated(ui, filter_x - 16 - search_width, 25, search_label,
                              13, COLOR_TEXT, search_width);
        }
    }

    if (!g_manifest.count) {
        ui_text(ui, 24, 104, "Nothing here yet", 26, COLOR_TEXT);
        ui_text(ui, 24, 146, "Press R for a catalog, or ZR for repo actions.", 16, COLOR_MUTED);
        ui_text(ui, 24, 174, REPOS_PATH, 13, COLOR_ACCENT);
    } else if (!selected) {
        ui_text(ui, 24, 104, "Nothing matches this filter", 26, COLOR_TEXT);
        ui_text(ui, 24, 146, "Press Y to change filter.", 16, COLOR_MUTED);
    } else {
        if (g_list_view) {
            for (int slot = 0; slot < LIST_VISIBLE_COUNT; slot++) {
                const int index = first + slot;
                if (index >= visible)
                    break;
                HbPackage* package = visible_package(index);
                if (!package)
                    break;
                draw_list_row(ui, package, 24, 76 + slot * 64, 872,
                              index == g_selected, is_marked(package));
            }
        } else {
            for (int slot = 0; slot < VISIBLE_COUNT; slot++) {
                const int index = first + slot;
                if (index >= visible)
                    break;
                HbPackage* package = visible_package(index);
                if (!package)
                    break;
                const int column = slot % VISIBLE_COLUMNS;
                const int row = slot / VISIBLE_COLUMNS;
                draw_card(ui, package, 32 + column * 292,
                          76 + row * 264, index == g_selected, is_marked(package));
            }
        }
        draw_scrollbar(ui, first, page_size, visible);

        ui_rect(ui, 936, 76, 312, 568, COLOR_PANEL);

        ui_text_truncated(ui, 958, 100, selected->title, 25, COLOR_TEXT, 268);
        ui_text_truncated(ui, 958, 136, selected->author, 14, COLOR_MUTED, 268);
        {
            const InstalledState* selected_state = package_state(selected);
            if (selected_state && selected_state->installed) {
                ui_text(ui, 958, 164, selected_state->update ? "Update available" : "Installed",
                        14, selected_state->update ? COLOR_ACCENT : COLOR_GREEN);
            }
        }
        {
            const InstalledState* selected_state = package_state(selected);
            const bool installed = selected_state && selected_state->installed;
            char version[64] = "";
            char hash[65];
            char file[512];
            unsigned long long installed_at = 0;
            unsigned long long updated_at = 0;
            const bool have_state = installed &&
                read_state(selected, version, sizeof(version), hash, sizeof(hash),
                           file, sizeof(file), &installed_at, &updated_at);

            char line[128];
            snprintf(line, sizeof(line), "Available  %s", display_version(selected));
            ui_text_truncated(ui, 958, 188, line, 14, COLOR_MUTED, 268);

            int description_y = 236;
            if (installed) {
                snprintf(line, sizeof(line), "Installed  %s",
                         have_state && version[0] ? version : "(external)");
                ui_text_truncated(ui, 958, 210, line, 14, COLOR_MUTED, 268);

                const unsigned long long when = updated_at ? updated_at : installed_at;
                if (when) {
                    char date[64];
                    format_installed_time(when, date, sizeof(date));
                    snprintf(line, sizeof(line), "Updated    %s", date);
                    ui_text_truncated(ui, 958, 232, line, 12, COLOR_MUTED, 268);
                }
                description_y = 260;
            }

            ui_markdown_wrap(ui, 958, description_y, selected->description, 15, COLOR_TEXT,
                             268, 7, 21);
        }

        ui_text_truncated(ui, 958, 522, selected->target, 12, COLOR_MUTED, 268);
        const int button_y = 556;
        const InstalledState* selected_state = package_state(selected);
        const bool selected_installed = selected_state && selected_state->installed;
        const bool selected_update = selected_state && selected_state->update;
        const char* primary_label = selected_installed
                                       ? (selected_update ? "A  Update" : "A  Reinstall")
                                       : "A  Install";
        draw_primary_button(ui, 958, button_y, 268, 42, primary_label);
    }

    ui_rect(ui, 0, 660, UI_WIDTH, 60, COLOR_PANEL);
    if (g_status[0]) {
        ui_text_truncated(ui, 24, 670, g_status, 14, COLOR_TEXT, UI_WIDTH - 48);
        ui_text_truncated(ui, 24, 696,
                          "A install   L select   X menu   - view   Y filter   L3 search   RS versions   ZL settings   B exit",
                          12, COLOR_MUTED, UI_WIDTH - 48);
    } else {
        ui_text_truncated(ui, 24, 682,
                          "A install   L select   X menu   - view   Y filter   L3 search   RS versions   ZL settings   B exit",
                          13, COLOR_MUTED, UI_WIDTH - 48);
    }

}

static int test_repo_present(void) {
    return access(TEST_REPO_PATH, F_OK) == 0;
}

static int repo_entry_count(void) {
    return g_repos.count + test_repo_present();
}

static void format_file_time(const char* path, char* out, size_t out_size) {
    struct stat status;
    if (stat(path, &status) != 0) {
        snprintf(out, out_size, "Updated never");
        return;
    }
    struct tm parts;
    if (!localtime_r(&status.st_mtime, &parts)) {
        snprintf(out, out_size, "Updated unknown");
        return;
    }
    char date[32];
    strftime(date, sizeof(date), "%Y-%m-%d %H:%M", &parts);
    snprintf(out, out_size, "Updated %s", date);
}

static void draw_repo_manager(Ui* ui) {
    if (!ui)
        return;
    ui_begin(ui);
    ui_clear(ui, COLOR_BACKGROUND);

    ui_rect(ui, 0, 0, UI_WIDTH, 56, COLOR_PANEL);
    ui_text(ui, 24, 14, "Repositories", 24, COLOR_TEXT);

    const int entries = repo_entry_count();
    if (!entries) {
        ui_text(ui, 24, 100, "No repositories.", 20, COLOR_TEXT);
        ui_text(ui, 24, 132, "Press R on the shop screen to add one.", 15, COLOR_MUTED);
    } else {
        for (int row = 0; row < REPO_VISIBLE_ROWS; row++) {
            const int i = g_repo_scroll + row;
            if (i >= entries)
                break;
            const int y = 80 + row * 52;
            if (i == g_repo_selected)
                ui_rect(ui, 24, y - 6, UI_WIDTH - 48, 44, COLOR_CARD_HOVER);
            if (i < g_repos.count) {
                char cache[640];
                char updated[64];
                repo_cache_path(g_repos.urls[i], cache, sizeof(cache));
                format_file_time(cache, updated, sizeof(updated));
                const int date_width = ui_text_width(ui, updated, 12);
                const int date_x = UI_WIDTH - 40 - date_width;
                int right = date_x - 16;
                if (!g_repos.enabled[i]) {
                    const char* disabled = "(disabled)";
                    const int disabled_width = ui_text_width(ui, disabled, 13);
                    ui_text(ui, right - disabled_width, y, disabled, 13, COLOR_MUTED);
                    right -= disabled_width + 12;
                }
                int url_width = right - 40;
                if (url_width < 40)
                    url_width = 40;
                ui_text_truncated(ui, 40, y, g_repos.urls[i], 16,
                                  g_repos.enabled[i] ? COLOR_TEXT : COLOR_MUTED,
                                  url_width);
                ui_text(ui, date_x, y + 2, updated, 12, COLOR_MUTED);
            } else {
                char updated[64];
                format_file_time(TEST_REPO_PATH, updated, sizeof(updated));
                const int date_width = ui_text_width(ui, updated, 12);
                ui_text(ui, 40, y, "Test repository", 16, COLOR_TEXT);
                ui_text(ui, 250, y + 1, "test-repo.json", 14, COLOR_MUTED);
                ui_text(ui, UI_WIDTH - 40 - date_width, y + 2, updated, 12, COLOR_MUTED);
            }
        }
    }

    ui_rect(ui, 0, 660, UI_WIDTH, 60, COLOR_PANEL);
    if (g_status[0])
        ui_text_truncated(ui, 24, 670, g_status, 14, COLOR_TEXT, UI_WIDTH - 48);
    ui_text(ui, 24, 696, "A disable/enable   R catalog   L git repo   X delete log   B close", 13, COLOR_MUTED);
    ui_end(ui);
}

static void draw_confirm(Ui* ui) {
    if (!ui)
        return;
    ui_begin(ui);
    ui_clear(ui, COLOR_BACKGROUND);
    ui_rect(ui, 0, 0, UI_WIDTH, 56, COLOR_PANEL);
    ui_text(ui, 24, 14, "Confirm", 24, COLOR_TEXT);

    const char* message = "Are you sure?";
    switch (g_confirm_action) {
        case CONFIRM_DISABLE_REPO: message = "Disable this repository?"; break;
        case CONFIRM_ENABLE_REPO: message = "Enable this repository?"; break;
        case CONFIRM_REMOVE_TEST: message = "Remove the test repository?"; break;
        case CONFIRM_DELETE_LOG: message = "Delete catalog.log?"; break;
        default: break;
    }
    ui_text(ui, 24, 120, message, 22, COLOR_TEXT);
    if (g_confirm_action == CONFIRM_DELETE_LOG)
        ui_text(ui, 24, 158, CACHE_DIR "/catalog.log", 14, COLOR_MUTED);
    else if (g_confirm_index >= 0 && g_confirm_index < g_repos.count)
        ui_text_truncated(ui, 24, 158, g_repos.urls[g_confirm_index], 14,
                          COLOR_MUTED, UI_WIDTH - 48);

    ui_rect(ui, 24, 260, 180, 46, g_confirm_yes ? COLOR_ACCENT : COLOR_CARD);
    ui_text(ui, 90, 273, "Yes", 17, g_confirm_yes ? COLOR_TEXT : COLOR_MUTED);
    ui_rect(ui, 220, 260, 180, 46, g_confirm_yes ? COLOR_CARD : COLOR_ACCENT);
    ui_text(ui, 292, 273, "No", 17, g_confirm_yes ? COLOR_MUTED : COLOR_TEXT);
    ui_text(ui, 24, 696, "Left/Right select   A confirm   B cancel", 13, COLOR_MUTED);
    ui_end(ui);
}

static void request_repo_action(void) {
    if (g_repo_selected >= repo_entry_count())
        return;
    g_confirm_index = g_repo_selected;
    g_confirm_yes = false;
    if (g_repo_selected >= g_repos.count)
        g_confirm_action = CONFIRM_REMOVE_TEST;
    else
        g_confirm_action = g_repos.enabled[g_repo_selected]
                               ? CONFIRM_DISABLE_REPO : CONFIRM_ENABLE_REPO;
    draw_confirm(g_ui);
}

static void request_log_delete(void) {
    g_confirm_action = CONFIRM_DELETE_LOG;
    g_confirm_yes = false;
    draw_confirm(g_ui);
}

static void perform_confirm(void) {
    char error[256] = {0};
    if (g_confirm_action == CONFIRM_DELETE_LOG) {
        if (remove(CACHE_DIR "/catalog.log") == 0)
            set_status("Catalog log deleted.");
        else
            set_status("No catalog log to delete.");
    } else if (g_confirm_action == CONFIRM_REMOVE_TEST) {
        if (remove(TEST_REPO_PATH) == 0)
            set_status("Test repository removed.");
        else
            set_status("Could not remove test-repo.json.");
        refresh_catalogs(false);
    } else if (g_confirm_index >= 0 && g_confirm_index < g_repos.count) {
        const bool enable = g_confirm_action == CONFIRM_ENABLE_REPO;
        g_repos.enabled[g_confirm_index] = enable;
        if (repos_save(REPOS_PATH, &g_repos, error, sizeof(error)) != 0) {
            set_status("Could not save repos.json: %s", error);
        } else {
            set_status(enable ? "Repository enabled." : "Repository disabled.");
            refresh_catalogs(false);
        }
    }
    g_confirm_action = CONFIRM_NONE;
    draw_repo_manager(g_ui);
}

static void format_age(unsigned long long published, char* out, size_t out_size) {
    if (!published) {
        snprintf(out, out_size, "released unknown");
        return;
    }
    const unsigned long long now = (unsigned long long)time(NULL);
    const unsigned long long age = now > published ? now - published : 0;
    if (age < 3600) {
        snprintf(out, out_size, "released %llu min ago", age / 60);
    } else if (age < 86400) {
        snprintf(out, out_size, "released %llu h ago", age / 3600);
    } else {
        snprintf(out, out_size, "released %llu days ago", age / 86400);
    }
}

static int first_stable_version(void) {
    for (int i = 0; i < g_version_count; i++)
        if (!g_version_choices[i].prerelease)
            return i;
    return 0;
}

static void draw_versions(Ui* ui) {
    if (!ui)
        return;
    ui_begin(ui);
    ui_clear(ui, COLOR_BACKGROUND);
    ui_rect(ui, 0, 0, UI_WIDTH, 56, COLOR_PANEL);
    ui_text(ui, 24, 14, "Versions", 24, COLOR_TEXT);
    if (g_version_package)
        ui_text_truncated(ui, 24, 70, g_version_package->title, 17, COLOR_MUTED,
                          UI_WIDTH - 48);
    for (int slot = 0; slot < VERSION_VISIBLE_ROWS; slot++) {
        const int index = g_version_scroll + slot;
        if (index >= g_version_count)
            break;
        const int y = 104 + slot * 64;
        const bool selected = index == g_version_selected;
        if (selected)
            ui_rect(ui, 20, y - 4, UI_WIDTH - 40, 60, COLOR_CARD_HOVER);
        if (g_version_choices[index].prerelease)
            ui_text(ui, 34, y, g_version_choices[index].tag, 18, COLOR_ACCENT);
        else
            ui_text_truncated(ui, 34, y, g_version_choices[index].tag, 18, COLOR_TEXT,
                              UI_WIDTH - 240);
        char age[64];
        format_age(g_version_choices[index].published, age, sizeof(age));
        char meta[192];
        if (g_version_choices[index].prerelease)
            snprintf(meta, sizeof(meta), "pre-release   %s", age);
        else
            snprintf(meta, sizeof(meta), "%s", age);
        ui_text_truncated(ui, 34, y + 24, meta, 12, COLOR_MUTED, UI_WIDTH - 240);
        ui_text_truncated(ui, 34, y + 42, g_version_choices[index].asset, 11, COLOR_MUTED,
                          UI_WIDTH - 240);
        if (selected) {
            const int action_width = 200;
            const int action_x = UI_WIDTH - action_width;
            const int install_width = ui_text_width(ui, "A install", 13);
            ui_text(ui, action_x + (action_width - install_width) / 2, y + 20,
                    "A install", 13, COLOR_ACCENT);
        }
    }
    ui_rect(ui, 0, 660, UI_WIDTH, 60, COLOR_PANEL);
    ui_text(ui, 24, 682, "Up/down select   A install   B back", 13, COLOR_MUTED);
    ui_end(ui);
}

static void draw_settings(Ui* ui) {
    if (!ui)
        return;
    if (g_setting_row > 4)
        g_setting_row = 4;

    ui_begin(ui);
    ui_clear(ui, COLOR_BACKGROUND);
    ui_rect(ui, 0, 0, UI_WIDTH, 56, COLOR_PANEL);
    ui_text(ui, 24, 14, "Settings", 24, COLOR_TEXT);

    if (g_setting_row == 0)
        ui_rect(ui, 24, 88, UI_WIDTH - 48, 62, COLOR_CARD_HOVER);
    ui_text(ui, 40, 98, "Default install path", 17, COLOR_TEXT);
    ui_text_truncated(ui, 40, 124, g_settings.default_install_path, 15, COLOR_ACCENT,
                      UI_WIDTH - 80);

    if (g_setting_row == 1)
        ui_rect(ui, 24, 156, UI_WIDTH - 48, 62, COLOR_CARD_HOVER);
    ui_text(ui, 40, 166, "Download proxy", 17, COLOR_TEXT);
    ui_text_truncated(ui, 40, 192,
                      g_settings.download_proxy[0] ? g_settings.download_proxy : "(none)",
                      15, g_settings.download_proxy[0] ? COLOR_ACCENT : COLOR_MUTED,
                      UI_WIDTH - 80);

    if (g_setting_row == 2)
        ui_rect(ui, 24, 224, UI_WIDTH - 48, 62, COLOR_CARD_HOVER);
    ui_text(ui, 40, 234, "Button theme", 17, COLOR_TEXT);
    ui_text(ui, 40, 260, button_theme_name(g_settings.button_theme), 15,
            COLOR_ACCENT);

    if (g_setting_row == 3)
        ui_rect(ui, 24, 292, UI_WIDTH - 48, 62, COLOR_CARD_HOVER);
    ui_text(ui, 40, 302, "Ignore SHA-256 mismatch (dangerous)", 17, COLOR_TEXT);
    ui_text(ui, 40, 328, g_settings.ignore_sha ? "On" : "Off", 15,
            g_settings.ignore_sha ? COLOR_ACCENT : COLOR_MUTED);

    if (g_setting_row == 4)
        ui_rect(ui, 24, 360, UI_WIDTH - 48, 62, COLOR_CARD_HOVER);
    ui_text(ui, 40, 370, "Clear catalog cache", 17, COLOR_TEXT);
    ui_text(ui, 40, 396, "Delete downloaded manifests and refresh", 13, COLOR_MUTED);

    ui_rect(ui, 0, 660, UI_WIDTH, 60, COLOR_PANEL);
    if (g_status[0])
        ui_text_truncated(ui, 24, 670, g_status, 14, COLOR_TEXT, UI_WIDTH - 48);
    ui_text(ui, 24, 696, "ZR repositories   A edit/change   B close", 13, COLOR_MUTED);
    ui_end(ui);
}

static void cycle_button_theme(int delta) {
    const int previous = g_settings.button_theme;
    const int next = (previous + (delta < 0 ? BUTTON_THEME_COUNT - 1 : 1)) %
                     BUTTON_THEME_COUNT;
    g_settings.button_theme = next;
    char error[128] = {0};
    if (settings_save(SETTINGS_PATH, &g_settings, error, sizeof(error)) != 0) {
        g_settings.button_theme = previous;
        set_status("Could not save settings: %s", error);
    } else {
        set_status("Button theme: %s.", button_theme_name(next));
    }
    draw_settings(g_ui);
}

static void clear_catalog_cache(void) {
    path_remove_tree(REPO_CACHE_DIR);
    path_mkdirs(REPO_CACHE_DIR);
    refresh_catalogs(true);
    set_status("Catalog cache cleared.");
    draw_settings(g_ui);
}

static void toggle_ignore_sha(void) {
    const bool previous = g_settings.ignore_sha;
    g_settings.ignore_sha = !previous;
    char error[128] = {0};
    if (settings_save(SETTINGS_PATH, &g_settings, error, sizeof(error)) != 0) {
        g_settings.ignore_sha = previous;
        set_status("Could not save settings: %s", error);
    } else {
        set_status("Ignore SHA-256 mismatch: %s.", g_settings.ignore_sha ? "On" : "Off");
    }
    draw_settings(g_ui);
}

static void edit_default_path(void) {
    char path[256];
    if (!prompt_text("Default install path", "Folder under sdmc:/switch or sdmc:/brewstore",
                     g_settings.default_install_path, path, sizeof(path))) {
        set_status("Path edit cancelled.");
        draw_settings(g_ui);
        return;
    }
    size_t path_length = strlen(path);
    while (path_length > 12 && path[path_length - 1] == '/')
        path[--path_length] = '\0';
    if (!settings_valid_path(path)) {
        set_status("Path must be under sdmc:/switch or sdmc:/brewstore.");
        draw_settings(g_ui);
        return;
    }
    char error[128] = {0};
    snprintf(g_settings.default_install_path, sizeof(g_settings.default_install_path),
             "%s", path);
    if (settings_save(SETTINGS_PATH, &g_settings, error, sizeof(error)) != 0) {
        set_status("Could not save settings: %s", error);
        draw_settings(g_ui);
        return;
    }
    path_mkdirs(g_settings.default_install_path);
    refresh_catalogs(false);
    set_status("Default install path saved.");
    draw_settings(g_ui);
}

static void edit_proxy(void) {
    char proxy[256];
    if (!prompt_text("Download proxy", "URL prefix, or empty to disable",
                     g_settings.download_proxy, proxy, sizeof(proxy))) {
        set_status("Proxy edit cancelled.");
        draw_settings(g_ui);
        return;
    }
    if (!settings_valid_proxy(proxy)) {
        set_status("Proxy must be an https URL or empty.");
        draw_settings(g_ui);
        return;
    }
    char error[128] = {0};
    snprintf(g_settings.download_proxy, sizeof(g_settings.download_proxy), "%s", proxy);
    if (settings_save(SETTINGS_PATH, &g_settings, error, sizeof(error)) != 0) {
        set_status("Could not save settings: %s", error);
        draw_settings(g_ui);
        return;
    }
    http_set_proxy_prefix(g_settings.download_proxy);
    set_status(g_settings.download_proxy[0] ? "Download proxy saved." : "Download proxy disabled.");
    draw_settings(g_ui);
}

typedef struct {
    Thread thread;
    volatile bool running;
    volatile bool cancel;
    volatile int result;
    volatile unsigned long long received;
    volatile unsigned long long total;
    unsigned long long expected_total;
    unsigned long long size_limit;
    volatile bool size_exceeded;
    const char* title;
    char url[1024];
    char cache[512];
    char hash[65];
    char error[256];
} DownloadJob;

static void draw_theme_bar(Ui* ui, int x, int y, int width, int height) {
    if (width <= 0)
        return;
    int theme = g_settings.button_theme;
    if (theme < 0 || theme >= BUTTON_THEME_COUNT)
        theme = DEFAULT_BUTTON_THEME;
    const ButtonTheme* style = &kButtonThemes[theme];
    for (int i = 0; i < style->count; i++) {
        const int top = y + height * i / style->count;
        const int bottom = y + height * (i + 1) / style->count;
        ui_rect(ui, x, top, width, bottom - top, style->colors[i]);
    }
}

static void draw_download_progress(Ui* ui, const char* title,
                                   unsigned long long received,
                                   unsigned long long total) {
    if (!ui)
        return;
    ui_begin(ui);
    ui_clear(ui, COLOR_BACKGROUND);
    ui_text(ui, 28, 96, "Downloading", 26, COLOR_TEXT);
    if (g_batch_active) {
        char batch[32];
        snprintf(batch, sizeof(batch), "%d / %d", g_batch_index, g_batch_total);
        const int width = ui_text_width(ui, batch, 15);
        ui_text(ui, UI_WIDTH - 28 - width, 104, batch, 15, COLOR_ACCENT);
    }
    ui_text_truncated(ui, 28, 136, title, 17, COLOR_MUTED, UI_WIDTH - 56);

    const int bar_x = 28;
    const int bar_y = 340;
    const int bar_width = UI_WIDTH - 56;
    const int bar_height = 18;
    ui_rect(ui, bar_x, bar_y, bar_width, bar_height, COLOR_CARD);
    if (total) {
        unsigned long long percent = received * 100 / total;
        if (percent > 100)
            percent = 100;
        const int filled = (int)((unsigned long long)bar_width * percent / 100);
        draw_theme_bar(ui, bar_x, bar_y, filled, bar_height);
        char line[128];
        snprintf(line, sizeof(line), "%llu%%   %.1f / %.1f MB", percent,
                 (double)received / (1024.0 * 1024.0),
                 (double)total / (1024.0 * 1024.0));
        ui_text(ui, bar_x, bar_y + 32, line, 15, COLOR_MUTED);
    } else {
        const int filled = bar_width / 4;
        const int travel = bar_width - filled;
        const int offset = (int)((received / 32768) % (unsigned long long)(travel + 1));
        draw_theme_bar(ui, bar_x + offset, bar_y, filled, bar_height);
        char line[96];
        snprintf(line, sizeof(line), "%.1f MB", (double)received / (1024.0 * 1024.0));
        ui_text(ui, bar_x, bar_y + 32, line, 15, COLOR_MUTED);
    }
    ui_text(ui, 28, 680, "B cancel", 13, COLOR_MUTED);
    ui_end(ui);
}

static void download_progress_callback(unsigned long long received,
                                       unsigned long long total, void* user) {
    DownloadJob* job = user;
    job->received = received;
    job->total = total ? total : job->expected_total;
    if (job->size_limit && received > job->size_limit + 65536) {
        job->size_exceeded = true;
        job->cancel = true;
    }
}

static bool has_free_space(unsigned long long bytes) {
    if (!bytes)
        return true;
    struct statvfs fs;
    if (statvfs("sdmc:/", &fs) != 0)
        return true;
    const unsigned long long available =
        (unsigned long long)fs.f_bavail * (unsigned long long)fs.f_frsize;
    const unsigned long long needed = bytes + bytes / 20 + 16 * 1024 * 1024;
    return available >= needed;
}

static bool download_cancel_callback(void* user) {
    return ((DownloadJob*)user)->cancel;
}

static void download_worker(void* user) {
    DownloadJob* job = user;
    job->result = http_download_cancel(job->url, job->cache, job->hash,
                                       job->error, sizeof(job->error),
                                       download_progress_callback, job,
                                       download_cancel_callback, job);
    job->running = false;
}

static int run_download(DownloadJob* job, PadState* pad) {
    job->result = -1;
    job->running = true;
    job->cancel = false;
    job->received = 0;
    job->total = job->expected_total;
    job->hash[0] = '\0';
    job->error[0] = '\0';

    if (R_FAILED(threadCreate(&job->thread, download_worker, job, NULL,
                              0x40000, 0x2C, -2)))
        return -1;
    if (R_FAILED(threadStart(&job->thread))) {
        threadClose(&job->thread);
        return -1;
    }

    const u64 frequency = armGetSystemTickFreq();
    u64 last_draw = 0;
    while (job->running) {
        if (!appletMainLoop()) {
            job->cancel = true;
            break;
        }
        padUpdate(pad);
        const u64 down = padGetButtonsDown(pad);
        HidKeyboardState keyboard;
        memset(&keyboard, 0, sizeof(keyboard));
        hidGetKeyboardStates(&keyboard, 1);
        if ((down & HidNpadButton_B) ||
            hidKeyboardStateGetKey(&keyboard, HidKeyboardKey_Escape)) {
            job->cancel = true;
            if (hidKeyboardStateGetKey(&keyboard, HidKeyboardKey_Escape))
                g_quit = true;
        }
        const u64 now = svcGetSystemTick();
        if (!last_draw || now - last_draw >= frequency / 4) {
            draw_download_progress(g_ui, job->title, job->received, job->total);
            last_draw = now;
        }
        svcSleepThread(16 * 1000 * 1000);
    }

    threadWaitForExit(&job->thread);
    threadClose(&job->thread);
    return job->result;
}

static bool is_plain_github_repo(const char* url) {
    char name[64];
    if (github_repo_name(url, name, sizeof(name)) != 0)
        return false;
    return !strstr(url, "/releases/") && !strstr(url, "/archive/") &&
           !strstr(url, "/download/") && !strstr(url, ".zip") &&
           !strstr(url, ".tar") && !strstr(url, ".tgz") &&
           !strstr(url, ".txz") && !strstr(url, ".nro");
}

static void draw_install_progress(Ui* ui, const char* title, const char* step,
                                  int percent) {
    if (!ui)
        return;
    ui_begin(ui);
    ui_clear(ui, COLOR_BACKGROUND);
    ui_rect(ui, 0, 0, UI_WIDTH, 56, COLOR_PANEL);
    ui_text(ui, 24, 14, "Installing", 24, COLOR_TEXT);
    if (g_batch_active) {
        char batch[32];
        snprintf(batch, sizeof(batch), "%d / %d", g_batch_index, g_batch_total);
        const int width = ui_text_width(ui, batch, 15);
        ui_text(ui, UI_WIDTH - 28 - width, 22, batch, 15, COLOR_ACCENT);
    }
    ui_text_truncated(ui, 28, 96, title, 20, COLOR_TEXT, UI_WIDTH - 56);
    ui_text_truncated(ui, 28, 132, step, 16, COLOR_MUTED, UI_WIDTH - 56);

    const int bar_x = 28;
    const int bar_y = 340;
    const int bar_width = UI_WIDTH - 56;
    const int bar_height = 18;
    ui_rect(ui, bar_x, bar_y, bar_width, bar_height, COLOR_CARD);
    if (percent < 0)
        percent = 0;
    if (percent > 100)
        percent = 100;
    if (g_batch_active && g_batch_total > 0)
        percent = (g_batch_completed * 100 + percent) / g_batch_total;
    if (percent > 100)
        percent = 100;
    const int filled = bar_width * percent / 100;
    draw_theme_bar(ui, bar_x, bar_y, filled, bar_height);
    char line[64];
    snprintf(line, sizeof(line), "%d%%", percent);
    ui_text(ui, bar_x, bar_y + 32, line, 15, COLOR_MUTED);
    ui_end(ui);
}

static void draw_uninstall_progress(Ui* ui, const char* title, int current, int total,
                                    const char* path) {
    if (!ui)
        return;
    ui_begin(ui);
    ui_clear(ui, COLOR_BACKGROUND);
    ui_rect(ui, 0, 0, UI_WIDTH, 56, COLOR_PANEL);
    ui_text(ui, 24, 14, "Uninstalling", 24, COLOR_TEXT);
    ui_text_truncated(ui, 28, 96, title, 20, COLOR_TEXT, UI_WIDTH - 56);
    char step[320];
    const char* base = path ? strrchr(path, '/') : NULL;
    base = base ? base + 1 : path;
    if (base && base[0])
        snprintf(step, sizeof(step), "%d / %d  %s", current, total, base);
    else
        snprintf(step, sizeof(step), "%d / %d", current, total);
    ui_text_truncated(ui, 28, 132, step, 16, COLOR_MUTED, UI_WIDTH - 56);
    const int bar_x = 28;
    const int bar_y = 340;
    const int bar_width = UI_WIDTH - 56;
    const int bar_height = 18;
    ui_rect(ui, bar_x, bar_y, bar_width, bar_height, COLOR_CARD);
    int percent = total > 0 ? current * 100 / total : 0;
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    draw_theme_bar(ui, bar_x, bar_y, bar_width * percent / 100, bar_height);
    char line[64];
    snprintf(line, sizeof(line), "%d%%", percent);
    ui_text(ui, bar_x, bar_y + 32, line, 15, COLOR_MUTED);
    ui_end(ui);
}

typedef struct {
    const HbPackage* package;
    FILE* list;
    char temporary[768];
    char final[768];
} ExtractCapture;

static void capture_begin(ExtractCapture* capture, const HbPackage* package) {
    memset(capture, 0, sizeof(*capture));
    capture->package = package;
    snprintf(capture->final, sizeof(capture->final), INSTALLED_DIR "/%s.files",
             package->name);
    snprintf(capture->temporary, sizeof(capture->temporary), "%.762s.tmp", capture->final);
    char old[800];
    snprintf(old, sizeof(old), "%s.old", capture->final);
    remove(old);
    rename(capture->final, old);
    capture->list = fopen(capture->temporary, "wb");
}

static void capture_add(ExtractCapture* capture, const char* relative) {
    if (!capture->list || !relative[0])
        return;
    char full[1024];
    const size_t length = strlen(capture->package->target);
    const bool slash = length && capture->package->target[length - 1] == '/';
    const int written = snprintf(full, sizeof(full), slash ? "%s%s" : "%s/%s",
                                 capture->package->target, relative);
    if (written > 0 && (size_t)written < sizeof(full))
        fprintf(capture->list, "%s\n", full);
}

static unsigned int path_hash(const char* path) {
    unsigned int hash = 2166136261u;
    while (*path)
        hash = (hash ^ (unsigned char)*path++) * 16777619u;
    return hash;
}

static void remove_stale_files(const char* old_path, const char* new_path) {
    FILE* new_file = fopen(new_path, "rb");
    if (!new_file)
        return;
    if (fseek(new_file, 0, SEEK_END) != 0) {
        fclose(new_file);
        return;
    }
    const long size = ftell(new_file);
    rewind(new_file);
    if (size <= 0) {
        fclose(new_file);
        return;
    }
    char* data = malloc((size_t)size + 1);
    if (!data) {
        fclose(new_file);
        return;
    }
    const size_t length = fread(data, 1, (size_t)size, new_file);
    fclose(new_file);
    data[length] = '\0';

    int lines = 0;
    for (size_t i = 0; i < length; i++) {
        if (data[i] == '\n')
            lines++;
    }
    size_t capacity = 16;
    while (capacity < (size_t)lines * 2 + 1)
        capacity <<= 1;
    int* slots = calloc(capacity, sizeof(int));
    if (!slots) {
        free(data);
        return;
    }

    for (char* line = data; line < data + length; ) {
        char* end = strchr(line, '\n');
        if (end)
            *end = '\0';
        if (line[0]) {
            unsigned int slot = path_hash(line) & (unsigned int)(capacity - 1);
            while (slots[slot])
                slot = (slot + 1) & (unsigned int)(capacity - 1);
            slots[slot] = (int)(line - data) + 1;
        }
        if (!end)
            break;
        line = end + 1;
    }
    FILE* old_file = fopen(old_path, "rb");
    if (!old_file) {
        free(slots);
        free(data);
        return;
    }
    char line[1024];
    while (fgets(line, sizeof(line), old_file)) {
        line[strcspn(line, "\r\n")] = '\0';
        if (strncmp(line, "sdmc:/", 6) != 0 || strstr(line, "..") ||
            strcmp(line, "sdmc:/") == 0)
            continue;
        unsigned int slot = path_hash(line) & (unsigned int)(capacity - 1);
        bool found = false;
        while (slots[slot]) {
            if (strcmp(data + slots[slot] - 1, line) == 0) {
                found = true;
                break;
            }
            slot = (slot + 1) & (unsigned int)(capacity - 1);
        }
        if (!found) {
            char path[1024];
            snprintf(path, sizeof(path), "%s", line);
            remove(path);
            prune_empty_parents(path);
        }
    }
    fclose(old_file);
    free(slots);
    free(data);
}

static void capture_finish(ExtractCapture* capture) {
    if (!capture->final[0])
        return;
    char old[800];
    snprintf(old, sizeof(old), "%s.old", capture->final);
    if (!capture->list) {
        remove(capture->final);
        rename(old, capture->final);
        return;
    }
    fclose(capture->list);
    capture->list = NULL;
    remove(capture->final);
    if (rename(capture->temporary, capture->final) != 0) {
        remove(capture->temporary);
        rename(old, capture->final);
        return;
    }
    remove_stale_files(old, capture->final);
    remove(old);
}

static void capture_cancel(ExtractCapture* capture) {
    if (!capture->final[0])
        return;
    if (capture->list) {
        fclose(capture->list);
        capture->list = NULL;
    }
    remove(capture->temporary);
    char old[800];
    snprintf(old, sizeof(old), "%s.old", capture->final);
    remove(capture->final);
    rename(old, capture->final);
}

static void capture_single(const HbPackage* package, const char* path) {
    ExtractCapture capture;
    capture_begin(&capture, package);
    if (capture.list) {
        fprintf(capture.list, "%s\n", path);
        capture_finish(&capture);
    }
}

static void extraction_progress(int current, int total, const char* name, void* user) {
    ExtractCapture* capture = user;
    const HbPackage* package = capture ? capture->package : NULL;
    const char* title = package ? package->title : "";
    if (capture)
        capture_add(capture, name);
    const char* base = strrchr(name, '/');
    base = base ? base + 1 : name;
    int stride = total / 50;
    if (stride < 1)
        stride = 1;
    if (current != total && current % stride != 0)
        return;
    char step[320];
    snprintf(step, sizeof(step), "%d / %d  %s", current, total, base);
    int percent = total > 0 ? 45 + current * 45 / total : 45;
    if (percent > 89)
        percent = 89;
    draw_install_progress(g_ui, title, step, percent);
}

static bool install_complete(const HbPackage* package) {
    return !package->marker[0] || access(package->marker, F_OK) == 0;
}

static bool name_contains_ci(const char* text, const char* needle) {
    const size_t length = strlen(needle);
    if (!length)
        return true;
    for (; *text; text++) {
        size_t i = 0;
        while (i < length && text[i] &&
               tolower((unsigned char)text[i]) == tolower((unsigned char)needle[i]))
            i++;
        if (i == length)
            return true;
    }
    return false;
}

static void find_nro_in(const char* directory, const char* package,
                        char* preferred, size_t preferred_size,
                        char* fallback, size_t fallback_size, int* count, int depth) {
    if (depth > 8)
        return;
    DIR* dir = opendir(directory);
    if (!dir)
        return;
    struct dirent* entry;
    while ((entry = readdir(dir))) {
        if (entry->d_name[0] == '.')
            continue;
        char path[1024];
        const int written = snprintf(path, sizeof(path), "%s/%s", directory, entry->d_name);
        if (written <= 0 || (size_t)written >= sizeof(path))
            continue;
        struct stat status;
        if (lstat(path, &status) != 0)
            continue;
        if (S_ISDIR(status.st_mode)) {
            find_nro_in(path, package, preferred, preferred_size,
                        fallback, fallback_size, count, depth + 1);
        } else if (ends_with_ci(path, ".nro")) {
            (*count)++;
            if (!fallback[0])
                snprintf(fallback, fallback_size, "%.*s", (int)fallback_size - 1, path);
            if (!preferred[0]) {
                const char* base = strrchr(path, '/');
                base = base ? base + 1 : path;
                if (name_contains_ci(base, package))
                    snprintf(preferred, preferred_size, "%s", path);
            }
        }
    }
    closedir(dir);
}

static bool shortcut_target(const HbPackage* package, const char* installed_file,
                            char* out, size_t out_size) {
    out[0] = '\0';
    if (installed_file && installed_file[0] && ends_with_ci(installed_file, ".nro") &&
        access(installed_file, F_OK) == 0) {
        snprintf(out, out_size, "%s", installed_file);
        return true;
    }
    if (package->marker[0] && ends_with_ci(package->marker, ".nro") &&
        access(package->marker, F_OK) == 0) {
        snprintf(out, out_size, "%s", package->marker);
        return true;
    }
    if (strcmp(package->target, "sdmc:/") == 0)
        return false;

    char preferred[512] = "";
    char fallback[512] = "";
    int count = 0;
    find_nro_in(package->target, package->name, preferred, sizeof(preferred),
                fallback, sizeof(fallback), &count, 0);
    if (preferred[0]) {
        snprintf(out, out_size, "%s", preferred);
        return true;
    }
    if (count == 1 && fallback[0]) {
        snprintf(out, out_size, "%s", fallback);
        return true;
    }
    return false;
}

static void hbmenu_star_path(const char* nro, char* out, size_t out_size) {
    const char* slash = strrchr(nro, '/');
    if (!slash || !slash[1]) {
        out[0] = '\0';
        return;
    }
    snprintf(out, out_size, "%.*s.%s.star", (int)(slash - nro + 1), nro, slash + 1);
}

/* hbmenu keeps a .<nro>.star marker beside the NRO. If an update changes the
 * NRO filename (or directory), carry the marker over so the star survives. */
static void preserve_hbmenu_star(const char* old_nro, const char* new_nro) {
    if (!old_nro[0] || !new_nro[0] || strcmp(old_nro, new_nro) == 0)
        return;
    char old_star[1024];
    char new_star[1024];
    hbmenu_star_path(old_nro, old_star, sizeof(old_star));
    hbmenu_star_path(new_nro, new_star, sizeof(new_star));
    if (!old_star[0] || !new_star[0] || access(old_star, F_OK) != 0 ||
        access(new_star, F_OK) == 0)
        return;
    rename(old_star, new_star);
}

static void update_shortcut(const HbPackage* package, const char* installed_file) {
    char nro[512];
    if (package->shortcut &&
        shortcut_target(package, installed_file, nro, sizeof(nro)))
        shortcuts_set(SHORTCUTS_PATH, package->name, package->title, nro);
    else
        shortcuts_remove(SHORTCUTS_PATH, package->name);
}

static void install_redraw(void) {
    if (!g_batch_active)
        draw_shop(g_ui);
}

static void install_package(const HbPackage* package, PadState* pad,
                            const ReleaseChoice* choice) {
    if (!g_network_ready) {
        set_status("Network is unavailable.");
        install_redraw();
        return;
    }

    char url[1024];
    char asset_name[128] = "";
    char error[256] = {0};
    unsigned long long asset_size = 0;
    if (choice) {
        snprintf(url, sizeof(url), "%s", choice->url);
        snprintf(asset_name, sizeof(asset_name), "%s", choice->asset);
    } else if (package->download_url[0]) {
        snprintf(url, sizeof(url), "%s", package->download_url);
        snprintf(asset_name, sizeof(asset_name), "%s", package->asset);
        if (package->size_known)
            asset_size = package->size;
    } else if (package->release[0] || is_plain_github_repo(package->url)) {
        set_status("Finding latest release for %s ...", package->title);
        install_redraw();
        if (release_latest_asset(package->url, package->asset, CACHE_DIR "/release.json",
                                 url, sizeof(url), asset_name, sizeof(asset_name),
                                 &asset_size, NULL, 0, error, sizeof(error)) != 0) {
            set_status("Release failed: %s", error);
            install_redraw();
            return;
        }
    } else if (package->asset[0]) {
        snprintf(url, sizeof(url), "%s", package->url);
        snprintf(asset_name, sizeof(asset_name), "%s", package->asset);
    } else if (http_github_archive_url(package->url, package->branch, url, sizeof(url)) != 0) {
        set_status("Could not resolve the package URL.");
        install_redraw();
        return;
    } else {
        snprintf(asset_name, sizeof(asset_name), "package.zip");
    }

    if (asset_size && !has_free_space(asset_size)) {
        set_status("Not enough free SD space for %s.", package->title);
        install_redraw();
        return;
    }

    char cache[512];
    snprintf(cache, sizeof(cache), CACHE_DIR "/%s", asset_name[0] ? "release-asset" : "package.zip");
    set_status("Downloading %s ...", package->title);
    install_redraw();

    DownloadJob job;
    memset(&job, 0, sizeof(job));
    snprintf(job.url, sizeof(job.url), "%s", url);
    snprintf(job.cache, sizeof(job.cache), "%s", cache);
    job.expected_total = asset_size;
    job.size_limit = asset_size;
    job.title = package->title;
    const int download_result = run_download(&job, pad);
    if (download_result == -2 || job.cancel) {
        g_ignore_b = true;
        g_batch_stop = true;
        if (job.size_exceeded)
            set_status("Download exceeded the expected asset size.");
        else
            set_status("Download cancelled.");
        install_redraw();
        return;
    }
    if (download_result != 0) {
        set_status("Download failed: %s", job.error[0] ? job.error : "network error");
        install_redraw();
        return;
    }
    char hash[65];
    snprintf(hash, sizeof(hash), "%s", job.hash);
    const char* expected_sha = choice ? choice->sha256 : package->sha256;
    if (!g_settings.ignore_sha && expected_sha[0] &&
        strcasecmp(hash, expected_sha) != 0) {
        remove(cache);
        set_status("SHA-256 mismatch.");
        install_redraw();
        return;
    }

    if (strcmp(package->target, "sdmc:/") == 0 && !asset_name[0]) {
        set_status("The SD card root target needs a release asset.");
        install_redraw();
        return;
    }
    if (strcmp(package->target, "sdmc:/switch") == 0 &&
        (!asset_name[0] || archive_is_archive(asset_name))) {
        set_status("sdmc:/switch only accepts a single non-archive asset.");
        install_redraw();
        return;
    }

    draw_install_progress(g_ui, package->title, "Preparing install", 10);
    if (strcmp(package->target, "sdmc:/") != 0 && path_mkdirs(package->target) != 0) {
        set_status("Could not prepare the install target.");
        install_redraw();
        return;
    }

    draw_install_progress(g_ui, package->title,
                          asset_name[0] && !archive_is_archive(asset_name)
                              ? "Writing application" : "Extracting files", 45);
    char installed_file[512] = "";
    ExtractCapture capture;
    memset(&capture, 0, sizeof(capture));
    if (asset_name[0]) {
        if (archive_is_archive(asset_name)) {
            capture_begin(&capture, package);
            if (archive_extract(cache, asset_name, package->target, package->strip_prefix, error, sizeof(error),
                        extraction_progress, &capture) != 0) {
                capture_cancel(&capture);
                set_status("Extract failed: %s", error);
                install_redraw();
                return;
            }
        } else {
            snprintf(installed_file, sizeof(installed_file),
                     package->target[strlen(package->target) - 1] == '/' ? "%s%s" : "%s/%s",
                     package->target, asset_name);
            if (path_copy_file(cache, installed_file) != 0) {
                set_status("Could not write the release asset.");
                install_redraw();
                return;
            }
            capture_single(package, installed_file);
        }
    } else {
        capture_begin(&capture, package);
        if (archive_extract(cache, asset_name, package->target, package->strip_prefix, error,
                        sizeof(error), extraction_progress, &capture) != 0) {
            capture_cancel(&capture);
            set_status("Extract failed: %s", error);
            install_redraw();
            return;
        }
    }
    if (!install_complete(package)) {
        capture_cancel(&capture);
        set_status("Install incomplete: expected file was not written.");
        install_redraw();
        return;
    }
    capture_finish(&capture);

    draw_install_progress(g_ui, package->title, "Finishing", 90);
    char old_version[64];
    char old_hash[65];
    char old_file[512];
    unsigned long long installed_at = 0;
    unsigned long long updated_at = 0;
    read_state(package, old_version, sizeof(old_version), old_hash, sizeof(old_hash),
               old_file, sizeof(old_file), &installed_at, &updated_at);
    char new_file[512];
    if (!shortcut_target(package, installed_file, new_file, sizeof(new_file)))
        new_file[0] = '\0';
    preserve_hbmenu_star(old_file, new_file);
    const unsigned long long now = (unsigned long long)time(NULL);
    if (!installed_at)
        installed_at = now;
    updated_at = now;
    write_state(package, hash, installed_file, installed_at, updated_at);
    invalidate_visible();
    update_shortcut(package, installed_file);
    if (strcmp(package->version, "latest") == 0 ||
        strcmp(package->version, "test") == 0 ||
        strcmp(package->version, "unknown") == 0)
        set_status("Installed %s", package->title);
    else
        set_status("Installed %s %s", package->title, package->version);
    install_redraw();
}

static void prune_empty_parents(char* path) {
    char* slash = strrchr(path, '/');
    while (slash) {
        *slash = '\0';
        if (strcmp(path, "sdmc:") == 0 || strcmp(path, "sdmc:/") == 0 ||
            strcmp(path, "sdmc:/switch") == 0 || strcmp(path, "sdmc:/brewstore") == 0)
            break;
        if (rmdir(path) != 0)
            break;
        slash = strrchr(path, '/');
    }
}

static int remove_file_list(const HbPackage* package) {
    char list_path[512];
    snprintf(list_path, sizeof(list_path), INSTALLED_DIR "/%s.files", package->name);
    FILE* list = fopen(list_path, "rb");
    if (!list)
        return 0;

    char line[1024];
    int total = 0;
    while (fgets(line, sizeof(line), list)) {
        line[strcspn(line, "\r\n")] = '\0';
        if (strncmp(line, "sdmc:/", 6) == 0 && !strstr(line, "..") &&
            strcmp(line, "sdmc:/") != 0)
            total++;
    }
    rewind(list);
    draw_uninstall_progress(g_ui, package->title, 0, total, NULL);
    int done = 0;
    int stride = total / 50;
    if (stride < 1)
        stride = 1;
    while (fgets(line, sizeof(line), list)) {
        line[strcspn(line, "\r\n")] = '\0';
        if (strncmp(line, "sdmc:/", 6) != 0 || strstr(line, "..") ||
            strcmp(line, "sdmc:/") == 0)
            continue;
        char path[1024];
        snprintf(path, sizeof(path), "%s", line);
        if (remove(path) != 0 && errno != ENOENT) {
            fclose(list);
            return -1;
        }
        prune_empty_parents(path);
        done++;
        if (done == total || done % stride == 0)
            draw_uninstall_progress(g_ui, package->title, done, total, line);
    }
    fclose(list);
    remove(list_path);
    return 1;
}

static void remove_package(const HbPackage* package) {
    char installed_version[64];
    char installed_hash[65];
    char installed_file[512];
    const bool has_state = read_state(package, installed_version, sizeof(installed_version),
                                      installed_hash, sizeof(installed_hash),
                                      installed_file, sizeof(installed_file),
                                      NULL, NULL);
    const int list_result = remove_file_list(package);
    if (list_result < 0) {
        set_status("Could not remove %s; close it and retry.", package->title);
        draw_shop(g_ui);
        return;
    }
    if (list_result > 0) {
        /* All files written by the last install were listed. */
    } else if (has_state && installed_file[0]) {
        remove(installed_file);
    } else if (strcmp(package->target, "sdmc:/") == 0 && package->marker[0]) {
        char marker[512];
        snprintf(marker, sizeof(marker), "%s", package->marker);
        if (access(marker, F_OK) == 0 && remove(marker) != 0) {
            set_status("Could not remove %s.", package->title);
            draw_shop(g_ui);
            return;
        }
        prune_empty_parents(marker);
    } else {
        set_status("No file list for %s; remove it manually.", package->title);
        draw_shop(g_ui);
        return;
    }
    char path[512];
    if (state_path(package, path, sizeof(path)) == 0)
        remove(path);
    invalidate_visible();
    shortcuts_remove(SHORTCUTS_PATH, package->name);
    clamp_selection();
    set_status("Removed %s.", package->title);
    draw_shop(g_ui);
}

typedef struct {
    HbPackage* package;
    char cache[512];
    char hash[65];
    char asset_name[128];
    char error[128];
    bool ok;
} PreparedPackage;

static bool batch_prepare(PreparedPackage* prepared, HbPackage* package, int index) {
    memset(prepared, 0, sizeof(*prepared));
    prepared->package = package;
    char url[1024] = "";
    char error[128] = {0};
    unsigned long long asset_size = 0;
    if (package->download_url[0]) {
        snprintf(url, sizeof(url), "%s", package->download_url);
        snprintf(prepared->asset_name, sizeof(prepared->asset_name), "%s", package->asset);
        if (package->size_known)
            asset_size = package->size;
    } else if (package->release[0] || is_plain_github_repo(package->url)) {
        if (release_latest_asset(package->url, package->asset, CACHE_DIR "/release.json",
                                 url, sizeof(url), prepared->asset_name,
                                 sizeof(prepared->asset_name), &asset_size,
                                 NULL, 0, error, sizeof(error)) != 0) {
            snprintf(prepared->error, sizeof(prepared->error), "%s", error);
            return false;
        }
    } else if (package->asset[0]) {
        snprintf(url, sizeof(url), "%s", package->url);
        snprintf(prepared->asset_name, sizeof(prepared->asset_name), "%s", package->asset);
    } else if (http_github_archive_url(package->url, package->branch, url, sizeof(url)) != 0) {
        snprintf(prepared->error, sizeof(prepared->error), "cannot resolve URL");
        return false;
    } else {
        snprintf(prepared->asset_name, sizeof(prepared->asset_name), "package.zip");
    }

    snprintf(prepared->cache, sizeof(prepared->cache), CACHE_DIR "/batch-%d", index);
    DownloadJob job;
    memset(&job, 0, sizeof(job));
    snprintf(job.url, sizeof(job.url), "%s", url);
    snprintf(job.cache, sizeof(job.cache), "%s", prepared->cache);
    job.expected_total = asset_size;
    job.size_limit = asset_size;
    job.title = package->title;
    const int result = run_download(&job, g_pad);
    if (result == -2 || job.cancel) {
        g_batch_stop = true;
        g_ignore_b = true;
        snprintf(prepared->error, sizeof(prepared->error), "cancelled");
        return false;
    }
    if (result != 0) {
        snprintf(prepared->error, sizeof(prepared->error), "%.127s",
                 job.error[0] ? job.error : "download failed");
        return false;
    }
    snprintf(prepared->hash, sizeof(prepared->hash), "%s", job.hash);
    const char* expected_sha = package->sha256;
    if (!g_settings.ignore_sha && expected_sha[0] &&
        strcasecmp(prepared->hash, expected_sha) != 0) {
        snprintf(prepared->error, sizeof(prepared->error), "SHA-256 mismatch");
        remove(prepared->cache);
        return false;
    }
    prepared->ok = true;
    return true;
}

static bool batch_install(const PreparedPackage* prepared) {
    HbPackage* package = prepared->package;
    if (strcmp(package->target, "sdmc:/") == 0 && !prepared->asset_name[0]) {
        set_status("The SD card root target needs a release asset.");
        return false;
    }
    if (strcmp(package->target, "sdmc:/switch") == 0 &&
        (!prepared->asset_name[0] || archive_is_archive(prepared->asset_name))) {
        set_status("sdmc:/switch needs a single non-archive asset.");
        return false;
    }
    draw_install_progress(g_ui, package->title, "Preparing install", 10);
    if (strcmp(package->target, "sdmc:/") != 0 && path_mkdirs(package->target) != 0) {
        set_status("Could not prepare %s.", package->title);
        return false;
    }
    draw_install_progress(g_ui, package->title,
                          prepared->asset_name[0] &&
                                  !archive_is_archive(prepared->asset_name)
                              ? "Writing application" : "Extracting files", 45);
    char installed_file[512] = "";
    char error[256] = {0};
    ExtractCapture capture;
    memset(&capture, 0, sizeof(capture));
    if (prepared->asset_name[0]) {
        if (archive_is_archive(prepared->asset_name)) {
            capture_begin(&capture, package);
            if (archive_extract(prepared->cache, prepared->asset_name, package->target, package->strip_prefix, error, sizeof(error),
                        extraction_progress, &capture) != 0) {
                capture_cancel(&capture);
                set_status("Extract failed: %s", error);
                return false;
            }
        } else {
            snprintf(installed_file, sizeof(installed_file),
                     package->target[strlen(package->target) - 1] == '/' ? "%.255s%.127s"
                                                                        : "%.255s/%.127s",
                     package->target, prepared->asset_name);
            if (path_copy_file(prepared->cache, installed_file) != 0) {
                set_status("Could not write %s.", package->title);
                return false;
            }
            capture_single(package, installed_file);
        }
    } else {
        capture_begin(&capture, package);
        if (archive_extract(prepared->cache, prepared->asset_name, package->target,
                        package->strip_prefix, error, sizeof(error), extraction_progress, &capture) != 0) {
            capture_cancel(&capture);
            set_status("Extract failed: %s", error);
            return false;
        }
    }
    if (!install_complete(package)) {
        capture_cancel(&capture);
        set_status("Install incomplete: expected file was not written.");
        return false;
    }
    capture_finish(&capture);
    draw_install_progress(g_ui, package->title, "Finishing", 90);
    char old_version[64];
    char old_hash[65];
    char old_file[512];
    unsigned long long installed_at = 0;
    unsigned long long updated_at = 0;
    read_state(package, old_version, sizeof(old_version), old_hash, sizeof(old_hash),
               old_file, sizeof(old_file), &installed_at, &updated_at);
    char new_file[512];
    if (!shortcut_target(package, installed_file, new_file, sizeof(new_file)))
        new_file[0] = '\0';
    preserve_hbmenu_star(old_file, new_file);
    const unsigned long long now = (unsigned long long)time(NULL);
    if (!installed_at)
        installed_at = now;
    write_state(package, prepared->hash, installed_file, installed_at, now);
    invalidate_visible();
    update_shortcut(package, installed_file);
    return true;
}

static void install_selected_or_current(HbPackage* current, PadState* pad) {
    (void)pad;
    if (!g_marked_count) {
        install_package(current, g_pad, NULL);
        return;
    }
    int total = 0;
    for (int i = 0; i < g_manifest.count; i++) {
        if (is_marked(&g_manifest.items[i]))
            total++;
    }
    if (!total)
        return;
    PreparedPackage* prepared = calloc((size_t)total, sizeof(*prepared));
    if (!prepared) {
        set_status("Out of memory preparing batch install.");
        return;
    }

    g_batch_active = true;
    g_batch_stop = false;
    g_batch_total = total;
    g_batch_completed = 0;
    int number = 0;
    for (int i = 0; i < g_manifest.count && !g_batch_stop; i++) {
        if (!is_marked(&g_manifest.items[i]))
            continue;
        g_batch_index = ++number;
        if (!batch_prepare(&prepared[number - 1], &g_manifest.items[i], number)) {
            prepared[number - 1].ok = false;
        }
    }

    number = 0;
    for (int i = 0; i < total && !g_batch_stop; i++) {
        g_batch_index = ++number;
        g_batch_completed = number - 1;
        if (prepared[i].ok)
            batch_install(&prepared[i]);
        g_batch_completed = number;
    }
    if (!g_batch_stop && total) {
        g_batch_completed = total;
        draw_install_progress(g_ui, "All packages", "Finishing", 100);
    }

    g_batch_active = false;
    g_batch_stop = false;
    g_batch_index = 0;
    g_batch_total = 0;
    g_batch_completed = 0;
    clear_marks();
    free(prepared);
    clamp_selection();
    draw_shop(g_ui);
}

static void remove_selected_or_current(HbPackage* current) {
    if (!g_marked_count) {
        if (is_installed(current)) {
            remove_package(current);
            clamp_selection();
        } else {
            set_status("%s is not installed.", current->title);
            draw_shop(g_ui);
        }
        return;
    }
    for (int i = 0; i < g_manifest.count; i++) {
        if (is_marked(&g_manifest.items[i]) && is_installed(&g_manifest.items[i]))
            remove_package(&g_manifest.items[i]);
    }
    clear_marks();
    clamp_selection();
    draw_shop(g_ui);
}

static void draw_context(Ui* ui) {
    if (!ui)
        return;
    char sort_option[64];
    snprintf(sort_option, sizeof(sort_option), "Sort order: %s", sort_name());
    const char* options[] = {
        "Install / Update", "Remove", "Refresh catalogs", sort_option
    };
    ui_begin(ui);
    ui_clear(ui, COLOR_BACKGROUND);
    draw_shop_content(ui);
    ui_rect(ui, 0, 0, UI_WIDTH, UI_HEIGHT, RGBA8(0, 0, 0, 170));

    const int panel_w = 480;
    const int panel_h = 300;
    const int panel_x = (UI_WIDTH - panel_w) / 2;
    const int panel_y = (UI_HEIGHT - panel_h) / 2;
    ui_rect(ui, panel_x, panel_y, panel_w, panel_h, COLOR_PANEL);
    ui_rect(ui, panel_x, panel_y, panel_w, 3, COLOR_ACCENT);
    const int title_width = ui_text_width(ui, "Actions", 22);
    ui_text(ui, panel_x + (panel_w - title_width) / 2, panel_y + 22,
            "Actions", 22, COLOR_TEXT);
    for (int i = 0; i < 4; i++) {
        const int y = panel_y + 72 + i * 48;
        if (i == g_context_selected)
            ui_rect(ui, panel_x + 12, y - 8, panel_w - 24, 40, COLOR_CARD_HOVER);
        const int option_width = ui_text_width(ui, options[i], 17);
        ui_text(ui, panel_x + (panel_w - option_width) / 2, y + 4, options[i], 17,
                i == g_context_selected ? COLOR_TEXT : COLOR_MUTED);
    }
    const char* footer = "A select   B close";
    const int footer_width = ui_text_width(ui, footer, 12);
    ui_text(ui, panel_x + (panel_w - footer_width) / 2, panel_y + panel_h - 24,
            footer, 12, COLOR_MUTED);
    ui_end(ui);
}

static void context_action(void) {
    HbPackage* package = visible_package(g_selected);
    if (!package)
        return;
    switch (g_context_selected) {
        case 0:
            g_context_menu = false;
            install_selected_or_current(package, g_pad);
            return;
        case 1:
            g_context_menu = false;
            remove_selected_or_current(package);
            return;
        case 2:
            g_context_menu = false;
            refresh_catalogs(true);
            draw_shop(g_ui);
            return;
        case 3:
            if (g_sort_reverse) {
                g_sort_reverse = false;
                g_sort = (g_sort + 1) % 3;
            } else {
                g_sort_reverse = true;
            }
            invalidate_visible();
            g_selected = 0;
            g_scroll_page = 0;
            clamp_selection();
            draw_context(g_ui);
            return;
        default:
            return;
    }
}

static bool append_package(HbPackage package) {
    if (g_manifest.count >= HB_PACKAGES_SANITY_MAX) {
        snprintf(g_manifest_error, sizeof(g_manifest_error),
                 "catalogue exceeds %d packages", HB_PACKAGES_SANITY_MAX);
        return false;
    }
    for (int existing = 0; existing < g_manifest.count; existing++) {
        if (strcasecmp(g_manifest.items[existing].name, package.name) == 0)
            return true;
    }
    if (!package.target_explicit) {
        const size_t path_length = strlen(g_settings.default_install_path);
        const size_t name_length = strlen(package.name);
        if (path_length + 1 + name_length + 1 <= sizeof(package.target)) {
            memcpy(package.target, g_settings.default_install_path, path_length);
            package.target[path_length] = '/';
            memcpy(package.target + path_length + 1, package.name,
                   name_length + 1);
        } else {
            snprintf(package.target, sizeof(package.target),
                     "sdmc:/switch/%.63s", package.name);
        }
    }
    if (strcmp(package.author, "Unknown") == 0) {
        char owner[128];
        char repo[128];
        if (github_repo_parts(package.url, owner, sizeof(owner),
                              repo, sizeof(repo)) == 0)
            snprintf(package.author, sizeof(package.author), "%.63s", owner);
    }
    if (hb_list_push(&g_manifest, &package) != 0) {
        snprintf(g_manifest_error, sizeof(g_manifest_error),
                 "out of memory adding %s", package.name);
        return false;
    }
    return true;
}

static void append_catalog(const HbManifest* source) {
    for (int i = 0; i < source->count; i++)
        append_package(source->items[i]);
}


static void repo_cache_path(const char* url, char* out, size_t out_size) {
    u32 hash = 2166136261u;
    for (const unsigned char* p = (const unsigned char*)url; *p; p++) {
        hash ^= *p;
        hash *= 16777619u;
    }
    snprintf(out, out_size, REPO_CACHE_DIR "/%08x.json", hash);
}

static int validate_cache_file(const char* path, char* err, size_t err_size) {
    HbPackageList list = {0};
    CatalogStats stats;
    const int catalog_result = catalog_load(path, &list, &stats, err, err_size);
    hb_list_free(&list);
    if (catalog_result == 0)
        return 0;
    if (catalog_result == -1)
        return -1;

    HbManifest* manifest = malloc(sizeof(*manifest));
    if (!manifest)
        return -1;
    const int result = manifest_load(path, manifest, err, err_size);
    free(manifest);
    return result == 0 ? 0 : -1;
}

static int fill_cache(const char* url, const char* cache, bool fetch,
                      char* err, size_t err_size) {
    char validation_error[256] = "";
    const bool cache_exists = access(cache, F_OK) == 0;
    const bool have_old = cache_exists &&
                          validate_cache_file(cache, validation_error,
                                              sizeof(validation_error)) == 0;
    if (cache_exists && !have_old) {
        remove(cache);
        g_cache_corrupt = true;
        if (!fetch)
            snprintf(err, err_size, "catalog cache was corrupt");
    }
    if (!fetch && have_old)
        return 0;
    if (!g_network_ready)
        return have_old ? 0 : -1;

    char temporary[700];
    snprintf(temporary, sizeof(temporary), "%s.new", cache);
    char hash[65];
    if (http_download(url, temporary, hash, err, err_size) != 0) {
        remove(temporary);
        return have_old ? 0 : -1;
    }
    if (validate_cache_file(temporary, err, err_size) != 0) {
        remove(temporary);
        return have_old ? 0 : -1;
    }
    remove(cache);
    if (rename(temporary, cache) != 0) {
        remove(temporary);
        return have_old ? 0 : -1;
    }
    return 0;
}

static int resolve_include(const char* base, const char* include,
                           char* out, size_t out_size) {
    if (strncmp(include, "https://", 8) == 0 || strncmp(include, "sdmc:", 5) == 0) {
        if (strlen(include) >= out_size)
            return -1;
        strcpy(out, include);
        return 0;
    }
    const char* slash = strrchr(base, '/');
    if (!slash)
        return -1;
    const int written = snprintf(out, out_size, "%.*s%s",
                                 (int)(slash - base + 1), base, include);
    return written > 0 && (size_t)written < out_size ? 0 : -1;
}

static bool load_catalog_file(const char* path, const char* base, bool fetch,
                              int depth, int* failures, char* err, size_t err_size) {
    if (depth > 8) {
        snprintf(err, err_size, "include depth exceeded");
        return false;
    }
    HbPackageList catalog_list = {0};
    CatalogStats catalog_stats;
    const int catalog_result = catalog_load(path, &catalog_list, &catalog_stats,
                                            err, err_size);
    if (catalog_result == 0) {
        for (int i = 0; i < catalog_list.count; i++)
            append_package(catalog_list.items[i]);
        if (g_catalog_log)
            fprintf(g_catalog_log, "OK %s catalogue packages=%d skipped=%d\n",
                    path, catalog_stats.packages, catalog_stats.skipped);
        hb_list_free(&catalog_list);
        return true;
    }
    hb_list_free(&catalog_list);
    if (catalog_result == -1) {
        if (g_catalog_log)
            fprintf(g_catalog_log, "FAIL %s: %s\n", path, err);
        (*failures)++;
        return false;
    }
    /* HbManifest is large; keep it off the stack so the include recursion
     * does not overflow it as the catalog grows. */
    HbManifest* manifest = malloc(sizeof(*manifest));
    if (!manifest) {
        (*failures)++;
        return false;
    }
    if (manifest_load(path, manifest, err, err_size) != 0) {
        if (g_catalog_log)
            fprintf(g_catalog_log, "FAIL %s: %s\n", path, err);
        (*failures)++;
        free(manifest);
        return false;
    }
    if (g_catalog_log)
        fprintf(g_catalog_log, "OK %s packages=%d includes=%d\n",
                path, manifest->count, manifest->include_count);
    append_catalog(manifest);

    for (int i = 0; i < manifest->include_count; i++) {
        char included[640];
        if (resolve_include(base, manifest->includes[i], included, sizeof(included)) != 0) {
            if (g_catalog_log)
                fprintf(g_catalog_log, "FAIL resolve %s\n", manifest->includes[i]);
            (*failures)++;
            continue;
        }
        if (strncmp(included, "https://", 8) == 0) {
            char cache[640];
            repo_cache_path(included, cache, sizeof(cache));
            if (fill_cache(included, cache, fetch, err, err_size) != 0) {
                if (g_catalog_log)
                    fprintf(g_catalog_log, "FAIL download %s: %s\n",
                            included, err);
                (*failures)++;
                continue;
            }
            load_catalog_file(cache, included, fetch, depth + 1, failures,
                              err, err_size);
        } else {
            if (access(included, F_OK) != 0) {
                (*failures)++;
                continue;
            }
            load_catalog_file(included, included, fetch, depth + 1, failures,
                              err, err_size);
        }
    }
    free(manifest);
    return true;
}

static void refresh_catalogs(bool fetch) {
    /* g_manifest.items may be reallocated only during this refresh. No UI/state
     * pointer may survive across it; g_visible and package_state use indices
     * or are rebuilt after loading. */
    icon_prefetch_stop();
    char error[256] = {0};
    int failures = 0;
    hb_list_free(&g_manifest);
    g_manifest_error[0] = '\0';
    g_cache_corrupt = false;
    invalidate_visible();
    path_mkdirs(REPO_CACHE_DIR);
    g_catalog_log = fopen(CACHE_DIR "/catalog.log", "w");

    if (access(MANIFEST_PATH, F_OK) == 0 &&
        !load_catalog_file(MANIFEST_PATH, MANIFEST_PATH, false, 0, &failures,
                           error, sizeof(error)))
        snprintf(g_manifest_error, sizeof(g_manifest_error), "%s", error);

    if (access(TEST_REPO_PATH, F_OK) == 0) {
        HbManifest* test = malloc(sizeof(*test));
        if (test && manifest_load(TEST_REPO_PATH, test, error, sizeof(error)) == 0) {
            for (int i = 0; i < test->count; i++) {
                test->items[i].target_explicit = false;
                test->items[i].readme_fallback = true;
                if (strcmp(test->items[i].version, "test") == 0)
                    snprintf(test->items[i].version, sizeof(test->items[i].version), "latest");
                if (strcmp(test->items[i].author, "Git") == 0 ||
                    strcmp(test->items[i].author, "Miella Selene / Xayah") == 0) {
                    char owner[128];
                    char repo[128];
                    if (github_repo_parts(test->items[i].url, owner, sizeof(owner),
                                          repo, sizeof(repo)) == 0)
                        snprintf(test->items[i].author, sizeof(test->items[i].author),
                                 "%.63s", owner);
                }
            }
            append_catalog(test);
        } else {
            failures++;
        }
        free(test);
    }

    for (int i = 0; i < g_repos.count; i++) {
        if (!g_repos.enabled[i])
            continue;
        char cache[640];
        repo_cache_path(g_repos.urls[i], cache, sizeof(cache));
        if (fill_cache(g_repos.urls[i], cache, fetch, error, sizeof(error)) != 0) {
            if (g_catalog_log)
                fprintf(g_catalog_log, "FAIL repo %s: %s\n",
                        g_repos.urls[i], error);
            failures++;
            continue;
        }
        load_catalog_file(cache, g_repos.urls[i], fetch, 0, &failures,
                          error, sizeof(error));
    }

    if (g_catalog_log) {
        fprintf(g_catalog_log, "TOTAL packages=%d failures=%d\n",
                g_manifest.count, failures);
        fclose(g_catalog_log);
        g_catalog_log = NULL;
    }

    if (ensure_installed_capacity(g_manifest.count) != 0)
        snprintf(g_manifest_error, sizeof(g_manifest_error), "out of memory for installed state");
    else
        ensure_visible_capacity(g_manifest.count);
    clamp_selection();

    if (!g_manifest.count) {
        if (g_cache_corrupt)
            set_status("No packages: catalog cache was corrupt; reconnect to refresh.");
        else if (g_manifest_error[0])
            set_status("No packages: %s", g_manifest_error);
        else if (!g_repos.count)
            set_status("No repositories. Press R to add one.");
        else
            set_status("No packages loaded. Press R to add a catalog.");
    } else {
        if (g_cache_corrupt)
            set_status("Loaded %d package%s; catalog cache was corrupt.",
                       g_manifest.count, g_manifest.count == 1 ? "" : "s");
        else
            set_status("Loaded %d package%s%s", g_manifest.count,
                       g_manifest.count == 1 ? "" : "s",
                       failures ? " (some catalogs unavailable)" : "");
    }
}

static bool prompt_text(const char* header, const char* guide, const char* initial,
                        char* out, size_t out_size) {
    SwkbdConfig config;
    if (R_FAILED(swkbdCreate(&config, 0)))
        return false;
    swkbdConfigMakePresetDefault(&config);
    swkbdConfigSetHeaderText(&config, header);
    swkbdConfigSetGuideText(&config, guide);
    swkbdConfigSetInitialText(&config, initial);
    swkbdConfigSetStringLenMax(&config, 511);

    char text[512] = {0};
    const Result rc = swkbdShow(&config, text, sizeof(text));
    swkbdClose(&config);
    if (R_FAILED(rc))
        return false;

    HidKeyboardState keyboard;
    memset(&keyboard, 0, sizeof(keyboard));
    hidGetKeyboardStates(&keyboard, 1);
    memcpy(g_previous_keys, keyboard.keys, sizeof(g_previous_keys));

    char* start = text;
    while (*start == ' ' || *start == '\t' || *start == '\r' || *start == '\n')
        start++;
    char* end = start + strlen(start);
    while (end > start && (end[-1] == ' ' || end[-1] == '\t' ||
                          end[-1] == '\r' || end[-1] == '\n'))
        end--;
    *end = '\0';
    if (strlen(start) >= out_size)
        return false;
    strcpy(out, start);
    return true;
}

static void edit_search(void) {
    char query[sizeof(g_search)];
    if (!prompt_text("Search apps", "App name or title; empty clears",
                     g_search, query, sizeof(query))) {
        set_status("Search cancelled.");
        draw_shop(g_ui);
        return;
    }
    snprintf(g_search, sizeof(g_search), "%s", query);
    invalidate_visible();
    g_selected = 0;
    g_scroll_page = 0;
    clamp_selection();
    if (g_search[0])
        set_status("Search: %s", g_search);
    else
        set_status("Search cleared.");
    draw_shop(g_ui);
}

static bool prompt_repository(char* out, size_t out_size) {
    return prompt_text("Add repository", "URL of a packages.json catalog",
                       "https://", out, out_size);
}

static bool prompt_git_repo(char* out, size_t out_size) {
    return prompt_text("Test a git repository", "GitHub repo or direct archive URL",
                       "https://github.com/", out, out_size);
}

static bool url_asset_name(const char* url, char* out, size_t out_size) {
    const char* slash = strrchr(url, '/');
    if (!slash || !slash[1])
        return false;
    const char* end = strchr(slash + 1, '?');
    const size_t length = end ? (size_t)(end - slash - 1) : strlen(slash + 1);
    if (!length || length >= out_size)
        return false;
    for (size_t i = 0; i < length; i++) {
        const char c = slash[1 + i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_'))
            return false;
    }
    memcpy(out, slash + 1, length);
    out[length] = '\0';
    return true;
}

static bool ends_with_ci(const char* text, const char* suffix) {
    const size_t text_length = strlen(text);
    const size_t suffix_length = strlen(suffix);
    return suffix_length <= text_length &&
           strcasecmp(text + text_length - suffix_length, suffix) == 0;
}

static bool write_test_package(const char* url, char* err, size_t err_size) {
    if (strchr(url, '"') || strchr(url, '\\') || strchr(url, '\n') || strchr(url, '\r')) {
        snprintf(err, err_size, "URL contains unsupported characters");
        return false;
    }
    if (strncmp(url, "https://", 8) != 0) {
        snprintf(err, err_size, "URL must use https");
        return false;
    }

    const bool direct = strstr(url, "/releases/download/") ||
                        strstr(url, "/archive/") ||
                        strstr(url, "/download/") ||
                        strstr(url, ".zip") ||
                        strstr(url, ".tar") ||
                        strstr(url, ".tgz") ||
                        strstr(url, ".txz") ||
                        strstr(url, ".nro");
    char name[64];
    char asset[128] = "";
    if (direct) {
        if (!url_asset_name(url, asset, sizeof(asset))) {
            snprintf(err, err_size, "cannot derive an asset name from the URL");
            return false;
        }
        snprintf(name, sizeof(name), "%.*s",
                 (int)(strcspn(asset, ".") ? strcspn(asset, ".") : strlen(asset)), asset);
        if (!name[0] || strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
            snprintf(name, sizeof(name), "test-package");
    } else if (github_repo_name(url, name, sizeof(name)) != 0) {
        snprintf(err, err_size, "not a GitHub repository or direct asset URL");
        return false;
    }

    char author[64] = "Unknown";
    char owner[128];
    char repo[128];
    if (github_repo_parts(url, owner, sizeof(owner), repo, sizeof(repo)) == 0)
        snprintf(author, sizeof(author), "%.63s", owner);

    const char* release = direct ? "" : "latest";
    char temporary[640];
    snprintf(temporary, sizeof(temporary), TEST_REPO_PATH ".tmp");
    FILE* file = fopen(temporary, "wb");
    if (!file) {
        snprintf(err, err_size, "cannot create test-repo.json");
        return false;
    }
    fprintf(file,
            "{\n"
            "  \"packages\": [\n"
            "    {\n"
            "      \"name\": \"%s\",\n"
            "      \"title\": \"%s\",\n"
            "      \"author\": \"%s\",\n"
            "      \"description\": \"\",\n"
            "      \"version\": \"latest\",\n"
            "      \"url\": \"%s\",\n"
            "      \"release\": \"%s\",\n"
            "      \"asset\": \"%s\",\n"
            "      \"branch\": \"main\",\n"
            "      \"sha256\": \"\"\n"
            "    }\n"
            "  ]\n"
            "}\n",
            name, name, author, url, release, asset);
    if (fclose(file) != 0) {
        remove(temporary);
        snprintf(err, err_size, "cannot write test-repo.json");
        return false;
    }
    char readme_cache[512];
    snprintf(readme_cache, sizeof(readme_cache), README_DIR "/%s.txt", name);
    remove(readme_cache);
    remove(TEST_REPO_PATH);
    if (rename(temporary, TEST_REPO_PATH) != 0) {
        remove(temporary);
        snprintf(err, err_size, "cannot replace test-repo.json");
        return false;
    }
    return true;
}

static void redraw_after_prompt(void) {
    if (g_return_to_repos)
        draw_repo_manager(g_ui);
    else
        draw_shop(g_ui);
}

static void add_test_repo(void) {
    char url[512];
    if (!prompt_git_repo(url, sizeof(url))) {
        set_status("Git repository URL was cancelled or invalid.");
        redraw_after_prompt();
        return;
    }
    char error[256] = {0};
    if (!write_test_package(url, error, sizeof(error))) {
        set_status("Test repository failed: %s", error);
        redraw_after_prompt();
        return;
    }
    g_filter = FILTER_ALL;
    refresh_catalogs(false);
    for (int i = 0; i < visible_count(); i++) {
        HbPackage* package = visible_package(i);
        if (package && strcmp(package->url, url) == 0) {
            g_selected = i;
            g_scroll_page = i / view_page_size();
            break;
        }
    }
    set_status("Added test repository. Press A to install.");
    redraw_after_prompt();
}

static void add_repository(void) {
    char url[512];
    if (!prompt_repository(url, sizeof(url))) {
        set_status("Repository URL was cancelled or invalid.");
        redraw_after_prompt();
        return;
    }
    const int added = repos_add(&g_repos, url);
    if (added < 0) {
        set_status("Repository URL is invalid or the list is full.");
        redraw_after_prompt();
        return;
    }
    if (added == 1) {
        set_status("Repository is already in the list.");
        redraw_after_prompt();
        return;
    }
    char error[256] = {0};
    if (repos_save(REPOS_PATH, &g_repos, error, sizeof(error)) != 0) {
        set_status("Could not save repos.json: %s", error);
        redraw_after_prompt();
        return;
    }
    set_status("Added repository; loading catalog ...");
    redraw_after_prompt();
    refresh_catalogs(true);
    redraw_after_prompt();
}

static bool keyboard_edge(const HidKeyboardState* state, HidKeyboardKey key) {
    const bool now = hidKeyboardStateGetKey(state, key);
    const bool before = (g_previous_keys[key / 64] & (1ull << (key & 63))) != 0;
    return now && !before;
}

static void move_selection(int delta) {
    const int count = visible_count();
    if (!count)
        return;
    int next = g_selected + delta;
    if (next < 0)
        next = 0;
    if (next >= count)
        next = count - 1;
    g_selected = next;
    g_scroll_page = g_selected / view_page_size();
}

static void replace_file_prefix(const char* path, const char* old_prefix,
                               const char* new_prefix) {
    FILE* input = fopen(path, "rb");
    if (!input)
        return;
    char temporary[768];
    snprintf(temporary, sizeof(temporary), "%.760s.tmp", path);
    FILE* output = fopen(temporary, "wb");
    if (!output) {
        fclose(input);
        return;
    }
    char line[2048];
    while (fgets(line, sizeof(line), input)) {
        char* hit = strstr(line, old_prefix);
        if (hit) {
            const size_t before = (size_t)(hit - line);
            fwrite(line, 1, before, output);
            fputs(new_prefix, output);
            fputs(hit + strlen(old_prefix), output);
        } else {
            fputs(line, output);
        }
    }
    fclose(input);
    if (fclose(output) != 0) {
        remove(temporary);
        return;
    }
    remove(path);
    if (rename(temporary, path) != 0)
        remove(temporary);
}

static void rewrite_installed_paths(void) {
    DIR* dir = opendir(INSTALLED_DIR);
    if (!dir)
        return;
    struct dirent* entry;
    while ((entry = readdir(dir))) {
        if (entry->d_name[0] == '.')
            continue;
        char path[1024];
        snprintf(path, sizeof(path), INSTALLED_DIR "/%s", entry->d_name);
        replace_file_prefix(path, "sdmc:/switch/brewstore/apps/", "sdmc:/brewstore/apps/");
    }
    closedir(dir);
}

static bool move_legacy_entry(const char* name) {
    char old_path[512];
    char new_path[512];
    snprintf(old_path, sizeof(old_path), "%s/%s", LEGACY_BREWSTORE_ROOT, name);
    snprintf(new_path, sizeof(new_path), "%s/%s", BREWSTORE_ROOT, name);
    if (access(new_path, F_OK) == 0 || access(old_path, F_OK) != 0)
        return false;
    return rename(old_path, new_path) == 0;
}

static void migrate_brewstore_root(void) {
    if (access(LEGACY_BREWSTORE_ROOT, F_OK) != 0)
        return;

    bool moved = false;
    bool apps_moved = false;
    if (access(BREWSTORE_ROOT, F_OK) != 0) {
        if (rename(LEGACY_BREWSTORE_ROOT, BREWSTORE_ROOT) != 0)
            return;
        moved = true;
        apps_moved = true;
    } else {
        const char* entries[] = {
            "packages.json", "test-repo.json", "repos.json", "settings.json",
            "cache", "installed", "apps"
        };
        for (size_t i = 0; i < sizeof(entries) / sizeof(entries[0]); i++) {
            if (move_legacy_entry(entries[i])) {
                moved = true;
                if (strcmp(entries[i], "apps") == 0)
                    apps_moved = true;
            }
        }
    }
    if (!moved || !apps_moved)
        return;
    rewrite_installed_paths();
    replace_file_prefix(SHORTCUTS_PATH, "sdmc:/switch/brewstore/apps/",
                        "sdmc:/brewstore/apps/");
}

static void migrate_default_repository(void) {
    int new_index = -1;
    for (int i = 0; i < g_repos.count; i++)
        if (strcmp(g_repos.urls[i], DEFAULT_REPO_URL) == 0) {
            new_index = i;
            break;
        }
    bool changed = false;
    for (int i = 0; i < g_repos.count; ) {
        if (strcmp(g_repos.urls[i], LEGACY_DEFAULT_REPO_URL) != 0) {
            i++;
            continue;
        }
        if (new_index >= 0 && new_index != i) {
            g_repos.enabled[new_index] = g_repos.enabled[new_index] || g_repos.enabled[i];
            repos_remove(&g_repos, i);
            if (i < new_index)
                new_index--;
        } else {
            snprintf(g_repos.urls[i], sizeof(g_repos.urls[i]), "%s", DEFAULT_REPO_URL);
            new_index = i;
        }
        changed = true;
        i++;
    }
    if (changed) {
        char error[128] = {0};
        if (repos_save(REPOS_PATH, &g_repos, error, sizeof(error)) != 0)
            snprintf(g_status, sizeof(g_status), "repos.json migration failed: %s", error);
    }
}

int main(void) {
    Ui ui;
    char error[256] = {0};
    if (ui_init(&ui, error, sizeof(error)) != 0)
        return 1;
    g_ui = &ui;
    draw_loading(&ui, "Starting...");

    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    PadState pad;
    padInitializeDefault(&pad);
    g_pad = &pad;

    migrate_brewstore_root();
    path_mkdirs(BREWSTORE_ROOT);
    path_mkdirs(CACHE_DIR);
    path_mkdirs(ICON_DIR);
    path_mkdirs(INSTALLED_DIR);
    path_mkdirs(REPO_CACHE_DIR);
    path_mkdirs(README_DIR);

    if (settings_load(SETTINGS_PATH, &g_settings, error, sizeof(error)) != 0)
        snprintf(g_status, sizeof(g_status), "settings.json: %s", error);
    g_list_view = g_settings.list_view != 0;
    path_mkdirs(g_settings.default_install_path);

    if (repos_load(REPOS_PATH, &g_repos, error, sizeof(error)) != 0) {
        snprintf(g_status, sizeof(g_status), "repos.json: %s", error);
    } else {
        migrate_default_repository();
        if (g_repos.count == 0) {
            repos_add(&g_repos, DEFAULT_REPO_URL);
            if (repos_save(REPOS_PATH, &g_repos, error, sizeof(error)) != 0)
                snprintf(g_status, sizeof(g_status), "repos.json: %s", error);
        }
    }

    g_network_ready = http_global_init(error, sizeof(error)) == 0;
    if (!g_network_ready)
        snprintf(g_status, sizeof(g_status), "Network init failed: %s", error);
    http_set_proxy_prefix(g_settings.download_proxy);

    hidInitializeKeyboard();
    draw_loading(&ui, "Updating catalogs...");
    refresh_catalogs(false);
    if (!g_manifest.count && g_repos.count > 0) {
        draw_loading(&ui, "Updating catalogs...");
        refresh_catalogs(true);
    }
    draw_shop(&ui);

    while (!g_quit && appletMainLoop()) {
        padUpdate(&pad);
        const u64 down = padGetButtonsDown(&pad);
        static u64 nav_held;
        static u64 nav_next_tick;
        const u64 nav_mask = HidNpadButton_Up | HidNpadButton_Down |
                             HidNpadButton_Left | HidNpadButton_Right;
        const u64 held_nav = padGetButtons(&pad) & nav_mask;
        const u64 now_tick = armGetSystemTick();
        u64 repeat_nav = 0;
        if (held_nav != nav_held) {
            nav_held = held_nav;
            if (held_nav)
                nav_next_tick = now_tick + armNsToTicks(400000000ULL);
        } else if (held_nav && now_tick >= nav_next_tick) {
            repeat_nav = held_nav;
            nav_next_tick = now_tick + armNsToTicks(200000000ULL);
        }
        const u64 nav = down | repeat_nav;

        HidKeyboardState keyboard;
        memset(&keyboard, 0, sizeof(keyboard));
        hidGetKeyboardStates(&keyboard, 1);
        const bool key_left = keyboard_edge(&keyboard, HidKeyboardKey_LeftArrow);
        const bool key_right = keyboard_edge(&keyboard, HidKeyboardKey_RightArrow);
        const bool key_up = keyboard_edge(&keyboard, HidKeyboardKey_UpArrow);
        const bool key_down = keyboard_edge(&keyboard, HidKeyboardKey_DownArrow);
        const bool key_accept = keyboard_edge(&keyboard, HidKeyboardKey_Return) ||
                                keyboard_edge(&keyboard, HidKeyboardKey_NumPadEnter);
        const bool key_remove = keyboard_edge(&keyboard, HidKeyboardKey_Delete);
        const bool key_refresh = keyboard_edge(&keyboard, HidKeyboardKey_X);
        const bool key_context = keyboard_edge(&keyboard, HidKeyboardKey_X);
        const bool key_view = keyboard_edge(&keyboard, HidKeyboardKey_V);
        const bool key_add = keyboard_edge(&keyboard, HidKeyboardKey_R);
        const bool key_test = keyboard_edge(&keyboard, HidKeyboardKey_L);
        const bool key_manager = keyboard_edge(&keyboard, HidKeyboardKey_M);
        const bool key_settings = keyboard_edge(&keyboard, HidKeyboardKey_S);
        const bool key_exit = keyboard_edge(&keyboard, HidKeyboardKey_Escape);

        if (g_confirm_action != CONFIRM_NONE) {
            bool changed = false;
            if ((down & HidNpadButton_Left) || key_left) {
                g_confirm_yes = true;
                changed = true;
            } else if ((down & HidNpadButton_Right) || key_right) {
                g_confirm_yes = false;
                changed = true;
            } else if ((down & HidNpadButton_A) || key_accept) {
                if (g_confirm_yes)
                    perform_confirm();
                else
                    g_confirm_action = CONFIRM_NONE;
            } else if ((down & HidNpadButton_B) || key_exit) {
                g_ignore_b = true;
                g_confirm_action = CONFIRM_NONE;
            }
            memcpy(g_previous_keys, keyboard.keys, sizeof(g_previous_keys));
            if (g_confirm_action == CONFIRM_NONE)
                draw_repo_manager(&ui);
            else if (changed)
                draw_confirm(&ui);
            continue;
        }

        if (g_context_menu) {
            bool changed = false;
            if ((down & HidNpadButton_Up) || key_up) {
                if (g_context_selected > 0)
                    g_context_selected--;
                changed = true;
            } else if ((down & HidNpadButton_Down) || key_down) {
                if (g_context_selected < 3)
                    g_context_selected++;
                changed = true;
            } else if ((down & HidNpadButton_A) || key_accept) {
                context_action();
            } else if ((down & HidNpadButton_B) || key_exit) {
                g_ignore_b = true;
                g_context_menu = false;
                draw_shop(&ui);
            }
            memcpy(g_previous_keys, keyboard.keys, sizeof(g_previous_keys));
            if (changed)
                draw_context(&ui);
            continue;
        }

        if (g_versions_screen) {
            bool changed = false;
            if ((down & HidNpadButton_B) || key_exit) {
                g_ignore_b = true;
                g_versions_screen = false;
                draw_shop(&ui);
            } else if (((nav & HidNpadButton_Up) || key_up) && g_version_selected > 0) {
                g_version_selected--;
                if (g_version_selected < g_version_scroll)
                    g_version_scroll = g_version_selected;
                changed = true;
            } else if (((nav & HidNpadButton_Down) || key_down) &&
                       g_version_selected + 1 < g_version_count) {
                g_version_selected++;
                if (g_version_selected >= g_version_scroll + VERSION_VISIBLE_ROWS)
                    g_version_scroll = g_version_selected - VERSION_VISIBLE_ROWS + 1;
                changed = true;
            } else if ((down & HidNpadButton_A) || key_accept) {
                ReleaseChoice choice = g_version_choices[g_version_selected];
                HbPackage chosen = *g_version_package;
                snprintf(chosen.version, sizeof(chosen.version), "%.31s", choice.tag);
                g_versions_screen = false;
                install_package(&chosen, &pad, &choice);
            }
            memcpy(g_previous_keys, keyboard.keys, sizeof(g_previous_keys));
            if (changed)
                draw_versions(&ui);
            continue;
        }

        if (g_settings_screen) {
            bool changed = false;
            if ((down & (HidNpadButton_B | HidNpadButton_ZL)) || key_exit) {
                g_ignore_b = true;
                g_settings_screen = false;
                draw_shop(&ui);
            } else if ((down & HidNpadButton_ZR) || key_manager) {
                g_settings_screen = false;
                g_repo_manager = true;
                g_repo_selected = 0;
                g_repo_scroll = 0;
                draw_repo_manager(&ui);
            } else if (((down & HidNpadButton_Up) || key_up) && g_setting_row > 0) {
                g_setting_row--;
                changed = true;
            } else if (((down & HidNpadButton_Down) || key_down) &&
                       g_setting_row < 4) {
                g_setting_row++;
                changed = true;
            } else if (g_setting_row == 2 &&
                       ((down & (HidNpadButton_Left | HidNpadButton_Right)) ||
                        key_left || key_right)) {
                cycle_button_theme((down & HidNpadButton_Left) || key_left ? -1 : 1);
            } else if (g_setting_row == 3 &&
                       ((down & (HidNpadButton_Left | HidNpadButton_Right)) ||
                        key_left || key_right)) {
                toggle_ignore_sha();
            } else if (g_setting_row == 4 &&
                       ((down & HidNpadButton_A) || key_accept)) {
                clear_catalog_cache();
            } else if ((down & HidNpadButton_A) || key_accept) {
                if (g_setting_row == 0)
                    edit_default_path();
                else if (g_setting_row == 1)
                    edit_proxy();
                else if (g_setting_row == 2)
                    cycle_button_theme(1);
                else
                    toggle_ignore_sha();
            }
            memcpy(g_previous_keys, keyboard.keys, sizeof(g_previous_keys));
            if (changed)
                draw_settings(&ui);
            continue;
        }

        if (g_repo_manager) {
            bool changed = false;
            if ((down & (HidNpadButton_B | HidNpadButton_ZR)) || key_exit || key_manager) {
                g_ignore_b = true;
                g_repo_manager = false;
                refresh_catalogs(false);
                g_settings_screen = true;
                g_setting_row = 0;
                draw_settings(&ui);
            } else if (((down & HidNpadButton_Up) || key_up) && g_repo_selected > 0) {
                g_repo_selected--;
                if (g_repo_selected < g_repo_scroll)
                    g_repo_scroll = g_repo_selected;
                changed = true;
            } else if (((down & HidNpadButton_Down) || key_down) &&
                       g_repo_selected + 1 < repo_entry_count()) {
                g_repo_selected++;
                if (g_repo_selected >= g_repo_scroll + REPO_VISIBLE_ROWS)
                    g_repo_scroll = g_repo_selected - REPO_VISIBLE_ROWS + 1;
                changed = true;
            } else if ((down & HidNpadButton_A) || key_accept) {
                request_repo_action();
            } else if ((down & HidNpadButton_X) || key_refresh) {
                request_log_delete();
            } else if ((down & HidNpadButton_L) || key_test) {
                g_return_to_repos = true;
                add_test_repo();
                g_return_to_repos = false;
            } else if ((down & HidNpadButton_R) || key_add) {
                g_return_to_repos = true;
                add_repository();
                g_return_to_repos = false;
            }
            memcpy(g_previous_keys, keyboard.keys, sizeof(g_previous_keys));
            if (changed)
                draw_repo_manager(&ui);
            continue;
        }

        const bool b_down = (down & HidNpadButton_B) != 0;
        const bool b_held = (padGetButtons(&pad) & HidNpadButton_B) != 0;
        bool swallow_b = false;
        if (g_ignore_b) {
            if (b_down) {
                swallow_b = true;
                g_ignore_b = false;
            } else if (!b_held) {
                g_ignore_b = false;
            }
        }
        if ((b_down && !swallow_b) || key_exit)
            break;

        if ((down & HidNpadButton_ZL) || key_settings) {
            g_settings_screen = true;
            draw_settings(&ui);
            memcpy(g_previous_keys, keyboard.keys, sizeof(g_previous_keys));
            continue;
        }

        if (down & HidNpadButton_StickL) {
            edit_search();
            memcpy(g_previous_keys, keyboard.keys, sizeof(g_previous_keys));
            continue;
        }

        bool changed = false;
        const int selection_step = g_list_view ? 1 : VISIBLE_COLUMNS;
        if (down & HidNpadButton_StickR) {
            HbPackage* package = visible_package(g_selected);
            if (package) {
                char version_error[256] = {0};
                g_version_count = release_list_assets(package->url, package->asset,
                                                      CACHE_DIR "/release.json",
                                                      g_version_choices, RELEASE_CHOICES_MAX,
                                                      version_error, sizeof(version_error));
                if (g_version_count > 0) {
                    g_version_package = package;
                    g_version_selected = first_stable_version();
                    g_version_scroll = 0;
                    g_versions_screen = true;
                    draw_versions(&ui);
                } else {
                    set_status("Versions failed: %s", version_error);
                    draw_shop(&ui);
                }
                memcpy(g_previous_keys, keyboard.keys, sizeof(g_previous_keys));
                continue;
            }
        } else if ((nav & HidNpadButton_Left) || key_left) {
            move_selection(-1);
            changed = true;
        } else if ((nav & HidNpadButton_Right) || key_right) {
            move_selection(1);
            changed = true;
        } else if ((nav & HidNpadButton_Up) || key_up) {
            move_selection(-selection_step);
            changed = true;
        } else if ((nav & HidNpadButton_Down) || key_down) {
            move_selection(selection_step);
            changed = true;
        } else if ((down & HidNpadButton_X) || key_context) {
            if (visible_count()) {
                g_context_menu = true;
                g_context_selected = 0;
                draw_context(&ui);
                memcpy(g_previous_keys, keyboard.keys, sizeof(g_previous_keys));
                continue;
            }
        } else if ((down & HidNpadButton_Minus) || key_view) {
            g_list_view = !g_list_view;
            g_settings.list_view = g_list_view ? 1 : 0;
            g_scroll_page = visible_count() ? g_selected / view_page_size() : 0;
            char view_error[128] = {0};
            if (settings_save(SETTINGS_PATH, &g_settings, view_error, sizeof(view_error)) != 0)
                set_status("Could not save view: %s", view_error);
            changed = true;
        } else if ((down & HidNpadButton_L) || key_test) {
            HbPackage* package = visible_package(g_selected);
            if (package) {
                toggle_mark(package);
                changed = true;
            }
        } else if ((down & HidNpadButton_Y) || key_remove) {
            g_filter = (g_filter + 1) % 4;
            invalidate_visible();
            g_selected = 0;
            g_scroll_page = 0;
            clamp_selection();
            changed = true;
        } else if (visible_count() && ((down & HidNpadButton_A) || key_accept)) {
            HbPackage* package = visible_package(g_selected);
            if (package) {
                install_selected_or_current(package, &pad);
                changed = true;
            }
        }

        memcpy(g_previous_keys, keyboard.keys, sizeof(g_previous_keys));
        if (changed)
            draw_shop(&ui);
    }

    icon_prefetch_stop();
    free_icons();
    http_global_exit();
    ui_exit(&ui);
    return 0;
}
