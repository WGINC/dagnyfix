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
// All-or-nothing install + rollback for a set of vtable hooks.

#pragma once

#include <cstddef>

namespace spectralfix
{
    enum class HookTransactionResult
    {
        installed,
        rolledBack,
        rollbackIncomplete,
    };

    // Slot needs target/hook/previous/tracked fields. Writer attempts one slot write
    // and reports whether the requested value stuck. Even a "failed" write gets
    // rolled back - a writer can publish the value and still lose the verify race.
    template<typename Slot, typename Writer>
    HookTransactionResult install_hook_transaction(
        Slot* slots,
        const size_t count,
        Writer&& writer)
    {
        if (slots == nullptr)
            return HookTransactionResult::rolledBack;

        size_t attempted = 0;
        for (size_t i = 0; i < count; ++i)
        {
            attempted = i + 1;
            if (!writer(slots[i].target, slots[i].hook))
                break;
            slots[i].tracked = true;
        }

        if (attempted == count && (count == 0 || slots[count - 1].tracked))
            return HookTransactionResult::installed;

        bool rollbackComplete = true;
        for (size_t i = attempted; i > 0; --i)
        {
            auto& slot = slots[i - 1];
            if (slot.target == nullptr || slot.hook == nullptr || slot.previous == nullptr)
            {
                rollbackComplete = false;
                continue;
            }

            if (*slot.target == slot.hook)
            {
                slot.tracked = true;
                (void)writer(slot.target, slot.previous);
            }

            if (*slot.target == slot.previous)
            {
                slot.tracked = false;
            }
            else
            {
                // Someone else won the slot - we're not on top anymore, don't claim it.
                // State's still not back to original though, so this still counts as incomplete.
                if (*slot.target != slot.hook)
                    slot.tracked = false;
                rollbackComplete = false;
            }
        }

        return rollbackComplete
            ? HookTransactionResult::rolledBack
            : HookTransactionResult::rollbackIncomplete;
    }
}
