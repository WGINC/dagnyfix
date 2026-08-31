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
// d3d8.dll proxy entry point for the Windower port of SpectralFix.
//
// FFXI loads whatever "d3d8.dll" sits next to polboot.exe/pol.exe via normal
// DLL search order. This DLL takes that slot, forwards every real export to
// the actual D3D8 implementation (native, dgVoodoo2, whatever), and only
// intervenes at one seam: patching IDirect3D8::CreateDevice so once the
// game's real device exists it gets handed to WindowerCore, same as Ashita's
// Direct3DInitialize did.
//
// No user-facing config for "which d3d8 backend" - we just probe a fixed
// list of renamed-file conventions and fall back to System32's copy by
// explicit path so we can't ever recursively load ourselves.

#include "windower_core.hpp"

#include <array>
#include <cstdint>
#include <string>

namespace
{
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

    constexpr uint32_t kCreateDeviceSlot = 15; // IDirect3D8::CreateDevice vtable index.

    // Rename conventions from windower/README.md, tried in order - first one
    // present and loadable wins.
    constexpr std::array<const char*, 2> kKnownRenamedBackends = {
        "d3d8_dgvoodoo.dll", // dgVoodoo2's own d3d8.dll, renamed so it can sit beneath us.
        "d3d8_orig.dll",     // generic "the real d3d8 you replaced", for any other wrapper.
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

    // Never load a bare "d3d8.dll" by unqualified name here - since we ARE the
    // local d3d8.dll, that would just load ourselves again. Everything below is
    // loaded by explicit full path for that reason.
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

        if (SUCCEEDED(result) && returnedDeviceInterface != nullptr && *returnedDeviceInterface != nullptr && gCore != nullptr)
        {
            // FFXI only makes one device in practice, but attach_device() is
            // idempotent in case something ever recreates it.
            try
            {
                gCore->attach_device(*returnedDeviceInterface);
            }
            catch (...)
            {
                // don't let this take device creation down with it
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
            return; // already hooked, or nothing to hook onto
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
            // no LoadLibrary/device work here - standard loader-lock hygiene.
            // load_real_backend()/ensure_core_initialized() happen lazily on the
            // first real Direct3DCreate8 call instead.
            ::DisableThreadLibraryCalls(module);
            break;
        case DLL_PROCESS_DETACH:
            if (gCore != nullptr)
            {
                gCore->release();
                // if release() refused (enlargement still live), WindowerCore's
                // destructor deliberately leaks impl_ - the hook functions are
                // still reachable from the device vtable at process-exit time.
                delete gCore;
                gCore = nullptr;
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
