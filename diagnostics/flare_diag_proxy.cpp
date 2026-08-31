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
// flare_diag: throwaway d3d8.dll proxy that logs render state around
// additive draw calls, built to track down "distant light flares render
// behind models inconsistently" in retail FFXI. Confirmed to repro with
// dgVoodoo2 removed entirely, so it's native engine behavior.
//
// Purely a watcher - never touches render state or geometry. Reuses the
// same DLL-search-order proxy trick and CreateDevice hook as SpectralFix's
// windower/src/d3d8_proxy.cpp (see that file for why it works) since it's a
// proven way to get a live device pointer without Windower's cooperation.
//
// What it hooks and why:
//   - SetRenderState / SetTexture (stage 0): passive state tracking so later
//     draws can be evaluated against it. Never modified.
//   - DrawPrimitiveUP: SpectralFix's own aura quads go through here, so it's
//     the natural first place to look for another effect quad. Matching
//     draws get their vertices summarized via the same summarize_vertices()
//     SpectralFix's geometry code already uses.
//   - DrawPrimitive: the buffer-based path, for a flare drawn from a
//     pre-built vertex buffer. No geometry capture here (would need
//     SetStreamSource tracking + a live Lock, which risks interfering with
//     rendering) - just render state, texture, and primitive counts.
//   - Present: frame counter + periodic status line, same as spectralfix.log.
//
// v3: the first two rounds gated on "must be alpha-blended", which only ever
// turned up ambient particles and full-screen overlays - nothing shaped
// like a handful of fixed lights. So DrawPrimitiveUP/DrawIndexedPrimitiveUP
// dropped the blend/Z prefilter entirely: any small-quad-shaped draw
// (<=8 verts) gets logged with full render state, so the real blend/Z config
// can be read off the log instead of guessed up front. The buffer-based
// DrawPrimitive path (no geometry to fall back on) kept the blend prefilter
// since it still needs some filter to stay readable.
//
// v5: two real bugs, found after a v4 session crashed and then (once fixed)
// came back empty. (1) SetStreamSource was hooked at slot 76, which is
// actually SetVertexShader (1 arg) - every real SetVertexShader call landed
// in our 4-arg hook and corrupted the stack. Real slot is 83. (2)
// DrawPrimitiveUP fires so often it filled the single 60-slot signature
// cache within a few hundred frames, starving the buffer-based
// DrawPrimitive path (hundreds of thousands of matches, zero logged). Each
// hook now gets its own cache.
//
// v6: v5 pinned down the flare's render-state signature, but a fix built on
// it (D3DRS_ZBIAS) didn't help under dgVoodoo2, and depth-buffer-bit-depth
// experiments made things worse or no better - evidence against depth
// precision loss as the cause. New theory: a fixed geometric offset between
// the billboard and its lamp housing, which needs real vertex/camera data to
// test (something this tool never captured before - the buffer path
// explicitly skipped geometry). v6 adds that narrowly:
//   - New SetTransform hook (world/view only) tracks the matrices needed to
//     place a vertex in view space.
//   - SetStreamSource now also remembers the vertex buffer pointer, not just
//     stride, so a matching draw can be traced back to its data.
//   - hook_draw_primitive, only when the current state exactly matches
//     flarefix's own tight glow signature (not the loose "any quad" gate
//     used elsewhere), does a single read-only Lock on that draw's vertex
//     range, transforms positions through World then View, and logs the
//     view-space Z spread across the quad. Near-zero spread = a proper
//     camera-facing billboard, consistent with "just positioned behind the
//     housing". A real spread would mean it's not billboarded and can recede
//     from some angles - a different, still mundane, explanation. Only
//     place this tool ever locks a buffer, and only for draws already
//     matching the narrow signature.

#include <Windows.h>

#include "d3d8/includes/d3d8.h"

#include "../src/geometry_rewrite.hpp"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <unordered_set>

