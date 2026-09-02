// SPDX-License-Identifier: GPL-3.0-only
//
// Dagnyfix project - by Dellingr.
//
// Special thanks to the Ashita devs (RZN, atom0s, and the rest of the core
// team) for the addon/plugin framework this all runs on top of, and to
// Krauerlabs for the original SpectralFix aura fix this project builds on.
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
// Dagnyfix - merges SpectralFix's Windower aura fix, the standalone flare
// depth-offset fix, and the ground-decal aspect/zoom fix into one d3d8.dll
// proxy, so it's one DLL instead of three you'd have to choose between or
// chain.
//
// =============================================================================
// Merge notes / why this is safe
// =============================================================================
//
// Same "d3d8.dll" slot-takeover all three source tools used (full history in
// windower/src/d3d8_proxy.cpp, flarefix/flarefix_proxy.cpp, and
// decalfix/README.md). We hook CreateDevice once and hand the real device to
// all three fixes, in this order:
//
//   1. attach_flare_hooks() FIRST - installs the flare hooks (SetRenderState,
//      DrawPrimitive, SetStreamSource, Present) and reads the real
//      DrawPrimitiveUP pointer without hooking it. Order matters here, see below.
//   2. WindowerCore::attach_device() (aura fix, unmodified) SECOND - hooks
//      CreateTexture, SetTexture, DrawPrimitiveUP, Present.
//   3. attach_decal_hooks() THIRD - hooks Reset, SetTexture, DrawPrimitive,
//      DrawIndexedPrimitive, Present, and reads the real GetTransform/
//      SetTransform pointers without hooking them.
//
// Slot overlap across the three fixes:
//   Flare: SetRenderState(50), DrawPrimitive(70), SetStreamSource(83)
//   Aura:  CreateTexture(20), SetTexture(61)
//   Decal: Reset(14), SetTexture(61), DrawPrimitive(70), DrawIndexedPrimitive(71)
// Collisions: DrawPrimitiveUP(72) between flare/aura; SetTexture(61) between
// aura/decal; DrawPrimitive(70) between flare/decal; Present(15) across all
// three.
//
//   - DrawPrimitiveUP: aura fix hooks this (its quads go through here and get
//     corrected in flight). Flare fix never hooks it - just needs the raw
//     pointer to fire its own already-nudged synthetic draws straight past
//     the game's vertex buffer. THIS is why the order above matters: we grab
//     that pointer before the aura fix's hook goes on the slot, so the flare
//     fix always calls the true DrawPrimitiveUP and its synthetic draws never
//     get run back through the aura fix's selector/correction logic. Decal
//     fix never touches this slot at all.
//   - SetTexture: decal fix hooks this AFTER the aura fix does (attach order
//     above), so decal's "original" for this slot is the aura fix's own
//     hook, not the raw real SetTexture - a normal, harmless chain. Decal
//     only observes which texture landed in stages 0/1 after forwarding the
//     call on; it never alters the texture argument, so there's nothing for
//     the two fixes to step on each other over.
//   - DrawPrimitive: decal fix hooks this AFTER the flare fix does, so
//     decal's "original" is the flare fix's hook. Decal wraps the WHOLE
//     call - reads WORLD, scales it if this draw's texture was confirmed a
//     decal, calls through original (which runs flare's own signature check
//     and either calls the real DrawPrimitive or bypasses to
//     DrawPrimitiveUP for a matched glow billboard), then restores WORLD.
//     That's correct regardless of which path flare takes internally,
//     because decal is only holding a D3D8 device-state scope (WORLD) open
//     around the call, not touching any buffer contents. The two fixes'
//     match domains are also disjoint in practice - flare matches by
//     render-state signature + 36-byte-stride vertex buffers (<=8
//     vertices), decal by VIEW-matrix signature + a completely different
//     texture format - so a draw call matching both at once isn't a
//     realistic case, just a safe one if it ever happened.
//   - Present: all three fixes just want a once-per-frame tick (counter +
//     periodic log line), nobody inspects or alters the args. Plain hook
//     chaining handles this fine - whoever hooks last wraps everyone before
//     them, every body still fires once a frame. No ordering requirement here.
//
// Nothing about any fix's actual correction logic changed for this merge:
// the aura side is WindowerCore verbatim, the flare side is the same
// signature-match/nudge logic as flarefix v3.1 (0.4 nudge, tuned against
// real playtests - see flarefix/README.md), and the decal side is the same
// VIEW-signature/WORLD-rescale logic as decalfix (see decalfix/README.md).
// Only the shell (backend loading, CreateDevice hook, exports, DllMain) is
// actually shared, since there's only one real d3d8 backend and one
// CreateDevice call regardless.
//
// =============================================================================
// Flare fix summary (full history: ../flarefix/README.md, ../diagnostics/README.md)
// =============================================================================
//
// Bug: lamp/lantern flares rendered behind their own housing instead of in
// front. Diagnostics on 22,000+ real draws showed the billboard is a
// flawless camera-facing quad with zero per-vertex depth variance - so not
// a depth-precision bug, not a tilted quad, just an anchor point that sits
// at or behind the housing's actual surface.
//
// Fix: nudge each matched draw's local Z toward the camera by a small
// constant before submitting, so the quad clears the housing. v2 nudged the
// vertices in place inside the game's shared buffer and restored it after -
// raced against D3D's async command execution and flickered visibly. v3
// (here) never touches the game's buffer: read-only lock, nudge a local
// copy, resubmit that copy via DrawPrimitiveUP. No race. See the slot-overlap
// note above for how that interacts with the aura fix's own DrawPrimitiveUP hook.
//
// =============================================================================
// Decal fix summary (full history: ../decalfix/README.md)
// =============================================================================
//
// Bug: ground-effect decals (avatar summoning pentagrams, ground-targeted
// spell circles, certain NPC ground effects) render at the wrong size and
// the wrong aspect ratio - stretched into an ellipse on non-square displays,
// and swelling or shrinking as the camera zooms instead of holding still on
// the ground. Root cause: these decals are drawn with a fixed, canonical
// top-down VIEW matrix combined with the scene's ordinary perspective
// PROJECTION, so their size tracks camera FOV directly and world-X gets
// squished by 1/aspect.
//
// Fix: identify a decal draw two ways at once (exact VIEW-matrix signature
// AND a small/DXT3-compressed bound texture - neither signal is reliable
// alone), cache that texture's identity so later draws skip straight to a
// pointer check, then for a matched draw scale the WORLD matrix's
// upper-left 3x3 block to cancel both the FOV-tracking and the aspect
// squish, issue the real draw, and restore WORLD immediately after. Ported
// and hardened from an earlier prototype that proved the technique but
// wasn't written to a standard worth merging as-is - see decalfix/README.md
// for the full list of what changed.

