# flare_diag

A throwaway diagnostic tool, built to investigate the "distant light flares render
behind models inconsistently" issue - separate from, and unrelated to, the
SpectralFix aura fix. This does not change anything about how the game renders. It
only watches Direct3D8 calls and writes a log. Confirmed reproducible with
dgVoodoo2 completely removed from the chain, so this is investigating a stock FFXI
engine behavior, not a wrapper compatibility bug.

## What it does

Same `d3d8.dll`-proxy trick as SpectralFix's Windower port: this DLL takes the
`d3d8.dll` slot in your game folder, forwards the real Direct3DCreate8 call
through to the actual backend, and hooks a handful of `IDirect3DDevice8` vtable
calls to watch what's happening:

- `SetRenderState` / `SetTexture` (stage 0): tracked passively, never modified.
- `DrawPrimitiveUP`: every call made while the currently-tracked blend state
  "looks additive" (alpha blending on, blend factors are either
  `SRCALPHA`/`ONE` or `ONE`/`ONE` - the standard glow/flare configuration) gets
  its vertex geometry summarized and logged: bounding box, first two vertex
  positions, whether it carries UVs, current ZENABLE/ZWRITEENABLE/ZFUNC, and the
  bound texture pointer.
- `DrawPrimitive`: same filter, but since this draws from a pre-built vertex
  buffer rather than an immediate array, the geometry itself isn't captured in
  general - only render state, texture pointer, and primitive counts. As of
  v6, the one narrow exception is a draw whose state is an exact match for the
  flare's own signature: that specific case gets a single read-only `Lock` on
  just its vertex range, to compute a view-space Z spread across the quad (see
  "v6 changes" above). Every other draw through this hook is left untouched.
- `SetTransform` (as of v6): `D3DTS_WORLD` and `D3DTS_VIEW` are tracked
  passively, purely so a matched `DrawPrimitive`'s vertices can be placed in
  view space after the fact.
- `Present`: frame counter, and a periodic status line every 600 frames.

To keep the log readable, each distinct (texture, approximate quad size)
combination is logged in full only once, then only resurfaces every 600 matching
draws after that. A hard 8 MiB cap stops the log from growing unbounded during a
long play session.

## Build

Same as the main Windower port:

```
cmake -S diagnostics -B build-flare-diag -A Win32 -D D3D8_SDK_PATH=C:/dev/AshitaADK/ADK
cmake --build build-flare-diag --config Debug
```

Or through Visual Studio's CMake integration the same way you're already building
`windower/` - open `diagnostics/` as its own CMake project (or add it as a
sibling target), same `D3D8_SDK_PATH` cache variable, same `-A Win32`
requirement.

Output is `d3d8.dll` (renamed via `OUTPUT_NAME`, same as the real fix).

## What happens if nothing shows up

If the log ends up empty (or only shows the periodic status lines with
`matched_drawprimitiveup=0 matched_drawprimitive=0`), that means the flare
sprites aren't using an additive-looking blend state the way I guessed - which
is itself useful information, not a failure. It would mean the effect is drawn
some other way (a different blend mode entirely, or through a call this tool
doesn't hook yet, like `DrawIndexedPrimitive`), and the filter needs to be
widened rather than the theory being right and the tool just needing more time.
