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
#pragma once

#include "settings.hpp"

#include <cstdint>

namespace spectralfix
{
    constexpr bool selector_fields_valid(
        const uint32_t version,
        const uint32_t moduleTimestamp,
        const uint32_t moduleSize,
        const uint32_t callerRva,
        const uint32_t stackHash,
        const uint32_t signatureOrdinal,
        const uint32_t targetSize)
    {
        (void)callerRva;
        return version == kSettingsVersion
            && moduleTimestamp != 0
            && moduleSize != 0
            && stackHash != 0
            && signatureOrdinal != 0
            && target_size_supported(targetSize);
    }
}
