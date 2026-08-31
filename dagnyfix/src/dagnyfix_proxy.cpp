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
// Dagnyfix - merges SpectralFix's Windower aura fix and the standalone
// flare depth-offset fix into one d3d8.dll proxy, so it's one DLL instead of
// two you'd have to choose between or chain.
//
// =============================================================================
// Merge notes / why this is safe
// =============================================================================
//
// Same "d3d8.dll" slot-takeover both source tools used (full history in
// windower/src/d3d8_proxy.cpp and flarefix/flarefix_proxy.cpp). We hook
// CreateDevice once and hand the real device to both fixes, in this order:
//
//   1. attach_flare_hooks() FIRST - installs the flare hooks (SetRenderState,
//      DrawPrimitive, SetStreamSource, Present) and reads the real
//      DrawPrimitiveUP pointer without hooking it. Order matters here, see below.
//   2. WindowerCore::attach_device() (aura fix, unmodified) SECOND - hooks
//      CreateTexture, SetTexture, DrawPrimitiveUP, Present.
//
// Slot overlap between the two fixes is nearly nil:
//   Flare: SetRenderState(50), DrawPrimitive(70), SetStreamSource(83)
//   Aura:  CreateTexture(20), SetTexture(61)
// They collide on two slots: DrawPrimitiveUP(72) and Present(15).
//
//   - DrawPrimitiveUP: aura fix hooks this (its quads go through here and get
//     corrected in flight). Flare fix never hooks it - just needs the raw
//     pointer to fire its own already-nudged synthetic draws straight past
//     the game's vertex buffer. THIS is why the order above matters: we grab
//     that pointer before the aura fix's hook goes on the slot, so the flare
//     fix always calls the true DrawPrimitiveUP and its synthetic draws never
//     get run back through the aura fix's selector/correction logic.
//   - Present: both fixes just want a once-per-frame tick (counter + periodic
//     log line), nobody inspects or alters the args. Plain hook chaining
//     handles this fine - whoever hooks second wraps whoever hooked first,
//     both bodies still fire once a frame. No ordering requirement here.
//
// Nothing about either fix's actual correction logic changed for this merge:
// the aura side is WindowerCore verbatim, the flare side below is the same
// signature-match/nudge logic as flarefix v3.1 (0.4 nudge, tuned against
// real playtests - see flarefix/README.md). Only the shell (backend
// loading, CreateDevice hook, exports, DllMain) is actually shared, since
// there's only one real d3d8 backend and one CreateDevice call regardless.
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

#include "../../windower/src/windower_core.hpp"

#include <array>
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
    // CreateDevice hook - hands the real device to both fixes, in order.
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
            // one fix failing can't take down device creation or the other fix.
            try { attach_flare_hooks(*returnedDeviceInterface); } catch (...) {}
            if (gCore != nullptr)
            {
                try { gCore->attach_device(*returnedDeviceInterface); } catch (...) {}
            }
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
    flare_log_line("=== Dagnyfix proxy loaded (aura fix + flare fix v3.1) ===");
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
