// SPDX-License-Identifier: MIT
#include "release.h"

#include "archive.h"
#include "json_util.h"

#include "github.h"
#include "http.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define RELEASE_JSON_MAX (1024 * 1024)
#define RELEASE_TOKENS 8192

static int fail(char* err, size_t err_size, const char* message) {
    if (err && err_size)
        snprintf(err, err_size, "%s", message);
    return -1;
}

#define token_string json_token_string
#define token_skip json_token_skip
#define object_get json_object_get
#define array_get json_array_get
#define object_get_object json_object_get_object
#define object_get_bool json_object_get_bool

static int safe_asset_name(const char* name) {
    if (!name[0] || strchr(name, '/') || strchr(name, '\\') ||
        strchr(name, ':') || strstr(name, ".."))
        return 0;
    return 1;
}

static int ends_with_ci(const char* text, const char* suffix) {
    const size_t text_length = strlen(text);
    const size_t suffix_length = strlen(suffix);
    if (suffix_length > text_length)
        return 0;
    return strcasecmp(text + text_length - suffix_length, suffix) == 0;
}

static void copy_digest(const char* text, char* digest, size_t digest_size) {
    if (!digest || !digest_size)
        return;
    digest[0] = '\0';
    if (!text)
        return;
    if (strncmp(text, "sha256:", 7) == 0)
        text += 7;
    if (strlen(text) == 64)
        snprintf(digest, digest_size, "%s", text);
}

static bool is_debug_asset(const char* name) {
    return strcasestr(name, "debug") || strcasestr(name, "symbols") ||
           strcasestr(name, "source");
}

static int ends_with_ci(const char* text, const char* suffix);

static int read_file(const char* path, char** out, size_t* out_size, char* err, size_t err_size) {
    FILE* file = fopen(path, "rb");
    if (!file)
        return fail(err, err_size, "cannot open GitHub release response");
    char* text = malloc(RELEASE_JSON_MAX + 1);
    if (!text) {
        fclose(file);
        return fail(err, err_size, "out of memory");
    }
    const size_t length = fread(text, 1, RELEASE_JSON_MAX, file);
    fclose(file);
    if (!length || length >= RELEASE_JSON_MAX) {
        free(text);
        return fail(err, err_size, "GitHub release response is empty or too large");
    }
    text[length] = '\0';
    *out = text;
    *out_size = length;
    return 0;
}

static int gitlab_project_path(const char* url, char* out, size_t out_size) {
    static const char prefix[] = "https://gitlab.com/";
    if (strncmp(url, prefix, sizeof(prefix) - 1) != 0)
        return -1;
    const char* start = url + sizeof(prefix) - 1;
    const char* end = strstr(start, "/-/");
    if (!end)
        end = start + strlen(start);
    while (end > start && end[-1] == '/')
        end--;
    if (end > start + 4 && strncmp(end - 4, ".git", 4) == 0)
        end -= 4;
    if (end <= start)
        return -1;
    size_t used = 0;
    for (const char* p = start; p < end; p++) {
        const char c = *p;
        if (c == '/') {
            if (used + 3 >= out_size)
                return -1;
            out[used++] = '%';
            out[used++] = '2';
            out[used++] = 'F';
        } else {
            if (used + 1 >= out_size)
                return -1;
            out[used++] = c;
        }
    }
    out[used] = '\0';
    return used ? 0 : -1;
}