#include "../../windower/src/windower_core.hpp"

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>

namespace
{
    // =====================================================================
    // Shared shell: backend loading + CreateDevice hook, used by both fixes.
    // =====================================================================

    using Direct3DCreate8Fn = IDirect3D8*(WINAPI*)(UINT);
    using DebugSetMuteFn    = void(WINAPI*)();
    using ValidatePixelShaderFn = HRESULT(WINAPI*)(const DWORD*, const DWORD*, BOOL, DWORD*);
    using ValidateVertexShaderFn = HRESULT(WINAPI*)(const DWORD*, const DWORD*, const DWORD*, BOOL, DWORD*);
    using CreateDeviceFn = HRESULT(__stdcall*)(
        IDirect3D8*, UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS*, IDirect3DDevice8**);

    HMODULE gRealModule = nullptr;
    Direct3DCreate8Fn gRealDirect3DCreate8 = nullptr;
    DebugSetMuteFn gRealDebugSetMute = nullptr;
    ValidatePixelShaderFn gRealValidatePixelShader = nullptr;
    ValidateVertexShaderFn gRealValidateVertexShader = nullptr;
    CreateDeviceFn gOriginalCreateDevice = nullptr;
    void** gCreateDeviceSlot = nullptr;
    thread_local bool gInsideCreateDevice = false;
    spectralfix_w::WindowerCore* gCore = nullptr;
    bool gCoreInitialized = false;

    constexpr uint32_t kCreateDeviceSlot = 15; // CreateDevice vtable slot.

    // Rename conventions both source tools used. Tried in order, first one
    // that exists and loads wins.
    constexpr std::array<const char*, 2> kKnownRenamedBackends = {
        "d3d8_dgvoodoo.dll",
        "d3d8_orig.dll",
    };

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

    // Never load a bare "d3d8.dll" - if this proxy IS the local d3d8.dll,
    // that loads ourselves. Always use full paths; fall back to System32's
    // copy by explicit path.
    bool load_real_backend()
    {
        if (gRealModule != nullptr)
            return true;

        const auto dir = module_directory();
        for (const auto* name : kKnownRenamedBackends)
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
        gRealDebugSetMute = reinterpret_cast<DebugSetMuteFn>(::GetProcAddress(gRealModule, "DebugSetMute"));
        gRealValidatePixelShader = reinterpret_cast<ValidatePixelShaderFn>(::GetProcAddress(gRealModule, "ValidatePixelShader"));
        gRealValidateVertexShader = reinterpret_cast<ValidateVertexShaderFn>(::GetProcAddress(gRealModule, "ValidateVertexShader"));
        return gRealDirect3DCreate8 != nullptr;
    }

    // =====================================================================
    // Flare fix (v3.1). Everything here is prefixed kFlare/gFlare/hook_flare_
    // to keep it obviously separate from the aura fix's device-attach call.
    // =====================================================================

    using FlareSetRenderStateFn = HRESULT(__stdcall*)(IDirect3DDevice8*, D3DRENDERSTATETYPE, DWORD);
    using FlareDrawPrimitiveFn = HRESULT(__stdcall*)(IDirect3DDevice8*, D3DPRIMITIVETYPE, UINT, UINT);
    using FlareDrawPrimitiveUPFn = HRESULT(__stdcall*)(IDirect3DDevice8*, D3DPRIMITIVETYPE, UINT, const void*, UINT);
    using FlareSetStreamSourceFn = HRESULT(__stdcall*)(IDirect3DDevice8*, UINT, IDirect3DVertexBuffer8*, UINT);
    using FlarePresentFn = HRESULT(__stdcall*)(IDirect3DDevice8*, const RECT*, const RECT*, HWND, const RGNDATA*);

    // Fixed D3D8 ABI slots. kFlareDrawPrimitiveUPSlot is read once, never
    // hooked - attach_flare_hooks() must run before WindowerCore's
    // attach_device() for that to matter (see header comment).
    constexpr uint32_t kFlareSetRenderStateSlot  = 50;
    constexpr uint32_t kFlareDrawPrimitiveSlot   = 70;
    constexpr uint32_t kFlareDrawPrimitiveUPSlot = 72; // read raw, never hooked
    constexpr uint32_t kFlareSetStreamSourceSlot = 83; // not 76, that's SetVertexShader
    constexpr uint32_t kFlarePresentSlot         = 15;

    // Exact render-state signature the diagnostic tool found for the glow
    // billboard - exact match on purpose so we can't clip other translucent effects.
    constexpr DWORD kFlareTargetSrcBlend = D3DBLEND_SRCALPHA;
    constexpr DWORD kFlareTargetDestBlend = D3DBLEND_ONE;
    constexpr UINT kFlareTargetVertexStride = 36;
    constexpr uint32_t kFlareMaxMatchedVertices = 8;

    // Tuned in-game: 0.25 flickered at the boundary, 1.0 detached the glow
    // visibly. 0.4 is the sweet spot.
    constexpr float kFlareGlowZNudge = 0.4F;

    constexpr size_t kFlareMaxLocalCopyBytes = static_cast<size_t>(kFlareMaxMatchedVertices) * kFlareTargetVertexStride;
    constexpr size_t kFlarePositionZByteOffset = 2 * sizeof(float);

    constexpr uint64_t kFlareLogEveryMatchUpTo = 20;
    constexpr uint64_t kFlareLogEveryNthMatchAfter = 6000;
    constexpr uint64_t kFlareStatusLineEveryNFrames = 600;
    constexpr uint64_t kFlareMaxLogBytes = 8ULL * 1024 * 1024;

