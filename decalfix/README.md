# decalfix

A standalone `d3d8.dll` proxy that fixes ground-effect decals -- avatar
summoning pentagrams, ground-targeted spell circles, and certain NPC ground
effects -- rendering at the wrong size and the wrong aspect ratio: stretched
into an ellipse on non-square displays, and swelling or shrinking as the
camera zooms instead of holding still on the ground.

This is a **separate, unrelated** issue from the aura-aliasing fix and the
flare depth-sort fix. The three happen to share the same D3D8 proxy-DLL
plumbing because that is the only available integration point on Windower 4,
not because the underlying bugs are related. Evaluate and merge each
independently -- this fix is also merged into `../dagnyfix/`, the combined
production build.

## Root cause

These decals are drawn with a fixed, canonical top-down VIEW matrix combined
with the scene's ordinary perspective PROJECTION -- not the VIEW matrix the
rest of the 3D scene uses. Two consequences follow:

1. A decal's on-screen size tracks the camera's field of view directly,
   instead of getting the normal size-vs-distance falloff every other piece
   of scene geometry gets for free. Zoom the camera and an avatar's
   pentagram visibly swells or shrinks, even though nothing about the
   avatar's position or size changed.
2. The decal's world-X axis gets squished by 1/aspect somewhere in that
   same fixed pipeline, so a circle renders as an ellipse on any display
   that isn't square.

## The fix

Same diagnosis as an earlier prototype fix that proved the technique works
but, in its own author's words, wasn't "up to even a good standard of
quality." This is a clean-room rewrite of that idea against this project's
own conventions, not a port of its code:

1. Identify a decal draw two ways at once, because neither signal alone is
   reliable: the VIEW matrix has to match the exact canonical top-down
   signature (a looser "any top-down view" match also catches zone-shadow
   draws and corrupts them), **and** the bound stage-0/1 texture has to look
   like a decal texture (small, DXT3-compressed -- an avatar's idle ground
   glow uses the same draw pipeline but an uncompressed format, so format is
   what tells them apart).
2. Once a texture's been positively identified as a decal texture (or ruled
   out), remember it, so later draws using that texture skip straight to a
   cheap pointer check instead of re-running the signature comparison on
   every draw call in the game.
3. For a matched draw: read the current WORLD matrix, scale its upper-left
   3x3 block to cancel the FOV-tracking bug (uniformly) and the world-X
   squish (X only, on top of that), issue the real draw with the corrected
   matrix in place, then restore the original WORLD matrix immediately
   after. This is exactly how the game already sets a fresh WORLD matrix
   before every object's draw call, thousands of times a frame -- ordinary,
   synchronous, race-free D3D8 usage. It is not the same situation as
   flarefix v2's race, which mutated a shared *vertex buffer's* contents
   around an asynchronous draw; this never touches a buffer, only
   fixed-function transform state, which the D3D8 runtime always serializes
   correctly with the draw call it belongs to.

## What changed from the prototype

- Real D3D8 interface types throughout (`IDirect3DDevice8*`,
  `IDirect3DTexture8*`, `D3DMATRIX`, ...) instead of `void*` and hand-rolled
  vtable byte offsets.
- Checks `GetType() == D3DRTYPE_TEXTURE` before treating a bound texture as
  an `IDirect3DTexture8` and calling `GetLevelDesc` on it. The prototype
  skipped this check; a cube or volume texture landing in the same stage
  would have hit the wrong vtable slots.
- The known-decal / known-not-decal texture caches are a real fixed-size
  ring buffer (O(1) circular insert) implemented once as a reusable type,
  instead of two near-identical hand-written arrays shifted one slot at a
  time on every eviction.
- Hooks `IDirect3DDevice8::Reset` to clear those caches automatically.
  `D3DPOOL_DEFAULT` texture pointers get invalidated across a Reset, which
  correlates closely with zone changes and resolution switches -- the
  prototype left this as a manual `Reset()` nobody actually called.
- The texture-identity check now runs before the VIEW-matrix read, not
  after: a texture already ruled in or out skips the `GetTransform` call
  and the signature comparison entirely, instead of paying for both on
  every draw in the game regardless of whether the bound texture could
  possibly be a decal.
- Real logging: attach banner, matched/candidate counts, periodic status
  line, same shape as every other fix in this project. The prototype had
  none, so there was no way to tell from a deployed build whether the
  caches or the signature match were behaving as intended.

## Status / open questions

- **Untested in-game.** This has been cross-compiled and linked clean
  (mingw, 32-bit, C++17) and, merged, links clean as part of `dagnyfix`
  too, but hasn't been run against a live client. Needs the same
  playtest-and-tune cycle every other fix in this project went through
  before being trusted.
- **`kFovReferenceProjScale` (currently `1.529`)** is carried over from the
  prototype's own observed value. It's dimensionally sound -- `proj[5] =
  cot(fovY/2)`, the standard aspect-independent FOV term -- but it's
  calibrated at whatever zoom level the prototype's author tested at, not
  verified against a playtest of this rewrite. Watch `matched_draw_count`
  and the visual result in-game; retune if it's off.
- The decal-view signature and the DXT3/size texture heuristic are both
  carried over from the prototype's own diagnostic work, not re-derived
  here -- there's no fresh diagnostic capture behind this fix the way there
  was for flarefix. If it misfires on some decal this project hasn't seen
  yet, that's the first place to look.
- Assumes decals are drawn under vtable slot `D3DTS_WORLDMATRIX(0)`
  (`WORLD0`), matching every other transform assumption already used
  elsewhere in this project.
- The header currently credits the original prototype's author only as
  "whoever wrote the earlier prototype this fix is ported and hardened
  from" -- if you know who that is and want them named specifically, say so
  and it'll be updated.

## Build

Same as the other tools in this repo:

```
cmake -S decalfix -B build-decalfix -A Win32 -D D3D8_SDK_PATH=C:/dev/AshitaADK/ADK
cmake --build build-decalfix --config Debug
```

Or through Visual Studio's CMake integration -- same `D3D8_SDK_PATH` cache
variable, same `-A Win32` requirement.

Output is `d3d8.dll` (renamed via `OUTPUT_NAME`, same as the other builds).

## Install

Same rename convention as every other tool here: if you already have a
`d3d8.dll` in your FFXI folder, rename it to `d3d8_dgvoodoo.dll` (if it's
dgVoodoo2) or `d3d8_orig.dll` (anything else) before dropping this one in --
the proxy auto-detects and chains to it.

Logs are written to `logs\decalfix\decalfix.log` under the FFXI install
directory.