static int gitlab_latest_asset(const char* repo_url, const char* asset_hint,
                               const char* cache_path, char* asset_url, size_t url_size,
                               char* asset_name, size_t name_size,
                               unsigned long long* asset_size,
                               char* digest, size_t digest_size,
                               char* err, size_t err_size) {
    char project[512];
    if (gitlab_project_path(repo_url, project, sizeof(project)) != 0)
        return fail(err, err_size, "not a GitLab project URL");

    char api[1024];
    snprintf(api, sizeof(api), "https://gitlab.com/api/v4/projects/%s/releases?per_page=10",
             project);
    char hash[65];
    if (http_download(api, cache_path, hash, err, err_size) != 0)
        return -1;

    char* json = NULL;
    size_t length = 0;
    if (read_file(cache_path, &json, &length, err, err_size) != 0)
        return -1;
    jsmntok_t* tokens = malloc(sizeof(jsmntok_t) * RELEASE_TOKENS);
    if (!tokens) {
        free(json);
        return fail(err, err_size, "out of memory");
    }
    jsmn_parser parser;
    jsmn_init(&parser);
    const int count = jsmn_parse(&parser, json, length, tokens, RELEASE_TOKENS);
    if (count < 1 || tokens[0].type != JSMN_ARRAY) {
        free(tokens);
        free(json);
        return fail(err, err_size, "GitLab release response is not JSON");
    }

    char fallback_url[1024] = "";
    char fallback_name[128] = "";
    unsigned long long fallback_size = 0;
    int release_position = 1;
    for (int release = 0; release < tokens[0].size; release++) {
        const int release_object = release_position;
        release_position = token_skip(tokens, release_object, count);
        if (release_object >= count || tokens[release_object].type != JSMN_OBJECT)
            continue;
        const int assets = object_get_object(json, tokens, count, release_object, "assets");
        if (assets < 0)
            continue;
        const int links = array_get(json, tokens, count, assets, "links");
        if (links < 0)
            continue;
        int position = links + 1;
        for (int index = 0; index < tokens[links].size; index++) {
            const int object = position;
            position = token_skip(tokens, object, count);
            if (object >= count || tokens[object].type != JSMN_OBJECT)
                continue;
            char name[128];
            char url[1024];
            char direct[1024] = "";
            if (object_get(json, tokens, count, object, "name", name, sizeof(name)) != 0)
                continue;
            object_get(json, tokens, count, object, "direct_asset_url", direct, sizeof(direct));
            if (!direct[0] && object_get(json, tokens, count, object, "url", url, sizeof(url)) != 0)
                continue;
            const char* link = direct[0] ? direct : url;
            if (!safe_asset_name(name) || strncmp(link, "https://", 8) != 0)
                continue;
            if (asset_hint && asset_hint[0]) {
                if (ends_with_ci(name, asset_hint)) {
                    snprintf(asset_url, url_size, "%s", link);
                    snprintf(asset_name, name_size, "%s", name);
                    if (asset_size)
                        *asset_size = 0;
                    if (digest && digest_size)
                        digest[0] = '\0';
                    free(tokens);
                    free(json);
                    return 0;
                }
            } else if (ends_with_ci(name, ".nro")) {
                snprintf(asset_url, url_size, "%s", link);
                snprintf(asset_name, name_size, "%s", name);
                if (asset_size)
                    *asset_size = 0;
                if (digest && digest_size)
                    digest[0] = '\0';
                free(tokens);
                free(json);
                return 0;
            } else if (!fallback_name[0] && archive_is_archive(name) &&
                       !is_debug_asset(name)) {
                snprintf(fallback_url, sizeof(fallback_url), "%s", link);
                snprintf(fallback_name, sizeof(fallback_name), "%s", name);
            }
        }
    }

    if (fallback_name[0]) {
        snprintf(asset_url, url_size, "%s", fallback_url);
        snprintf(asset_name, name_size, "%s", fallback_name);
        if (asset_size)
            *asset_size = fallback_size;
        if (digest && digest_size)
            digest[0] = '\0';
        free(tokens);
        free(json);
        return 0;
    }
    free(tokens);
    free(json);
    return fail(err, err_size, "GitLab release has no .nro or archive asset");
}

