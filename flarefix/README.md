# flarefix

A standalone `d3d8.dll` proxy that attempts to fix a distant light-flare
depth-sorting bug in the retail FFXI client: light-source flares (lamps,
lanterns) render behind nearby models inconsistently instead of in front of
them, flickering between correct and incorrect from frame to frame.

This is a **separate, unrelated** issue from the aura-aliasing bug SpectralFix's
main fix addresses. The two happen to share the same D3D8 proxy-DLL plumbing
because that is the only available integration point on Windower 4, not
because the underlying bugs are related. Evaluate and merge each
independently.

Full investigation trail, including the companion diagnostic tool that
isolated the target geometry, is in `../diagnostics/`.

## Root cause

The diagnostic tool identified a small, consistent group of draw calls
sharing one exact render state:

- Submitted through `IDirect3DDevice8::DrawPrimitive` from a real vertex
  buffer with a 36-byte vertex stride - an ordinary world-space vertex
  format. Unlike the game's UI and particle-effect quads (which use a
  pre-transformed, screen-space vertex format), this geometry goes through
  the normal 3D transform and depth-test pipeline. It is a genuine
  3D-positioned billboard, not a screen overlay.
- `D3DRS_ZENABLE = TRUE`: depth-tested against the rest of the scene, so it
  can legitimately be occluded by real geometry.
- `D3DRS_ZWRITEENABLE = FALSE`: does not write to the depth buffer itself, as
  expected for a translucent effect.
- `D3DRS_ALPHABLENDENABLE = TRUE`, `SRCBLEND = D3DBLEND_SRCALPHA`,
  `DESTBLEND = D3DBLEND_ONE`: a soft-additive blend, the standard technique
  for a light glow / lens flare.
- Drawn as a single quad (`D3DPT_TRIANGLELIST`, 2 triangles, 6 vertices).

Because this geometry is depth-tested as ordinary 3D content, the reported
symptom looked initially consistent with depth-buffer precision loss at
range - but that theory was ruled out empirically:
forcing dgVoodoo2's depth buffer to higher bit depths made the symptom worse
or no better, and a later diagnostic pass confirmed the billboard itself is
a flawless camera-facing quad with no per-vertex depth variance. The current
working explanation is a fixed positional offset between the billboard's
anchor and its lamp housing's own surface - see below.


## Status / open questions

- **Vertex format assumption**: this fix assumes the first 12 bytes of each
  vertex are an untransformed `(x, y, z)` position, which held for every
  matched draw seen in diagnostic data so far, but isn't something the D3D8
  API exposes a generic way to confirm without parsing the active FVF.
- **`kGlowZNudge` is still a starting value** (currently 1.0 world units,
  carried over from the last v2 test) - v2's flicker meant the underlying
  approach needed fixing before this value could really be evaluated. Needs
  fresh in-game testing under v3 to tune properly.
- **Lock failures**: v3's Lock is `D3DLOCK_READONLY`, generally less likely
  to be rejected than v2's read/write lock, but an unusual driver could
  still refuse it. `flarefix.log` reports `lock_failures` in both per-draw
  and periodic status lines - if that climbs, matched draws are falling
  back to the real, unmodified `DrawPrimitive` rather than being nudged.
- **Native D3D8** (no dgVoodoo2 in the chain): not yet tested with v3.
  Testing under dgVoodoo2 removed was previously blocked by an unrelated
  login/network error, diagnosed as the proxy's fallback to the system
  `d3d8.dll` failing to find a working backend on the test machine -
  resolving that needs either a real working `d3d8.dll` placed as
  `d3d8_orig.dll` for the proxy to chain to, or confirmation the system
  fallback path works independent of this proxy.

`flarefix.log` reports `fixed_draw_count` (how many draws matched the
signature) and `lock_failures` (how many of those fell back to the
unmodified draw) - check both alongside the visual result. A climbing
`fixed_draw_count` with a flat `lock_failures` and no visible improvement
now points at `kGlowZNudge` needing tuning, not at a matching, locking, or
race problem.

## Build

Same as SpectralFix's Windower port and the diagnostic tool:

```
cmake -S flarefix -B build-flarefix -A Win32 -D D3D8_SDK_PATH=C:/dev/AshitaADK/ADK
cmake --build build-flarefix --config Debug
```

Or through Visual Studio's CMake integration the same way `windower/` and
`diagnostics/` are built - same `D3D8_SDK_PATH` cache variable, same
`-A Win32` requirement.

Output is `d3d8.dll` (renamed via `OUTPUT_NAME`, same as the other builds).