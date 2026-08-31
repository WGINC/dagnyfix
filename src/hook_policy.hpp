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
// Decision logic for the D3D8 vtable hook table - no actual memory writes here,
// those live in spectralfix.cpp, so this stays unit testable.

#pragma once

#include <cstddef>
#include <cstdint>

namespace spectralfix
{
    constexpr uint32_t kDrawChainMissThreshold = 3;

    struct HookSlotView
    {
        const void* ours{nullptr};    // our hook function for this slot
        const void* current{nullptr}; // what's actually in the vtable right now
        bool tracked{false};          // false once we deliberately released the slot
    };

    inline bool slot_is_intact(const HookSlotView& slot)
    {
        return !slot.tracked || slot.current == slot.ours;
    }

    // A slot we released on purpose (e.g. handing the SetTexture observer back to
    // native D3D8) doesn't count as a failure here - otherwise we'd strand the rest.
    inline bool tracked_hooks_intact(const HookSlotView* slots, const size_t count)
    {
        if (slots == nullptr)
            return false;
        for (size_t i = 0; i < count; ++i)
        {
            if (!slot_is_intact(slots[i]))
                return false;
        }
        return true;
    }

    inline bool slot_is_displaced(const HookSlotView& slot)
    {
        return slot.tracked && slot.current != slot.ours;
    }

    constexpr bool must_retain_hooks_on_release(
        const bool hooksPublished,
        const bool enlargementPublished)
    {
        return hooksPublished && enlargementPublished;
    }

    enum class DrawChainHealth
    {
        owned,
        active,
        inconclusive,
        lost,
    };

    struct DrawChainSample
    {
        DrawChainHealth health{DrawChainHealth::inconclusive};
        uint32_t consecutiveMisses{0};
    };

    // Another hook sitting on top of DrawPrimitiveUP might still forward to us,
    // so one quiet sample isn't proof the chain broke - only declare it lost after
    // several consecutive windows with frames presented but nothing intercepted.
    inline DrawChainSample evaluate_draw_chain_sample(
        const bool drawSlotOwned,
        const uint64_t interceptedNow,
        const uint64_t interceptedAtLastSample,
        const uint32_t previousMisses,
        const uint32_t missesRequired)
    {
        if (drawSlotOwned)
            return {DrawChainHealth::owned, 0};
        if (interceptedNow != interceptedAtLastSample)
            return {DrawChainHealth::active, 0};

        const auto misses = previousMisses + 1;
        return {
            misses >= missesRequired ? DrawChainHealth::lost : DrawChainHealth::inconclusive,
            misses,
        };
    }
}
