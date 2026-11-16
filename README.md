# brewstore

A simple **homebrew** shop for Switch 2.  
It is a homebrew NRO. It does not install NSPs, touch NAND, register services,
or add a Home Menu tile.  

Currently uses a fork of [hbmenu](https://github.com/ouncebrew/ounce-hbmenu/pull/1)  

## Disclaimer

I am not responsible for any bricks, lost data or anything else.  
Use this tool on you own responsibility.

## Fixed

- Repo manager list scrolls instead of drawing entries off-screen.
- Failed downloads no longer delete the previous good cache.
- Root-target packages can be uninstalled using their recorded file list.
- Reinstalls remove files dropped from newer archives.
- Uninstall removes only tracked files and leaves user-added files and folders alone.
- Installs without a file list refuse automatic folder deletion instead of wiping user data.
- Installed detection ignores non-empty folders left by failed installs.
- Shortcut selection no longer picks a random NRO from a multi-NRO package.
- Release selection skips debug/symbol/source archives.
- Archive paths reject traversal, invalid UTF-8, control bytes, and FAT-invalid characters.
- Prerelease versions compare correctly.
- `+` no longer exits brewstore.
- Brewstore data moved to `sdmc:/brewstore/` with migration from `/switch/brewstore/`.
- Uninstall shows a file-by-file progress screen.
- Ouncesshell is excluded while it has no releases.

## Build

```sh
export DEVKITPRO=/opt/devkitpro
make -j4
```

It will compile as brewstore.nro.  
Use the netloader to copy it to the System.  

```text
sdmc:/switch/brewstore.nro
```

## Default catalog

On first launch, `repos.json` is created with:

```text
https://raw.githubusercontent.com/HerXayah/brewstore-repo/main/packages.json
```

The catalog is hosted in the `brewstore-repo` Git repository. Its
`packages.json` is an include index:

```json
{
  "include": [
    "packages/ounceshell/manifest.json"
  ]
}
```

Each package manifest lives under `packages/<name>/manifest.json`.

## Thanks to

[Ouncebrew](https://github.com/ouncebrew)

## License

GNU General Public License v3.0. See [LICENSE](LICENSE).