int release_latest_asset(const char* repo_url, const char* asset_hint,
                         const char* cache_path, char* asset_url, size_t url_size,
                         char* asset_name, size_t name_size,
                         unsigned long long* asset_size,
                         char* digest, size_t digest_size,
                         char* err, size_t err_size) {
    if (strncmp(repo_url, "https://gitlab.com/", 19) == 0)
        return gitlab_latest_asset(repo_url, asset_hint, cache_path, asset_url, url_size,
                                   asset_name, name_size, asset_size, digest, digest_size,
                                   err, err_size);
    char owner[128];
    char repo[128];
    if (github_repo_parts(repo_url, owner, sizeof(owner), repo, sizeof(repo)) != 0)
        return fail(err, err_size, "not a GitHub repository URL");

    char api[1024];
    snprintf(api, sizeof(api), "https://api.github.com/repos/%s/%s/releases?per_page=10",
             owner, repo);
    char hash[65];
    if (http_download(api, cache_path, hash, err, err_size) != 0)
        return -1;

    char* json = NULL;
    size_t length = 0;
    if (read_file(cache_path, &json, &length, err, err_size) != 0)
        return -1;

    jsmntok_t* tokens = malloc(sizeof(jsmntok_t) * RELEASE_TOKENS);
    if (!tokens) {
        free(json);
        return fail(err, err_size, "out of memory");
    }
    jsmn_parser parser;
    jsmn_init(&parser);
    const int count = jsmn_parse(&parser, json, length, tokens, RELEASE_TOKENS);
    if (count < 1 || (tokens[0].type != JSMN_OBJECT && tokens[0].type != JSMN_ARRAY)) {
        free(tokens);
        free(json);
        return fail(err, err_size, "GitHub release response is not JSON");
    }

    char fallback_url[1024] = "";
    char fallback_name[128] = "";
    char fallback_digest[65] = "";
    unsigned long long fallback_size = 0;
    const int releases = tokens[0].type == JSMN_ARRAY ? tokens[0].size : 1;
    int release_position = tokens[0].type == JSMN_ARRAY ? 1 : 0;
    for (int release = 0; release < releases; release++) {
        const int release_object = release_position;
        release_position = token_skip(tokens, release_object, count);
        if (release_object >= count || tokens[release_object].type != JSMN_OBJECT)
            continue;
        const int assets = array_get(json, tokens, count, release_object, "assets");
        if (assets < 0)
            continue;

        int position = assets + 1;
        for (int index = 0; index < tokens[assets].size; index++) {
            const int object = position;
            position = token_skip(tokens, object, count);
            if (object >= count || tokens[object].type != JSMN_OBJECT)
                continue;
            char name[128];
            char url[1024];
            char size_text[32] = "0";
            if (object_get(json, tokens, count, object, "name", name, sizeof(name)) != 0 ||
                object_get(json, tokens, count, object, "browser_download_url", url, sizeof(url)) != 0)
                continue;
            object_get(json, tokens, count, object, "size", size_text, sizeof(size_text));
            char digest_text[128] = "";
            object_get(json, tokens, count, object, "digest", digest_text, sizeof(digest_text));
            const unsigned long long size = strtoull(size_text, NULL, 10);
            if (!safe_asset_name(name) || strncmp(url, "https://", 8) != 0)
                continue;
            if (asset_hint && asset_hint[0]) {
                if (ends_with_ci(name, asset_hint)) {
                    snprintf(asset_url, url_size, "%s", url);
                    snprintf(asset_name, name_size, "%s", name);
                    copy_digest(digest_text, digest, digest_size);
                    if (asset_size)
                        *asset_size = size;
                    free(tokens);
                    free(json);
                    return 0;
                }
            } else if (ends_with_ci(name, ".nro")) {
                snprintf(asset_url, url_size, "%s", url);
                snprintf(asset_name, name_size, "%s", name);
                copy_digest(digest_text, digest, digest_size);
                if (asset_size)
                    *asset_size = size;
                free(tokens);
                free(json);
                return 0;
            } else if (!fallback_name[0] && archive_is_archive(name) &&
                       !is_debug_asset(name)) {
                snprintf(fallback_url, sizeof(fallback_url), "%s", url);
                snprintf(fallback_name, sizeof(fallback_name), "%s", name);
                copy_digest(digest_text, fallback_digest, sizeof(fallback_digest));
                fallback_size = size;
            }
        }
    }

    if (fallback_name[0]) {
        snprintf(asset_url, url_size, "%s", fallback_url);
        snprintf(asset_name, name_size, "%s", fallback_name);
        copy_digest(fallback_digest, digest, digest_size);
        if (asset_size)
            *asset_size = fallback_size;
        free(tokens);
        free(json);
        return 0;
    }

    free(tokens);
    free(json);
    return fail(err, err_size, "release has no .nro or archive asset");
}

static unsigned long long days_from_civil(int y, unsigned m, unsigned d) {
    y -= m <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return (unsigned long long)(era * 146097 + (int)doe - 719468);
}

static unsigned long long iso_epoch(const char* text) {
    int y, m, d;
    if (!text || sscanf(text, "%d-%d-%d", &y, &m, &d) != 3 || m < 1 || m > 12 ||
        d < 1 || d > 31)
        return 0;
    return days_from_civil(y, (unsigned)m, (unsigned)d) * 86400ULL;
}

static bool prerelease_tag(const char* tag) {
    return strcasestr(tag, "-rc") || strcasestr(tag, "-beta") ||
           strcasestr(tag, "-alpha") || strcasestr(tag, "preview");
}

static int copy_release_choice(const char* tag, const char* asset_name, const char* asset_url,
                               const char* digest, bool prerelease,
                               unsigned long long published, ReleaseChoice* out) {
    if (!tag[0] || !asset_name[0] || !asset_url[0])
        return -1;
    snprintf(out->tag, sizeof(out->tag), "%s", tag);
    snprintf(out->asset, sizeof(out->asset), "%s", asset_name);
    snprintf(out->url, sizeof(out->url), "%s", asset_url);
    snprintf(out->sha256, sizeof(out->sha256), "%s", digest ? digest : "");
    out->prerelease = prerelease || prerelease_tag(tag);
    out->published = published;
    return 0;
}

