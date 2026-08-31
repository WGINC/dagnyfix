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
// flarefix - standalone d3d8.dll proxy that fixes a depth-sort bug where
// distant light flares (lamps, lanterns) render behind nearby models. It
// flickers rather than failing outright: same flare, same player position,
// visible one moment and occluded the next.
//
// Unrelated to SpectralFix's aura-aliasing fix - they just share the D3D8
// proxy-DLL plumbing because that's the only integration point Windower 4
// gives us. Should be evaluated/merged separately.
//
// Repros with dgVoodoo2 completely removed, so it's native engine behavior,
// not a wrapper bug. Forcing a 16-bit depth buffer in dgVoodoo2 doesn't help.
//
// == Root cause ==
//
// The companion diagnostic tool (../diagnostics/) traced the game's D3D8
// calls and isolated a small consistent group of draws (four textures in one
// session, one shared render state):
//   - IDirect3DDevice8::DrawPrimitive from a real vertex buffer, 36-byte
//     stride - ordinary world-space format, not the pre-transformed XYZRHW
//     screen-space stuff the UI/particles use. So a real 3D billboard, not
//     an overlay.
//   - ZENABLE=TRUE, ZWRITEENABLE=FALSE - depth-tested but doesn't write, as
//     you'd expect for a translucent effect.
//   - ALPHABLENDENABLE=TRUE, SRCBLEND=SRCALPHA, DESTBLEND=ONE - soft
//     additive blend, standard glow/flare technique.
//   - One quad (TRIANGLELIST, 2 tris, 6 verts).
//
// Since it's depth-tested like normal geometry, this smelled like D3D8-era
// depth precision loss at range (non-linear falloff means a distant
// billboard's depth compare against nearby geometry can flip frame to frame
// with nothing actually moving). Would explain why it's distance-specific
// and why changing dgVoodoo2's depth-buffer bit depth doesn't fix it -
// that shifts the precision curve, not the falloff itself.
//
// == Known limitations ==
//
// - Same vertex-format assumption as v2: first 12 bytes = untransformed
//   (x, y, z). True for every draw seen so far, but D3D8 gives no generic
//   way to confirm it.
// - kGlowZNudge is a starting value, not verified to fully fix things at
//   every distance - watch fixed_draw_count in flarefix.log and expect to
//   retune.
// - The read-only Lock can still fail on an unusual driver (e.g. a true
//   WRITEONLY buffer rejecting reads). Logged as lock_failures; falls back
//   to the real unmodified DrawPrimitive when that happens.

#include <Windows.h>

#include "d3d8/includes/d3d8.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>

namespace
{
    // ----------------------------------
    // D3D8 function pointer types
    // ----------------------------------

    using Direct3DCreate8Fn = IDirect3D8*(WINAPI*)(UINT);
    using CreateDeviceFn = HRESULT(__stdcall*)(
        IDirect3D8*, UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS*, IDirect3DDevice8**);
    using SetRenderStateFn = HRESULT(__stdcall*)(IDirect3DDevice8*, D3DRENDERSTATETYPE, DWORD);
    using DrawPrimitiveFn = HRESULT(__stdcall*)(IDirect3DDevice8*, D3DPRIMITIVETYPE, UINT, UINT);
    using DrawPrimitiveUPFn = HRESULT(__stdcall*)(IDirect3DDevice8*, D3DPRIMITIVETYPE, UINT, const void*, UINT);
    using SetStreamSourceFn = HRESULT(__stdcall*)(IDirect3DDevice8*, UINT, IDirect3DVertexBuffer8*, UINT);
    using PresentFn = HRESULT(__stdcall*)(IDirect3DDevice8*, const RECT*, const RECT*, HWND, const RGNDATA*);

    // ----------------------------------
    // IDirect3DDevice8 / IDirect3D8 vtable slot indices
    // ----------------------------------
    //
    // Fixed by the public D3D8 ABI, not by this game/SDK. CreateDevice and
    // Present both being 15 is fine - different interfaces.
    //
    // kSetStreamSourceSlot really is 83, not 76 - 76 is SetVertexShader
    // (single DWORD arg). An earlier diagnostic build hooked slot 76 by
    // mistake and corrupted the stack on every real SetVertexShader call.
    // Don't repeat that. XP

    constexpr uint32_t kCreateDeviceSlot    = 15; // IDirect3D8::CreateDevice
    constexpr uint32_t kPresentSlot         = 15; // IDirect3DDevice8::Present
    constexpr uint32_t kSetRenderStateSlot  = 50;
    constexpr uint32_t kDrawPrimitiveSlot   = 70;
    constexpr uint32_t kDrawPrimitiveUPSlot = 72; // read once at attach, never hooked - see gRealDrawPrimitiveUP
    constexpr uint32_t kSetStreamSourceSlot = 83;