namespace
{
    using Direct3DCreate8Fn = IDirect3D8*(WINAPI*)(UINT);
    using CreateDeviceFn = HRESULT(__stdcall*)(
        IDirect3D8*, UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS*, IDirect3DDevice8**);
    using SetRenderStateFn = HRESULT(__stdcall*)(IDirect3DDevice8*, D3DRENDERSTATETYPE, DWORD);
    using SetTransformFn = HRESULT(__stdcall*)(IDirect3DDevice8*, D3DTRANSFORMSTATETYPE, const D3DMATRIX*);
    using SetTextureFn = HRESULT(__stdcall*)(IDirect3DDevice8*, DWORD, IDirect3DBaseTexture8*);
    using DrawPrimitiveFn = HRESULT(__stdcall*)(IDirect3DDevice8*, D3DPRIMITIVETYPE, UINT, UINT);
    using DrawPrimitiveUPFn = HRESULT(__stdcall*)(IDirect3DDevice8*, D3DPRIMITIVETYPE, UINT, const void*, UINT);
    using DrawIndexedPrimitiveUPFn = HRESULT(__stdcall*)(
        IDirect3DDevice8*, D3DPRIMITIVETYPE, UINT, UINT, UINT, const void*, D3DFORMAT, const void*, UINT);
    using SetStreamSourceFn = HRESULT(__stdcall*)(IDirect3DDevice8*, UINT, IDirect3DVertexBuffer8*, UINT);
    using PresentFn = HRESULT(__stdcall*)(IDirect3DDevice8*, const RECT*, const RECT*, HWND, const RGNDATA*);

    constexpr uint32_t kCreateDeviceSlot   = 15;
    constexpr uint32_t kPresentSlot        = 15; // different interface, same numeric slot
    constexpr uint32_t kSetTextureSlot     = 61;
    constexpr uint32_t kSetRenderStateSlot = 50;
    constexpr uint32_t kSetTransformSlot   = 37; // verified against the SDK header (GetTransform=38,
                                                  // MultiplyTransform=39 follow) - same re-check that
                                                  // caught the slot-83 mistake below
    constexpr uint32_t kDrawPrimitiveSlot  = 70;
    constexpr uint32_t kDrawPrimitiveUPSlot = 72;
    constexpr uint32_t kDrawIndexedPrimitiveUPSlot = 73;
    constexpr uint32_t kSetStreamSourceSlot = 83; // NOT 76 - that's SetVertexShader (1 DWORD arg).
                                                   // v4 had it wrong at 76: every real SetVertexShader call
                                                   // landed in our 4-arg hook and corrupted the stack.

    constexpr size_t kMaxFullyLoggedSignatures = 60;
    constexpr uint64_t kMaxLogBytes = 8ULL * 1024 * 1024;

    HMODULE gRealModule = nullptr;
    Direct3DCreate8Fn gRealDirect3DCreate8 = nullptr;
    CreateDeviceFn gOriginalCreateDevice = nullptr;
    void** gCreateDeviceSlot = nullptr;
    thread_local bool gInsideCreateDevice = false;

    void** gDeviceVtable = nullptr;
    SetRenderStateFn gOriginalSetRenderState = nullptr;
    SetTransformFn gOriginalSetTransform = nullptr;
    SetTextureFn gOriginalSetTexture = nullptr;
    DrawPrimitiveFn gOriginalDrawPrimitive = nullptr;
    DrawPrimitiveUPFn gOriginalDrawPrimitiveUP = nullptr;
    DrawIndexedPrimitiveUPFn gOriginalDrawIndexedPrimitiveUP = nullptr;
    SetStreamSourceFn gOriginalSetStreamSource = nullptr;
    PresentFn gOriginalPresent = nullptr;
    thread_local bool gInsideSetRenderState = false;
    thread_local bool gInsideSetTransform = false;
    thread_local bool gInsideSetTexture = false;
    thread_local bool gInsideDrawPrimitive = false;
    thread_local bool gInsideDrawPrimitiveUP = false;
    thread_local bool gInsideDrawIndexedPrimitiveUP = false;
    thread_local bool gInsideSetStreamSource = false;
    thread_local bool gInsidePresent = false;