static int github_release_list(const char* owner, const char* repo, const char* asset_hint,
                               const char* cache_path, ReleaseChoice* out, int max,
                               char* err, size_t err_size) {
    char api[512];
    snprintf(api, sizeof(api), "https://api.github.com/repos/%s/%s/releases?per_page=20",
             owner, repo);
    char hash[65];
    if (http_download(api, cache_path, hash, err, err_size) != 0)
        return -1;
    char* json = NULL;
    size_t length = 0;
    if (read_file(cache_path, &json, &length, err, err_size) != 0)
        return -1;
    jsmntok_t* tokens = malloc(sizeof(jsmntok_t) * RELEASE_TOKENS);
    if (!tokens) {
        free(json);
        return fail(err, err_size, "out of memory");
    }
    jsmn_parser parser;
    jsmn_init(&parser);
    const int count = jsmn_parse(&parser, json, length, tokens, RELEASE_TOKENS);
    if (count < 1 || tokens[0].type != JSMN_ARRAY) {
        free(tokens); free(json);
        return fail(err, err_size, "GitHub release response is not JSON");
    }
    int found = 0, release_position = 1;
    for (int release = 0; release < tokens[0].size && found < max; release++) {
        const int release_object = release_position;
        release_position = token_skip(tokens, release_object, count);
        if (release_object >= count || tokens[release_object].type != JSMN_OBJECT)
            continue;
        char tag[64];
        if (object_get(json, tokens, count, release_object, "tag_name", tag, sizeof(tag)) != 0)
            continue;
        bool prerelease = false;
        object_get_bool(json, tokens, count, release_object, "prerelease", &prerelease);
        char published_text[32] = "";
        object_get(json, tokens, count, release_object, "published_at",
                   published_text, sizeof(published_text));
        const unsigned long long published = iso_epoch(published_text);
        const int assets = array_get(json, tokens, count, release_object, "assets");
        if (assets < 0)
            continue;
        char chosen_name[128] = "", chosen_url[1024] = "", chosen_digest[65] = "";
        char fallback_name[128] = "", fallback_url[1024] = "", fallback_digest[65] = "";
        int position = assets + 1;
        for (int index = 0; index < tokens[assets].size; index++) {
            const int object = position;
            position = token_skip(tokens, object, count);
            if (object >= count || tokens[object].type != JSMN_OBJECT)
                continue;
            char name[128], url[1024], digest[128] = "";
            if (object_get(json, tokens, count, object, "name", name, sizeof(name)) != 0 ||
                object_get(json, tokens, count, object, "browser_download_url", url,
                           sizeof(url)) != 0)
                continue;
            object_get(json, tokens, count, object, "digest", digest, sizeof(digest));
            if (!safe_asset_name(name) || strncmp(url, "https://", 8) != 0)
                continue;
            const int match = asset_hint && asset_hint[0]
                                  ? ends_with_ci(name, asset_hint)
                                  : ends_with_ci(name, ".nro");
            if (match && !chosen_name[0]) {
                snprintf(chosen_name, sizeof(chosen_name), "%s", name);
                snprintf(chosen_url, sizeof(chosen_url), "%s", url);
                copy_digest(digest, chosen_digest, sizeof(chosen_digest));
            } else if (!asset_hint && !fallback_name[0] && archive_is_archive(name) &&
                       !is_debug_asset(name)) {
                snprintf(fallback_name, sizeof(fallback_name), "%s", name);
                snprintf(fallback_url, sizeof(fallback_url), "%s", url);
                copy_digest(digest, fallback_digest, sizeof(fallback_digest));
            }
        }
        if (!chosen_name[0] && fallback_name[0]) {
            snprintf(chosen_name, sizeof(chosen_name), "%s", fallback_name);
            snprintf(chosen_url, sizeof(chosen_url), "%s", fallback_url);
            snprintf(chosen_digest, sizeof(chosen_digest), "%s", fallback_digest);
        }
        if (copy_release_choice(tag, chosen_name, chosen_url, chosen_digest,
                                prerelease, published, &out[found]) == 0)
            found++;
    }
    free(tokens); free(json);
    if (!found)
        return fail(err, err_size, "No matching release assets");
    return found;
}

