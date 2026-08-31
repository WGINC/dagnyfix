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

#include <cstdint>

namespace spectralfix
{
    enum class SelectorActivityDecision
    {
        pending,
        confirmed,
        mismatch,
    };

    constexpr bool candidate_context_is_trusted(
        const bool deviceMatches,
        const uint32_t stackHash)
    {
        return deviceMatches && stackHash != 0;
    }

    constexpr bool resource_marker_dimensions_valid(
        const uint32_t originalSize,
        const uint32_t actualSize,
        const uint32_t expectedOriginalSize)
    {
        return expectedOriginalSize != 0
            && originalSize == expectedOriginalSize
            && actualSize >= originalSize;
    }

    constexpr bool should_arm_ordinal_one_default(
        const bool selectorValid,
        const uint32_t candidateId,
        const uint32_t signatureOrdinal)
    {
        return !selectorValid && candidateId == 1 && signatureOrdinal == 1;
    }

    constexpr bool selector_identity_matches(
        const uint32_t selectedModuleTimestamp,
        const uint32_t selectedModuleSize,
        const uint32_t selectedCallerRva,
        const uint32_t selectedStackHash,
        const uint32_t selectedSignatureOrdinal,
        const uint32_t observedModuleTimestamp,
        const uint32_t observedModuleSize,
        const uint32_t observedCallerRva,
        const uint32_t observedStackHash,
        const uint32_t observedSignatureOrdinal)
    {
        return selectedModuleTimestamp == observedModuleTimestamp
            && selectedModuleSize == observedModuleSize
            && selectedCallerRva == observedCallerRva
            && selectedStackHash == observedStackHash
            && selectedSignatureOrdinal == observedSignatureOrdinal;
    }

    constexpr SelectorActivityDecision evaluate_selector_activity(
        const bool verificationPending,
        const bool identityMatches)
    {
        if (!verificationPending)
            return SelectorActivityDecision::pending;
        return identityMatches
            ? SelectorActivityDecision::confirmed
            : SelectorActivityDecision::mismatch;
    }

    constexpr bool selected_aura_marker_is_trackable(
        const bool selectorValid,
        const bool identityMatches,
        const uint32_t actualSize,
        const uint32_t originalSize)
    {
        return selectorValid && identityMatches
            && actualSize >= originalSize && originalSize != 0;
    }

}