    void** gFlareDeviceVtable = nullptr;
    FlareSetRenderStateFn gFlareOriginalSetRenderState = nullptr;
    FlareDrawPrimitiveFn gFlareOriginalDrawPrimitive = nullptr;
    FlareSetStreamSourceFn gFlareOriginalSetStreamSource = nullptr;
    FlarePresentFn gFlareOriginalPresent = nullptr;
    thread_local bool gFlareInsideSetRenderState = false;
    thread_local bool gFlareInsideDrawPrimitive = false;
    thread_local bool gFlareInsideSetStreamSource = false;
    thread_local bool gFlareInsidePresent = false;

    // Grabbed before WindowerCore hooks this slot (see header note). Stays
    // null if that read fails, in which case matched draws just fall back
    // to the real DrawPrimitive.
    FlareDrawPrimitiveUPFn gFlareRealDrawPrimitiveUP = nullptr;

    struct FlareTrackedState
    {
        DWORD zEnable{D3DZB_FALSE};
        DWORD zWriteEnable{TRUE};
        DWORD alphaBlendEnable{FALSE};
        DWORD srcBlend{D3DBLEND_ONE};
        DWORD destBlend{D3DBLEND_ZERO};
        UINT stream0Stride{0};
        IDirect3DVertexBuffer8* stream0Buffer{nullptr};
    } gFlareState;

    uint64_t gFlareFrame = 0;
    uint64_t gFlareFixedDrawCount = 0;
    uint64_t gFlareLockFailures = 0;
    FILE* gFlareLogFile = nullptr;
    uint64_t gFlareLogBytesWritten = 0;

    void ensure_flare_log_open()
    {
        if (gFlareLogFile != nullptr)
            return;
        const auto dir = module_directory() + "logs\\dagnyfix\\";
        ::CreateDirectoryA((module_directory() + "logs").c_str(), nullptr);
        ::CreateDirectoryA(dir.c_str(), nullptr);
        gFlareLogFile = std::fopen((dir + "dagnyfix_flare.log").c_str(), "a");
    }

