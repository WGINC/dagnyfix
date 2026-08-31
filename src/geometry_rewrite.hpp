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

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace spectralfix
{
    struct GeometrySummary
    {
        bool valid{false};
        bool hasColor{false};
        bool uniformColor{false};
        bool hasUv{false};
        uint32_t storedVertices{0};
        uint32_t firstColor{0};
        float minX{0.0F};
        float minY{0.0F};
        float maxX{0.0F};
        float maxY{0.0F};
        std::array<float, 4> x{};
        std::array<float, 4> y{};
        std::array<float, 4> u{};
        std::array<float, 4> v{};
    };

    inline GeometrySummary summarize_vertices(
        const void* vertexData,
        const uint32_t stride,
        const uint32_t count)
    {
        GeometrySummary result{};
        if (vertexData == nullptr || stride < 16 || stride > 256 || count == 0 || count > 16)
            return result;

        const auto source = static_cast<const uint8_t*>(vertexData);
        result.minX = 3.402823466e+38F;
        result.minY = 3.402823466e+38F;
        result.maxX = -3.402823466e+38F;
        result.maxY = -3.402823466e+38F;
        result.hasColor = stride >= 20;
        result.hasUv = stride >= 28;
        result.uniformColor = result.hasColor;
        result.storedVertices = std::min<uint32_t>(count, 4);

        for (uint32_t i = 0; i < count; ++i)
        {
            const auto vertex = source + static_cast<size_t>(i) * stride;
            const auto values = reinterpret_cast<const float*>(vertex);
            if (!std::isfinite(values[0]) || !std::isfinite(values[1]))
                return GeometrySummary{};

            result.minX = std::min(result.minX, values[0]);
            result.minY = std::min(result.minY, values[1]);
            result.maxX = std::max(result.maxX, values[0]);
            result.maxY = std::max(result.maxY, values[1]);

            if (i < result.storedVertices)
            {
                result.x[i] = values[0];
                result.y[i] = values[1];
            }

            if (result.hasColor)
            {
                uint32_t color = 0;
                std::memcpy(&color, vertex + 16, sizeof(color));
                if (i == 0)
                    result.firstColor = color;
                else if (color != result.firstColor)
                    result.uniformColor = false;
            }

            if (result.hasUv)
            {
                float uv[2]{};
                std::memcpy(uv, vertex + 20, sizeof(uv));
                if (!std::isfinite(uv[0]) || !std::isfinite(uv[1]))
                    result.hasUv = false;
                else if (i < result.storedVertices)
                {
                    result.u[i] = uv[0];
                    result.v[i] = uv[1];
                }
            }
        }

        result.valid = true;
        return result;
    }

    inline bool matches_center_composite_vertices(
        const void* vertexData,
        const uint32_t stride,
        const uint32_t count,
        const uint32_t renderWidth,
        const uint32_t renderHeight)
    {
        if (renderWidth == 0 || renderHeight == 0)
            return false;
        const auto summary = summarize_vertices(vertexData, stride, count);
        if (!summary.valid || !summary.hasUv || summary.storedVertices != 4 || count != 4)
            return false;

        constexpr float tolerance = 0.01F;
        const auto withinTolerance = [](const float left, const float right) {
            return std::fabs(left - right) <= tolerance;
        };
        const auto width = static_cast<float>(renderWidth);
        const auto height = static_cast<float>(renderHeight);
        return withinTolerance(summary.x[0], 0.0F) && withinTolerance(summary.y[0], 0.0F)
            && withinTolerance(summary.x[1], width) && withinTolerance(summary.y[1], 0.0F)
            && withinTolerance(summary.x[2], 0.0F) && withinTolerance(summary.y[2], height)
            && withinTolerance(summary.x[3], width) && withinTolerance(summary.y[3], height)
            && withinTolerance(summary.u[0], 0.0F) && withinTolerance(summary.v[0], 0.0F)
            && withinTolerance(summary.u[1], 1.0F) && withinTolerance(summary.v[1], 0.0F)
            && withinTolerance(summary.u[2], 0.0F) && withinTolerance(summary.v[2], 1.0F)
            && withinTolerance(summary.u[3], 1.0F) && withinTolerance(summary.v[3], 1.0F);
    }

    inline bool rewrite_uniform_alpha(
        const void* vertexData,
        const uint32_t stride,
        const uint32_t count,
        const float opacityPercent,
        uint8_t* output,
        const size_t outputSize)
    {
        if (vertexData == nullptr || output == nullptr || stride < 20 || count == 0
            || count > outputSize / stride || !std::isfinite(opacityPercent)
            || opacityPercent < 0.0F || opacityPercent > 100.0F)
            return false;

        std::memcpy(output, vertexData, static_cast<size_t>(count) * stride);
        const auto alpha = static_cast<uint32_t>(std::lround(opacityPercent * 2.55F));
        for (uint32_t i = 0; i < count; ++i)
        {
            auto vertex = output + static_cast<size_t>(i) * stride;
            uint32_t color = 0;
            std::memcpy(&color, vertex + 16, sizeof(color));
            color = (color & 0x00FFFFFFU) | (std::min(alpha, 255U) << 24);
            std::memcpy(vertex + 16, &color, sizeof(color));
        }
        return true;
    }

    struct TapRewriteResult
    {
        bool matched{false};
        bool rewritten{false};
        bool spreadAdjusted{false};
        bool opacityAdjusted{false};
        float effectiveSpread{1.0F};
    };

    inline bool rewrite_downsample_vertices(
        const void* vertexData,
        const uint32_t stride,
        const uint32_t count,
        const uint32_t targetSize,
        const uint32_t originalSize,
        uint8_t* output,
        const size_t outputSize)
    {
        if (vertexData == nullptr || output == nullptr || stride < sizeof(float) * 2
            || count == 0 || originalSize == 0 || targetSize <= originalSize
            || count > outputSize / stride)
            return false;

        const auto source = static_cast<const uint8_t*>(vertexData);
        float maxX        = -3.402823466e+38F;
        float maxY        = -3.402823466e+38F;
        for (uint32_t i = 0; i < count; ++i)
        {
            const auto f = reinterpret_cast<const float*>(source + i * stride);
            if (f[0] != f[0] || f[1] != f[1]
                || f[0] < -2.0F || f[0] > static_cast<float>(originalSize) + 2.0F
                || f[1] < -2.0F || f[1] > static_cast<float>(originalSize) + 2.0F)
                return false;
            maxX = std::max(maxX, f[0]);
            maxY = std::max(maxY, f[1]);
        }
        if (maxX <= 1.5F && maxY <= 1.5F)
            return false;

        std::memcpy(output, source, static_cast<size_t>(count) * stride);
        const auto scale = static_cast<float>(targetSize) / static_cast<float>(originalSize);
        for (uint32_t i = 0; i < count; ++i)
        {
            auto f = reinterpret_cast<float*>(output + i * stride);
            f[0]   = (f[0] + 0.5F) * scale - 0.5F;
            f[1]   = (f[1] + 0.5F) * scale - 0.5F;
        }
        return true;
    }

    inline TapRewriteResult rewrite_tap_vertices(
        const void* vertexData,
        const uint32_t stride,
        const uint32_t count,
        const float spread,
        const float opacityScale,
        uint8_t* output,
        const size_t outputSize)
    {
        TapRewriteResult result{};
        result.effectiveSpread = spread;
        if (vertexData == nullptr || output == nullptr || stride < 20
            || count != 4 || !std::isfinite(spread) || spread < 1.0F || spread > 16.0F
            || !std::isfinite(opacityScale) || opacityScale < 0.0F || opacityScale > 1.0F
            || count > outputSize / stride)
            return result;

        const auto source = static_cast<const uint8_t*>(vertexData);
        float minX        = 3.402823466e+38F;
        float minY        = 3.402823466e+38F;
        float maxX        = -3.402823466e+38F;
        float maxY        = -3.402823466e+38F;
        for (uint32_t i = 0; i < count; ++i)
        {
            const auto f = reinterpret_cast<const float*>(source + i * stride);
            if (f[0] != f[0] || f[1] != f[1])
                return result;
            minX = std::min(minX, f[0]);
            minY = std::min(minY, f[1]);
            maxX = std::max(maxX, f[0]);
            maxY = std::max(maxY, f[1]);
        }

        if ((maxX - minX) < 256.0F || (maxY - minY) < 256.0F)
            return result;
        if (minX < -64.0F || minX > 64.0F || minY < -64.0F || minY > 64.0F)
            return result;
        if (minX == 0.0F && minY == 0.0F)
            return result;

        uint32_t firstColor = 0;
        std::memcpy(&firstColor, source + 16, sizeof(firstColor));
        const auto firstAlpha = static_cast<uint8_t>(firstColor >> 24);
        if (firstAlpha == 0 || firstAlpha > 64)
            return result;
        for (uint32_t i = 1; i < count; ++i)
        {
            uint32_t color = 0;
            std::memcpy(&color, source + i * stride + 16, sizeof(color));
            if (color != firstColor)
                return result;
        }

        result.matched         = true;
        result.spreadAdjusted  = spread > 1.0001F;
        result.opacityAdjusted = opacityScale < 0.9999F;
        if (!result.spreadAdjusted && !result.opacityAdjusted)
            return result;

        std::memcpy(output, source, static_cast<size_t>(count) * stride);
        const auto extra = spread - 1.0F;
        for (uint32_t i = 0; i < count; ++i)
        {
            auto f = reinterpret_cast<float*>(output + i * stride);
            if (result.spreadAdjusted)
            {
                f[0] += minX * extra;
                f[1] += minY * extra;
            }
            if (result.opacityAdjusted)
            {
                uint32_t color = 0;
                std::memcpy(&color, output + i * stride + 16, sizeof(color));
                const auto alpha = static_cast<uint32_t>(color >> 24);
                const auto scaledAlpha = static_cast<uint32_t>(
                    std::lround(static_cast<float>(alpha) * opacityScale));
                color = (color & 0x00FFFFFFU) | (std::min(scaledAlpha, 255U) << 24);
                std::memcpy(output + i * stride + 16, &color, sizeof(color));
            }
        }
        result.rewritten = true;
        return result;
    }
}
