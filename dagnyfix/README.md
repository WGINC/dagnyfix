# Dagnyfix

The production `d3d8.dll` proxy: SpectralFix's aura-aliasing fix and the
flare-billboard depth fix, merged into one DLL.

Both fixes share the same D3D8 proxy-DLL plumbing (there's only one `d3d8.dll`
slot in the game folder), so from Windower's point of view this is the one
tool to install. The two underlying bugs are unrelated and were investigated,
built, and tuned independently:

- Aura-glow fix: `../windower/` (the Windower 4 port of SpectralFix's main
  fix).
- Flare-billboard fix: `../flarefix/` and `../diagnostics/` (the standalone
  investigation and tuning trail for the light-flare depth-sorting bug,
  confirmed working at `kGlowZNudge = 0.4` as flarefix v3.1).

`dagnyfix/src/dagnyfix_proxy.cpp` is the merged proxy shell. It reuses
`windower/src/d3d8_proxy.cpp`'s real-backend-loading and `CreateDevice`-hook
logic essentially unchanged, and ports flarefix v3.1's fix logic in
unmodified (same signature matching, same read-only-Lock + local-copy +
`DrawPrimitiveUP`-resubmission design, same `kGlowZNudge = 0.4`). It still
builds `windower/src/windower_core.cpp` directly (via `../windower/src` in
this project's `CMakeLists.txt`) rather than duplicating it, so the aura fix
itself is untouched by the merge.

## Is this safe to merge? Why the two fixes don't interfere

Both fixes hook a handful of `IDirect3DDevice8` vtable slots. Checked against
each other:

| Slot | Used by aura fix | Used by flare fix |
|---|---|---|
| `CreateDevice` (15, `IDirect3D8`) | shared proxy shell | shared proxy shell |
| `Present` (15, `IDirect3DDevice8`) | yes | yes |
| `DrawPrimitiveUP` (72) | yes (hooked) | yes (real pointer read, never hooked) |
| `CreateTexture` (20) | yes | no |
| `SetTexture` (61) | yes | no |
| `SetRenderState` (50) | no | yes |
| `DrawPrimitive` (70) | no | yes |
| `SetStreamSource` (83) | no | yes |

Only two slots overlap:

- **`Present`**: both fixes just need a once-per-frame tick and correctly
  chain to whatever hook was already installed. Order doesn't matter here.
- **`DrawPrimitiveUP`**: this is the one that needs care. The aura fix
  *hooks* this slot (it's part of its own correction logic). The flare fix
  never hooks it - it only needs to *call* the real, original
  `DrawPrimitiveUP` once per matched draw, to submit its locally-nudged
  vertex copy without touching the game's own vertex buffer. If the flare
  fix read this function pointer from the vtable *after* the aura fix's
  hook were installed, it would end up calling the aura fix's hook instead
  of the real function - meaning every flare-fix draw would also run back
  through the aura fix's own `DrawPrimitiveUP` correction logic, which is
  never what either fix intended, and would tangle the two fixes' behavior
  together in a way neither one was designed or tested for.

  `hook_create_device()` in `dagnyfix_proxy.cpp` avoids that by calling
  `attach_flare_hooks()` **before** `WindowerCore::attach_device()`. The
  flare fix's raw read of the `DrawPrimitiveUP` vtable slot happens first,
  while that slot still holds the true, unhooked engine function -
  guaranteeing the flare fix's synthetic draws bypass the aura fix's hook
  entirely, in both directions. The two attach calls are independently
  wrapped in `try`/`catch` so a failure in either one can never take down
  device creation or the other fix.

No other slot is shared, so nothing else about the merge needed special
handling - it's the same reasoning as running the two original DLLs
side-by-side, minus the part where a single game folder can't actually
hold two `d3d8.dll` files at once.

## Build

```
cmake -S dagnyfix -B build-dagnyfix -A Win32 -D D3D8_SDK_PATH=C:/dev/AshitaADK/ADK
cmake -build build-dagnyfix -config Debug
```

Or through Visual Studio's CMake integration, same as the other tools in
this repo - same `D3D8_SDK_PATH` cache variable, same `-A Win32`
requirement.

Output is `d3d8.dll` (renamed via `OUTPUT_NAME`, same as the other builds).
