# Dagnyfix

A `d3d8.dll` proxy for FFXI on Windower 4, Ashita, and vanilla that fixes three unrelated rendering bugs:

- **Aura aliasing** - actor auras render jagged instead of smooth. Ported from SpectralFix, the original Ashita plugin this project is built on.
- **Flare depth-sorting** - distant light-source flares (lamps, lanterns) render behind their housing instead of in front of it, flickering between correct and incorrect.
- **Ground-decal aspect/zoom** - ground-effect decals (avatar summoning pentagrams, ground-targeted spell circles, certain NPC ground effects) render at the wrong size and aspect ratio: stretched into an ellipse on non-square displays, and swelling or shrinking as the camera zooms instead of holding still on the ground.

All three fixes are merged into a single DLL because the game only has one `d3d8.dll` slot to fill.

## Install

1. Rename whatever d3d8.dll is currently in your FFXI/Windower folder, don't just back it up elsewhere, it needs to stay in that folder under a new name. Dagnyfix is a proxy: it needs something to forward the real Direct3D calls to, and it looks for that "something" by name.

If you're running dgVoodoo2, rename its d3d8.dll to d3d8_dgvoodoo.dll.
Otherwise (some other wrapper, or you just want the game's own d3d8.dll kept in the chain), rename it to d3d8_orig.dll.

If neither renamed file is found, Dagnyfix falls back to the real d3d8.dll in System32 - which works, but means whatever you had installed before is no longer in the chain at all.

2. Drop `dagnyfix`'s built `d3d8.dll` in its place.

4. Launch and play. Both fixes are active automatically - no config needed.

Needs the [Visual C++ 2015-2022 x86 Redistributable](https://aka.ms/vs/17/release/vc_redist.x86.exe) on the machine it runs on.

## Build

Requires a 32-bit (x86) build, since FFXI is a 32-bit process.

```
cmake -S dagnyfix -B build-dagnyfix -A Win32 -D D3D8_SDK_PATH=C:/path/to/AshitaADK/ADK
cmake --build build-dagnyfix --config Release
```

`D3D8_SDK_PATH` needs to point at a directory containing `d3d8/includes/d3d8.h` - an Ashita SDK checkout has one.

Needs the [Visual C++ 2015-2022 x86 Redistributable](https://aka.ms/vs/17/release/vc_redist.x86.exe)
on the machine it runs on.

## Repo layout

- `dagnyfix/` - the production build: all three fixes merged into one proxy DLL. Start here.
- `windower/` - the aura fix on its own, ported from the original Ashita plugin.
- `flarefix/` - the flare fix on its own, plus its own build/install notes.
- `decalfix/` - the ground-decal fix on its own, plus its own build/install notes and open items.
- `diagnostics/` - the diagnostic tool used to track down the flare bug's root cause.
- `src/` - shared, framework-agnostic logic used by more than one of the above.

Each folder has its own README with the deeper technical detail and investigation history.

## Credits

Built on top of [Ashita](https://ashitaxi.com/), the addon/plugin framework this all runs on - created by RZN, currently maintained by atom0s, with Thorny (Lolwutt) as a core contributor - and Krauerlabs's original SpectralFix, the aura fix this project extends to Windower and builds on. The ground-decal fix's core technique (matching a decal draw by its VIEW matrix, then temporarily rescaling WORLD around it) is ported and hardened from an earlier prototype by **Daleterrence** on the FFXIAH forums, who has spent years cataloging FFXI's PC-client graphical bugs and provided the decalfix prototype this fix is built on; see `decalfix/README.md` for more.

By Dellingr.

## License

GPL-3.0-only. See [LICENSE](LICENSE).
