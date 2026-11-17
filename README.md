# brewstore

A simple **homebrew** shop for Switch 2.

Currently uses a fork of [hbmenu](https://github.com/ouncebrew/ounce-hbmenu/pull/1)  

## Disclaimer

I am not responsible for any bricks, lost data or anything else.  
Use this tool on you own responsibility.  
Same goes for using custom repositories.  

## Installation

- Grab the modified HB-Menu
  - https://github.com/HerXayah/brewstore-repo/releases/download/main/hbmenu.nro
- Replace the original `hbmenu.nro` in your kaiHEN directory on PC
- Put `brewloader.nro` into programs in your kaiHEN directory on PC
- Load up kaiHEN
- Version string should say ``v1337.69420.l33t``
- Boot up kaiHEN
- Profit!

## Dev?

- Head over to [brewstore-repo](https://github.com/HerXayah/brewstore-repo) to add your app!

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
