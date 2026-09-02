// SPDX-License-Identifier: GPL-3.0-only
//
// decalfix project - by Dellingr.
//
// Special thanks to the Ashita devs (RZN, atom0s, and the rest of the core
// team) for the addon/plugin framework this all runs on top of, to
// Krauerlabs for the original SpectralFix aura fix this project builds on,
// and to whoever wrote the earlier prototype this fix is ported and
// hardened from -- the core technique (matching the decal draw by its VIEW
// matrix, then temporarily rescaling WORLD around it) is theirs. That
// prototype confirmed the idea works; this is a from-scratch rewrite of the
// same idea against this project's own conventions, not a copy-paste.
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program. If not, see <https://www.gnu.org/licenses/>.
//
// standalone d3d8.dll proxy that fixes ground-effect decals (avatar
// summoning pentagrams, ground-targeted spell circles, certain NPC ground
// effects) rendering at the wrong size and the wrong aspect ratio: stretched
// into an ellipse on non-square displays, and swelling or shrinking as the
// camera zooms in and out instead of holding still on the ground.
//
// Unrelated to the aura-aliasing fix and the flare depth-sort fix -- shares
// the D3D8 proxy-DLL plumbing because that's the only integration point
// Windower 4 gives us. Evaluate/merge separately, same as flarefix was.
//
// == Root cause ==
//
// These decals are drawn with a fixed, canonical top-down VIEW matrix
// combined with the scene's ordinary perspective PROJECTION -- not with the
// VIEW matrix the rest of the 3D scene uses. Two consequences follow from
// that:
//
//   1. A decal's on-screen size tracks the camera's field of view directly,
//      instead of the size-vs-distance relationship every other piece of
//      scene geometry gets for free from the shared VIEW/PROJECTION setup.
//      Zoom the camera and an avatar's summoning pentagram visibly swells or
//      shrinks, even though nothing about the avatar's actual position or
//      size changed.
//   2. The decal's world-X axis gets squished by 1/aspect somewhere in that
//      same fixed pipeline, so a circle renders as an ellipse on any
//      display that isn't square -- which is every display anyone actually
//      plays on.
//
// == Fix ==
//
// Same core idea as an earlier prototype that got the diagnosis right and
// proved the technique works, but wasn't written to a standard worth
// shipping (its own words: "proto crap"). This is a clean-room rewrite
// against that same diagnosis, not a port of its code:
//
//   1. Identify a decal draw two ways at once, because neither is reliable
//      alone: the VIEW matrix has to match the exact canonical top-down
//      signature (see matches_decal_view_signature -- a loose "any top-down
//      view" match also catches zone shadow draws and corrupts them), AND
//      the currently-bound stage-0/1 texture has to look like a decal
//      texture (small, DXT3-compressed -- an avatar's idle ground glow uses
//      the same draw pipeline but an uncompressed format, so format is what
//      tells them apart).
//   2. Once a texture's been positively identified as a decal texture (or
//      positively ruled out), remember it, so every later draw using that
//      same texture skips straight to the cheap pointer-identity check
//      instead of re-running GetTransform + the signature comparison on
//      every single draw call in the game.
//   3. For a matched draw: read the current WORLD matrix, scale its
//      upper-left 3x3 block to cancel the FOV-tracking bug (uniformly) and
//      the world-X squish (on top of that, X only), issue the real draw
//      with the corrected matrix in place, then restore the original WORLD
//      matrix immediately after. This is exactly how the game already sets
//      a fresh WORLD matrix before every single object's draw call,
//      thousands of times a frame -- ordinary, synchronous, race-free D3D8
//      usage. (Not the same situation as the flare fix's v2 problem, which
//      was about mutating a shared *vertex buffer*'s contents around an
//      asynchronous draw -- this never touches a buffer, only fixed-function
//      transform state, which the D3D8 runtime always serializes correctly
//      with the draw call it belongs to.)
//
// == What changed from the prototype ==
//
// - Real D3D8 interface types throughout (IDirect3DDevice8*,
//   IDirect3DTexture8*, D3DMATRIX, ...) instead of void* and hand-rolled
//   vtable byte offsets -- same reasoning as every other tool in this
//   project: real types are real compiler-checked safety, not paperwork.
// - Checks GetType() == D3DRTYPE_TEXTURE before treating a bound texture as
//   an IDirect3DTexture8 and calling GetLevelDesc/GetSurfaceLevel on it. The
//   prototype skipped this; a cube or volume texture landing in the same
//   stage would have had those calls run against the wrong vtable slots.
// - The known-decal / known-not-decal texture caches are a real fixed-size
//   ring buffer (O(1) circular insert) instead of a flat array shifted one
//   slot at a time on every eviction. Same idea, done as one reusable type
//   used twice instead of two near-identical copies of the same logic.
// - Hooks IDirect3DDevice8::Reset to clear those caches automatically.
//   D3DPOOL_DEFAULT texture pointers get invalidated across a Reset, which
//   correlates closely with zone changes and resolution switches in this
//   game -- the prototype left this as a manual Reset() nobody actually
//   called.
// - The texture-identity check now runs before the VIEW-matrix read, not
//   after -- a texture that's already been ruled in or out skips the
//   GetTransform call and the seven-way signature comparison entirely,
//   instead of paying for both on every draw in the game regardless of
//   whether the bound texture could possibly be a decal.
// - Real logging: attach banner, matched/candidate counts, periodic status
//   line, same shape as every other fix in this project. The prototype had
//   none, which means there was no way to tell from a deployed build
//   whether the caches or the signature match were behaving as intended.
//
// == Known limitations ==
//
// - kFovReferenceProjScale (the FOV-cancel calibration constant) is carried
//   over from the prototype's own observed value. It's dimensionally sound
//   (proj[5] = cot(fovY/2), the standard aspect-independent FOV term -- see
//   the comment on WorldApply), but it's still calibrated at whatever zoom
//   level the prototype's author happened to test at, not verified against
//   this project's own playtest. Watch matched_draw_count and the visual
//   result in-game; retune if it's off.
// - The decal-view signature and the DXT3/size texture heuristic are both
//   carried over from the prototype's own real diagnostic work, not
//   re-derived here -- there's no fresh diagnostic capture behind this fix
//   the way there was for the flare fix. If it misfires on some decal this
//   project hasn't seen tested (a different expansion's summoning effect,
//   for instance), that's the first place to look.
// - Same vertex-format-adjacent assumption as everything else in this
//   family of fixes: this reads and restores whichever WORLD slot the
//   decal actually renders under (D3DTS_WORLD, i.e. D3DTS_WORLDMATRIX(0)),
//   which held for every draw the prototype observed but isn't something
//   D3D8 gives a generic way to confirm going in.

