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
// One-shot latch for hot-path failure notifications.

#pragma once

namespace spectralfix
{
    class OneShotNotice
    {
    public:
        constexpr void record()
        {
            if (!queuedOrReported_)
            {
                queuedOrReported_ = true;
                pending_          = true;
            }
        }

        constexpr bool consume()
        {
            if (!pending_)
                return false;
            pending_ = false;
            return true;
        }

        constexpr bool queued_or_reported() const
        {
            return queuedOrReported_;
        }

    private:
        bool pending_{false};
        bool queuedOrReported_{false};
    };
}