    // World/view matrices, tracked so a matched draw's vertices can be placed
    // in view space later. Never used to change anything. D3DMATRIX doesn't
    // zero-init by default, so these start as identity explicitly - a
    // missed SetTransform before the first draw should look obviously wrong
    // (identity) rather than read as uninitialized junk.
    D3DMATRIX gWorld = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    D3DMATRIX gView = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};

    // Passively tracked state, written only from our hooks after forwarding
    // to the real call. Never used to change anything.
    struct TrackedState
    {
        DWORD zEnable{D3DZB_FALSE};
        DWORD zWriteEnable{TRUE};
        DWORD zFunc{D3DCMP_LESSEQUAL};
        DWORD alphaBlendEnable{FALSE};
        DWORD srcBlend{D3DBLEND_ONE};
        DWORD destBlend{D3DBLEND_ZERO};
        DWORD blendOp{D3DBLENDOP_ADD};
        DWORD alphaTestEnable{FALSE};
        const void* stage0Texture{nullptr};
        UINT stream0Stride{0};
        IDirect3DVertexBuffer8* stream0Buffer{nullptr}; // which buffer, so a matched draw can be Locked
    } gState;

    // flarefix's exact target signature, duplicated here (not shared, so this
    // stays a standalone tool) to trigger the one-off geometry capture below.
    // Deliberately exact - this is the only place this tool locks a buffer,
    // so it must never fire on unrelated geometry.
    constexpr DWORD kGlowSrcBlend = D3DBLEND_SRCALPHA;
    constexpr DWORD kGlowDestBlend = D3DBLEND_ONE;
    constexpr UINT kGlowVertexStride = 36;
    constexpr uint32_t kGlowMaxVertices = 8;

    bool current_state_matches_glow_signature(uint32_t vertexCount)
    {
        return gState.zEnable != D3DZB_FALSE
            && gState.zWriteEnable == FALSE
            && gState.alphaBlendEnable != FALSE
            && gState.srcBlend == kGlowSrcBlend
            && gState.destBlend == kGlowDestBlend
            && gState.stream0Stride == kGlowVertexStride
            && vertexCount > 0 && vertexCount <= kGlowMaxVertices;
    }

    uint64_t gFrame = 0;
    uint64_t gMatchedDrawPrimitiveUP = 0;
    uint64_t gMatchedDrawPrimitive = 0;
    uint64_t gSuppressedFullLogs = 0;
    // v5 fix: DrawPrimitiveUP hits so many distinct signatures it was filling
    // the shared 60-slot cap within a few hundred frames, silently starving
    // the buffer-based DrawPrimitive path (a signature that never makes the
    // set never gets logged at all - matched_drawprimitive hit the hundreds
    // of thousands in a v4 session with zero log lines to show for it). Each
    // hook now gets its own set/cap.
    std::unordered_set<uint64_t> gLoggedSignaturesImmediate; // DrawPrimitiveUP + DrawIndexedPrimitiveUP
    std::unordered_set<uint64_t> gLoggedSignaturesBuffer;    // DrawPrimitive (buffer-based)
    FILE* gLogFile = nullptr;
    uint64_t gLogBytesWritten = 0;

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
        const auto dir = module_directory() + "logs\\flare_diag\\";
        ::CreateDirectoryA((module_directory() + "logs").c_str(), nullptr);
        ::CreateDirectoryA(dir.c_str(), nullptr);
        gLogFile = std::fopen((dir + "flare_diag.log").c_str(), "a");
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
        if (gLogBytesWritten >= kMaxLogBytes)
        {
            const auto notice = std::string(stamp) + " | Log size cap reached (8 MiB); further lines suppressed.\n";
            std::fwrite(notice.data(), 1, notice.size(), gLogFile);
            std::fflush(gLogFile);
        }
    }

    const char* describe_zfunc(DWORD value)
    {
        switch (value)
        {
            case D3DCMP_NEVER: return "NEVER";
            case D3DCMP_LESS: return "LESS";
            case D3DCMP_EQUAL: return "EQUAL";
            case D3DCMP_LESSEQUAL: return "LESSEQUAL";
            case D3DCMP_GREATER: return "GREATER";
            case D3DCMP_NOTEQUAL: return "NOTEQUAL";
            case D3DCMP_GREATEREQUAL: return "GREATEREQUAL";
            case D3DCMP_ALWAYS: return "ALWAYS";
            default: return "UNKNOWN";
        }
    }

    const char* describe_blend(DWORD value)
    {
        switch (value)
        {
            case D3DBLEND_ZERO: return "ZERO";
            case D3DBLEND_ONE: return "ONE";
            case D3DBLEND_SRCCOLOR: return "SRCCOLOR";
            case D3DBLEND_INVSRCCOLOR: return "INVSRCCOLOR";
            case D3DBLEND_SRCALPHA: return "SRCALPHA";
            case D3DBLEND_INVSRCALPHA: return "INVSRCALPHA";
            case D3DBLEND_DESTALPHA: return "DESTALPHA";
            case D3DBLEND_INVDESTALPHA: return "INVDESTALPHA";
            case D3DBLEND_DESTCOLOR: return "DESTCOLOR";
            case D3DBLEND_INVDESTCOLOR: return "INVDESTCOLOR";
            case D3DBLEND_SRCALPHASAT: return "SRCALPHASAT";
            default: return "UNKNOWN";
        }
    }

    const char* describe_zenable(DWORD value)
    {
        switch (value)
        {
            case D3DZB_FALSE: return "FALSE";
            case D3DZB_TRUE: return "TRUE";
            case D3DZB_USEW: return "USEW";
            default: return "UNKNOWN";
        }
    }

    // v2/v3 gated on blend state ("must be alpha-blended [with Z off]") and
    // that only ever turned up ambient particles and full-screen overlays -
    // the assumption itself might've been hiding the real thing (could just
    // as easily be an opaque billboard with a Z-value bug, nothing to do with
    // blend state). v4 drops the blend gate everywhere - including the
    // buffer-based DrawPrimitive path, which still had it - in favor of
    // gating purely on shape (see shape_is_quad_like()). Cheap to do: caps at
    // kMaxQuadVertices before ever touching summarize_vertices, so no real
    // cost added on ordinary scene geometry.
    constexpr uint32_t kMaxQuadVertices = 8;

    bool shape_is_quad_like(uint32_t vertexCount)
    {
        return vertexCount > 0 && vertexCount <= kMaxQuadVertices;
    }

    // Covers the primitive topologies actually seen for small effect quads;
    // anything else is skipped rather than guessed at.
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

    std::string render_state_suffix()
    {
        char buffer[320]{};
        std::snprintf(
            buffer, sizeof(buffer),
            "zenable=%s zwrite=%s zfunc=%s alphablend=%s src=%s dst=%s blendop=%u alphatest=%s texture=0x%p",
            describe_zenable(gState.zEnable),
            gState.zWriteEnable ? "TRUE" : "FALSE",
            describe_zfunc(gState.zFunc),
            gState.alphaBlendEnable ? "TRUE" : "FALSE",
            describe_blend(gState.srcBlend),
            describe_blend(gState.destBlend),
            static_cast<unsigned>(gState.blendOp),
            gState.alphaTestEnable ? "TRUE" : "FALSE",
            gState.stage0Texture);
        return buffer;
    }

    uint64_t signature_for(const void* texture, float width, float height)
    {
        // Coarse fingerprint (texture + rough quad size + blend/z state) -
        // two draws matching on all of that are almost certainly the same
        // effect instance, no need to log in full every time. Blend/z is
        // included since the same texture can be drawn multiple ways.
        const auto textureBits = reinterpret_cast<uint64_t>(texture);
        const auto sizeBits = static_cast<uint64_t>(width) << 20 ^ static_cast<uint64_t>(height);
        const auto stateBits = (static_cast<uint64_t>(gState.zEnable) << 24)
            ^ (static_cast<uint64_t>(gState.srcBlend) << 16)
            ^ (static_cast<uint64_t>(gState.destBlend) << 8)
            ^ static_cast<uint64_t>(gState.blendOp);
        return textureBits ^ (sizeBits * 1099511628211ULL) ^ (stateBits * 2654435761ULL);
    }

    void handle_candidate_draw_up(
        D3DPRIMITIVETYPE primitiveType, UINT primitiveCount, const void* vertexData, UINT stride)
    {
        ++gMatchedDrawPrimitiveUP;
        const auto count = vertex_count_for(primitiveType, primitiveCount);
        const auto summary = spectralfix::summarize_vertices(vertexData, stride, count);
        if (!summary.valid)
        {
            log_line(
                "DrawPrimitiveUP additive match, geometry not summarizable (stride=" + std::to_string(stride)
                + " count=" + std::to_string(count) + ") " + render_state_suffix());
            return;
        }

        const auto width = summary.maxX - summary.minX;
        const auto height = summary.maxY - summary.minY;
        const auto signature = signature_for(gState.stage0Texture, width, height);
        if (gLoggedSignaturesImmediate.count(signature) != 0)
        {
            if ((gMatchedDrawPrimitiveUP % 600) != 0)
                return; // already logged once; only resurface occasionally
        }
        else if (gLoggedSignaturesImmediate.size() >= kMaxFullyLoggedSignatures)
        {
            ++gSuppressedFullLogs;
            return;
        }
        else
        {
            gLoggedSignaturesImmediate.insert(signature);
        }

        char verts[256]{};
        std::snprintf(
            verts, sizeof(verts), "bbox=(%.1f,%.1f)-(%.1f,%.1f) v0=(%.1f,%.1f) v1=(%.1f,%.1f) hasUv=%s uniformColor=%s",
            summary.minX, summary.minY, summary.maxX, summary.maxY,
            summary.x[0], summary.y[0], summary.x[1], summary.y[1],
            summary.hasUv ? "true" : "false", summary.uniformColor ? "true" : "false");

        log_line(
            "frame=" + std::to_string(gFrame) + " DrawPrimitiveUP additive match count=" + std::to_string(count)
            + " stride=" + std::to_string(stride) + " " + verts + " " + render_state_suffix());
    }

    // Row-vector * row-major-matrix transform (D3D8's convention), just to
    // get a raw vertex into view space for the Z-spread check below. World
    // and view matrices are affine (no projection row) so w stays 1 and can
    // be dropped - deliberately doesn't project, since only relative Z
    // ordering within the quad matters here, not screen position.
    struct Vec3 { float x, y, z; };

    Vec3 transform_point(const Vec3& v, const D3DMATRIX& m)
    {
        return Vec3{
            v.x * m._11 + v.y * m._21 + v.z * m._31 + m._41,
            v.x * m._12 + v.y * m._22 + v.z * m._32 + m._42,
            v.x * m._13 + v.y * m._23 + v.z * m._33 + m._43};
    }

    // The only place this tool ever locks a buffer, only for draws that
    // already matched the exact glow signature. Read-only, single vertex
    // range, unlocked right away. Logs the view-space Z spread across the
    // quad - see the v6 header note for what that number means.
    void log_glow_geometry(UINT startVertex, uint32_t vertexCount, UINT stride)
    {
        static bool sLockFailureLogged = false;
        auto* buffer = gState.stream0Buffer;
        if (buffer == nullptr)
        {
            if (!sLockFailureLogged)
            {
                log_line("v6 geometry capture: no stream0 buffer tracked, skipping.");
                sLockFailureLogged = true;
            }
            return;
        }

        BYTE* raw = nullptr;
        const auto offset = static_cast<UINT>(startVertex) * stride;
        const auto size = static_cast<UINT>(vertexCount) * stride;
        // READONLY: never writes back, lets the driver skip write-lock dirty
        // tracking. Some buffers are WRITEONLY and may reject or return
        // garbage on a read - just skip the draw rather than trust it.
        const auto hr = buffer->Lock(offset, size, &raw, D3DLOCK_READONLY);
        if (FAILED(hr) || raw == nullptr)
        {
            static uint64_t sLockFailures = 0;
            ++sLockFailures;
            if (sLockFailures <= 5) // a few samples is enough; don't flood the log
                log_line("v6 geometry capture: Lock failed (hr=0x" + std::to_string(static_cast<unsigned>(hr)) + "), skipping this draw.");
            return;
        }

        float minZ = 0.0F;
        float maxZ = 0.0F;
        char detail[512]{};
        int detailLen = 0;
        for (uint32_t i = 0; i < vertexCount; ++i)
        {
            Vec3 position{};
            std::memcpy(&position, raw + static_cast<size_t>(i) * stride, sizeof(Vec3));
            const auto world = transform_point(position, gWorld);
            const auto view = transform_point(world, gView);
            if (i == 0 || view.z < minZ) minZ = view.z;
            if (i == 0 || view.z > maxZ) maxZ = view.z;
            if (detailLen >= 0 && static_cast<size_t>(detailLen) < sizeof(detail) - 64)
            {
                detailLen += std::snprintf(
                    detail + detailLen, sizeof(detail) - static_cast<size_t>(detailLen),
                    "v%u=(%.2f,%.2f,%.2f)->viewZ=%.4f ", i, position.x, position.y, position.z, view.z);
            }
        }
        buffer->Unlock();

        log_line(
            "frame=" + std::to_string(gFrame) + " v6 glow geometry match vertexCount=" + std::to_string(vertexCount)
            + " viewZ_spread=" + std::to_string(maxZ - minZ) + " viewZ_min=" + std::to_string(minZ)
            + " viewZ_max=" + std::to_string(maxZ) + " " + std::string(detail));
    }

    void handle_candidate_draw_buffer(D3DPRIMITIVETYPE primitiveType, UINT startVertex, UINT primitiveCount)
    {
        ++gMatchedDrawPrimitive;

        // Runs on every matching draw, independent of the signature-cache
        // throttling below - throttling the geometry capture would defeat
        // the point of this version. Already narrow enough (exact signature,
        // not "any quad") to stay cheap.
        const auto vertexCount = vertex_count_for(primitiveType, primitiveCount);
        if (current_state_matches_glow_signature(vertexCount))
            log_glow_geometry(startVertex, vertexCount, gState.stream0Stride);

        const auto signature = signature_for(gState.stage0Texture, 0.0F, 0.0F);
        if (gLoggedSignaturesBuffer.count(signature) != 0)
        {
            if ((gMatchedDrawPrimitive % 600) != 0)
                return;
        }
        else if (gLoggedSignaturesBuffer.size() >= kMaxFullyLoggedSignatures)
        {
            ++gSuppressedFullLogs;
            return;
        }
        else
        {
            gLoggedSignaturesBuffer.insert(signature);
        }

        log_line(
            "frame=" + std::to_string(gFrame) + " DrawPrimitive (buffer-based, geometry not captured) type="
            + std::to_string(static_cast<int>(primitiveType)) + " startVertex=" + std::to_string(startVertex)
            + " primitiveCount=" + std::to_string(primitiveCount)
            + " stream0stride=" + std::to_string(gState.stream0Stride) + " " + render_state_suffix());
    }

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
            case D3DRS_ZFUNC: gState.zFunc = value; break;
            case D3DRS_ALPHABLENDENABLE: gState.alphaBlendEnable = value; break;
            case D3DRS_SRCBLEND: gState.srcBlend = value; break;
            case D3DRS_DESTBLEND: gState.destBlend = value; break;
            case D3DRS_BLENDOP: gState.blendOp = value; break;
            case D3DRS_ALPHATESTENABLE: gState.alphaTestEnable = value; break;
            default: break;
        }
        return result;
    }

    HRESULT __stdcall hook_set_transform(IDirect3DDevice8* self, D3DTRANSFORMSTATETYPE state, const D3DMATRIX* matrix)
    {
        const auto original = gOriginalSetTransform;
        if (gInsideSetTransform || original == nullptr)
            return original != nullptr ? original(self, state, matrix) : D3DERR_INVALIDCALL;
        gInsideSetTransform = true;
        const auto result = original(self, state, matrix);
        gInsideSetTransform = false;

        // Only World/View tracked - Projection doesn't matter for the
        // view-space Z-spread check, and other slots (texture transforms,
        // skinning matrices, etc.) are ignored entirely.
        if (matrix != nullptr)
        {
            if (state == D3DTS_WORLD)
                gWorld = *matrix;
            else if (state == D3DTS_VIEW)
                gView = *matrix;
        }
        return result;
    }

    HRESULT __stdcall hook_set_texture(IDirect3DDevice8* self, DWORD stage, IDirect3DBaseTexture8* texture)
    {
        const auto original = gOriginalSetTexture;
        if (gInsideSetTexture || original == nullptr)
            return original != nullptr ? original(self, stage, texture) : D3DERR_INVALIDCALL;
        gInsideSetTexture = true;
        const auto result = original(self, stage, texture);
        gInsideSetTexture = false;

        if (stage == 0)
            gState.stage0Texture = texture;
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

        // Just stashes stride and buffer pointer - never touches the
        // buffer's contents here. Used later only for a draw that already
        // matched the glow signature (see log_glow_geometry).
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
        const auto result = original(self, primitiveType, startVertex, primitiveCount);
        gInsideDrawPrimitive = false;

        // Same v4 change as DrawPrimitiveUP: gate on shape, not blend state -
        // knowable here without touching the buffer contents.
        if (shape_is_quad_like(vertex_count_for(primitiveType, primitiveCount)))
            handle_candidate_draw_buffer(primitiveType, startVertex, primitiveCount);
        return result;
    }

    HRESULT __stdcall hook_draw_primitive_up(
        IDirect3DDevice8* self, D3DPRIMITIVETYPE primitiveType, UINT primitiveCount,
        const void* vertexData, UINT stride)
    {
        const auto original = gOriginalDrawPrimitiveUP;
        if (gInsideDrawPrimitiveUP || original == nullptr)
            return original != nullptr ? original(self, primitiveType, primitiveCount, vertexData, stride) : D3DERR_INVALIDCALL;
        gInsideDrawPrimitiveUP = true;
        const auto result = original(self, primitiveType, primitiveCount, vertexData, stride);
        gInsideDrawPrimitiveUP = false;

        if (shape_is_quad_like(vertex_count_for(primitiveType, primitiveCount)))
            handle_candidate_draw_up(primitiveType, primitiveCount, vertexData, stride);
        return result;
    }

    HRESULT __stdcall hook_draw_indexed_primitive_up(
        IDirect3DDevice8* self, D3DPRIMITIVETYPE primitiveType, UINT minVertexIndex, UINT numVertices,
        UINT primitiveCount, const void* indexData, D3DFORMAT indexFormat, const void* vertexData, UINT stride)
    {
        const auto original = gOriginalDrawIndexedPrimitiveUP;
        if (gInsideDrawIndexedPrimitiveUP || original == nullptr)
        {
            return original != nullptr
                ? original(self, primitiveType, minVertexIndex, numVertices, primitiveCount, indexData, indexFormat, vertexData, stride)
                : D3DERR_INVALIDCALL;
        }
        gInsideDrawIndexedPrimitiveUP = true;
        const auto result = original(
            self, primitiveType, minVertexIndex, numVertices, primitiveCount, indexData, indexFormat, vertexData, stride);
        gInsideDrawIndexedPrimitiveUP = false;

        // Vertex array is still laid out linearly despite the index buffer,
        // so the same summarizer works fine - bounding box over all
        // supplied vertices, even if not all are referenced by this draw.
        if (shape_is_quad_like(numVertices))
        {
            ++gMatchedDrawPrimitiveUP;
            const auto summary = spectralfix::summarize_vertices(vertexData, stride, numVertices);
            if (!summary.valid)
            {
                log_line(
                    "DrawIndexedPrimitiveUP match, geometry not summarizable (stride=" + std::to_string(stride)
                    + " numVertices=" + std::to_string(numVertices) + ") " + render_state_suffix());
                return result;
            }
            const auto width = summary.maxX - summary.minX;
            const auto height = summary.maxY - summary.minY;
            const auto signature = signature_for(gState.stage0Texture, width, height);
            if (gLoggedSignaturesImmediate.count(signature) == 0)
            {
                if (gLoggedSignaturesImmediate.size() < kMaxFullyLoggedSignatures)
                {
                    gLoggedSignaturesImmediate.insert(signature);
                    char verts[256]{};
                    std::snprintf(
                        verts, sizeof(verts), "bbox=(%.1f,%.1f)-(%.1f,%.1f) v0=(%.1f,%.1f) v1=(%.1f,%.1f) hasUv=%s uniformColor=%s",
                        summary.minX, summary.minY, summary.maxX, summary.maxY,
                        summary.x[0], summary.y[0], summary.x[1], summary.y[1],
                        summary.hasUv ? "true" : "false", summary.uniformColor ? "true" : "false");
                    log_line(
                        "frame=" + std::to_string(gFrame) + " DrawIndexedPrimitiveUP match numVertices="
                        + std::to_string(numVertices) + " primitiveCount=" + std::to_string(primitiveCount)
                        + " stride=" + std::to_string(stride) + " " + verts + " " + render_state_suffix());
                }
                else
                {
                    ++gSuppressedFullLogs;
                }
            }
        }
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
        if (gFrame % 600 == 0)
        {
            log_line(
                "status[periodic] frames=" + std::to_string(gFrame)
                + " matched_drawprimitiveup=" + std::to_string(gMatchedDrawPrimitiveUP)
                + " matched_drawprimitive=" + std::to_string(gMatchedDrawPrimitive)
                + " unique_signatures_immediate=" + std::to_string(gLoggedSignaturesImmediate.size())
                + " unique_signatures_buffer=" + std::to_string(gLoggedSignaturesBuffer.size())
                + " suppressed=" + std::to_string(gSuppressedFullLogs));
        }
        return result;
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
                return; // already hooked
            *originalStorageAsVoidPtr = previous;
            write_vtable_slot(slot, hook);
        };

        hookOne(gDeviceVtable, kSetRenderStateSlot, reinterpret_cast<void*>(&hook_set_render_state),
            reinterpret_cast<void**>(&gOriginalSetRenderState));
        hookOne(gDeviceVtable, kSetTransformSlot, reinterpret_cast<void*>(&hook_set_transform),
            reinterpret_cast<void**>(&gOriginalSetTransform));
        hookOne(gDeviceVtable, kSetTextureSlot, reinterpret_cast<void*>(&hook_set_texture),
            reinterpret_cast<void**>(&gOriginalSetTexture));
        hookOne(gDeviceVtable, kDrawPrimitiveSlot, reinterpret_cast<void*>(&hook_draw_primitive),
            reinterpret_cast<void**>(&gOriginalDrawPrimitive));
        hookOne(gDeviceVtable, kDrawPrimitiveUPSlot, reinterpret_cast<void*>(&hook_draw_primitive_up),
            reinterpret_cast<void**>(&gOriginalDrawPrimitiveUP));
        hookOne(gDeviceVtable, kDrawIndexedPrimitiveUPSlot, reinterpret_cast<void*>(&hook_draw_indexed_primitive_up),
            reinterpret_cast<void**>(&gOriginalDrawIndexedPrimitiveUP));
        hookOne(gDeviceVtable, kSetStreamSourceSlot, reinterpret_cast<void*>(&hook_set_stream_source),
            reinterpret_cast<void**>(&gOriginalSetStreamSource));
        hookOne(gDeviceVtable, kPresentSlot, reinterpret_cast<void*>(&hook_present),
            reinterpret_cast<void**>(&gOriginalPresent));

        log_line(
            "flare_diag v6 attached to device; DrawPrimitiveUP/DrawIndexedPrimitiveUP/DrawPrimitive still log "
            "every quad-shaped draw (<=8 vertices) regardless of blend/Z state (v4/v5). New in v6: a "
            "SetTransform hook tracks World/View matrices, and any DrawPrimitive matching the tight glow "
            "signature (zenable, zwrite off, additive srcalpha/one blend, stride=36, <=8 verts) gets a single "
            "read-only Lock on its vertex range so positions can be transformed to view space and their Z "
            "spread logged - testing whether the billboard is camera-facing or offset from its housing.");
    }

    // Below: same proxy/CreateDevice-hook shell as windower/src/d3d8_proxy.cpp
    // (see that file for commentary). Duplicated rather than shared so this
    // stays a standalone throwaway tool, never built/shipped with the real fix.

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
    log_line("=== flare_diag proxy loaded ===");
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
