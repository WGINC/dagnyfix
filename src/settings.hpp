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
// Parsing/serialization for spectralfix.ini. No Windows/D3D deps, so the round
// trip is unit testable.

#pragma once

#include <cmath>
#include <cstdint>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

namespace spectralfix
{
    constexpr uint32_t kMediumTargetSize  = 1024;
    constexpr uint32_t kDefaultTargetSize = 2048;
    constexpr uint32_t kUltraTargetSize   = 4096;
    constexpr uint32_t kSettingsVersion   = 1;

    constexpr float kMinManualSpread                = 1.0F;
    constexpr float kMaxManualSpread                = 16.0F;
    constexpr float kDefaultSpread                  = 2.0F;
    constexpr float kDefaultOpacityPercent          = 100.0F;
    constexpr float kDefaultCompositeOpacityPercent = 25.0F;

    inline std::string lower_copy(std::string value)
    {
        for (auto& character : value)
        {
            if (character >= 'A' && character <= 'Z')
                character = static_cast<char>(character - 'A' + 'a');
        }
        return value;
    }

    inline std::string trim_copy(std::string value)
    {
        const auto first = value.find_first_not_of(" \t\r\n");
        if (first == std::string::npos)
            return {};
        const auto last = value.find_last_not_of(" \t\r\n");
        return value.substr(first, last - first + 1);
    }

    inline bool parse_float(const std::string& text, float& value)
    {
        if (text.empty())
            return false;
        try
        {
            size_t used = 0;
            const auto number = std::stof(text, &used);
            if (used != text.size() || !std::isfinite(number))
                return false;
            value = number;
            return true;
        }
        catch (...)
        {
            return false;
        }
    }

    // Rejects negatives explicitly - std::stoul would otherwise silently wrap
    // them into a huge positive number.
    inline bool parse_u32(const std::string& text, uint32_t& value)
    {
        if (text.empty() || text.front() == '-')
            return false;
        try
        {
            size_t used = 0;
            const auto number = std::stoul(text, &used, 0);
            if (used != text.size())
                return false;
            value = static_cast<uint32_t>(number);
            return true;
        }
        catch (...)
        {
            return false;
        }
    }

    // Identifies the specific client-build allocation we enlarge.
    struct SelectorFields
    {
        uint32_t version{0};
        uint32_t moduleTimestamp{0};
        uint32_t moduleSize{0};
        uint32_t callerRva{0};
        uint32_t stackHash{0};
        uint32_t signatureOrdinal{0};
        uint32_t targetSize{kDefaultTargetSize};
    };

    // Valid independently of the selector - these are just appearance knobs.
    struct VisualSettings
    {
        float spreadOverride{kDefaultSpread}; // 0 = automatic
        float opacityPercent{kDefaultOpacityPercent};
        float compositeOpacityPercent{kDefaultCompositeOpacityPercent};
        bool compositeOpacityOverride{true};
    };

    struct SettingsFile
    {
        SelectorFields selector{};
        VisualSettings visual{};
    };

    constexpr bool spread_override_in_range(const float value)
    {
        return value == 0.0F || (value >= kMinManualSpread && value <= kMaxManualSpread);
    }

    constexpr bool opacity_percent_in_range(const float value)
    {
        return value >= 0.0F && value <= 100.0F;
    }

    constexpr bool target_size_supported(const uint32_t value)
    {
        return value == kMediumTargetSize
            || value == kDefaultTargetSize
            || value == kUltraTargetSize;
    }

    constexpr bool selector_identity_present(const SelectorFields& selector)
    {
        return selector.moduleTimestamp != 0
            || selector.moduleSize != 0
            || selector.callerRva != 0
            || selector.stackHash != 0
            || selector.signatureOrdinal != 0;
    }

