# Changelog

## 1.2.4

- Prefer the generated `catalog.json` catalogue instead of resolving releases
  on the console.
- Migrate the old default `packages.json` repository URL in place.
- Keep the legacy include index and per-package manifests supported for
  third-party repositories.
- Validate cached catalogues before replacing them and keep the previous cache
  when a refresh fails.
- Install catalogue packages directly from their resolved `download_url`,
  with required SHA-256 verification and optional size checks.
- Added dynamic package storage and host-side parser tests.
- Clear catalog cache now removes `.new` leftovers as part of the cache
  directory reset.