    // ----------------------------------
    // Target signature and fix parameters
    // ----------------------------------

    // Signature from the diagnostic tool. Matching is exact, not "any
    // depth-tested additive quad", so we can't clip some unrelated effect.
    constexpr DWORD kTargetSrcBlend = D3DBLEND_SRCALPHA;
    constexpr DWORD kTargetDestBlend = D3DBLEND_ONE;
    constexpr UINT kTargetVertexStride = 36;

    // Observed candidate is a 6-vertex quad; cap a little above that so we
    // tolerate minor variation without matching unrelated batch geometry.
    constexpr uint32_t kMaxMatchedVertices = 8;

    // Amount subtracted from each matched vertex's local Z, in the game's
    // own world units. Matched quads have local half-extents of roughly
    // 1-8 units, so this is small relative to that: enough to clear a
    // housing surface the billboard sits just behind, not so much it
    // visibly detaches the glow from the lamp. Still a starting value, not
    // tuned final - watch fixed_draw_count in flarefix.log.
    //
    // History: v2 tried 0.25 (not quite enough) then 1.0 (right order of
    // magnitude, but that test was muddied by v2's write/restore race
    // flicker). v3's first clean test at 1.0 confirmed no more race, but
    // 1.0 visibly overshoots and detaches the glow. 0.4 splits the
    // difference as the next data point.
    constexpr float kGlowZNudge = 0.4F;

    // Max bytes this fix will ever read/submit for one draw (max vertices *
    // target stride) - sizes the on-stack copy buffer. Stays on the stack
    // since this runs inside a hot render-thread draw call.
    constexpr size_t kMaxLocalCopyBytes = static_cast<size_t>(kMaxMatchedVertices) * kTargetVertexStride;

    // Byte offset of Z within a vertex; assumes ordinary (x, y, z, ...)
    // starting at byte 0. True for everything seen so far - see "Known
    // limitations" up top.
    constexpr size_t kPositionZByteOffset = 2 * sizeof(float);

    // Log every match for the first N draws (so you can confirm the fix is
    // engaging after a fresh launch), then only every Nth after that so the
    // log doesn't grow unbounded during normal play.
    constexpr uint64_t kLogEveryMatchUpTo = 20;
    constexpr uint64_t kLogEveryNthMatchAfter = 6000;
    constexpr uint64_t kStatusLineEveryNFrames = 600;
    constexpr uint64_t kMaxLogBytes = 8ULL * 1024 * 1024;

    // ----------------------------------
    // Global state
    // ----------------------------------

    HMODULE gRealModule = nullptr;
    Direct3DCreate8Fn gRealDirect3DCreate8 = nullptr;
    CreateDeviceFn gOriginalCreateDevice = nullptr;
    void** gCreateDeviceSlot = nullptr;
    thread_local bool gInsideCreateDevice = false;

    void** gDeviceVtable = nullptr;
    SetRenderStateFn gOriginalSetRenderState = nullptr;
    DrawPrimitiveFn gOriginalDrawPrimitive = nullptr;
    SetStreamSourceFn gOriginalSetStreamSource = nullptr;
    PresentFn gOriginalPresent = nullptr;
    thread_local bool gInsideSetRenderState = false;
    thread_local bool gInsideDrawPrimitive = false;
    thread_local bool gInsideSetStreamSource = false;
    thread_local bool gInsidePresent = false;

    // Real DrawPrimitiveUP, grabbed straight from the vtable, never hooked -
    // we only need to call it ourselves to submit a matched draw's nudged
    // copy. Stays null if the slot read
    // fails for some reason, and matched draws just fall back to the
    // unmodified real DrawPrimitive.
    DrawPrimitiveUPFn gRealDrawPrimitiveUP = nullptr;

    // Render state relevant to the target signature, tracked passively from
    // our hooks after forwarding to the real implementation.
    //
    // stream0Buffer is whatever's currently bound to stream 0, so a matched
    // draw can be traced back to its real vertex data. Only ever read via a
    // read-only Lock that's unlocked right away - never written, never held
    // past one draw call.
    struct TrackedState
    {
        DWORD zEnable{D3DZB_FALSE};
        DWORD zWriteEnable{TRUE};
        DWORD alphaBlendEnable{FALSE};
        DWORD srcBlend{D3DBLEND_ONE};
        DWORD destBlend{D3DBLEND_ZERO};
        UINT stream0Stride{0};
        IDirect3DVertexBuffer8* stream0Buffer{nullptr};
    } gState;

    uint64_t gFrame = 0;
    uint64_t gFixedDrawCount = 0;
    uint64_t gLockFailures = 0;
    FILE* gLogFile = nullptr;
    uint64_t gLogBytesWritten = 0;