#include <Windows.h>

#include "d3d8/includes/d3d8.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>

namespace
{
    // -------------------------------------------------------------------
    // D3D8 function pointer types
    // -------------------------------------------------------------------

    using Direct3DCreate8Fn = IDirect3D8*(WINAPI*)(UINT);
    using CreateDeviceFn = HRESULT(__stdcall*)(
        IDirect3D8*, UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS*, IDirect3DDevice8**);
    using ResetFn = HRESULT(__stdcall*)(IDirect3DDevice8*, D3DPRESENT_PARAMETERS*);
    using SetTextureFn = HRESULT(__stdcall*)(IDirect3DDevice8*, DWORD, IDirect3DBaseTexture8*);
    using DrawPrimitiveFn = HRESULT(__stdcall*)(IDirect3DDevice8*, D3DPRIMITIVETYPE, UINT, UINT);
    using DrawIndexedPrimitiveFn = HRESULT(__stdcall*)(IDirect3DDevice8*, D3DPRIMITIVETYPE, UINT, UINT, UINT, UINT);
    using GetTransformFn = HRESULT(__stdcall*)(IDirect3DDevice8*, D3DTRANSFORMSTATETYPE, D3DMATRIX*);
    using SetTransformFn = HRESULT(__stdcall*)(IDirect3DDevice8*, D3DTRANSFORMSTATETYPE, const D3DMATRIX*);
    using PresentFn = HRESULT(__stdcall*)(IDirect3DDevice8*, const RECT*, const RECT*, HWND, const RGNDATA*);

    // -------------------------------------------------------------------
    // IDirect3DDevice8 / IDirect3D8 vtable slot indices
    // -------------------------------------------------------------------
    //
    // Fixed by the public D3D8 ABI. Cross-checked against this project's own
    // independently-derived slots elsewhere (SetTexture=61, DrawPrimitive=70,
    // SetTransform=37 all match diagnostics'/flarefix's own numbers) -- good
    // sign the newer ones here (Reset, DrawIndexedPrimitive, GetTransform)
    // are right too.

