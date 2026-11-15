// SPDX-License-Identifier: MIT
// brewstore: a homebrew package browser for SD-card NROs and assets.
// It downloads GitHub archives, verifies optional SHA-256, and extracts them
// under sdmc:/switch. It never handles NSP/NAND content.
#include <switch.h>

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "github.h"
#include "http.h"
#include "manifest.h"
#include "release.h"
#include "readme.h"
#include "repos.h"
#include "settings.h"
#include "ui.h"
#include "version.h"
#include "zip.h"

#define MANIFEST_PATH "sdmc:/switch/brewstore/packages.json"
#define CACHE_DIR "sdmc:/switch/brewstore/cache"
#define ICON_DIR CACHE_DIR "/icons"
#define REPO_CACHE_DIR CACHE_DIR "/repos"
#define REPOS_PATH "sdmc:/switch/brewstore/repos.json"
#define TEST_REPO_PATH "sdmc:/switch/brewstore/test-repo.json"
#define INSTALLED_DIR "sdmc:/switch/brewstore/installed"
#define README_DIR CACHE_DIR "/readme"
#define SETTINGS_PATH "sdmc:/switch/brewstore/settings.json"
#define DEFAULT_REPO_URL "https://raw.githubusercontent.com/HerXayah/brewstore-repo/main/packages.json"
#define VISIBLE_COLUMNS 3
#define VISIBLE_ROWS 2
#define VISIBLE_COUNT (VISIBLE_COLUMNS * VISIBLE_ROWS)

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

typedef struct {
    char name[64];
    u32* pixels;
    int width;
    int height;
} CachedIcon;

static HbManifest g_manifest;
static RepoList g_repos;
static int g_selected;
enum { FILTER_ALL = 0, FILTER_UPDATES, FILTER_INSTALLED, FILTER_NOT_INSTALLED };
static int g_filter;
static char g_marked[HBPKG_MAX_PACKAGES][64];
static int g_marked_count;
enum { SORT_NAME = 0, SORT_AUTHOR, SORT_INSTALLED, SORT_UPDATED };
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
static bool g_repo_manager;
static bool g_settings_screen;
static int g_setting_row;
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
static bool g_network_ready;
static char g_status[256];
static char g_manifest_error[256];
static CachedIcon g_icons[12];
static int g_icon_count;
static Ui* g_ui;
static u64 g_previous_keys[4];

static void refresh_catalogs(bool fetch);
static bool ends_with_ci(const char* text, const char* suffix);
static bool prompt_text(const char* header, const char* guide, const char* initial,
                        char* out, size_t out_size);
static void draw_confirm(Ui* ui);
static void add_test_repo(void);
static void add_repository(void);

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

