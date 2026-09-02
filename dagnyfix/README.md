# Dagnyfix

The production `d3d8.dll` proxy: SpectralFix's aura-aliasing fix, the
flare-billboard depth fix, and the ground-decal aspect/zoom fix, merged into
one DLL.

All three fixes share the same D3D8 proxy-DLL plumbing (there's only one
`d3d8.dll` slot in the game folder), so from Windower's point of view this is
the one tool to install. The three underlying bugs are unrelated and were
investigated, built, and tuned independently:

- Aura-glow fix: `../windower/` (the Windower 4 port of SpectralFix's main
  fix).
- Flare-billboard fix: `../flarefix/` and `../diagnostics/` (the standalone
  investigation and tuning trail for the light-flare depth-sorting bug,
  confirmed working at `kGlowZNudge = 0.4` as flarefix v3.1).
- Ground-decal fix: `../decalfix/` (avatar summoning pentagrams,
  ground-targeted spell circles, and certain NPC ground effects rendering at
  the wrong size and aspect ratio; ported and hardened from an earlier
  prototype).

`dagnyfix/src/dagnyfix_proxy.cpp` is the merged proxy shell. It reuses
`windower/src/d3d8_proxy.cpp`'s real-backend-loading and `CreateDevice`-hook
logic essentially unchanged, ports flarefix v3.1's fix logic in unmodified
(same signature matching, same read-only-Lock + local-copy +
`DrawPrimitiveUP`-resubmission design, same `kGlowZNudge = 0.4`), and ports
decalfix's fix logic unmodified (same VIEW-signature detection, same
WORLD-rescale-around-draw design, same `kFovReferenceProjScale = 1.529`). It
still builds `windower/src/windower_core.cpp` directly (via `../windower/src`
in this project's `CMakeLists.txt`) rather than duplicating it, so the aura
fix itself is untouched by the merge.

## Is this safe to merge? Why the three fixes don't interfere

All three fixes hook a handful of `IDirect3DDevice8` vtable slots. Checked
against each other:

| Slot | Aura fix | Flare fix | Decal fix |
|---|---|---|---|
| `CreateDevice` (15, `IDirect3D8`) | shared proxy shell | shared proxy shell | shared proxy shell |
| `Present` (15, `IDirect3DDevice8`) | yes | yes | yes |
| `DrawPrimitiveUP` (72) | yes (hooked) | yes (real pointer read, never hooked) | no |
| `CreateTexture` (20) | yes | no | no |
| `SetTexture` (61) | yes | no | yes (chains through aura fix's hook) |
| `SetRenderState` (50) | no | yes | no |
| `DrawPrimitive` (70) | no | yes | yes (chains through flare fix's hook) |
| `SetStreamSource` (83) | no | yes | no |
| `Reset` (14) | no | no | yes |
| `DrawIndexedPrimitive` (71) | no | no | yes |
| `GetTransform` (38) | no | no | yes (real pointer read, never hooked) |
| `SetTransform` (37) | no | no | yes (real pointer read, never hooked) |

Four slots overlap:

- **`Present`**: all three fixes just need a once-per-frame tick and
  correctly chain to whatever hook was already installed. Order doesn't
  matter here.
- **`DrawPrimitiveUP`**: this is the one that needs care between the aura
  and flare fixes. The aura fix *hooks* this slot (it's part of its own
  correction logic). The flare fix never hooks it - it only needs to *call*
  the real, original `DrawPrimitiveUP` once per matched draw, to submit its
  locally-nudged vertex copy without touching the game's own vertex buffer.
  If the flare fix read this function pointer from the vtable *after* the
  aura fix's hook were installed, it would end up calling the aura fix's
  hook instead of the real function - meaning every flare-fix draw would
  also run back through the aura fix's own `DrawPrimitiveUP` correction
  logic, which is never what either fix intended.

  `hook_create_device()` avoids that by calling `attach_flare_hooks()`
  **before** `WindowerCore::attach_device()`. The flare fix's raw read of
  the `DrawPrimitiveUP` vtable slot happens first, while that slot still
  holds the true, unhooked engine function - guaranteeing the flare fix's
  synthetic draws bypass the aura fix's hook entirely, in both directions.
  The decal fix never touches this slot at all.

- **`SetTexture`**: the decal fix hooks this *after* the aura fix does (see
  attach order below), so its "original" for this slot is the aura fix's
  own hook, not the raw real `SetTexture` - a normal, harmless chain. The
  decal fix only observes which texture landed in stages 0/1 after
  forwarding the call on; it never alters the texture argument, so there's
  nothing for the two fixes to step on each other over.

- **`DrawPrimitive`**: the decal fix hooks this *after* the flare fix does,
  so its "original" is the flare fix's hook. The decal fix wraps the whole
  call - reads `WORLD`, scales it if this draw's bound texture was confirmed
  a decal, calls through original (which runs the flare fix's own
  render-state signature check and either calls the real `DrawPrimitive` or
  bypasses to `DrawPrimitiveUP` for a matched glow billboard), then restores
  `WORLD`. That's correct regardless of which path the flare fix takes
  internally, because the decal fix is only holding a D3D8 device-state
  scope (`WORLD`) open around the call, not touching any buffer contents.
  The two fixes' match domains are also disjoint in practice - flare
  matches by render-state signature plus 36-byte-stride vertex buffers
  (≤8 vertices), decal by VIEW-matrix signature plus a completely different
  texture format - so a single draw matching both at once isn't a realistic
  case, just a safe one if it ever happened.

`hook_create_device()` enforces the full attach order: `attach_flare_hooks()`
first, `WindowerCore::attach_device()` (aura fix) second, `attach_decal_hooks()`
third. All three attach calls are independently wrapped in `try`/`catch` so a
failure in any one of them can never take down device creation or the other
two fixes.

No other slot is shared, so nothing else about the merge needed special
handling - it's the same reasoning as running the three original DLLs
side-by-side, minus the part where a single game folder can't actually hold
three `d3d8.dll` files at once.

## Status

The aura fix and the flare fix (v3.1) are both confirmed working in-game.
The decal fix is untested in-game as of this merge - it compiles and links
clean, but needs the same playtest-and-tune pass the other two fixes went
through, particularly around `kDecalFovReferenceProjScale` (carried over
from an earlier prototype's own calibration, not re-verified here). See
`../decalfix/README.md` for the full list of open items.

## Build

```
cmake -S dagnyfix -B build-dagnyfix -A Win32 -D D3D8_SDK_PATH=C:/dev/AshitaADK/ADK
cmake --build build-dagnyfix --config Release
```

Or through Visual Studio's CMake integration, same as the other tools in
this repo - same `D3D8_SDK_PATH` cache variable, same `-A Win32`
requirement.

Output is `d3d8.dll` (renamed via `OUTPUT_NAME`, same as the other builds).

Needs the [Visual C++ 2015-2022 x86 Redistributable](https://aka.ms/vs/17/release/vc_redist.x86.exe)
on the machine it runs on.