    constexpr uint32_t kCreateDeviceSlot        = 15; // IDirect3D8::CreateDevice
    constexpr uint32_t kPresentSlot             = 15; // IDirect3DDevice8::Present
    constexpr uint32_t kResetSlot               = 14;
    constexpr uint32_t kSetTextureSlot          = 61;
    constexpr uint32_t kDrawPrimitiveSlot       = 70;
    constexpr uint32_t kDrawIndexedPrimitiveSlot = 71;
    constexpr uint32_t kGetTransformSlot        = 38; // read once at attach, never hooked
    constexpr uint32_t kSetTransformSlot        = 37; // read once at attach, never hooked

    // -------------------------------------------------------------------
    // Decal signature and fix parameters
    // -------------------------------------------------------------------

    // Observed canonical decal VIEW, row-major:
    //   row0 = [-1, 0, 0, 0]   X flipped
    //   row1 = [ 0, 0,-1, 0]   Y/Z swapped
    //   row2 = [ 0,-1, 0, 0]
    //   row3 = [ 0, 0,-2, 1]
    // A looser "any top-down view" match also catches zone shadow draws and
    // corrupts them -- the translation row is what tells them apart.
    bool matches_decal_view_signature(const D3DMATRIX& m)
    {
        if (std::fabs(m._11) < 0.95F) return false;
        if (std::fabs(m._22) > 0.05F) return false;
        if (std::fabs(m._33) > 0.05F) return false;
        if (std::fabs(m._23) < 0.95F && std::fabs(m._32) < 0.95F) return false;
        if (std::fabs(m._41) > 0.1F) return false;
        if (std::fabs(m._42) > 0.1F) return false;
        if (std::fabs(m._43 + 2.0F) > 0.1F) return false;
        return true;
    }

    // Decal textures observed so far: small (<=256 either dimension) and
    // DXT3-compressed. An avatar's idle ground glow goes through this same
    // draw pipeline but is uncompressed A8R8G8B8, which is how the two get
    // told apart despite sharing everything else.
    constexpr UINT kMaxDecalTextureDimension = 256;

    // proj[5] (= cot(fovY/2), the standard aspect-independent FOV term) at
    // the reference zoom level this was calibrated against. 0 disables the
    // FOV cancel and applies kUniformScale flat. Carried over from the
    // prototype's own observed value -- see "Known limitations" above.
    constexpr float kFovReferenceProjScale = 1.529F;
    constexpr float kUniformScale = 1.0F;

    constexpr uint32_t kTextureCacheCapacity = 64;

    constexpr uint64_t kLogEveryMatchUpTo = 20;
    constexpr uint64_t kLogEveryNthMatchAfter = 6000;
    constexpr uint64_t kStatusLineEveryNFrames = 600;
    constexpr uint64_t kMaxLogBytes = 8ULL * 1024 * 1024;

    // -------------------------------------------------------------------
    // Fixed-capacity texture identity cache
    // -------------------------------------------------------------------
    //
    // Two of these run side by side: one for textures confirmed to be
    // decals, one for textures confirmed NOT to be. Real circular buffer --
    // O(1) insert, oldest entry silently overwritten once full -- rather
    // than a flat array shifted down by one on every eviction.

    template <uint32_t Capacity>
    class TextureIdentityCache
    {
    public:
        bool Contains(IDirect3DBaseTexture8* texture) const
        {
            if (texture == nullptr)
                return false;
            for (uint32_t i = 0; i < count_; ++i)
            {
                if (entries_[i] == texture)
                    return true;
            }
            return false;
        }

        void Add(IDirect3DBaseTexture8* texture)
        {
            if (texture == nullptr || Contains(texture))
                return;
            entries_[writeIndex_] = texture;
            writeIndex_ = (writeIndex_ + 1) % Capacity;
            if (count_ < Capacity)
                ++count_;
        }

        void Clear()
        {
            entries_.fill(nullptr);
            writeIndex_ = 0;
            count_ = 0;
        }

        uint32_t Count() const { return count_; }

    private:
        std::array<IDirect3DBaseTexture8*, Capacity> entries_{};
        uint32_t writeIndex_{0};
        uint32_t count_{0};
    };

    // -------------------------------------------------------------------
    // Global state
    // -------------------------------------------------------------------

    HMODULE gRealModule = nullptr;
    Direct3DCreate8Fn gRealDirect3DCreate8 = nullptr;
    CreateDeviceFn gOriginalCreateDevice = nullptr;
    void** gCreateDeviceSlot = nullptr;
    thread_local bool gInsideCreateDevice = false;

