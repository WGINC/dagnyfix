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
// Snapshot/restore of the render state touched by the center-composite override.
// Templated on Device so tests can swap in a recording stand-in instead of a real D3D device.

#pragma once

#include <Windows.h>

#include "d3d8/includes/d3d8.h"

namespace spectralfix
{
    struct StateValue
    {
        DWORD value{0};
        bool valid{false};
    };

    template <typename Device>
    StateValue read_render_state(Device* device, const D3DRENDERSTATETYPE state)
    {
        StateValue result{};
        result.valid = device != nullptr && SUCCEEDED(device->GetRenderState(state, &result.value));
        return result;
    }

    template <typename Device>
    StateValue read_texture_state(Device* device, const D3DTEXTURESTAGESTATETYPE state)
    {
        StateValue result{};
        result.valid = device != nullptr
            && SUCCEEDED(device->GetTextureStageState(0, state, &result.value));
        return result;
    }

    inline bool state_is(const StateValue& state, const DWORD expected)
    {
        return state.valid && state.value == expected;
    }

    // Restores state on every exit path, including the destructor - a faulting draw
    // still needs to unwind, or our blend state leaks into the rest of the frame.
    template <typename Device>
    class BasicCompositeStateScope final
    {
    public:
        BasicCompositeStateScope(Device* device, bool* restoreFailedOut)
            : device_(device)
            , restoreFailedOut_(restoreFailedOut)
        {
            alphaBlend_ = read_render_state(device, D3DRS_ALPHABLENDENABLE);
            srcBlend_   = read_render_state(device, D3DRS_SRCBLEND);
            dstBlend_   = read_render_state(device, D3DRS_DESTBLEND);
            blendOp_    = read_render_state(device, D3DRS_BLENDOP);
            alphaTest_  = read_render_state(device, D3DRS_ALPHATESTENABLE);
            alphaOp_    = read_texture_state(device, D3DTSS_ALPHAOP);
            alphaArg1_  = read_texture_state(device, D3DTSS_ALPHAARG1);
            alphaArg2_  = read_texture_state(device, D3DTSS_ALPHAARG2);
            captured_   = device_ != nullptr
                && alphaBlend_.valid && srcBlend_.valid && dstBlend_.valid && blendOp_.valid
                && alphaTest_.valid && alphaOp_.valid && alphaArg1_.valid && alphaArg2_.valid;
        }

        ~BasicCompositeStateScope()
        {
            if (!restore() && restoreFailedOut_ != nullptr)
                *restoreFailedOut_ = true;
        }

        BasicCompositeStateScope(const BasicCompositeStateScope&)            = delete;
        BasicCompositeStateScope& operator=(const BasicCompositeStateScope&) = delete;

        bool captured() const { return captured_; }

        // applied_ is set before the writes start, so a partial apply still gets restored.
        bool apply()
        {
            if (!captured_)
                return false;
            applied_  = true;
            bool done = true;
            done = SUCCEEDED(device_->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE)) && done;
            done = SUCCEEDED(device_->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA)) && done;
            done = SUCCEEDED(device_->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA)) && done;
            done = SUCCEEDED(device_->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD)) && done;
            done = SUCCEEDED(device_->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE)) && done;
            done = SUCCEEDED(device_->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE)) && done;
            done = SUCCEEDED(device_->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE)) && done;
            done = SUCCEEDED(device_->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE)) && done;
            return done;
        }

        // Safe to call multiple times; no-op if apply() never ran.
        bool restore()
        {
            if (!captured_ || !applied_ || restored_)
                return true;
            restored_ = true;
            bool done = true;
            done = SUCCEEDED(device_->SetTextureStageState(0, D3DTSS_ALPHAARG2, alphaArg2_.value)) && done;
            done = SUCCEEDED(device_->SetTextureStageState(0, D3DTSS_ALPHAARG1, alphaArg1_.value)) && done;
            done = SUCCEEDED(device_->SetTextureStageState(0, D3DTSS_ALPHAOP, alphaOp_.value)) && done;
            done = SUCCEEDED(device_->SetRenderState(D3DRS_ALPHATESTENABLE, alphaTest_.value)) && done;
            done = SUCCEEDED(device_->SetRenderState(D3DRS_BLENDOP, blendOp_.value)) && done;
            done = SUCCEEDED(device_->SetRenderState(D3DRS_DESTBLEND, dstBlend_.value)) && done;
            done = SUCCEEDED(device_->SetRenderState(D3DRS_SRCBLEND, srcBlend_.value)) && done;
            done = SUCCEEDED(device_->SetRenderState(D3DRS_ALPHABLENDENABLE, alphaBlend_.value)) && done;
            return done;
        }

    private:
        Device* device_{nullptr};
        bool* restoreFailedOut_{nullptr};
        StateValue alphaBlend_{};
        StateValue srcBlend_{};
        StateValue dstBlend_{};
        StateValue blendOp_{};
        StateValue alphaTest_{};
        StateValue alphaOp_{};
        StateValue alphaArg1_{};
        StateValue alphaArg2_{};
        bool captured_{false};
        bool applied_{false};
        bool restored_{false};
    };

    using CompositeStateScope = BasicCompositeStateScope<IDirect3DDevice8>;
}
