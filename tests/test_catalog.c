#include "catalog.h"
#include "manifest.h"
#include "path_safe.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef FIXTURE_DIR
#define FIXTURE_DIR "fixtures"
#endif

static int failures;

static void check(int condition, const char* test, const char* detail) {
    if (!condition) {
        fprintf(stderr, "FAIL %s: %s\n", test, detail ? detail : "assertion failed");
        failures++;
    } else {
        printf("PASS %s\n", test);
    }
}

static int load_manifest(const char* name, HbManifest* manifest, char* err, size_t err_size) {
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", FIXTURE_DIR, name);
    return manifest_load(path, manifest, err, err_size);
}

int main(void) {
    char err[256];
    HbManifest manifest;

    memset(err, 0, sizeof(err));
    if (load_manifest("legacy-packages.json", &manifest, err, sizeof(err)) == 0) {
        check(manifest.include_count == 1, "legacy include count", NULL);
        check(strcmp(manifest.includes[0], "legacy-manifest.json") == 0,
              "legacy include value", manifest.includes[0]);
    } else {
        check(0, "legacy include load", err);
    }

    memset(err, 0, sizeof(err));
    if (load_manifest("legacy-manifest.json", &manifest, err, sizeof(err)) == 0) {
        check(manifest.count == 1, "legacy package count", NULL);
        check(strcmp(manifest.items[0].name, "example") == 0, "legacy package name", NULL);
        check(strcmp(manifest.items[0].target, "sdmc:/switch/example") == 0,
              "legacy package target", manifest.items[0].target);
    } else {
        check(0, "legacy manifest load", err);
    }

    HbPackageList catalog_list = {0};
    CatalogStats catalog_stats;
    memset(err, 0, sizeof(err));
    check(catalog_load(FIXTURE_DIR "/valid-catalog.json", &catalog_list,
                       &catalog_stats, err, sizeof(err)) == 0,
          "valid catalogue loads", err);
    check(catalog_list.count == 1 && catalog_stats.packages == 1,
          "valid catalogue count", NULL);
    check(strcmp(catalog_list.items[0].strip_prefix, "bundle/") == 0,
          "catalogue stripPrefix normalized", catalog_list.items[0].strip_prefix);
    hb_list_free(&catalog_list);

    memset(err, 0, sizeof(err));
    check(catalog_load(FIXTURE_DIR "/wrong-schema.json", &catalog_list,
                       &catalog_stats, err, sizeof(err)) == -1,
          "wrong schema rejected", NULL);
    hb_list_free(&catalog_list);
    memset(err, 0, sizeof(err));
    check(catalog_load(FIXTURE_DIR "/count-mismatch.json", &catalog_list,
                       &catalog_stats, err, sizeof(err)) == -1,
          "count mismatch rejected", NULL);
    hb_list_free(&catalog_list);
    memset(err, 0, sizeof(err));
    check(catalog_load(FIXTURE_DIR "/missing-packages.json", &catalog_list,
                       &catalog_stats, err, sizeof(err)) == -1,
          "missing packages rejected", NULL);
    hb_list_free(&catalog_list);

    memset(err, 0, sizeof(err));
    check(catalog_load(FIXTURE_DIR "/missing-sha256.json", &catalog_list,
                       &catalog_stats, err, sizeof(err)) == 0 &&
              catalog_stats.skipped == 1,
          "invalid catalogue entry skipped", NULL);
    hb_list_free(&catalog_list);
    memset(err, 0, sizeof(err));
    check(catalog_load(FIXTURE_DIR "/overlong-download-url.json", &catalog_list,
                       &catalog_stats, err, sizeof(err)) == 0 &&
              catalog_stats.skipped == 1,
          "overlong catalogue field skipped", NULL);
    hb_list_free(&catalog_list);
    memset(err, 0, sizeof(err));
    check(catalog_load(FIXTURE_DIR "/bad-target.json", &catalog_list,
                       &catalog_stats, err, sizeof(err)) == 0 &&
              catalog_stats.skipped == 1,
          "bad catalogue target skipped", NULL);
    hb_list_free(&catalog_list);
    memset(err, 0, sizeof(err));
    check(catalog_load(FIXTURE_DIR "/legacy-manifest.json", &catalog_list,
                       &catalog_stats, err, sizeof(err)) == -2,
          "legacy format detected", NULL);
    hb_list_free(&catalog_list);
    memset(err, 0, sizeof(err));
    check(catalog_load(FIXTURE_DIR "/truncated.json", &catalog_list,
                       &catalog_stats, err, sizeof(err)) != 0,
          "truncated catalogue rejected", NULL);
    hb_list_free(&catalog_list);
    memset(err, 0, sizeof(err));
    check(catalog_load(FIXTURE_DIR "/empty.json", &catalog_list,
                       &catalog_stats, err, sizeof(err)) == -1,
          "empty catalogue rejected", NULL);
    hb_list_free(&catalog_list);
    memset(err, 0, sizeof(err));
    check(catalog_load(FIXTURE_DIR "/oversize-catalog.json", &catalog_list,
                       &catalog_stats, err, sizeof(err)) == -1,
          "oversize catalogue rejected", NULL);
    hb_list_free(&catalog_list);
    memset(err, 0, sizeof(err));
    check(catalog_load(FIXTURE_DIR "/duplicate-names.json", &catalog_list,
                       &catalog_stats, err, sizeof(err)) == 0 &&
              catalog_stats.packages == 1 && catalog_stats.skipped == 1,
          "duplicate catalogue names skipped", NULL);
    hb_list_free(&catalog_list);

    HbPackageList list = {0};
    for (int i = 0; i < 5000; i++) {
        HbPackage item;
        memset(&item, 0, sizeof(item));
        snprintf(item.name, sizeof(item.name), "package-%d", i);
        if (hb_list_push(&list, &item) != 0) {
            check(0, "dynamic package list push", "allocation failed");
            break;
        }
    }
    check(list.count == 5000, "dynamic package list count", NULL);
    check(strcmp(list.items[4999].name, "package-4999") == 0,
          "dynamic package list content", list.items[4999].name);
    hb_list_free(&list);
    check(list.items == NULL && list.count == 0 && list.capacity == 0,
          "dynamic package list free", NULL);

    check(path_safe_relative("switch/example"), "path safe relative", NULL);
    check(!path_safe_relative("../evil"), "path rejects traversal", NULL);
    check(!path_safe_relative("/absolute"), "path rejects absolute", NULL);
    check(!path_safe_relative("a\\b"), "path rejects backslash", NULL);
    check(!path_safe_relative("a:b"), "path rejects colon", NULL);

    memset(err, 0, sizeof(err));
    check(load_manifest("truncated.json", &manifest, err, sizeof(err)) != 0,
          "truncated manifest rejected", NULL);
    memset(err, 0, sizeof(err));
    check(load_manifest("empty.json", &manifest, err, sizeof(err)) != 0,
          "empty manifest rejected", NULL);
    memset(err, 0, sizeof(err));
    check(load_manifest("oversize.json", &manifest, err, sizeof(err)) != 0,
          "oversize manifest rejected", NULL);

    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("all tests passed\n");
    return 0;
}