    void** gDeviceVtable = nullptr;
    ResetFn gOriginalReset = nullptr;
    SetTextureFn gOriginalSetTexture = nullptr;
    DrawPrimitiveFn gOriginalDrawPrimitive = nullptr;
    DrawIndexedPrimitiveFn gOriginalDrawIndexedPrimitive = nullptr;
    PresentFn gOriginalPresent = nullptr;
    thread_local bool gInsideReset = false;
    thread_local bool gInsideSetTexture = false;
    thread_local bool gInsideDrawPrimitive = false;
    thread_local bool gInsideDrawIndexedPrimitive = false;
    thread_local bool gInsidePresent = false;

    // Real GetTransform/SetTransform, grabbed straight from the vtable, never
    // hooked -- we only ever need to call these ourselves around a matched
    // draw, not see the game's own calls to them.
    GetTransformFn gRealGetTransform = nullptr;
    SetTransformFn gRealSetTransform = nullptr;

    // Whatever's currently bound to texture stages 0 and 1, tracked passively
    // from the SetTexture hook after forwarding to the real implementation.
    IDirect3DBaseTexture8* gStage0Texture = nullptr;
    IDirect3DBaseTexture8* gStage1Texture = nullptr;

    TextureIdentityCache<kTextureCacheCapacity> gKnownDecalTextures;
    TextureIdentityCache<kTextureCacheCapacity> gKnownNonDecalTextures;

    uint64_t gFrame = 0;
    uint64_t gMatchedDrawCount = 0;
    uint64_t gTextureIdentifyAttempts = 0;
    FILE* gLogFile = nullptr;
    uint64_t gLogBytesWritten = 0;

    // -------------------------------------------------------------------
    // Logging
    // -------------------------------------------------------------------

    std::string module_directory()
    {
        HMODULE self = nullptr;
        ::GetModuleHandleExA(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
            reinterpret_cast<LPCSTR>(&module_directory), &self);
        std::array<char, MAX_PATH> path{};
        const auto length = ::GetModuleFileNameA(self, path.data(), static_cast<DWORD>(path.size()));
        if (length == 0)
            return {};
        std::string full(path.data(), length);
        const auto slash = full.find_last_of("\\/");
        return slash == std::string::npos ? std::string() : full.substr(0, slash + 1);
    }

    void ensure_log_open()
    {
        if (gLogFile != nullptr)
            return;
        const auto dir = module_directory() + "logs\\decalfix\\";
        ::CreateDirectoryA((module_directory() + "logs").c_str(), nullptr);
        ::CreateDirectoryA(dir.c_str(), nullptr);
        gLogFile = std::fopen((dir + "decalfix.log").c_str(), "a");
    }

