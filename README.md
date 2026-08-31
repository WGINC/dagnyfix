# Dagnyfix

A `d3d8.dll` proxy for FFXI on Windower 4, Ashita, and vanilla that fixes two unrelated rendering bugs:

- **Aura aliasing** - actor auras render jagged instead of smooth. Ported from SpectralFix, the original Ashita plugin this project is built on.
- **Flare depth-sorting** - distant light-source flares (lamps, lanterns) render behind their housing instead of in front of it, flickering between correct and incorrect.

Both fixes are merged into a single DLL because the game only has one `d3d8.dll` slot to fill.

## Install

1. Rename whatever d3d8.dll is currently in your FFXI/Windower folder, don't just back it up elsewhere, it needs to stay in that folder under a new name. Dagnyfix is a proxy: it needs something to forward the real Direct3D calls to, and it looks for that "something" by name.

If you're running dgVoodoo2, rename its d3d8.dll to d3d8_dgvoodoo.dll.
Otherwise (some other wrapper, or you just want the game's own d3d8.dll kept in the chain), rename it to d3d8_orig.dll.

If neither renamed file is found, Dagnyfix falls back to the real d3d8.dll in System32 - which works, but means whatever you had installed before is no longer in the chain at all.

2. Drop `dagnyfix`'s built `d3d8.dll` in its place.

4. Launch and play. Both fixes are active automatically - no config needed.

## Build

Requires a 32-bit (x86) build, since FFXI is a 32-bit process.

```
cmake -S dagnyfix -B build-dagnyfix -A Win32 -D D3D8_SDK_PATH=C:/path/to/AshitaADK/ADK
cmake --build build-dagnyfix --config Debug
```

`D3D8_SDK_PATH` needs to point at a directory containing `d3d8/includes/d3d8.h` - an Ashita SDK checkout has one.

## Repo layout

- `dagnyfix/` - the production build: both fixes merged into one proxy DLL. Start here.
- `windower/` - the aura fix on its own, ported from the original Ashita plugin.
- `flarefix/` - the flare fix on its own, plus its own build/install notes.
- `diagnostics/` - the diagnostic tool used to track down the flare bug's root cause.
- `src/` - shared, framework-agnostic logic used by more than one of the above.

Each folder has its own README with the deeper technical detail and investigation history.

## Credits

Built on top of [Ashita](https://ashitaxi.com/), the addon/plugin framework this all runs on - created by RZN, currently maintained by atom0s, with Thorny (Lolwutt) as a core contributor - and Krauerlabs's original SpectralFix, the aura fix this project extends to Windower and builds on.

By Dellingr.

## License

GPL-3.0-only. See [LICENSE](LICENSE).