static bool is_installed(const HbPackage* package) {
    char path[512];
    return state_path(package, path, sizeof(path)) == 0 && access(path, F_OK) == 0;
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

static void format_installed_time(unsigned long long timestamp, char* out, size_t out_size) {
    out[0] = '\0';
    if (!timestamp)
        return;
    const time_t value = (time_t)timestamp;
    struct tm parts;
    if (localtime_r(&value, &parts))
        strftime(out, out_size, "%Y-%m-%d %H:%M", &parts);
}

static bool update_available(const HbPackage* package) {
    if (!is_installed(package))
        return false;
    char installed_version[64];
    char installed_hash[65];
    if (!read_state(package, installed_version, sizeof(installed_version),
                    installed_hash, sizeof(installed_hash), NULL, 0, NULL, NULL))
        return false;
    if (version_is_newer(package->version, installed_version))
        return true;
    if (package->sha256[0] && installed_hash[0] &&
        strcasecmp(package->sha256, installed_hash) != 0)
        return true;
    return false;
}

static const char* filter_name(void) {
    switch (g_filter) {
        case FILTER_UPDATES: return "Updates";
        case FILTER_INSTALLED: return "Installed";
        case FILTER_NOT_INSTALLED: return "Not installed";
        default: return "All";
    }
}

static bool filter_matches(const HbPackage* package) {
    switch (g_filter) {
        case FILTER_UPDATES: return update_available(package);
        case FILTER_INSTALLED: return is_installed(package);
        case FILTER_NOT_INSTALLED: return !is_installed(package);
        default: return true;
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
    if (g_marked_count < HBPKG_MAX_PACKAGES)
        snprintf(g_marked[g_marked_count++], sizeof(g_marked[0]), "%s", package->name);
}

static void clear_marks(void) {
    g_marked_count = 0;
}

static int visible_count(void) {
    int count = 0;
    for (int i = 0; i < g_manifest.count; i++) {
        if (filter_matches(&g_manifest.items[i]))
            count++;
    }
    return count;
}

static unsigned long long package_timestamp(const HbPackage* package, bool installed) {
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
        const unsigned long long a_time = package_timestamp(a, g_sort == SORT_INSTALLED);
        const unsigned long long b_time = package_timestamp(b, g_sort == SORT_INSTALLED);
        if (a_time != b_time)
            result = a_time > b_time ? 1 : -1;
    }
    if (result == 0)
        result = strcasecmp(a->title, b->title);
    return g_sort_reverse ? -result : result;
}

static HbPackage* visible_package(int index) {
    HbPackage* matches[HBPKG_MAX_PACKAGES];
    int count = 0;
    for (int i = 0; i < g_manifest.count; i++) {
        if (filter_matches(&g_manifest.items[i]))
            matches[count++] = &g_manifest.items[i];
    }
    qsort(matches, (size_t)count, sizeof(matches[0]), compare_packages);
    return index >= 0 && index < count ? matches[index] : NULL;
}

static const char* sort_name(void) {
    if (g_sort == SORT_NAME)
        return g_sort_reverse ? "Name Z-A" : "Name A-Z";
    if (g_sort == SORT_AUTHOR)
        return g_sort_reverse ? "Author Z-A" : "Author A-Z";
    if (g_sort == SORT_INSTALLED)
        return g_sort_reverse ? "Installed newest" : "Installed oldest";
    return g_sort_reverse ? "Updated newest" : "Updated oldest";
}

static void clamp_selection(void) {
    const int count = visible_count();
    if (g_selected >= count)
        g_selected = count ? count - 1 : 0;
    if (g_selected < 0)
        g_selected = 0;
    g_scroll_page = count ? g_selected / VISIBLE_COUNT : 0;
}

static void free_icons(void) {
    for (int i = 0; i < g_icon_count; i++)
        free(g_icons[i].pixels);
    g_icon_count = 0;
}

static const CachedIcon* cache_icon(const HbPackage* package, u32* pixels,
                                    int width, int height) {
    if (g_icon_count >= (int)(sizeof(g_icons) / sizeof(g_icons[0]))) {
        free(g_icons[0].pixels);
        memmove(&g_icons[0], &g_icons[1], sizeof(g_icons[0]) * (g_icon_count - 1));
        g_icon_count--;
    }
    CachedIcon* slot = &g_icons[g_icon_count++];
    snprintf(slot->name, sizeof(slot->name), "%s", package->name);
    slot->pixels = pixels;
    slot->width = width;
    slot->height = height;
    return slot;
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
    const int height = 232;
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

    const int icon_size = 96;
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

    if (is_installed(package)) {
        const bool update = update_available(package);
        const char* label = update ? "Update" : "Installed";
        const int label_width = ui_text_width(ui, label, 13);
        ui_text(ui, x + width - 14 - label_width, y + 204, label, 13,
                update ? COLOR_ACCENT : COLOR_GREEN);
    }
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
}

static void draw_shop_content(Ui* ui) {
    const int visible = visible_count();
    HbPackage* selected = visible ? visible_package(g_selected) : NULL;
    if (selected)
        readme_for(selected);
    if (visible) {
        const int first = g_scroll_page * VISIBLE_COUNT;
        for (int slot = 0; slot < VISIBLE_COUNT; slot++) {
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
    char list_label[64];
    snprintf(list_label, sizeof(list_label), "%s · %s", filter_name(), sort_name());
    const int filter_width = ui_text_width(ui, list_label, 13);
    ui_text(ui, UI_WIDTH - 24 - filter_width, 25, list_label, 13, COLOR_ACCENT);

    if (!g_manifest.count) {
        ui_text(ui, 24, 104, "Nothing here yet", 26, COLOR_TEXT);
        ui_text(ui, 24, 146, "Press R for a catalog, or ZR for repo actions.", 16, COLOR_MUTED);
        ui_text(ui, 24, 174, REPOS_PATH, 13, COLOR_ACCENT);
    } else if (!selected) {
        ui_text(ui, 24, 104, "Nothing matches this filter", 26, COLOR_TEXT);
        ui_text(ui, 24, 146, "Press Y to change filter.", 16, COLOR_MUTED);
    } else {
        const int first = g_scroll_page * VISIBLE_COUNT;
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
                      76 + row * 248, index == g_selected, is_marked(package));
        }

        ui_rect(ui, 936, 76, 312, 568, COLOR_PANEL);

        ui_text_truncated(ui, 958, 100, selected->title, 25, COLOR_TEXT, 268);
        ui_text_truncated(ui, 958, 136, selected->author, 14, COLOR_MUTED, 268);
        if (is_installed(selected)) {
            const bool update = update_available(selected);
            ui_text(ui, 958, 164, update ? "Update available" : "Installed", 14,
                    update ? COLOR_ACCENT : COLOR_GREEN);
        }
        ui_text(ui, 958, 188, display_version(selected), 15, COLOR_MUTED);
        if (is_installed(selected)) {
            char version[64];
            char hash[65];
            char file[512];
            unsigned long long installed_at = 0;
            if (read_state(selected, version, sizeof(version), hash, sizeof(hash),
                           file, sizeof(file), &installed_at, NULL) && installed_at) {
                char date[64];
                format_installed_time(installed_at, date, sizeof(date));
                char line[96];
                snprintf(line, sizeof(line), "Installed %s", date);
                ui_text_truncated(ui, 958, 212, line, 12, COLOR_MUTED, 268);
            }
        }

        ui_markdown_wrap(ui, 958, 236, selected->description, 15, COLOR_TEXT,
                         268, 7, 21);

        ui_text_truncated(ui, 958, 522, selected->target, 12, COLOR_MUTED, 268);
        const int button_y = 556;
        const char* primary_label = is_installed(selected)
                                       ? (update_available(selected) ? "A  Update" : "A  Reinstall")
                                       : "A  Install";
        const int primary_width = ui_text_width(ui, primary_label, 15);
        ui_rect(ui, 958, button_y, 268, 42,
                is_installed(selected) ? COLOR_ACCENT_DIM : COLOR_ACCENT);
        ui_text(ui, 958 + (268 - primary_width) / 2, button_y + 13,
                primary_label, 15, COLOR_TEXT);
    }

    ui_rect(ui, 0, 660, UI_WIDTH, 60, COLOR_PANEL);
    if (g_status[0]) {
        ui_text_truncated(ui, 24, 670, g_status, 14, COLOR_TEXT, UI_WIDTH - 48);
        ui_text(ui, 24, 696, "A install   L select   X menu   Y filter   ZR repos   ZL settings   B exit",
                12, COLOR_MUTED);
    } else {
        ui_text(ui, 24, 682, "A install   L select   X menu   Y filter   ZR repos   ZL settings   B exit",
                13, COLOR_MUTED);
    }

}

static int test_repo_present(void) {
    return access(TEST_REPO_PATH, F_OK) == 0;
}

static int repo_entry_count(void) {
    return g_repos.count + test_repo_present();
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
        for (int i = 0; i < entries; i++) {
            const int y = 80 + i * 52;
            if (i == g_repo_selected)
                ui_rect(ui, 24, y - 6, UI_WIDTH - 48, 44, COLOR_CARD_HOVER);
            if (i < g_repos.count) {
                ui_text_truncated(ui, 40, y, g_repos.urls[i], 16,
                                  g_repos.enabled[i] ? COLOR_TEXT : COLOR_MUTED,
                                  UI_WIDTH - 80);
                if (!g_repos.enabled[i])
                    ui_text(ui, UI_WIDTH - 130, y, "(disabled)", 13, COLOR_MUTED);
            } else {
                ui_text(ui, 40, y, "Test repository", 16, COLOR_TEXT);
                ui_text(ui, 250, y + 1, "test-repo.json", 14, COLOR_MUTED);
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

static void draw_settings(Ui* ui) {
    if (!ui)
        return;
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

    ui_rect(ui, 0, 660, UI_WIDTH, 60, COLOR_PANEL);
    if (g_status[0])
        ui_text_truncated(ui, 24, 670, g_status, 14, COLOR_TEXT, UI_WIDTH - 48);
    ui_text(ui, 24, 696, "A edit   B close", 13, COLOR_MUTED);
    ui_end(ui);
}

static void edit_default_path(void) {
    char path[256];
    if (!prompt_text("Default install path", "Folder under sdmc:/switch",
                     g_settings.default_install_path, path, sizeof(path))) {
        set_status("Path edit cancelled.");
        draw_settings(g_ui);
        return;
    }
    size_t path_length = strlen(path);
    while (path_length > 12 && path[path_length - 1] == '/')
        path[--path_length] = '\0';
    if (!settings_valid_path(path)) {
        set_status("Path must be under sdmc:/switch.");
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
    const char* title;
    char url[1024];
    char cache[512];
    char hash[65];
    char error[256];
} DownloadJob;

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
        ui_rect(ui, bar_x, bar_y, filled, bar_height, COLOR_ACCENT);
        char line[128];
        snprintf(line, sizeof(line), "%llu%%   %.1f / %.1f MB", percent,
                 (double)received / (1024.0 * 1024.0),
                 (double)total / (1024.0 * 1024.0));
        ui_text(ui, bar_x, bar_y + 32, line, 15, COLOR_MUTED);
    } else {
        const int filled = bar_width / 4;
        const int travel = bar_width - filled;
        const int offset = (int)((received / 32768) % (unsigned long long)(travel + 1));
        ui_rect(ui, bar_x + offset, bar_y, filled, bar_height, COLOR_ACCENT);
        char line[96];
        snprintf(line, sizeof(line), "%.1f MB", (double)received / (1024.0 * 1024.0));
        ui_text(ui, bar_x, bar_y + 32, line, 15, COLOR_MUTED);
    }
    ui_text(ui, 28, 680, "B cancel   + exit", 13, COLOR_MUTED);
    ui_end(ui);
}

static void download_progress_callback(unsigned long long received,
                                       unsigned long long total, void* user) {
    DownloadJob* job = user;
    job->received = received;
    job->total = total ? total : job->expected_total;
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
        if ((down & (HidNpadButton_B | HidNpadButton_Plus)) ||
            hidKeyboardStateGetKey(&keyboard, HidKeyboardKey_Escape)) {
            job->cancel = true;
            if ((down & HidNpadButton_Plus) ||
                hidKeyboardStateGetKey(&keyboard, HidKeyboardKey_Escape))
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
           !strstr(url, ".nro");
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
    ui_rect(ui, bar_x, bar_y, filled, bar_height, COLOR_ACCENT);
    char line[64];
    snprintf(line, sizeof(line), "%d%%", percent);
    ui_text(ui, bar_x, bar_y + 32, line, 15, COLOR_MUTED);
    ui_end(ui);
}

static void extraction_progress(int current, int total, const char* name, void* user) {
    const HbPackage* package = user;
    const char* title = package ? package->title : "";
    const char* base = strrchr(name, '/');
    base = base ? base + 1 : name;
    char step[320];
    snprintf(step, sizeof(step), "%d / %d  %s", current, total, base);
    int percent = total > 0 ? 45 + current * 45 / total : 45;
    if (percent > 89)
        percent = 89;
    draw_install_progress(g_ui, title, step, percent);
}

static void install_redraw(void) {
    if (!g_batch_active)
        draw_shop(g_ui);
}

static void install_package(const HbPackage* package, PadState* pad) {
    if (!g_network_ready) {
        set_status("Network is unavailable.");
        install_redraw();
        return;
    }

    char url[1024];
    char asset_name[128] = "";
    char error[256] = {0};
    unsigned long long asset_size = 0;
    if (package->release[0] || is_plain_github_repo(package->url)) {
        set_status("Finding latest release for %s ...", package->title);
        install_redraw();
        if (release_latest_asset(package->url, package->asset, CACHE_DIR "/release.json",
                                 url, sizeof(url), asset_name, sizeof(asset_name),
                                 &asset_size, error, sizeof(error)) != 0) {
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
    job.title = package->title;
    const int download_result = run_download(&job, pad);
    if (download_result == -2 || job.cancel) {
        g_ignore_b = true;
        g_batch_stop = true;
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
    if (package->sha256[0] && strcasecmp(hash, package->sha256) != 0) {
        remove(cache);
        set_status("SHA-256 mismatch.");
        install_redraw();
        return;
    }

    const bool root_target = strcmp(package->target, "sdmc:/switch") == 0;
    if (root_target && (!asset_name[0] || ends_with_ci(asset_name, ".zip"))) {
        set_status("sdmc:/switch only accepts a single non-zip asset.");
        install_redraw();
        return;
    }

    draw_install_progress(g_ui, package->title, "Preparing install", 10);
    if (path_mkdirs(package->target) != 0) {
        set_status("Could not prepare the install target.");
        install_redraw();
        return;
    }

    draw_install_progress(g_ui, package->title,
                          asset_name[0] && !ends_with_ci(asset_name, ".zip")
                              ? "Writing application" : "Extracting files", 45);
    char installed_file[512] = "";
    if (asset_name[0]) {
        if (ends_with_ci(asset_name, ".zip")) {
            if (zip_extract(cache, package->target, "", error, sizeof(error),
                        extraction_progress, (void*)package) != 0) {
                set_status("Extract failed: %s", error);
                install_redraw();
                return;
            }
        } else {
            snprintf(installed_file, sizeof(installed_file), "%s/%s",
                     package->target, asset_name);
            if (path_copy_file(cache, installed_file) != 0) {
                set_status("Could not write the release asset.");
                install_redraw();
                return;
            }
        }
    } else if (zip_extract(cache, package->target, package->strip_prefix, error,
                        sizeof(error), extraction_progress, (void*)package) != 0) {
        set_status("Extract failed: %s", error);
        install_redraw();
        return;
    }

    draw_install_progress(g_ui, package->title, "Finishing", 90);
    char old_version[64];
    char old_hash[65];
    char old_file[512];
    unsigned long long installed_at = 0;
    unsigned long long updated_at = 0;
    read_state(package, old_version, sizeof(old_version), old_hash, sizeof(old_hash),
               old_file, sizeof(old_file), &installed_at, &updated_at);
    const unsigned long long now = (unsigned long long)time(NULL);
    if (!installed_at)
        installed_at = now;
    updated_at = now;
    write_state(package, hash, installed_file, installed_at, updated_at);
    if (strcmp(package->version, "latest") == 0 ||
        strcmp(package->version, "test") == 0 ||
        strcmp(package->version, "unknown") == 0)
        set_status("Installed %s", package->title);
    else
        set_status("Installed %s %s", package->title, package->version);
    install_redraw();
}

static void remove_package(const HbPackage* package) {
    char installed_version[64];
    char installed_hash[65];
    char installed_file[512];
    const bool has_state = read_state(package, installed_version, sizeof(installed_version),
                                      installed_hash, sizeof(installed_hash),
                                      installed_file, sizeof(installed_file),
                                      NULL, NULL);
    if (has_state && installed_file[0]) {
        remove(installed_file);
    } else if (strcmp(package->target, "sdmc:/switch") == 0) {
        set_status("Refusing to delete /switch itself.");
        draw_shop(g_ui);
        return;
    } else if (path_remove_tree(package->target) != 0) {
        set_status("Could not remove %s.", package->title);
        draw_shop(g_ui);
        return;
    }
    char path[512];
    if (state_path(package, path, sizeof(path)) == 0)
        remove(path);
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
    if (package->release[0] || is_plain_github_repo(package->url)) {
        if (release_latest_asset(package->url, package->asset, CACHE_DIR "/release.json",
                                 url, sizeof(url), prepared->asset_name,
                                 sizeof(prepared->asset_name), &asset_size,
                                 error, sizeof(error)) != 0) {
            snprintf(prepared->error, sizeof(prepared->error), "%s", error);
            return false;
        }
    } else if (package->asset[0]) {
        snprintf(url, sizeof(url), "%s", package->url);
        snprintf(prepared->asset_name, sizeof(prepared->asset_name), "%s", package->asset);
    } else if (http_github_archive_url(package->url, package->branch, url, sizeof(url)) != 0) {
        snprintf(prepared->error, sizeof(prepared->error), "cannot resolve URL");
        return false;
    }

    snprintf(prepared->cache, sizeof(prepared->cache), CACHE_DIR "/batch-%d", index);
    DownloadJob job;
    memset(&job, 0, sizeof(job));
    snprintf(job.url, sizeof(job.url), "%s", url);
    snprintf(job.cache, sizeof(job.cache), "%s", prepared->cache);
    job.expected_total = asset_size;
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
    prepared->ok = true;
    return true;
}

static bool batch_install(const PreparedPackage* prepared) {
    HbPackage* package = prepared->package;
    const bool root_target = strcmp(package->target, "sdmc:/switch") == 0;
    if (root_target && (!prepared->asset_name[0] ||
                        ends_with_ci(prepared->asset_name, ".zip"))) {
        set_status("sdmc:/switch needs a single non-zip asset.");
        return false;
    }
    draw_install_progress(g_ui, package->title, "Preparing install", 10);
    if (path_mkdirs(package->target) != 0) {
        set_status("Could not prepare %s.", package->title);
        return false;
    }
    draw_install_progress(g_ui, package->title,
                          prepared->asset_name[0] &&
                                  !ends_with_ci(prepared->asset_name, ".zip")
                              ? "Writing application" : "Extracting files", 45);
    char installed_file[512] = "";
    char error[256] = {0};
    if (prepared->asset_name[0]) {
        if (ends_with_ci(prepared->asset_name, ".zip")) {
            if (zip_extract(prepared->cache, package->target, "", error, sizeof(error),
                        extraction_progress, (void*)package) != 0) {
                set_status("Extract failed: %s", error);
                return false;
            }
        } else {
            snprintf(installed_file, sizeof(installed_file), "%.255s/%.127s",
                     package->target, prepared->asset_name);
            if (path_copy_file(prepared->cache, installed_file) != 0) {
                set_status("Could not write %s.", package->title);
                return false;
            }
        }
    } else if (zip_extract(prepared->cache, package->target, package->strip_prefix,
                           error, sizeof(error), extraction_progress, (void*)package) != 0) {
        set_status("Extract failed: %s", error);
        return false;
    }
    draw_install_progress(g_ui, package->title, "Finishing", 90);
    char old_version[64];
    char old_hash[65];
    char old_file[512];
    unsigned long long installed_at = 0;
    unsigned long long updated_at = 0;
    read_state(package, old_version, sizeof(old_version), old_hash, sizeof(old_hash),
               old_file, sizeof(old_file), &installed_at, &updated_at);
    const unsigned long long now = (unsigned long long)time(NULL);
    if (!installed_at)
        installed_at = now;
    write_state(package, prepared->hash, installed_file, installed_at, now);
    return true;
}

static void install_selected_or_current(HbPackage* current, PadState* pad) {
    (void)pad;
    if (!g_marked_count) {
        install_package(current, g_pad);
        return;
    }
    PreparedPackage prepared[HBPKG_MAX_PACKAGES];
    int total = 0;
    for (int i = 0; i < g_manifest.count; i++) {
        if (is_marked(&g_manifest.items[i]))
            total++;
    }
    if (!total)
        return;

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
    ui_text(ui, panel_x + 24, panel_y + 22, "Actions", 22, COLOR_TEXT);
    for (int i = 0; i < 4; i++) {
        const int y = panel_y + 72 + i * 48;
        if (i == g_context_selected)
            ui_rect(ui, panel_x + 12, y - 8, panel_w - 24, 40, COLOR_CARD_HOVER);
        ui_text(ui, panel_x + 28, y, options[i], 17,
                i == g_context_selected ? COLOR_TEXT : COLOR_MUTED);
    }
    ui_text(ui, panel_x + 24, panel_y + panel_h - 24,
            "A select   B close", 12, COLOR_MUTED);
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
                g_sort = (g_sort + 1) % 4;
            } else {
                g_sort_reverse = true;
            }
            g_selected = 0;
            g_scroll_page = 0;
            clamp_selection();
            draw_context(g_ui);
            return;
        default:
            return;
    }
}

static void append_catalog(const HbManifest* source) {
    for (int i = 0; i < source->count && g_manifest.count < HBPKG_MAX_PACKAGES; i++) {
        bool duplicate = false;
        for (int existing = 0; existing < g_manifest.count; existing++) {
            if (strcmp(g_manifest.items[existing].name, source->items[i].name) == 0) {
                duplicate = true;
                break;
            }
        }
        if (!duplicate) {
            HbPackage package = source->items[i];
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
            g_manifest.items[g_manifest.count++] = package;
        }
    }
}

static void repo_cache_path(const char* url, char* out, size_t out_size) {
    u32 hash = 2166136261u;
    for (const unsigned char* p = (const unsigned char*)url; *p; p++) {
        hash ^= *p;
        hash *= 16777619u;
    }
    snprintf(out, out_size, REPO_CACHE_DIR "/%08x.json", hash);
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
    HbManifest manifest;
    if (manifest_load(path, &manifest, err, err_size) != 0) {
        if (g_catalog_log)
            fprintf(g_catalog_log, "FAIL %s: %s\n", path, err);
        (*failures)++;
        return false;
    }
    if (g_catalog_log)
        fprintf(g_catalog_log, "OK %s packages=%d includes=%d\n",
                path, manifest.count, manifest.include_count);
    append_catalog(&manifest);

    for (int i = 0; i < manifest.include_count; i++) {
        char included[640];
        if (resolve_include(base, manifest.includes[i], included, sizeof(included)) != 0) {
            if (g_catalog_log)
                fprintf(g_catalog_log, "FAIL resolve %s\n", manifest.includes[i]);
            (*failures)++;
            continue;
        }
        if (strncmp(included, "https://", 8) == 0) {
            char cache[640];
            repo_cache_path(included, cache, sizeof(cache));
            if ((fetch || access(cache, F_OK) != 0) && g_network_ready) {
                char hash[65];
                char download_error[128];
                if (http_download(included, cache, hash, download_error,
                                  sizeof(download_error)) != 0) {
                    if (g_catalog_log)
                        fprintf(g_catalog_log, "FAIL download %s: %s\n",
                                included, download_error);
                    (*failures)++;
                    continue;
                }
            }
            if (access(cache, F_OK) != 0) {
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
    return true;
}

static void refresh_catalogs(bool fetch) {
    char error[256] = {0};
    int failures = 0;
    g_manifest.count = 0;
    g_manifest_error[0] = '\0';
    path_mkdirs(REPO_CACHE_DIR);
    g_catalog_log = fopen(CACHE_DIR "/catalog.log", "w");

    if (access(MANIFEST_PATH, F_OK) == 0 &&
        !load_catalog_file(MANIFEST_PATH, MANIFEST_PATH, false, 0, &failures,
                           error, sizeof(error)))
        snprintf(g_manifest_error, sizeof(g_manifest_error), "%s", error);

    if (access(TEST_REPO_PATH, F_OK) == 0) {
        HbManifest test;
        if (manifest_load(TEST_REPO_PATH, &test, error, sizeof(error)) == 0) {
            for (int i = 0; i < test.count; i++) {
                test.items[i].target_explicit = false;
                test.items[i].readme_fallback = true;
                if (strcmp(test.items[i].version, "test") == 0)
                    snprintf(test.items[i].version, sizeof(test.items[i].version), "latest");
                if (strcmp(test.items[i].author, "Git") == 0 ||
                    strcmp(test.items[i].author, "Miella Selene / Xayah") == 0) {
                    char owner[128];
                    char repo[128];
                    if (github_repo_parts(test.items[i].url, owner, sizeof(owner),
                                          repo, sizeof(repo)) == 0)
                        snprintf(test.items[i].author, sizeof(test.items[i].author),
                                 "%.63s", owner);
                }
            }
            append_catalog(&test);
        } else {
            failures++;
        }
    }

    for (int i = 0; i < g_repos.count; i++) {
        if (!g_repos.enabled[i])
            continue;
        char cache[640];
        repo_cache_path(g_repos.urls[i], cache, sizeof(cache));
        bool present = access(cache, F_OK) == 0;
        if ((fetch || !present) && g_network_ready) {
            char hash[65];
            char download_error[128];
            if (http_download(g_repos.urls[i], cache, hash, download_error,
                              sizeof(download_error)) != 0) {
                if (g_catalog_log)
                    fprintf(g_catalog_log, "FAIL repo %s: %s\n",
                            g_repos.urls[i], download_error);
                failures++;
                if (!present)
                    continue;
            } else {
                present = true;
            }
        }
        if (!present) {
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

    clamp_selection();

    if (!g_manifest.count) {
        if (g_manifest_error[0])
            set_status("No packages: %s", g_manifest_error);
        else if (!g_repos.count)
            set_status("No repositories. Press R to add one.");
        else
            set_status("No packages loaded. Press X to refresh.");
    } else {
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
            g_scroll_page = i / VISIBLE_COUNT;
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
    g_scroll_page = g_selected / VISIBLE_COUNT;
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

    path_mkdirs(CACHE_DIR);
    path_mkdirs(ICON_DIR);
    path_mkdirs(INSTALLED_DIR);
    path_mkdirs(REPO_CACHE_DIR);
    path_mkdirs(README_DIR);
    path_mkdirs("sdmc:/switch/brewstore");

    if (settings_load(SETTINGS_PATH, &g_settings, error, sizeof(error)) != 0)
        snprintf(g_status, sizeof(g_status), "settings.json: %s", error);

    if (repos_load(REPOS_PATH, &g_repos, error, sizeof(error)) != 0) {
        snprintf(g_status, sizeof(g_status), "repos.json: %s", error);
    } else if (g_repos.count == 0) {
        repos_add(&g_repos, DEFAULT_REPO_URL);
        if (repos_save(REPOS_PATH, &g_repos, error, sizeof(error)) != 0)
            snprintf(g_status, sizeof(g_status), "repos.json: %s", error);
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

        if (g_settings_screen) {
            if (down & HidNpadButton_Plus)
                break;
            bool changed = false;
            if ((down & (HidNpadButton_B | HidNpadButton_ZL)) || key_exit) {
                g_ignore_b = true;
                g_settings_screen = false;
                draw_shop(&ui);
            } else if ((down & HidNpadButton_Up) || key_up) {
                g_setting_row = 0;
                changed = true;
            } else if ((down & HidNpadButton_Down) || key_down) {
                g_setting_row = 1;
                changed = true;
            } else if ((down & HidNpadButton_A) || key_accept) {
                if (g_setting_row == 0)
                    edit_default_path();
                else
                    edit_proxy();
            }
            memcpy(g_previous_keys, keyboard.keys, sizeof(g_previous_keys));
            if (changed)
                draw_settings(&ui);
            continue;
        }

        if (g_repo_manager) {
            if (down & HidNpadButton_Plus)
                break;
            bool changed = false;
            if ((down & (HidNpadButton_B | HidNpadButton_ZR)) || key_exit || key_manager) {
                g_ignore_b = true;
                g_repo_manager = false;
                refresh_catalogs(false);
                draw_shop(&ui);
            } else if (((down & HidNpadButton_Up) || key_up) && g_repo_selected > 0) {
                g_repo_selected--;
                changed = true;
            } else if (((down & HidNpadButton_Down) || key_down) &&
                       g_repo_selected + 1 < repo_entry_count()) {
                g_repo_selected++;
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
        if ((down & HidNpadButton_Plus) || (b_down && !swallow_b) || key_exit)
            break;

        if ((down & HidNpadButton_ZL) || key_settings) {
            g_settings_screen = true;
            draw_settings(&ui);
            memcpy(g_previous_keys, keyboard.keys, sizeof(g_previous_keys));
            continue;
        }

        if ((down & HidNpadButton_ZR) || key_manager) {
            g_repo_manager = true;
            g_repo_selected = 0;
            draw_repo_manager(&ui);
            memcpy(g_previous_keys, keyboard.keys, sizeof(g_previous_keys));
            continue;
        }

        bool changed = false;
        if ((down & HidNpadButton_Left) || key_left) {
            move_selection(-1);
            changed = true;
        } else if ((down & HidNpadButton_Right) || key_right) {
            move_selection(1);
            changed = true;
        } else if ((down & HidNpadButton_Up) || key_up) {
            move_selection(-VISIBLE_COLUMNS);
            changed = true;
        } else if ((down & HidNpadButton_Down) || key_down) {
            move_selection(VISIBLE_COLUMNS);
            changed = true;
        } else if ((down & HidNpadButton_X) || key_refresh) {
            if (visible_count()) {
                g_context_menu = true;
                g_context_selected = 0;
                draw_context(&ui);
                memcpy(g_previous_keys, keyboard.keys, sizeof(g_previous_keys));
                continue;
            }
        } else if ((down & HidNpadButton_R) || key_add) {
            add_repository();
            changed = true;
        } else if ((down & HidNpadButton_L) || key_test) {
            HbPackage* package = visible_package(g_selected);
            if (package) {
                toggle_mark(package);
                changed = true;
            }
        } else if ((down & HidNpadButton_Y) || key_remove) {
            g_filter = (g_filter + 1) % 4;
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

    free_icons();
    http_global_exit();
    ui_exit(&ui);
    return 0;
}