    void log_line(const std::string& line)
    {
        ensure_log_open();
        if (gLogFile == nullptr || gLogBytesWritten >= kMaxLogBytes)
            return;
        const auto now = std::time(nullptr);
        std::tm local{};
        localtime_s(&local, &now);
        char stamp[32]{};
        std::strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &local);
        const auto full = std::string(stamp) + " | " + line + "\n";
        std::fwrite(full.data(), 1, full.size(), gLogFile);
        std::fflush(gLogFile);
        gLogBytesWritten += full.size();
    }

    // -------------------------------------------------------------------
    // Decal texture identification
    // -------------------------------------------------------------------

    // Cheap pointer-identity checks only -- caller is expected to have
    // already bailed if this returns kUnknown-equivalent (see
    // identify_texture_if_needed, which is what actually runs the expensive
    // path below).
    enum class TextureClass { Unknown, Decal, NotDecal };

    TextureClass classify_known_texture(IDirect3DBaseTexture8* texture)
    {
        if (texture == nullptr)
            return TextureClass::NotDecal;
        if (gKnownDecalTextures.Contains(texture))
            return TextureClass::Decal;
        if (gKnownNonDecalTextures.Contains(texture))
            return TextureClass::NotDecal;
        return TextureClass::Unknown;
    }

    // Only ever called for a texture classify_known_texture just said it
    // doesn't recognize yet. Checks GetType() before touching any
    // IDirect3DTexture8-specific method -- a cube or volume texture bound to
    // the same stage would otherwise get GetLevelDesc/GetSurfaceLevel called
    // against the wrong vtable slots entirely.
    void identify_unknown_texture(IDirect3DBaseTexture8* texture)
    {
        ++gTextureIdentifyAttempts;

        if (texture->GetType() != D3DRTYPE_TEXTURE)
        {
            gKnownNonDecalTextures.Add(texture);
            return;
        }

        auto* texture2d = static_cast<IDirect3DTexture8*>(texture);
        D3DSURFACE_DESC desc{};
        if (FAILED(texture2d->GetLevelDesc(0, &desc)))
        {
            gKnownNonDecalTextures.Add(texture);
            return;
        }

        const bool looksLikeDecal = desc.Width <= kMaxDecalTextureDimension
            && desc.Height <= kMaxDecalTextureDimension
            && desc.Format == D3DFMT_DXT3;

        if (looksLikeDecal)
            gKnownDecalTextures.Add(texture);
        else
            gKnownNonDecalTextures.Add(texture);
    }

    // Called on every candidate draw. Deliberately checks the cheap cache
    // lookup BEFORE ever reading the VIEW matrix -- the overwhelming
    // majority of draws in a real frame use a texture already classified one
    // way or the other, and should never pay for a GetTransform call plus
    // a seven-way signature comparison just to find that out again.
    void identify_texture_if_needed(IDirect3DDevice8* device, D3DMATRIX* viewScratch, bool* viewScratchValid)
    {
        if (classify_known_texture(gStage0Texture) != TextureClass::Unknown)
            return;

        if (!*viewScratchValid)
        {
            if (gRealGetTransform == nullptr
                || FAILED(gRealGetTransform(device, D3DTS_VIEW, viewScratch))
                || !matches_decal_view_signature(*viewScratch))
                return;
            *viewScratchValid = true;
        }
        else if (!matches_decal_view_signature(*viewScratch))
        {
            return;
        }

        identify_unknown_texture(gStage0Texture);
    }

    // -------------------------------------------------------------------
    // WORLD scale-around-draw
    // -------------------------------------------------------------------

    struct WorldSave
    {
        bool applied{false};
        D3DMATRIX matrix{};
    };

    // Scales the upper-left 3x3 block of the current WORLD matrix: uniformly
    // by kUniformScale * (kFovReferenceProjScale / proj[5]) to cancel the
    // FOV-tracking bug (proj[5] = cot(fovY/2) is the standard
    // aspect-independent term perspective projection scales screen size by,
    // so this re-derives the size-vs-zoom relationship the decal's fixed
    // pipeline skips), and on top of that scales world-X specifically by an
    // extra proj[5]/proj[0] to cancel the aspect squish. That second factor
    // only applies under the decal VIEW -- the same decal textures also turn
    // up on ordinary vertical quads (rising light pillars) drawn under the
    // scene's normal VIEW, where the aspect correction would be wrong.
    void apply_world_scale(IDirect3DDevice8* device, WorldSave& save)
    {
        if (gRealGetTransform == nullptr || gRealSetTransform == nullptr)
            return;
        if (FAILED(gRealGetTransform(device, D3DTS_WORLD, &save.matrix)))
            return;

        float uniformScale = kUniformScale;
        float extraXScale = 1.0F;

        if (kFovReferenceProjScale > 0.0F)
        {
            D3DMATRIX projection{};
            if (SUCCEEDED(gRealGetTransform(device, D3DTS_PROJECTION, &projection)) && projection._22 > 0.0001F)
            {
                uniformScale = kUniformScale * (kFovReferenceProjScale / projection._22);

                if (projection._11 > 0.0001F)
                {
                    D3DMATRIX view{};
                    if (SUCCEEDED(gRealGetTransform(device, D3DTS_VIEW, &view)) && matches_decal_view_signature(view))
                        extraXScale = projection._22 / projection._11;
                }
            }
        }

        D3DMATRIX scaled = save.matrix;
        scaled._11 *= uniformScale * extraXScale;
        scaled._12 *= uniformScale;
        scaled._13 *= uniformScale;
        scaled._21 *= uniformScale * extraXScale;
        scaled._22 *= uniformScale;
        scaled._23 *= uniformScale;
        scaled._31 *= uniformScale * extraXScale;
        scaled._32 *= uniformScale;
        scaled._33 *= uniformScale;

        gRealSetTransform(device, D3DTS_WORLD, &scaled);
        save.applied = true;
    }

    void restore_world(IDirect3DDevice8* device, const WorldSave& save)
    {
        if (!save.applied || gRealSetTransform == nullptr)
            return;
        gRealSetTransform(device, D3DTS_WORLD, &save.matrix);
    }

    bool current_draw_targets_decal()
    {
        return classify_known_texture(gStage0Texture) == TextureClass::Decal
            || classify_known_texture(gStage1Texture) == TextureClass::Decal;
    }

    void log_match(const char* drawKind)
    {
        ++gMatchedDrawCount;
        if (gMatchedDrawCount <= kLogEveryMatchUpTo || (gMatchedDrawCount % kLogEveryNthMatchAfter) == 0)
        {
            log_line(
                std::string("frame=") + std::to_string(gFrame) + " decal draw (" + drawKind
                + ") WORLD rescaled for this draw only, matched_draw_count=" + std::to_string(gMatchedDrawCount)
                + " known_decal_textures=" + std::to_string(gKnownDecalTextures.Count())
                + " known_non_decal_textures=" + std::to_string(gKnownNonDecalTextures.Count()));
        }
    }

    // -------------------------------------------------------------------
    // Hooks
    // -------------------------------------------------------------------

    HRESULT __stdcall hook_set_texture(IDirect3DDevice8* self, DWORD stage, IDirect3DBaseTexture8* texture)
    {
        const auto original = gOriginalSetTexture;
        if (gInsideSetTexture || original == nullptr)
            return original != nullptr ? original(self, stage, texture) : D3DERR_INVALIDCALL;
        gInsideSetTexture = true;
        const auto result = original(self, stage, texture);
        gInsideSetTexture = false;

        if (stage == 0)
            gStage0Texture = texture;
        else if (stage == 1)
            gStage1Texture = texture;

        return result;
    }

    HRESULT __stdcall hook_draw_primitive(
        IDirect3DDevice8* self, D3DPRIMITIVETYPE primitiveType, UINT startVertex, UINT primitiveCount)
    {
        const auto original = gOriginalDrawPrimitive;
        if (gInsideDrawPrimitive || original == nullptr)
            return original != nullptr ? original(self, primitiveType, startVertex, primitiveCount) : D3DERR_INVALIDCALL;
        gInsideDrawPrimitive = true;

        D3DMATRIX viewScratch{};
        bool viewScratchValid = false;
        identify_texture_if_needed(self, &viewScratch, &viewScratchValid);

        WorldSave save;
        const bool matched = current_draw_targets_decal();
        if (matched)
            apply_world_scale(self, save);

        const auto result = original(self, primitiveType, startVertex, primitiveCount);

        if (matched)
        {
            restore_world(self, save);
            log_match("DrawPrimitive");
        }

        gInsideDrawPrimitive = false;
        return result;
    }

    HRESULT __stdcall hook_draw_indexed_primitive(
        IDirect3DDevice8* self, D3DPRIMITIVETYPE primitiveType, UINT minIndex, UINT numVertices,
        UINT startIndex, UINT primitiveCount)
    {
        const auto original = gOriginalDrawIndexedPrimitive;
        if (gInsideDrawIndexedPrimitive || original == nullptr)
        {
            return original != nullptr
                ? original(self, primitiveType, minIndex, numVertices, startIndex, primitiveCount)
                : D3DERR_INVALIDCALL;
        }
        gInsideDrawIndexedPrimitive = true;

        D3DMATRIX viewScratch{};
        bool viewScratchValid = false;
        identify_texture_if_needed(self, &viewScratch, &viewScratchValid);

        WorldSave save;
        const bool matched = current_draw_targets_decal();
        if (matched)
            apply_world_scale(self, save);

        const auto result = original(self, primitiveType, minIndex, numVertices, startIndex, primitiveCount);

        if (matched)
        {
            restore_world(self, save);
            log_match("DrawIndexedPrimitive");
        }

        gInsideDrawIndexedPrimitive = false;
        return result;
    }

    HRESULT __stdcall hook_reset(IDirect3DDevice8* self, D3DPRESENT_PARAMETERS* presentationParameters)
    {
        const auto original = gOriginalReset;
        if (gInsideReset || original == nullptr)
            return original != nullptr ? original(self, presentationParameters) : D3DERR_INVALIDCALL;
        gInsideReset = true;
        const auto result = original(self, presentationParameters);
        gInsideReset = false;

        // D3DPOOL_DEFAULT texture pointers are invalidated across a Reset,
        // which correlates closely with zone changes and resolution
        // switches in this game. A stale cache entry after this point would
        // scale whatever unrelated texture the game recycles that address
        // for next.
        gKnownDecalTextures.Clear();
        gKnownNonDecalTextures.Clear();
        gStage0Texture = nullptr;
        gStage1Texture = nullptr;
        log_line("device Reset: texture identity caches cleared");

        return result;
    }

    HRESULT __stdcall hook_present(
        IDirect3DDevice8* self, const RECT* sourceRect, const RECT* destRect, HWND destWindow, const RGNDATA* dirtyRegion)
    {
        const auto original = gOriginalPresent;
        if (gInsidePresent || original == nullptr)
            return original != nullptr ? original(self, sourceRect, destRect, destWindow, dirtyRegion) : D3DERR_INVALIDCALL;
        gInsidePresent = true;
        const auto result = original(self, sourceRect, destRect, destWindow, dirtyRegion);
        gInsidePresent = false;

        ++gFrame;
        if (gFrame % kStatusLineEveryNFrames == 0)
        {
            log_line(
                "status[periodic] frames=" + std::to_string(gFrame)
                + " matched_draw_count=" + std::to_string(gMatchedDrawCount)
                + " texture_identify_attempts=" + std::to_string(gTextureIdentifyAttempts)
                + " known_decal_textures=" + std::to_string(gKnownDecalTextures.Count())
                + " known_non_decal_textures=" + std::to_string(gKnownNonDecalTextures.Count()));
        }
        return result;
    }

    // -------------------------------------------------------------------
    // Vtable patching
    // -------------------------------------------------------------------

    bool write_vtable_slot(void** slot, void* value)
    {
        if (slot == nullptr || value == nullptr)
            return false;
        DWORD oldProtect = 0;
        if (!::VirtualProtect(slot, sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect))
            return false;
        ::InterlockedExchangePointer(reinterpret_cast<PVOID volatile*>(slot), value);
        DWORD ignored = 0;
        ::VirtualProtect(slot, sizeof(void*), oldProtect, &ignored);
        ::FlushInstructionCache(::GetCurrentProcess(), slot, sizeof(void*));
        return *slot == value;
    }

    void attach_device(IDirect3DDevice8* device)
    {
        auto*** object = reinterpret_cast<void***>(device);
        if (object == nullptr || *object == nullptr)
            return;
        gDeviceVtable = *object;

        const auto hookOne = [](void** vtable, uint32_t slotIndex, void* hook, void** originalStorageAsVoidPtr) {
            auto* slot = &vtable[slotIndex];
            const auto previous = *slot;
            if (previous == hook)
                return; // already hooked (e.g. second CreateDevice call)
            *originalStorageAsVoidPtr = previous;
            write_vtable_slot(slot, hook);
        };

        hookOne(gDeviceVtable, kResetSlot, reinterpret_cast<void*>(&hook_reset),
            reinterpret_cast<void**>(&gOriginalReset));
        hookOne(gDeviceVtable, kSetTextureSlot, reinterpret_cast<void*>(&hook_set_texture),
            reinterpret_cast<void**>(&gOriginalSetTexture));
        hookOne(gDeviceVtable, kDrawPrimitiveSlot, reinterpret_cast<void*>(&hook_draw_primitive),
            reinterpret_cast<void**>(&gOriginalDrawPrimitive));
        hookOne(gDeviceVtable, kDrawIndexedPrimitiveSlot, reinterpret_cast<void*>(&hook_draw_indexed_primitive),
            reinterpret_cast<void**>(&gOriginalDrawIndexedPrimitive));
        hookOne(gDeviceVtable, kPresentSlot, reinterpret_cast<void*>(&hook_present),
            reinterpret_cast<void**>(&gOriginalPresent));

        // Not hooks -- just grab the real pointers once so apply_world_scale
        // can call them directly for a matched draw.
        gRealGetTransform = reinterpret_cast<GetTransformFn>(gDeviceVtable[kGetTransformSlot]);
        gRealSetTransform = reinterpret_cast<SetTransformFn>(gDeviceVtable[kSetTransformSlot]);

        log_line(
            "decalfix attached to device; draws whose bound stage-0/1 texture is confirmed a ground-decal "
            "texture (<=" + std::to_string(kMaxDecalTextureDimension) + "px, DXT3, seen under the canonical "
            "top-down decal VIEW) get their WORLD matrix rescaled for that draw only -- FOV-cancel ref="
            + std::to_string(kFovReferenceProjScale) + ", cleared on every device Reset.");
    }

    // -------------------------------------------------------------------
    // d3d8.dll proxy shell
    // -------------------------------------------------------------------
    //
    // Same structure as flarefix_proxy.cpp / windower/src/d3d8_proxy.cpp:
    // sits in for "d3d8.dll" via normal DLL search order, forwards every
    // real export to the actual backend, and hooks CreateDevice just long
    // enough to attach once the real device exists.

    bool load_real_backend()
    {
        if (gRealModule != nullptr)
            return true;
        const auto dir = module_directory();
        constexpr std::array<const char*, 2> known = {"d3d8_dgvoodoo.dll", "d3d8_orig.dll"};
        for (const auto* name : known)
        {
            const auto candidate = dir + name;
            if (::GetFileAttributesA(candidate.c_str()) == INVALID_FILE_ATTRIBUTES)
                continue;
            gRealModule = ::LoadLibraryA(candidate.c_str());
            if (gRealModule != nullptr)
                break;
        }
        if (gRealModule == nullptr)
        {
            std::array<char, MAX_PATH> systemDir{};
            const auto length = ::GetSystemDirectoryA(systemDir.data(), static_cast<UINT>(systemDir.size()));
            if (length == 0)
                return false;
            const auto systemPath = std::string(systemDir.data(), length) + "\\d3d8.dll";
            gRealModule = ::LoadLibraryA(systemPath.c_str());
        }
        if (gRealModule == nullptr)
            return false;
        gRealDirect3DCreate8 = reinterpret_cast<Direct3DCreate8Fn>(::GetProcAddress(gRealModule, "Direct3DCreate8"));
        return gRealDirect3DCreate8 != nullptr;
    }

    HRESULT __stdcall hook_create_device(
        IDirect3D8* self, UINT adapter, D3DDEVTYPE deviceType, HWND focusWindow, DWORD behaviorFlags,
        D3DPRESENT_PARAMETERS* presentationParameters, IDirect3DDevice8** returnedDeviceInterface)
    {
        const auto original = gOriginalCreateDevice;
        if (original == nullptr)
            return D3DERR_INVALIDCALL;
        if (gInsideCreateDevice)
            return original(self, adapter, deviceType, focusWindow, behaviorFlags, presentationParameters, returnedDeviceInterface);

        gInsideCreateDevice = true;
        const auto result = original(self, adapter, deviceType, focusWindow, behaviorFlags, presentationParameters, returnedDeviceInterface);
        gInsideCreateDevice = false;

        if (SUCCEEDED(result) && returnedDeviceInterface != nullptr && *returnedDeviceInterface != nullptr)
        {
            try { attach_device(*returnedDeviceInterface); } catch (...) {}
        }
        return result;
    }

    void hook_create_device_slot(IDirect3D8* d3d8)
    {
        auto*** object = reinterpret_cast<void***>(d3d8);
        if (object == nullptr || *object == nullptr)
            return;
        gCreateDeviceSlot = &(*object)[kCreateDeviceSlot];
        const auto previous = *gCreateDeviceSlot;
        if (previous == nullptr || previous == reinterpret_cast<void*>(&hook_create_device))
            return;
        gOriginalCreateDevice = reinterpret_cast<CreateDeviceFn>(previous);
        write_vtable_slot(gCreateDeviceSlot, reinterpret_cast<void*>(&hook_create_device));
    }
}

extern "C" IDirect3D8* WINAPI Direct3DCreate8(UINT sdkVersion)
{
    if (!load_real_backend())
        return nullptr;
    log_line("=== decalfix proxy loaded ===");
    const auto real = gRealDirect3DCreate8(sdkVersion);
    if (real != nullptr)
        hook_create_device_slot(real);
    return real;
}

extern "C" void WINAPI DebugSetMute() {}

extern "C" HRESULT WINAPI ValidatePixelShader(const DWORD*, const DWORD*, BOOL, DWORD*)
{
    return E_NOTIMPL;
}

extern "C" HRESULT WINAPI ValidateVertexShader(const DWORD*, const DWORD*, const DWORD*, BOOL, DWORD*)
{
    return E_NOTIMPL;
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
    switch (reason)
    {
        case DLL_PROCESS_ATTACH:
            ::DisableThreadLibraryCalls(module);
            break;
        case DLL_PROCESS_DETACH:
            if (gLogFile != nullptr)
            {
                std::fclose(gLogFile);
                gLogFile = nullptr;
            }
            if (gRealModule != nullptr)
            {
                ::FreeLibrary(gRealModule);
                gRealModule = nullptr;
            }
            break;
        default:
            break;
    }
    return TRUE;
}