    void flare_log_line(const std::string& line)
    {
        ensure_flare_log_open();
        if (gFlareLogFile == nullptr || gFlareLogBytesWritten >= kFlareMaxLogBytes)
            return;
        const auto now = std::time(nullptr);
        std::tm local{};
        localtime_s(&local, &now);
        char stamp[32]{};
        std::strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &local);
        const auto full = std::string(stamp) + " | " + line + "\n";
        std::fwrite(full.data(), 1, full.size(), gFlareLogFile);
        std::fflush(gFlareLogFile);
        gFlareLogBytesWritten += full.size();
    }

    uint32_t flare_vertex_count_for(D3DPRIMITIVETYPE type, UINT primitiveCount)
    {
        switch (type)
        {
            case D3DPT_TRIANGLELIST: return primitiveCount * 3;
            case D3DPT_TRIANGLESTRIP:
            case D3DPT_TRIANGLEFAN: return primitiveCount + 2;
            case D3DPT_LINELIST: return primitiveCount * 2;
            case D3DPT_LINESTRIP: return primitiveCount + 1;
            case D3DPT_POINTLIST: return primitiveCount;
            default: return 0;
        }
    }

    bool flare_matches_glow_signature(uint32_t vertexCount)
    {
        return vertexCount > 0 && vertexCount <= kFlareMaxMatchedVertices
            && gFlareState.zEnable == D3DZB_TRUE
            && gFlareState.zWriteEnable == FALSE
            && gFlareState.alphaBlendEnable == TRUE
            && gFlareState.srcBlend == kFlareTargetSrcBlend
            && gFlareState.destBlend == kFlareTargetDestBlend
            && gFlareState.stream0Stride == kFlareTargetVertexStride;
    }

    // Read-only lock into a local stack copy, Z nudged there - never writes
    // back to the game's buffer (that's what raced in v2).
    bool flare_build_nudged_vertex_copy(
        IDirect3DVertexBuffer8* buffer, UINT startVertex, UINT stride, uint32_t vertexCount, float nudge,
        std::array<BYTE, kFlareMaxLocalCopyBytes>& outBytes)
    {
        const auto size = static_cast<size_t>(vertexCount) * stride;
        if (buffer == nullptr || vertexCount == 0 || size == 0 || size > kFlareMaxLocalCopyBytes)
            return false;

        const auto offset = static_cast<UINT>(startVertex) * stride;
        BYTE* raw = nullptr;
        if (FAILED(buffer->Lock(offset, static_cast<UINT>(size), &raw, D3DLOCK_READONLY)) || raw == nullptr)
        {
            ++gFlareLockFailures;
            return false;
        }
        std::memcpy(outBytes.data(), raw, size);
        buffer->Unlock();

        for (uint32_t i = 0; i < vertexCount; ++i)
        {
            float z = 0.0F;
            BYTE* zBytes = outBytes.data() + static_cast<size_t>(i) * stride + kFlarePositionZByteOffset;
            std::memcpy(&z, zBytes, sizeof(z));
            z -= nudge;
            std::memcpy(zBytes, &z, sizeof(z));
        }
        return true;
    }

    HRESULT __stdcall hook_flare_set_render_state(IDirect3DDevice8* self, D3DRENDERSTATETYPE state, DWORD value)
    {
        const auto original = gFlareOriginalSetRenderState;
        if (gFlareInsideSetRenderState || original == nullptr)
            return original != nullptr ? original(self, state, value) : D3DERR_INVALIDCALL;
        gFlareInsideSetRenderState = true;
        const auto result = original(self, state, value);
        gFlareInsideSetRenderState = false;

        switch (state)
        {
            case D3DRS_ZENABLE: gFlareState.zEnable = value; break;
            case D3DRS_ZWRITEENABLE: gFlareState.zWriteEnable = value; break;
            case D3DRS_ALPHABLENDENABLE: gFlareState.alphaBlendEnable = value; break;
            case D3DRS_SRCBLEND: gFlareState.srcBlend = value; break;
            case D3DRS_DESTBLEND: gFlareState.destBlend = value; break;
            default: break;
        }
        return result;
    }

    HRESULT __stdcall hook_flare_set_stream_source(
        IDirect3DDevice8* self, UINT streamNumber, IDirect3DVertexBuffer8* buffer, UINT stride)
    {
        const auto original = gFlareOriginalSetStreamSource;
        if (gFlareInsideSetStreamSource || original == nullptr)
            return original != nullptr ? original(self, streamNumber, buffer, stride) : D3DERR_INVALIDCALL;
        gFlareInsideSetStreamSource = true;
        const auto result = original(self, streamNumber, buffer, stride);
        gFlareInsideSetStreamSource = false;

        if (streamNumber == 0)
        {
            gFlareState.stream0Stride = stride;
            gFlareState.stream0Buffer = buffer;
        }
        return result;
    }

    HRESULT __stdcall hook_flare_draw_primitive(
        IDirect3DDevice8* self, D3DPRIMITIVETYPE primitiveType, UINT startVertex, UINT primitiveCount)
    {
        const auto original = gFlareOriginalDrawPrimitive;
        if (gFlareInsideDrawPrimitive || original == nullptr)
            return original != nullptr ? original(self, primitiveType, startVertex, primitiveCount) : D3DERR_INVALIDCALL;
        gFlareInsideDrawPrimitive = true;

        const auto vertexCount = flare_vertex_count_for(primitiveType, primitiveCount);
        HRESULT result;

        if (flare_matches_glow_signature(vertexCount))
        {
            ++gFlareFixedDrawCount;
            std::array<BYTE, kFlareMaxLocalCopyBytes> localCopy{};
            const bool nudged = gFlareRealDrawPrimitiveUP != nullptr
                && flare_build_nudged_vertex_copy(
                    gFlareState.stream0Buffer, startVertex, gFlareState.stream0Stride, vertexCount, kFlareGlowZNudge, localCopy);

            if (nudged)
            {
                // Raw DrawPrimitiveUP pointer, so neither the real
                // DrawPrimitive nor the aura fix's hook sees this draw.
                result = gFlareRealDrawPrimitiveUP(self, primitiveType, primitiveCount, localCopy.data(), gFlareState.stream0Stride);
            }
            else
            {
                result = original(self, primitiveType, startVertex, primitiveCount);
            }

            if (gFlareFixedDrawCount <= kFlareLogEveryMatchUpTo || (gFlareFixedDrawCount % kFlareLogEveryNthMatchAfter) == 0)
            {
                flare_log_line(
                    "frame=" + std::to_string(gFlareFrame) + " glow-signature draw "
                    + (nudged ? "nudged (z-=" + std::to_string(kFlareGlowZNudge) + ", via DrawPrimitiveUP)" : "MATCHED BUT NOT NUDGED (fell back to original draw)")
                    + " fixed_draw_count=" + std::to_string(gFlareFixedDrawCount)
                    + " lock_failures=" + std::to_string(gFlareLockFailures));
            }
        }
        else
        {
            result = original(self, primitiveType, startVertex, primitiveCount);
        }

        gFlareInsideDrawPrimitive = false;
        return result;
    }

    HRESULT __stdcall hook_flare_present(
        IDirect3DDevice8* self, const RECT* sourceRect, const RECT* destRect, HWND destWindow, const RGNDATA* dirtyRegion)
    {
        const auto original = gFlareOriginalPresent;
        if (gFlareInsidePresent || original == nullptr)
            return original != nullptr ? original(self, sourceRect, destRect, destWindow, dirtyRegion) : D3DERR_INVALIDCALL;
        gFlareInsidePresent = true;
        const auto result = original(self, sourceRect, destRect, destWindow, dirtyRegion);
        gFlareInsidePresent = false;

        ++gFlareFrame;
        if (gFlareFrame % kFlareStatusLineEveryNFrames == 0)
        {
            flare_log_line(
                "status[periodic] frames=" + std::to_string(gFlareFrame)
                + " fixed_draw_count=" + std::to_string(gFlareFixedDrawCount)
                + " lock_failures=" + std::to_string(gFlareLockFailures));
        }
        return result;
    }

    // Must run before WindowerCore::attach_device() - hook_create_device
    // below enforces the order.
    void attach_flare_hooks(IDirect3DDevice8* device)
    {
        auto*** object = reinterpret_cast<void***>(device);
        if (object == nullptr || *object == nullptr)
            return;
        gFlareDeviceVtable = *object;

        const auto hookOne = [](void** vtable, uint32_t slotIndex, void* hook, void** originalStorageAsVoidPtr) {
            auto* slot = &vtable[slotIndex];
            const auto previous = *slot;
            if (previous == hook)
                return; // already hooked (second CreateDevice call)
            *originalStorageAsVoidPtr = previous;
            write_vtable_slot(slot, hook);
        };

        hookOne(gFlareDeviceVtable, kFlareSetRenderStateSlot, reinterpret_cast<void*>(&hook_flare_set_render_state),
            reinterpret_cast<void**>(&gFlareOriginalSetRenderState));
        hookOne(gFlareDeviceVtable, kFlareDrawPrimitiveSlot, reinterpret_cast<void*>(&hook_flare_draw_primitive),
            reinterpret_cast<void**>(&gFlareOriginalDrawPrimitive));
        hookOne(gFlareDeviceVtable, kFlareSetStreamSourceSlot, reinterpret_cast<void*>(&hook_flare_set_stream_source),
            reinterpret_cast<void**>(&gFlareOriginalSetStreamSource));
        hookOne(gFlareDeviceVtable, kFlarePresentSlot, reinterpret_cast<void*>(&hook_flare_present),
            reinterpret_cast<void**>(&gFlareOriginalPresent));

        // Just a read, not a hook - grab it before WindowerCore's
        // attach_device() (called right after this) hooks the same slot.
        gFlareRealDrawPrimitiveUP = reinterpret_cast<FlareDrawPrimitiveUPFn>(gFlareDeviceVtable[kFlareDrawPrimitiveUPSlot]);

        flare_log_line(
            "Dagnyfix flare fix (v3.1) attached to device; DrawPrimitive calls matching the glow-billboard "
            "signature (zenable=TRUE zwrite=FALSE alphablend=TRUE src=SRCALPHA dst=ONE stride=36, <="
            + std::to_string(kFlareMaxMatchedVertices) + " vertices) get their vertex data read via a "
            "read-only lock, nudged by -" + std::to_string(kFlareGlowZNudge) + " on local Z in a local copy, "
            "and resubmitted via the real (unhooked) DrawPrimitiveUP. See dagnyfix/README.md for how this "
            "coexists with the aura fix's own DrawPrimitiveUP hook in this merged build.");
    }

    // =====================================================================
    // Decal fix. Everything here is prefixed kDecal/gDecal/hook_decal_ to
    // keep it obviously separate from the other two fixes. Ported and
    // hardened from an earlier prototype - see decalfix/README.md for the
    // bug, the fix, and what changed from that prototype.
    // =====================================================================

    using DecalResetFn = HRESULT(__stdcall*)(IDirect3DDevice8*, D3DPRESENT_PARAMETERS*);
    using DecalSetTextureFn = HRESULT(__stdcall*)(IDirect3DDevice8*, DWORD, IDirect3DBaseTexture8*);
    using DecalDrawPrimitiveFn = HRESULT(__stdcall*)(IDirect3DDevice8*, D3DPRIMITIVETYPE, UINT, UINT);
    using DecalDrawIndexedPrimitiveFn = HRESULT(__stdcall*)(IDirect3DDevice8*, D3DPRIMITIVETYPE, UINT, UINT, UINT, UINT);
    using DecalGetTransformFn = HRESULT(__stdcall*)(IDirect3DDevice8*, D3DTRANSFORMSTATETYPE, D3DMATRIX*);
    using DecalSetTransformFn = HRESULT(__stdcall*)(IDirect3DDevice8*, D3DTRANSFORMSTATETYPE, const D3DMATRIX*);
    using DecalPresentFn = HRESULT(__stdcall*)(IDirect3DDevice8*, const RECT*, const RECT*, HWND, const RGNDATA*);

    // Fixed D3D8 ABI slots. Reset and DrawIndexedPrimitive are exclusively
    // ours; SetTexture and DrawPrimitive chain through the aura and flare
    // fixes' own hooks respectively (see header note); GetTransform/
    // SetTransform are read once, never hooked.
    constexpr uint32_t kDecalResetSlot = 14;
    constexpr uint32_t kDecalSetTextureSlot = 61;
    constexpr uint32_t kDecalDrawPrimitiveSlot = 70;
    constexpr uint32_t kDecalDrawIndexedPrimitiveSlot = 71;
    constexpr uint32_t kDecalPresentSlot = 15;
    constexpr uint32_t kDecalGetTransformSlot = 38; // read once at attach, never hooked
    constexpr uint32_t kDecalSetTransformSlot = 37; // read once at attach, never hooked

    // Observed canonical decal VIEW, row-major:
    //   row0 = [-1, 0, 0, 0]   X flipped
    //   row1 = [ 0, 0,-1, 0]   Y/Z swapped
    //   row2 = [ 0,-1, 0, 0]
    //   row3 = [ 0, 0,-2, 1]
    // A looser "any top-down view" match also catches zone shadow draws and
    // corrupts them - the translation row is what tells them apart.
    bool decal_matches_view_signature(const D3DMATRIX& m)
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
    constexpr UINT kDecalMaxTextureDimension = 256;

    // proj[5] (= cot(fovY/2), the standard aspect-independent FOV term) at
    // the reference zoom level this was calibrated against. 0 disables the
    // FOV cancel and applies kDecalUniformScale flat. Carried over from an
    // earlier prototype's own observed value, not re-verified against this
    // project's own playtest - see decalfix/README.md "Known limitations".
    constexpr float kDecalFovReferenceProjScale = 1.529F;
    constexpr float kDecalUniformScale = 1.0F;

    constexpr uint32_t kDecalTextureCacheCapacity = 64;

    constexpr uint64_t kDecalLogEveryMatchUpTo = 20;
    constexpr uint64_t kDecalLogEveryNthMatchAfter = 6000;
    constexpr uint64_t kDecalStatusLineEveryNFrames = 600;
    constexpr uint64_t kDecalMaxLogBytes = 8ULL * 1024 * 1024;

    // Two of these run side by side: one for textures confirmed to be
    // decals, one for textures confirmed NOT to be. Real circular buffer -
    // O(1) insert, oldest entry silently overwritten once full.
    template <uint32_t Capacity>
    class DecalTextureIdentityCache
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

    void** gDecalDeviceVtable = nullptr;
    DecalResetFn gDecalOriginalReset = nullptr;
    DecalSetTextureFn gDecalOriginalSetTexture = nullptr;
    DecalDrawPrimitiveFn gDecalOriginalDrawPrimitive = nullptr;
    DecalDrawIndexedPrimitiveFn gDecalOriginalDrawIndexedPrimitive = nullptr;
    DecalPresentFn gDecalOriginalPresent = nullptr;
    thread_local bool gDecalInsideReset = false;
    thread_local bool gDecalInsideSetTexture = false;
    thread_local bool gDecalInsideDrawPrimitive = false;
    thread_local bool gDecalInsideDrawIndexedPrimitive = false;
    thread_local bool gDecalInsidePresent = false;

    // Real GetTransform/SetTransform, grabbed straight from the vtable,
    // never hooked - only ever called ourselves around a matched draw, not
    // to observe the game's own calls to them.
    DecalGetTransformFn gDecalRealGetTransform = nullptr;
    DecalSetTransformFn gDecalRealSetTransform = nullptr;

    // Whatever's currently bound to texture stages 0 and 1, tracked
    // passively from the SetTexture hook after forwarding to original.
    IDirect3DBaseTexture8* gDecalStage0Texture = nullptr;
    IDirect3DBaseTexture8* gDecalStage1Texture = nullptr;

    DecalTextureIdentityCache<kDecalTextureCacheCapacity> gDecalKnownDecalTextures;
    DecalTextureIdentityCache<kDecalTextureCacheCapacity> gDecalKnownNonDecalTextures;

    uint64_t gDecalFrame = 0;
    uint64_t gDecalMatchedDrawCount = 0;
    uint64_t gDecalTextureIdentifyAttempts = 0;
    FILE* gDecalLogFile = nullptr;
    uint64_t gDecalLogBytesWritten = 0;

    void ensure_decal_log_open()
    {
        if (gDecalLogFile != nullptr)
            return;
        const auto dir = module_directory() + "logs\\dagnyfix\\";
        ::CreateDirectoryA((module_directory() + "logs").c_str(), nullptr);
        ::CreateDirectoryA(dir.c_str(), nullptr);
        gDecalLogFile = std::fopen((dir + "dagnyfix_decal.log").c_str(), "a");
    }

    void decal_log_line(const std::string& line)
    {
        ensure_decal_log_open();
        if (gDecalLogFile == nullptr || gDecalLogBytesWritten >= kDecalMaxLogBytes)
            return;
        const auto now = std::time(nullptr);
        std::tm local{};
        localtime_s(&local, &now);
        char stamp[32]{};
        std::strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &local);
        const auto full = std::string(stamp) + " | " + line + "\n";
        std::fwrite(full.data(), 1, full.size(), gDecalLogFile);
        std::fflush(gDecalLogFile);
        gDecalLogBytesWritten += full.size();
    }

    // Cheap pointer-identity checks only - caller is expected to have
    // already bailed if this returns Unknown (see decal_identify_texture_
    // if_needed, which is what actually runs the expensive path below).
    enum class DecalTextureClass { Unknown, Decal, NotDecal };

    DecalTextureClass decal_classify_known_texture(IDirect3DBaseTexture8* texture)
    {
        if (texture == nullptr)
            return DecalTextureClass::NotDecal;
        if (gDecalKnownDecalTextures.Contains(texture))
            return DecalTextureClass::Decal;
        if (gDecalKnownNonDecalTextures.Contains(texture))
            return DecalTextureClass::NotDecal;
        return DecalTextureClass::Unknown;
    }

    // Only ever called for a texture decal_classify_known_texture just said
    // it doesn't recognize yet. Checks GetType() before touching any
    // IDirect3DTexture8-specific method - a cube or volume texture bound to
    // the same stage would otherwise get GetLevelDesc called against the
    // wrong vtable slots entirely.
    void decal_identify_unknown_texture(IDirect3DBaseTexture8* texture)
    {
        ++gDecalTextureIdentifyAttempts;

        if (texture->GetType() != D3DRTYPE_TEXTURE)
        {
            gDecalKnownNonDecalTextures.Add(texture);
            return;
        }

        auto* texture2d = static_cast<IDirect3DTexture8*>(texture);
        D3DSURFACE_DESC desc{};
        if (FAILED(texture2d->GetLevelDesc(0, &desc)))
        {
            gDecalKnownNonDecalTextures.Add(texture);
            return;
        }

        const bool looksLikeDecal = desc.Width <= kDecalMaxTextureDimension
            && desc.Height <= kDecalMaxTextureDimension
            && desc.Format == D3DFMT_DXT3;

        if (looksLikeDecal)
            gDecalKnownDecalTextures.Add(texture);
        else
            gDecalKnownNonDecalTextures.Add(texture);
    }

    // Called on every candidate draw. Deliberately checks the cheap cache
    // lookup BEFORE ever reading the VIEW matrix - the overwhelming
    // majority of draws in a real frame use a texture already classified
    // one way or the other, and should never pay for a GetTransform call
    // plus a seven-way signature comparison just to find that out again.
    void decal_identify_texture_if_needed(IDirect3DDevice8* device, D3DMATRIX* viewScratch, bool* viewScratchValid)
    {
        if (decal_classify_known_texture(gDecalStage0Texture) != DecalTextureClass::Unknown)
            return;

        if (!*viewScratchValid)
        {
            if (gDecalRealGetTransform == nullptr
                || FAILED(gDecalRealGetTransform(device, D3DTS_VIEW, viewScratch))
                || !decal_matches_view_signature(*viewScratch))
                return;
            *viewScratchValid = true;
        }
        else if (!decal_matches_view_signature(*viewScratch))
        {
            return;
        }

        decal_identify_unknown_texture(gDecalStage0Texture);
    }

    struct DecalWorldSave
    {
        bool applied{false};
        D3DMATRIX matrix{};
    };

    // Scales the upper-left 3x3 block of the current WORLD matrix: uniformly
    // by kDecalUniformScale * (kDecalFovReferenceProjScale / proj[5]) to
    // cancel the FOV-tracking bug (proj[5] = cot(fovY/2) is the standard
    // aspect-independent term perspective projection scales screen size by,
    // so this re-derives the size-vs-zoom relationship the decal's fixed
    // pipeline skips), and on top of that scales world-X specifically by an
    // extra proj[5]/proj[0] to cancel the aspect squish. That second factor
    // only applies under the decal VIEW - the same decal textures also turn
    // up on ordinary vertical quads (rising light pillars) drawn under the
    // scene's normal VIEW, where the aspect correction would be wrong.
    void decal_apply_world_scale(IDirect3DDevice8* device, DecalWorldSave& save)
    {
        if (gDecalRealGetTransform == nullptr || gDecalRealSetTransform == nullptr)
            return;
        if (FAILED(gDecalRealGetTransform(device, D3DTS_WORLD, &save.matrix)))
            return;

        float uniformScale = kDecalUniformScale;
        float extraXScale = 1.0F;

        if (kDecalFovReferenceProjScale > 0.0F)
        {
            D3DMATRIX projection{};
            if (SUCCEEDED(gDecalRealGetTransform(device, D3DTS_PROJECTION, &projection)) && projection._22 > 0.0001F)
            {
                uniformScale = kDecalUniformScale * (kDecalFovReferenceProjScale / projection._22);

                if (projection._11 > 0.0001F)
                {
                    D3DMATRIX view{};
                    if (SUCCEEDED(gDecalRealGetTransform(device, D3DTS_VIEW, &view)) && decal_matches_view_signature(view))
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

        gDecalRealSetTransform(device, D3DTS_WORLD, &scaled);
        save.applied = true;
    }

    void decal_restore_world(IDirect3DDevice8* device, const DecalWorldSave& save)
    {
        if (!save.applied || gDecalRealSetTransform == nullptr)
            return;
        gDecalRealSetTransform(device, D3DTS_WORLD, &save.matrix);
    }

    bool decal_current_draw_targets_decal()
    {
        return decal_classify_known_texture(gDecalStage0Texture) == DecalTextureClass::Decal
            || decal_classify_known_texture(gDecalStage1Texture) == DecalTextureClass::Decal;
    }

    void decal_log_match(const char* drawKind)
    {
        ++gDecalMatchedDrawCount;
        if (gDecalMatchedDrawCount <= kDecalLogEveryMatchUpTo || (gDecalMatchedDrawCount % kDecalLogEveryNthMatchAfter) == 0)
        {
            decal_log_line(
                std::string("frame=") + std::to_string(gDecalFrame) + " decal draw (" + drawKind
                + ") WORLD rescaled for this draw only, matched_draw_count=" + std::to_string(gDecalMatchedDrawCount)
                + " known_decal_textures=" + std::to_string(gDecalKnownDecalTextures.Count())
                + " known_non_decal_textures=" + std::to_string(gDecalKnownNonDecalTextures.Count()));
        }
    }

    // Chains through whatever was previously in the SetTexture slot - the
    // aura fix's own hook, per the attach order in hook_create_device below.
    HRESULT __stdcall hook_decal_set_texture(IDirect3DDevice8* self, DWORD stage, IDirect3DBaseTexture8* texture)
    {
        const auto original = gDecalOriginalSetTexture;
        if (gDecalInsideSetTexture || original == nullptr)
            return original != nullptr ? original(self, stage, texture) : D3DERR_INVALIDCALL;
        gDecalInsideSetTexture = true;
        const auto result = original(self, stage, texture);
        gDecalInsideSetTexture = false;

        if (stage == 0)
            gDecalStage0Texture = texture;
        else if (stage == 1)
            gDecalStage1Texture = texture;

        return result;
    }

    // Chains through whatever was previously in the DrawPrimitive slot -
    // the flare fix's own hook, per the attach order in hook_create_device
    // below. Safe regardless of whether that inner hook ends up calling the
    // real DrawPrimitive or bypassing to DrawPrimitiveUP for a matched glow
    // billboard (see header note): WORLD is device state, held open for the
    // whole call either way.
    HRESULT __stdcall hook_decal_draw_primitive(
        IDirect3DDevice8* self, D3DPRIMITIVETYPE primitiveType, UINT startVertex, UINT primitiveCount)
    {
        const auto original = gDecalOriginalDrawPrimitive;
        if (gDecalInsideDrawPrimitive || original == nullptr)
            return original != nullptr ? original(self, primitiveType, startVertex, primitiveCount) : D3DERR_INVALIDCALL;
        gDecalInsideDrawPrimitive = true;

        D3DMATRIX viewScratch{};
        bool viewScratchValid = false;
        decal_identify_texture_if_needed(self, &viewScratch, &viewScratchValid);

        DecalWorldSave save;
        const bool matched = decal_current_draw_targets_decal();
        if (matched)
            decal_apply_world_scale(self, save);

        const auto result = original(self, primitiveType, startVertex, primitiveCount);

        if (matched)
        {
            decal_restore_world(self, save);
            decal_log_match("DrawPrimitive");
        }

        gDecalInsideDrawPrimitive = false;
        return result;
    }

    // Exclusively ours - no other fix hooks this slot.
    HRESULT __stdcall hook_decal_draw_indexed_primitive(
        IDirect3DDevice8* self, D3DPRIMITIVETYPE primitiveType, UINT minIndex, UINT numVertices,
        UINT startIndex, UINT primitiveCount)
    {
        const auto original = gDecalOriginalDrawIndexedPrimitive;
        if (gDecalInsideDrawIndexedPrimitive || original == nullptr)
        {
            return original != nullptr
                ? original(self, primitiveType, minIndex, numVertices, startIndex, primitiveCount)
                : D3DERR_INVALIDCALL;
        }
        gDecalInsideDrawIndexedPrimitive = true;

        D3DMATRIX viewScratch{};
        bool viewScratchValid = false;
        decal_identify_texture_if_needed(self, &viewScratch, &viewScratchValid);

        DecalWorldSave save;
        const bool matched = decal_current_draw_targets_decal();
        if (matched)
            decal_apply_world_scale(self, save);

        const auto result = original(self, primitiveType, minIndex, numVertices, startIndex, primitiveCount);

        if (matched)
        {
            decal_restore_world(self, save);
            decal_log_match("DrawIndexedPrimitive");
        }

        gDecalInsideDrawIndexedPrimitive = false;
        return result;
    }

    // Exclusively ours - no other fix hooks this slot.
    HRESULT __stdcall hook_decal_reset(IDirect3DDevice8* self, D3DPRESENT_PARAMETERS* presentationParameters)
    {
        const auto original = gDecalOriginalReset;
        if (gDecalInsideReset || original == nullptr)
            return original != nullptr ? original(self, presentationParameters) : D3DERR_INVALIDCALL;
        gDecalInsideReset = true;
        const auto result = original(self, presentationParameters);
        gDecalInsideReset = false;

        // D3DPOOL_DEFAULT texture pointers are invalidated across a Reset,
        // which correlates closely with zone changes and resolution
        // switches in this game. A stale cache entry after this point would
        // scale whatever unrelated texture the game recycles that address
        // for next.
        gDecalKnownDecalTextures.Clear();
        gDecalKnownNonDecalTextures.Clear();
        gDecalStage0Texture = nullptr;
        gDecalStage1Texture = nullptr;
        decal_log_line("device Reset: texture identity caches cleared");

        return result;
    }

    // Chains through whatever was previously in the Present slot - by this
    // point in the attach order, that's the aura fix's hook, which itself
    // chains to the flare fix's hook, which chains to the real Present.
    HRESULT __stdcall hook_decal_present(
        IDirect3DDevice8* self, const RECT* sourceRect, const RECT* destRect, HWND destWindow, const RGNDATA* dirtyRegion)
    {
        const auto original = gDecalOriginalPresent;
        if (gDecalInsidePresent || original == nullptr)
            return original != nullptr ? original(self, sourceRect, destRect, destWindow, dirtyRegion) : D3DERR_INVALIDCALL;
        gDecalInsidePresent = true;
        const auto result = original(self, sourceRect, destRect, destWindow, dirtyRegion);
        gDecalInsidePresent = false;

        ++gDecalFrame;
        if (gDecalFrame % kDecalStatusLineEveryNFrames == 0)
        {
            decal_log_line(
                "status[periodic] frames=" + std::to_string(gDecalFrame)
                + " matched_draw_count=" + std::to_string(gDecalMatchedDrawCount)
                + " texture_identify_attempts=" + std::to_string(gDecalTextureIdentifyAttempts)
                + " known_decal_textures=" + std::to_string(gDecalKnownDecalTextures.Count())
                + " known_non_decal_textures=" + std::to_string(gDecalKnownNonDecalTextures.Count()));
        }
        return result;
    }

    // Must run after WindowerCore::attach_device() and attach_flare_hooks()
    // - hook_create_device below enforces the order (see header note on the
    // SetTexture/DrawPrimitive chains).
    void attach_decal_hooks(IDirect3DDevice8* device)
    {
        auto*** object = reinterpret_cast<void***>(device);
        if (object == nullptr || *object == nullptr)
            return;
        gDecalDeviceVtable = *object;

        const auto hookOne = [](void** vtable, uint32_t slotIndex, void* hook, void** originalStorageAsVoidPtr) {
            auto* slot = &vtable[slotIndex];
            const auto previous = *slot;
            if (previous == hook)
                return; // already hooked (second CreateDevice call)
            *originalStorageAsVoidPtr = previous;
            write_vtable_slot(slot, hook);
        };

        hookOne(gDecalDeviceVtable, kDecalResetSlot, reinterpret_cast<void*>(&hook_decal_reset),
            reinterpret_cast<void**>(&gDecalOriginalReset));
        hookOne(gDecalDeviceVtable, kDecalSetTextureSlot, reinterpret_cast<void*>(&hook_decal_set_texture),
            reinterpret_cast<void**>(&gDecalOriginalSetTexture));
        hookOne(gDecalDeviceVtable, kDecalDrawPrimitiveSlot, reinterpret_cast<void*>(&hook_decal_draw_primitive),
            reinterpret_cast<void**>(&gDecalOriginalDrawPrimitive));
        hookOne(gDecalDeviceVtable, kDecalDrawIndexedPrimitiveSlot, reinterpret_cast<void*>(&hook_decal_draw_indexed_primitive),
            reinterpret_cast<void**>(&gDecalOriginalDrawIndexedPrimitive));
        hookOne(gDecalDeviceVtable, kDecalPresentSlot, reinterpret_cast<void*>(&hook_decal_present),
            reinterpret_cast<void**>(&gDecalOriginalPresent));

        // Not hooks - just grab the real pointers once so
        // decal_apply_world_scale can call them directly for a matched draw.
        gDecalRealGetTransform = reinterpret_cast<DecalGetTransformFn>(gDecalDeviceVtable[kDecalGetTransformSlot]);
        gDecalRealSetTransform = reinterpret_cast<DecalSetTransformFn>(gDecalDeviceVtable[kDecalSetTransformSlot]);

        decal_log_line(
            "Dagnyfix decal fix attached to device; draws whose bound stage-0/1 texture is confirmed a "
            "ground-decal texture (<=" + std::to_string(kDecalMaxTextureDimension) + "px, DXT3, seen under the "
            "canonical top-down decal VIEW) get their WORLD matrix rescaled for that draw only - FOV-cancel ref="
            + std::to_string(kDecalFovReferenceProjScale) + ", cleared on every device Reset. See "
            "dagnyfix/README.md for how this coexists with the other two fixes' hooks in this merged build.");
    }

    // =====================================================================
    // CreateDevice hook - hands the real device to all three fixes, in order.
    // =====================================================================

    HRESULT __stdcall hook_create_device(
        IDirect3D8* self, UINT adapter, D3DDEVTYPE deviceType, HWND focusWindow,
        DWORD behaviorFlags, D3DPRESENT_PARAMETERS* presentationParameters, IDirect3DDevice8** returnedDeviceInterface)
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
            // Order matters (see header note). Each attach is guarded so
            // one fix failing can't take down device creation or the other fixes.
            try { attach_flare_hooks(*returnedDeviceInterface); } catch (...) {}
            if (gCore != nullptr)
            {
                try { gCore->attach_device(*returnedDeviceInterface); } catch (...) {}
            }
            try { attach_decal_hooks(*returnedDeviceInterface); } catch (...) {}
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
            return; // already hooked, or nothing there to hook
        gOriginalCreateDevice = reinterpret_cast<CreateDeviceFn>(previous);
        write_vtable_slot(gCreateDeviceSlot, reinterpret_cast<void*>(&hook_create_device));
    }

    void ensure_core_initialized()
    {
        if (gCoreInitialized)
            return;
        gCoreInitialized = true;
        gCore = new spectralfix_w::WindowerCore();
        gCore->initialize(module_directory());
    }
}

extern "C" IDirect3D8* WINAPI Direct3DCreate8(UINT sdkVersion)
{
    if (!load_real_backend())
        return nullptr;
    flare_log_line("=== Dagnyfix proxy loaded (aura fix + flare fix v3.1 + decal fix) ===");
    ensure_core_initialized();

    const auto real = gRealDirect3DCreate8(sdkVersion);
    if (real != nullptr)
        hook_create_device_slot(real);
    return real;
}

extern "C" void WINAPI DebugSetMute()
{
    if (load_real_backend() && gRealDebugSetMute != nullptr)
        gRealDebugSetMute();
}

extern "C" HRESULT WINAPI ValidatePixelShader(
    const DWORD* pixelShader, const DWORD* reserved, BOOL flag, DWORD* errors)
{
    if (!load_real_backend() || gRealValidatePixelShader == nullptr)
        return E_FAIL;
    return gRealValidatePixelShader(pixelShader, reserved, flag, errors);
}

extern "C" HRESULT WINAPI ValidateVertexShader(
    const DWORD* vertexShader, const DWORD* reserved1, const DWORD* reserved2, BOOL flag, DWORD* errors)
{
    if (!load_real_backend() || gRealValidateVertexShader == nullptr)
        return E_FAIL;
    return gRealValidateVertexShader(vertexShader, reserved1, reserved2, flag, errors);
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    switch (reason)
    {
        case DLL_PROCESS_ATTACH:
            // Nothing heavy here (loader-lock hygiene) - backend load and
            // core init happen lazily on the first real Direct3DCreate8 call.
            ::DisableThreadLibraryCalls(module);
            break;
        case DLL_PROCESS_DETACH:
            if (gCore != nullptr)
            {
                gCore->release();
                // If release() refuses (enlargement still live), WindowerCore
                // leaks impl_ on purpose - same reason as the Ashita build's
                // expDestroyPlugin: the hooks are still live in the vtable.
                delete gCore;
                gCore = nullptr;
            }
            if (gFlareLogFile != nullptr)
            {
                std::fclose(gFlareLogFile);
                gFlareLogFile = nullptr;
            }
            if (gDecalLogFile != nullptr)
            {
                std::fclose(gDecalLogFile);
                gDecalLogFile = nullptr;
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
