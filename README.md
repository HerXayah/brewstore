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

## Thanks to

[Ouncebrew](https://github.com/ouncebrew)

## License

GNU General Public License v3.0. See [LICENSE](LICENSE).
