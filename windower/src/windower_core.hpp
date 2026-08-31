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
// Public interface of the Windower-hosted aura correction core.
//
// Windower counterpart to spectralfix::Plugin in ../../src/spectralfix.cpp --
// reuses the framework-agnostic headers from ../../src/ unchanged and
// reimplements just the Ashita-specific shell (IPlugin, IAshitaCore, chat
// manager, install path) for a standalone d3d8 proxy DLL. Ashita build is
// untouched by this port.

#pragma once

#include <Windows.h>

#include "d3d8/includes/d3d8.h"

#include <string>
#include <vector>

namespace spectralfix_w
{
    // Owns the hooked vtable slots: CreateTexture, SetTexture, DrawPrimitiveUP
    // (same as the Ashita build) plus Present, which we have to hook ourselves
    // here since there's no IPlugin callback to give us a per-frame tick.
    class WindowerCore
    {
    public:
        WindowerCore();
        ~WindowerCore();

        WindowerCore(const WindowerCore&)            = delete;
        WindowerCore& operator=(const WindowerCore&) = delete;

        // basePath: directory SpectralFix's own d3d8.dll lives in, used the same
        // way Ashita's GetInstallPath() was.
        bool initialize(const std::string& basePath);

        // Called once, right after the real CreateDevice call succeeds inside the
        // hook. Equivalent to Ashita's Direct3DInitialize.
        bool attach_device(IDirect3DDevice8* device);

        // Called from hooked Present, once per frame. Equivalent to Direct3DPresent.
        void present_tick();

        // args[0] is the subcommand ("help", "status", "spread", ...); no leading
        // slash or "spectralfix" token -- the caller strips that before calling in.
        void handle_command(const std::vector<std::string>& args);

        // Mirrors Plugin::Release(): refuses to drop the hooks while an enlarged
        // allocation is live. Nothing normally calls this (proxy DLL only dies at
        // process exit) but DllMain's DETACH path uses it to ask the same question
        // Ashita's unload path did.
        bool release();

        bool release_refused() const;

        // Static so d3d8_proxy.cpp's free-function hooks can reach the live instance.
        static WindowerCore* instance() { return instance_; }

    private:
        struct Impl;
        Impl* impl_;
        bool releaseRefused_{false};
        static WindowerCore* instance_;
    };
}