    // ----------------------------------
    // Logging
    // ----------------------------------

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
        const auto dir = module_directory() + "logs\\flarefix\\";
        ::CreateDirectoryA((module_directory() + "logs").c_str(), nullptr);
        ::CreateDirectoryA(dir.c_str(), nullptr);
        gLogFile = std::fopen((dir + "flarefix.log").c_str(), "a");
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

    // ----------------------------------
    // Signature matching
    // ----------------------------------

    uint32_t vertex_count_for(D3DPRIMITIVETYPE type, UINT primitiveCount)
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

    bool current_state_matches_glow_signature(uint32_t vertexCount)
    {
        return vertexCount > 0 && vertexCount <= kMaxMatchedVertices
            && gState.zEnable == D3DZB_TRUE
            && gState.zWriteEnable == FALSE
            && gState.alphaBlendEnable == TRUE
            && gState.srcBlend == kTargetSrcBlend
            && gState.destBlend == kTargetDestBlend
            && gState.stream0Stride == kTargetVertexStride;
    }

    // ----------------------------------
    // Vertex Z-nudge (read-only source, no writes to the game's buffer)
    // ----------------------------------

    // Reads a matched draw's vertex range via a READ-ONLY Lock (unlocked
    // right away) into a local on-stack copy, with kGlowZNudge already
    // subtracted from Z. Never writes to the game's buffer, so there's
    // nothing to restore and nothing for another draw to race against -
    // that's the whole point vs. v2's write-then-restore approach.
    //
    // Returns false (outBytes untouched) if there's no buffer bound, the
    // range is bigger than expected, or the Lock itself fails (e.g. a
    // WRITEONLY buffer that rejects even a read-only lock). Caller falls
    // back to the real unmodified DrawPrimitive in every failure case.
    bool build_nudged_vertex_copy(
        IDirect3DVertexBuffer8* buffer, UINT startVertex, UINT stride, uint32_t vertexCount, float nudge,
        std::array<BYTE, kMaxLocalCopyBytes>& outBytes)
    {
        const auto size = static_cast<size_t>(vertexCount) * stride;
        if (buffer == nullptr || vertexCount == 0 || size == 0 || size > kMaxLocalCopyBytes)
            return false;

        const auto offset = static_cast<UINT>(startVertex) * stride;
        BYTE* raw = nullptr;
        if (FAILED(buffer->Lock(offset, static_cast<UINT>(size), &raw, D3DLOCK_READONLY)) || raw == nullptr)
        {
            ++gLockFailures;
            return false;
        }
        std::memcpy(outBytes.data(), raw, size);
        buffer->Unlock();

        for (uint32_t i = 0; i < vertexCount; ++i)
        {
            float z = 0.0F;
            BYTE* zBytes = outBytes.data() + static_cast<size_t>(i) * stride + kPositionZByteOffset;
            std::memcpy(&z, zBytes, sizeof(z));
            z -= nudge;
            std::memcpy(zBytes, &z, sizeof(z));
        }
        return true;
    }

    // ----------------------------------
    // Hooks
    // ----------------------------------

    HRESULT __stdcall hook_set_render_state(IDirect3DDevice8* self, D3DRENDERSTATETYPE state, DWORD value)
    {
        const auto original = gOriginalSetRenderState;
        if (gInsideSetRenderState || original == nullptr)
            return original != nullptr ? original(self, state, value) : D3DERR_INVALIDCALL;
        gInsideSetRenderState = true;
        const auto result = original(self, state, value);
        gInsideSetRenderState = false;

        switch (state)
        {
            case D3DRS_ZENABLE: gState.zEnable = value; break;
            case D3DRS_ZWRITEENABLE: gState.zWriteEnable = value; break;
            case D3DRS_ALPHABLENDENABLE: gState.alphaBlendEnable = value; break;
            case D3DRS_SRCBLEND: gState.srcBlend = value; break;
            case D3DRS_DESTBLEND: gState.destBlend = value; break;
            default: break;
        }
        return result;
    }

    HRESULT __stdcall hook_set_stream_source(
        IDirect3DDevice8* self, UINT streamNumber, IDirect3DVertexBuffer8* buffer, UINT stride)
    {
        const auto original = gOriginalSetStreamSource;
        if (gInsideSetStreamSource || original == nullptr)
            return original != nullptr ? original(self, streamNumber, buffer, stride) : D3DERR_INVALIDCALL;
        gInsideSetStreamSource = true;
        const auto result = original(self, streamNumber, buffer, stride);
        gInsideSetStreamSource = false;

        // Just stashing stride/buffer here; the buffer itself only gets
        // touched later, for a draw that already matched the glow signature.
        if (streamNumber == 0)
        {
            gState.stream0Stride = stride;
            gState.stream0Buffer = buffer;
        }
        return result;
    }