static int gitlab_release_list(const char* project, const char* asset_hint,
                               const char* cache_path, ReleaseChoice* out, int max,
                               char* err, size_t err_size) {
    char api[1024];
    snprintf(api, sizeof(api), "https://gitlab.com/api/v4/projects/%s/releases?per_page=20",
             project);
    char hash[65];
    if (http_download(api, cache_path, hash, err, err_size) != 0)
        return -1;
    char* json = NULL;
    size_t length = 0;
    if (read_file(cache_path, &json, &length, err, err_size) != 0)
        return -1;
    jsmntok_t* tokens = malloc(sizeof(jsmntok_t) * RELEASE_TOKENS);
    if (!tokens) {
        free(json);
        return fail(err, err_size, "out of memory");
    }
    jsmn_parser parser;
    jsmn_init(&parser);
    const int count = jsmn_parse(&parser, json, length, tokens, RELEASE_TOKENS);
    if (count < 1 || tokens[0].type != JSMN_ARRAY) {
        free(tokens); free(json);
        return fail(err, err_size, "GitLab release response is not JSON");
    }
    int found = 0, release_position = 1;
    for (int release = 0; release < tokens[0].size && found < max; release++) {
        const int release_object = release_position;
        release_position = token_skip(tokens, release_object, count);
        if (release_object >= count || tokens[release_object].type != JSMN_OBJECT)
            continue;
        char tag[64];
        if (object_get(json, tokens, count, release_object, "tag_name", tag, sizeof(tag)) != 0)
            continue;
        bool prerelease = false;
        object_get_bool(json, tokens, count, release_object, "upcoming_release", &prerelease);
        char published_text[32] = "";
        object_get(json, tokens, count, release_object, "released_at",
                   published_text, sizeof(published_text));
        const unsigned long long published = iso_epoch(published_text);
        const int assets = object_get_object(json, tokens, count, release_object, "assets");
        if (assets < 0)
            continue;
        const int links = array_get(json, tokens, count, assets, "links");
        if (links < 0)
            continue;
        char chosen_name[128] = "", chosen_url[1024] = "", fallback_name[128] = "",
             fallback_url[1024] = "";
        int position = links + 1;
        for (int index = 0; index < tokens[links].size; index++) {
            const int object = position;
            position = token_skip(tokens, object, count);
            if (object >= count || tokens[object].type != JSMN_OBJECT)
                continue;
            char name[128], direct[1024] = "", url[1024] = "";
            if (object_get(json, tokens, count, object, "name", name, sizeof(name)) != 0)
                continue;
            object_get(json, tokens, count, object, "direct_asset_url", direct, sizeof(direct));
            if (!direct[0])
                object_get(json, tokens, count, object, "url", url, sizeof(url));
            const char* link = direct[0] ? direct : url;
            if (!safe_asset_name(name) || strncmp(link, "https://", 8) != 0)
                continue;
            const int match = asset_hint && asset_hint[0]
                                  ? ends_with_ci(name, asset_hint)
                                  : ends_with_ci(name, ".nro");
            if (match && !chosen_name[0]) {
                snprintf(chosen_name, sizeof(chosen_name), "%s", name);
                snprintf(chosen_url, sizeof(chosen_url), "%s", link);
            } else if (!asset_hint && !fallback_name[0] && archive_is_archive(name) &&
                       !is_debug_asset(name)) {
                snprintf(fallback_name, sizeof(fallback_name), "%s", name);
                snprintf(fallback_url, sizeof(fallback_url), "%s", link);
            }
        }
        if (!chosen_name[0] && fallback_name[0]) {
            snprintf(chosen_name, sizeof(chosen_name), "%s", fallback_name);
            snprintf(chosen_url, sizeof(chosen_url), "%s", fallback_url);
        }
        if (copy_release_choice(tag, chosen_name, chosen_url, "",
                                prerelease, published, &out[found]) == 0)
            found++;
    }
    free(tokens); free(json);
    if (!found)
        return fail(err, err_size, "No matching release assets");
    return found;
}

int release_list_assets(const char* repo_url, const char* asset_hint, const char* cache_path,
                        ReleaseChoice* out, int max, char* err, size_t err_size) {
    if (!repo_url || !out || max <= 0)
        return fail(err, err_size, "invalid release request");
    if (strncmp(repo_url, "https://gitlab.com/", 19) == 0) {
        char project[512];
        if (gitlab_project_path(repo_url, project, sizeof(project)) != 0)
            return fail(err, err_size, "not a GitLab project URL");
        return gitlab_release_list(project, asset_hint, cache_path, out, max, err, err_size);
    }
    char owner[128], repo[128];
    if (github_repo_parts(repo_url, owner, sizeof(owner), repo, sizeof(repo)) != 0)
        return fail(err, err_size, "not a GitHub repository URL");
    return github_release_list(owner, repo, asset_hint, cache_path, out, max, err, err_size);
}