    // Unknown keys are silently ignored; known keys with bad values keep the
    // existing setting and add a warning instead of failing outright.
    inline void parse_settings_text(
        const std::string& text,
        SettingsFile& settings,
        std::vector<std::string>& warnings)
    {
        std::istringstream input(text);
        std::string line;
        while (std::getline(input, line))
        {
            const auto hash = line.find('#');
            if (hash != std::string::npos)
                line.erase(hash);
            const auto equal = line.find('=');
            if (equal == std::string::npos)
                continue;

            const auto key   = lower_copy(trim_copy(line.substr(0, equal)));
            const auto value = trim_copy(line.substr(equal + 1));
            if (key.empty())
                continue;

            const auto reject = [&warnings, &key]() {
                warnings.push_back("Ignored malformed spectralfix.ini value for key: " + key);
            };

            if (key == "spread_override")
            {
                float number = 0.0F;
                if (parse_float(value, number) && spread_override_in_range(number))
                    settings.visual.spreadOverride = number;
                else
                    reject();
            }
            else if (key == "opacity_percent")
            {
                float number = 0.0F;
                if (parse_float(value, number) && opacity_percent_in_range(number))
                    settings.visual.opacityPercent = number;
                else
                    reject();
            }
            else if (key == "composite_opacity_percent")
            {
                const auto lowered = lower_copy(value);
                float number       = 0.0F;
                if (lowered == "stock" || lowered == "auto")
                {
                    settings.visual.compositeOpacityOverride = false;
                    settings.visual.compositeOpacityPercent  = kDefaultOpacityPercent;
                }
                else if (parse_float(value, number) && opacity_percent_in_range(number))
                {
                    settings.visual.compositeOpacityOverride = true;
                    settings.visual.compositeOpacityPercent  = number;
                }
                else
                {
                    reject();
                }
            }
            else if (key == "version" || key == "module_timestamp" || key == "module_size"
                || key == "caller_rva" || key == "stack_hash" || key == "signature_ordinal"
                || key == "target_size")
            {
                uint32_t number = 0;
                if (!parse_u32(value, number))
                {
                    reject();
                }
                else if (key == "version")
                {
                    settings.selector.version = number;
                }
                else if (key == "module_timestamp")
                {
                    settings.selector.moduleTimestamp = number;
                }
                else if (key == "module_size")
                {
                    settings.selector.moduleSize = number;
                }
                else if (key == "caller_rva")
                {
                    settings.selector.callerRva = number;
                }
                else if (key == "stack_hash")
                {
                    settings.selector.stackHash = number;
                }
                else if (key == "signature_ordinal")
                {
                    settings.selector.signatureOrdinal = number;
                }
                else if (target_size_supported(number))
                {
                    settings.selector.targetSize = number;
                }
                else
                {
                    reject();
                }
            }
        }
    }

    inline std::string serialize_settings_text(const SettingsFile& settings)
    {
        std::ostringstream out;
        out << "# SpectralFix selector and visual settings. The selector is client-build-specific.\n";
        out << "version=" << kSettingsVersion << '\n';
        out << std::hex << std::uppercase;
        out << "module_timestamp=0x" << settings.selector.moduleTimestamp << '\n';
        out << "module_size=0x" << settings.selector.moduleSize << '\n';
        out << "caller_rva=0x" << settings.selector.callerRva << '\n';
        out << "stack_hash=0x" << settings.selector.stackHash << '\n';
        out << std::dec << std::nouppercase;
        out << "signature_ordinal=" << settings.selector.signatureOrdinal << '\n';
        out << "target_size=" << settings.selector.targetSize << '\n';
        out << std::fixed << std::setprecision(2);
        out << "spread_override=" << settings.visual.spreadOverride << " # 0 = automatic\n";
        out << "opacity_percent=" << settings.visual.opacityPercent
            << " # 100 = stock tap opacity\n";
        out << "composite_opacity_percent=";
        if (settings.visual.compositeOpacityOverride)
            out << settings.visual.compositeOpacityPercent;
        else
            out << "stock";
        out << " # stock = original hard-cutout center pass\n";
        return out.str();
    }
}