    HRESULT __stdcall hook_draw_primitive(
        IDirect3DDevice8* self, D3DPRIMITIVETYPE primitiveType, UINT startVertex, UINT primitiveCount)
    {
        const auto original = gOriginalDrawPrimitive;
        if (gInsideDrawPrimitive || original == nullptr)
            return original != nullptr ? original(self, primitiveType, startVertex, primitiveCount) : D3DERR_INVALIDCALL;
        gInsideDrawPrimitive = true;

        const auto vertexCount = vertex_count_for(primitiveType, primitiveCount);
        HRESULT result;

        if (current_state_matches_glow_signature(vertexCount))
        {
            ++gFixedDrawCount;
            std::array<BYTE, kMaxLocalCopyBytes> localCopy{};
            const bool nudged = gRealDrawPrimitiveUP != nullptr
                && build_nudged_vertex_copy(gState.stream0Buffer, startVertex, gState.stream0Stride, vertexCount, kGlowZNudge, localCopy);

            if (nudged)
            {
                // Real DrawPrimitive/vertex buffer never touched for this draw.
                result = gRealDrawPrimitiveUP(self, primitiveType, primitiveCount, localCopy.data(), gState.stream0Stride);
            }
            else
            {
                // No buffer, range too big, Lock failed, or no real
                // DrawPrimitiveUP pointer - just draw it unmodified.
                result = original(self, primitiveType, startVertex, primitiveCount);
            }

            if (gFixedDrawCount <= kLogEveryMatchUpTo || (gFixedDrawCount % kLogEveryNthMatchAfter) == 0)
            {
                log_line(
                    "frame=" + std::to_string(gFrame) + " glow-signature draw "
                    + (nudged ? "nudged (z-=" + std::to_string(kGlowZNudge) + ", via DrawPrimitiveUP)" : "MATCHED BUT NOT NUDGED (fell back to original draw)")
                    + " fixed_draw_count=" + std::to_string(gFixedDrawCount)
                    + " lock_failures=" + std::to_string(gLockFailures));
            }
        }
        else
        {
            result = original(self, primitiveType, startVertex, primitiveCount);
        }

        gInsideDrawPrimitive = false;
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
                + " fixed_draw_count=" + std::to_string(gFixedDrawCount)
                + " lock_failures=" + std::to_string(gLockFailures));
        }
        return result;
    }

    // ----------------------------------
    // Vtable patching
    // ----------------------------------

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

        hookOne(gDeviceVtable, kSetRenderStateSlot, reinterpret_cast<void*>(&hook_set_render_state),
            reinterpret_cast<void**>(&gOriginalSetRenderState));
        hookOne(gDeviceVtable, kDrawPrimitiveSlot, reinterpret_cast<void*>(&hook_draw_primitive),
            reinterpret_cast<void**>(&gOriginalDrawPrimitive));
        hookOne(gDeviceVtable, kSetStreamSourceSlot, reinterpret_cast<void*>(&hook_set_stream_source),
            reinterpret_cast<void**>(&gOriginalSetStreamSource));
        hookOne(gDeviceVtable, kPresentSlot, reinterpret_cast<void*>(&hook_present),
            reinterpret_cast<void**>(&gOriginalPresent));

        // Not a hook - just grabs the real DrawPrimitiveUP pointer once so
        // hook_draw_primitive can call it for a matched draw's nudged copy.
        gRealDrawPrimitiveUP = reinterpret_cast<DrawPrimitiveUPFn>(gDeviceVtable[kDrawPrimitiveUPSlot]);

        log_line(
            "flarefix v3 attached to device; DrawPrimitive calls matching the glow-billboard signature "
            "(zenable=TRUE zwrite=FALSE alphablend=TRUE src=SRCALPHA dst=ONE stride=36, <="
            + std::to_string(kMaxMatchedVertices) + " vertices) get their vertex data read via a read-only "
            "lock, nudged by -" + std::to_string(kGlowZNudge) + " on local Z in a local copy, and resubmitted via "
            "DrawPrimitiveUP - the real DrawPrimitive/vertex buffer are never touched for a matched draw. "
            "Replaces v2's lock/modify/restore-on-the-shared-buffer approach, which raced against "
            "Direct3D's asynchronous command execution and produced a visible double-position flicker.");
    }

    // ----------------------------------
    // d3d8.dll proxy shell
    // ----------------------------------
    //
    // Same structure as windower/src/d3d8_proxy.cpp: sits in for "d3d8.dll"
    // via normal DLL search order, forwards every real export to the actual
    // backend, and hooks CreateDevice just long enough to attach once the
    // real device exists.

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
    log_line("=== flarefix proxy loaded ===");
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
