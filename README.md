# brewstore

A simple **homebrew** shop for Switch 2.  
It is a homebrew NRO. It does not install NSPs, touch NAND, register services,
or add a Home Menu tile.  

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
