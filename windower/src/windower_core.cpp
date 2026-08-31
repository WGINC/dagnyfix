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
// Windower-hosted counterpart of ../../src/spectralfix.cpp's Plugin class.
//
// Matches the Ashita build slot-for-slot: same three device hooks
// (CreateTexture/SetTexture/DrawPrimitiveUP), same slot indices, same
// selector/marker/activity tracking, same geometry correction, same settings
// format. Only real addition is a fourth hook, Present, standing in for the
// periodic tick Ashita gave us via IPlugin::Direct3DPresent - a bare d3d8
// proxy has no host to hand that to us, so we hook it ourselves.
//
// Diffs from the Ashita build:
//   - No IAshitaCore/IPlugin: host-agnostic, d3d8_proxy.cpp drives it.
//   - installPath_ comes from the caller instead of core_->GetInstallPath().
//   - chat() has no ChatManager. Logs as before, and also appends to
//     spectralfix_outbox.txt for the optional Lua addon to tail and echo via
//     windower.add_to_chat. No Lua addon? Still get everything in the log/outbox.
//   - handle_command() takes pre-split tokens instead of parsing "/spectralfix
//     ..." itself, since the caller already stripped the slash and command word.

#include "windower_core.hpp"

#include "../../src/composite_state.hpp"
#include "../../src/failure_policy.hpp"
#include "../../src/geometry_rewrite.hpp"
#include "../../src/hook_policy.hpp"
#include "../../src/hook_transaction.hpp"
#include "../../src/selector_policy.hpp"
#include "../../src/selector_validation.hpp"
#include "../../src/settings.hpp"

#include <intrin.h>

// _ReturnAddress() is an MSVC intrinsic; GCC/clang declare it in intrin.h but
// don't implement it, so non-MSVC builds (cross-check only, never shipped)
// need the builtin instead.
#if !defined(_MSC_VER)
#define _ReturnAddress() __builtin_return_address(0)
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace spectralfix_w
{
    using namespace spectralfix;

    namespace
    {
        constexpr uint32_t kMarkerMagic         = 0x58465053; // "SPFX" little endian.
        constexpr uint32_t kMarkerVersion       = 1;
        constexpr uint32_t kOriginalSize        = 256;
        constexpr uint32_t kSpreadBase          = 1024;
        constexpr uint32_t kCreateTextureSlot   = 20;
        constexpr uint32_t kSetTextureSlot      = 61;
        constexpr uint32_t kDrawPrimitiveUPSlot = 72;
        constexpr uint32_t kPresentSlot         = 15;

        constexpr size_t kHookCreateTexture   = 0;
        constexpr size_t kHookSetTexture      = 1;
        constexpr size_t kHookDrawPrimitiveUP = 2;
        constexpr size_t kHookPresent         = 3;
        constexpr size_t kHookCount           = 4;
        constexpr size_t kMaxSignatureEntries = 64;
        constexpr size_t kMaxActivityEntries  = 256;
        constexpr uint64_t kMaxLogBytes       = 4ULL * 1024ULL * 1024ULL;
        constexpr uint64_t kMaxOutboxBytes    = 256ULL * 1024ULL;
        constexpr uint32_t kMaxVertices       = 16;
        constexpr uint32_t kMaxStride         = 256;
        constexpr const char* kVersionString  = "1.0-windower";

        constexpr GUID kMarkerGuid = {
            0xa58e07d9, 0x907e, 0x45a1, {0x8a, 0x37, 0x2e, 0x96, 0x7a, 0x72, 0xf4, 0x11}
        };
        constexpr GUID kTexture8Guid = {
            0xe4cdd575, 0x2866, 0x4f01, {0xb1, 0x2e, 0x7e, 0xec, 0xe1, 0xec, 0x93, 0x58}
        };

        using CreateTextureFn = HRESULT(__stdcall*)(
            IDirect3DDevice8*, UINT, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL, IDirect3DTexture8**);
        using DrawPrimitiveUPFn = HRESULT(__stdcall*)(
            IDirect3DDevice8*, D3DPRIMITIVETYPE, UINT, const void*, UINT);
        using SetTextureFn = HRESULT(__stdcall*)(
            IDirect3DDevice8*, DWORD, IDirect3DBaseTexture8*);
        using PresentFn = HRESULT(__stdcall*)(
            IDirect3DDevice8*, const RECT*, const RECT*, HWND, const RGNDATA*);

#pragma pack(push, 1)
        struct ResourceMarker
        {
            uint32_t magic;
            uint32_t version;
            uint32_t candidateId;
            uint32_t moduleTimestamp;
            uint32_t moduleSize;
            uint32_t callerRva;
            uint32_t stackHash;
            uint32_t signatureOrdinal;
            uint32_t originalSize;
            uint32_t actualSize;
        };
#pragma pack(pop)

        struct ModuleIdentity
        {
            uintptr_t base{0};
            uint32_t timestamp{0};
            uint32_t size{0};
            bool valid{false};
        };

        struct Selector
        {
            uint32_t moduleTimestamp{0};
            uint32_t moduleSize{0};
            uint32_t callerRva{0};
            uint32_t stackHash{0};
            uint32_t signatureOrdinal{0};
            uint32_t targetSize{kDefaultTargetSize};
            bool valid{false};
        };

        struct CreatePlan
        {
            bool candidate{false};
            bool resize{false};
            uint32_t candidateId{0};
            uint32_t signatureOrdinal{0};
            uint32_t callerRva{0};
            uint32_t stackHash{0};
            UINT requestedWidth{0};
            UINT requestedHeight{0};
            UINT actualWidth{0};
            UINT actualHeight{0};
        };

        struct CandidateActivity
        {
            uint32_t dsNullUpDraws{0};
            bool firstSeenPending{false};
        };

        CreateTextureFn gOriginalCreateTexture     = nullptr;
        SetTextureFn gOriginalSetTexture           = nullptr;
        DrawPrimitiveUPFn gOriginalDrawPrimitiveUP = nullptr;
        PresentFn gOriginalPresent                 = nullptr;
        thread_local bool gInsideCreateTexture     = false;
        thread_local bool gInsideSetTexture        = false;
        thread_local bool gInsideDrawPrimitiveUP   = false;
        thread_local bool gInsidePresent           = false;

        uint64_t file_size_or_zero(const std::string& path)
        {
            WIN32_FILE_ATTRIBUTE_DATA data{};
            if (!::GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &data))
                return 0;
            ULARGE_INTEGER size{};
            size.HighPart = data.nFileSizeHigh;
            size.LowPart  = data.nFileSizeLow;
            return size.QuadPart;
        }

        uint32_t vertex_count(const D3DPRIMITIVETYPE type, const UINT primitiveCount)
        {
            switch (type)
            {
                case D3DPT_TRIANGLELIST:
                    return primitiveCount * 3;
                case D3DPT_TRIANGLESTRIP:
                case D3DPT_TRIANGLEFAN:
                    return primitiveCount + 2;
                default:
                    return 0;
            }
        }

        bool marker_valid(const ResourceMarker& marker, const DWORD size)
        {
            return size == sizeof(marker)
                && marker.magic == kMarkerMagic
                && marker.version == kMarkerVersion
                && resource_marker_dimensions_valid(
                    marker.originalSize, marker.actualSize, kOriginalSize);
        }
    }

    WindowerCore* WindowerCore::instance_ = nullptr;

    struct WindowerCore::Impl
    {
        struct HookSlot
        {
            const char* name{nullptr};
            uint32_t vtableIndex{0};
            void* hook{nullptr};
            void** target{nullptr};
            void* previous{nullptr};
            bool tracked{false};
        };
        std::array<HookSlot, kHookCount> hooks_{};

        IDirect3DDevice8* device_{nullptr};
        ModuleIdentity module_{};
        Selector selector_{};
        std::string installPath_{};
        std::string logPath_{};
        std::string logArchivePath_{};
        std::string configPath_{};
        std::string outboxPath_{};
        std::string commandPath_{};
        std::ofstream log_{};
        std::mutex logMutex_{};
        uint64_t logBytesWritten_{0};
        bool logRotationFailedThisRun_{false};
        bool loggingDisabledAtLimit_{false};
        std::unordered_map<uint64_t, uint32_t> signatureCounts_{};
        std::unordered_map<uint32_t, CandidateActivity> activity_{};
        std::array<uint8_t, kMaxVertices * kMaxStride> scratch_{};

        bool allowNewEnlargements_{true};
        bool auraFeaturesEnabled_{true};
        bool enlargementPublished_{false};
        bool hooksPublished_{false};
        std::atomic<bool> loggingAvailable_{false};
        std::atomic<bool> pendingLogDisabledWarning_{false};
        bool compositeRestoreFailed_{false};
        bool hookDisplaced_{false};
        bool correctionLost_{false};
        bool releaseRefused_{false};
        bool displacementVerdictPending_{false};
        bool drawChainSurvivalReported_{false};
        uint64_t interceptedAtDisplacement_{0};
        uint32_t missedDrawChainWindows_{0};
        bool targetUnsupported_{false};
        bool targetCapsKnown_{false};
        bool modulePinned_{false};
        uint32_t maxTextureWidth_{0};
        uint32_t maxTextureHeight_{0};
        bool nativeStageZeroQueryMode_{false};
        bool hookRollbackIncomplete_{false};
        bool createExceptionReported_{false};
        bool selectorLearnedThisRun_{false};
        bool selectorLoadedFromConfig_{false};
        bool selectorFromOrdinalDefault_{false};
        bool selectorVerificationPending_{false};
        bool selectorVerifiedThisRun_{false};
        bool selectorMismatch_{false};
        bool pendingSelectorConfirmation_{false};
        bool pendingSelectorMismatchWarning_{false};
        bool pendingLearn_{false};
        bool pendingCandidateSeen_{false};
        bool pendingCandidateDepthNull_{false};
        bool pendingFirstScale_{false};
        bool pendingFirstSpread_{false};
        bool pendingFirstOpacity_{false};
        bool pendingFirstComposite_{false};
        bool pendingCompositeStateFailure_{false};
        OneShotNotice drawFailureNotice_{};
        OneShotNotice drawExceptionNotice_{};
        OneShotNotice setTextureExceptionNotice_{};
        ResourceMarker pendingMarker_{};
        ResourceMarker pendingCandidateMarker_{};
        ResourceMarker pendingMismatchMarker_{};

        uint64_t frames_{0};
        uint32_t candidateCount_{0};
        uint64_t signatureTrackingDrops_{0};
        uint64_t activityTrackingDrops_{0};
        uint64_t candidateContextRejections_{0};
        uint32_t resizedAllocations_{0};
        uint32_t trackedStageZeroSize_{0};
        bool trackedStageZeroAura_{false};
        uint64_t interceptedDraws_{0};
        uint64_t stageZeroBindings_{0};
        uint64_t auraStageZeroBindings_{0};
        uint64_t stageZeroQueries_{0};
        uint64_t auraStageZeroQueries_{0};
        uint32_t resizeFailures_{0};
        uint32_t scaledDraws_{0};
        uint32_t tapDraws_{0};
        uint32_t spreadDraws_{0};
        uint32_t opacityDraws_{0};
        uint32_t centerCompositeDraws_{0};
        uint32_t centerCompositeAdjusted_{0};
        uint32_t centerCompositeSkipped_{0};
        uint32_t centerCompositeStateFailures_{0};
        uint32_t drawFailures_{0};
        uint32_t callbackFailures_{0};
        uint32_t unmarkedBlurDraws_{0};
        uint64_t targetQueryFailures_{0};
        uint64_t textureQueryFailures_{0};
        uint64_t surfaceMarkerHits_{0};
        uint64_t textureMarkerHits_{0};
        uint64_t targetDimensionFallbacks_{0};
        uint64_t textureDimensionFallbacks_{0};
        float spreadOverride_{kDefaultSpread};
        float opacityPercent_{kDefaultOpacityPercent};
        float compositeOpacityPercent_{kDefaultCompositeOpacityPercent};
        bool compositeOpacityOverride_{true};
        bool compositeAdjustmentDisabledThisSession_{false};
        float lastEffectiveSpread_{1.0F};
        uint32_t configuredTargetSize_{kDefaultTargetSize};

        // -- logging / chat -------------------------

        void log_line(const std::string& message)
        {
            if (!loggingAvailable_)
                return;
            SYSTEMTIME time{};
            ::GetLocalTime(&time);
            std::ostringstream record;
            record << std::setfill('0')
                << time.wYear << '-' << std::setw(2) << time.wMonth << '-' << std::setw(2) << time.wDay
                << ' ' << std::setw(2) << time.wHour << ':' << std::setw(2) << time.wMinute
                << ':' << std::setw(2) << time.wSecond << '.' << std::setw(3) << time.wMilliseconds
                << " | " << message << '\n';
            const auto text = record.str();

            std::unique_lock<std::mutex> lock(logMutex_);
            if (!log_.is_open())
            {
                loggingAvailable_          = false;
                pendingLogDisabledWarning_ = true;
                return;
            }
            if (!logRotationFailedThisRun_ && logBytesWritten_ + text.size() > kMaxLogBytes)
            {
                log_.flush();
                log_.close();
                if (::MoveFileExA(logPath_.c_str(), logArchivePath_.c_str(),
                        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
                {
                    logBytesWritten_ = 0;
                    log_.open(logPath_, std::ios::out | std::ios::trunc);
                }
                else
                {
                    logRotationFailedThisRun_ = true;
                    loggingDisabledAtLimit_   = true;
                    loggingAvailable_         = false;
                    pendingLogDisabledWarning_ = true;
                    return;
                }
            }
            if (!log_.is_open())
            {
                loggingAvailable_          = false;
                pendingLogDisabledWarning_ = true;
                return;
            }
            log_ << text;
            log_.flush();
            if (log_.good())
                logBytesWritten_ += text.size();
            else
            {
                log_.close();
                loggingAvailable_          = false;
                pendingLogDisabledWarning_ = true;
            }
        }

        // No ChatManager here - messages go to the log plus a small bounded
        // outbox file the optional Lua addon polls (windower/lua/spectralfix.lua).
        void chat(const std::string& message)
        {
            log_line("[chat] " + message);
            std::ofstream outbox(outboxPath_, std::ios::out | std::ios::app);
            if (!outbox.is_open())
                return;
            outbox << message << '\n';
            outbox.flush();
            outbox.close();
            if (file_size_or_zero(outboxPath_) > kMaxOutboxBytes)
                ::DeleteFileA(outboxPath_.c_str()); // Lua side treats a missing file as "nothing new"
        }

        // -- settings -----------------------------

        SettingsFile current_settings_file(const Selector& selected) const
        {
            SettingsFile file{};
            file.selector.version          = kSettingsVersion;
            file.selector.moduleTimestamp  = selected.moduleTimestamp;
            file.selector.moduleSize       = selected.moduleSize;
            file.selector.callerRva        = selected.callerRva;
            file.selector.stackHash        = selected.stackHash;
            file.selector.signatureOrdinal = selected.signatureOrdinal;
            file.selector.targetSize       = selected.targetSize;
            file.visual.spreadOverride           = spreadOverride_;
            file.visual.opacityPercent           = opacityPercent_;
            file.visual.compositeOpacityPercent  = compositeOpacityPercent_;
            file.visual.compositeOpacityOverride = compositeOpacityOverride_;
            return file;
        }

        bool write_config(const Selector& selected)
        {
            const auto text          = serialize_settings_text(current_settings_file(selected));
            const auto temporaryPath = configPath_ + ".tmp";

            std::ofstream config(temporaryPath, std::ios::out | std::ios::trunc);
            if (!config.is_open())
                return false;
            config << text;
            config.flush();
            const bool complete = config.good();
            config.close();
            if (!complete)
            {
                ::DeleteFileA(temporaryPath.c_str());
                return false;
            }
            if (!::MoveFileExA(temporaryPath.c_str(), configPath_.c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            {
                ::DeleteFileA(temporaryPath.c_str());
                return false;
            }
            return true;
        }

        bool save_selector(const ResourceMarker& marker)
        {
            Selector learned{
                marker.moduleTimestamp, marker.moduleSize, marker.callerRva,
                marker.stackHash, marker.signatureOrdinal, configuredTargetSize_, true,
            };
            return write_config(learned);
        }

        bool save_settings()
        {
            auto saved       = selector_.valid ? selector_ : Selector{};
            saved.targetSize = configuredTargetSize_;
            return write_config(saved);
        }

        void load_selector()
        {
            std::ifstream config(configPath_);
            if (!config.is_open())
                return;
            std::ostringstream buffer;
            buffer << config.rdbuf();
            config.close();

            SettingsFile file{};
            file.visual.spreadOverride           = spreadOverride_;
            file.visual.opacityPercent           = opacityPercent_;
            file.visual.compositeOpacityPercent  = compositeOpacityPercent_;
            file.visual.compositeOpacityOverride = compositeOpacityOverride_;

            std::vector<std::string> warnings;
            parse_settings_text(buffer.str(), file, warnings);
            for (const auto& warning : warnings)
                log_line(warning);

            spreadOverride_           = file.visual.spreadOverride;
            opacityPercent_           = file.visual.opacityPercent;
            compositeOpacityPercent_  = file.visual.compositeOpacityPercent;
            compositeOpacityOverride_ = file.visual.compositeOpacityOverride;

            Selector parsed{};
            parsed.moduleTimestamp  = file.selector.moduleTimestamp;
            parsed.moduleSize       = file.selector.moduleSize;
            parsed.callerRva        = file.selector.callerRva;
            parsed.stackHash        = file.selector.stackHash;
            parsed.signatureOrdinal = file.selector.signatureOrdinal;
            parsed.targetSize       = file.selector.targetSize;
            parsed.valid = selector_fields_valid(
                file.selector.version, parsed.moduleTimestamp, parsed.moduleSize,
                parsed.callerRva, parsed.stackHash, parsed.signatureOrdinal, parsed.targetSize);

            selector_                 = parsed;
            selectorLoadedFromConfig_ = selector_.valid;
            configuredTargetSize_     = file.selector.targetSize;
            if (!selector_.valid && selector_identity_present(file.selector))
                log_line("spectralfix.ini exists but is incomplete or invalid; observe-only mode retained.");
        }

        // -- module identity / stack capture ------------------

        ModuleIdentity read_module_identity() const
        {
            ModuleIdentity identity{};
            const auto module = ::GetModuleHandleA("FFXiMain.dll");
            if (module == nullptr)
                return identity;
            const auto base = reinterpret_cast<uintptr_t>(module);
            const auto dos  = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
            if (dos->e_magic != IMAGE_DOS_SIGNATURE)
                return identity;
            const auto nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
            if (nt->Signature != IMAGE_NT_SIGNATURE)
                return identity;
            identity.base      = base;
            identity.timestamp = nt->FileHeader.TimeDateStamp;
            identity.size      = nt->OptionalHeader.SizeOfImage;
            identity.valid     = identity.size != 0;
            return identity;
        }

        uint32_t address_rva(void* address) const
        {
            const auto value = reinterpret_cast<uintptr_t>(address);
            if (!module_.valid || value < module_.base || value >= module_.base + module_.size)
                return 0;
            return static_cast<uint32_t>(value - module_.base);
        }

        uint32_t capture_stack_hash() const
        {
            void* frames[24]{};
            const auto count = ::CaptureStackBackTrace(0, 24, frames, nullptr);
            uint32_t hash    = 2166136261u;
            uint32_t used    = 0;
            for (USHORT i = 0; i < count && used < 6; ++i)
            {
                const auto rva = address_rva(frames[i]);
                if (rva == 0)
                    continue;
                hash ^= rva;
                hash *= 16777619u;
                ++used;
            }
            return used == 0 ? 0 : hash;
        }

        static std::string module_path_for_address(void* address)
        {
            if (address == nullptr)
                return "<null>";
            HMODULE module = nullptr;
            if (!::GetModuleHandleExA(
                    GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                        | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                    reinterpret_cast<LPCSTR>(address), &module))
                return "<unmapped>";
            std::array<char, MAX_PATH> path{};
            const auto length = ::GetModuleFileNameA(module, path.data(), static_cast<DWORD>(path.size()));
            return length == 0 ? "<path unavailable>" : std::string(path.data(), length);
        }

        // -- hook install / rollback / watchdog ----------------
        // Same mechanism as Ashita's install_hooks()/restore_hooks(), just 4 slots
        // instead of 3 (Present added). hook_transaction.hpp/hook_policy.hpp reused as-is.

        static void publish_original(const size_t index, void* value)
        {
            switch (index)
            {
                case kHookCreateTexture: gOriginalCreateTexture = reinterpret_cast<CreateTextureFn>(value); break;
                case kHookSetTexture:    gOriginalSetTexture = reinterpret_cast<SetTextureFn>(value); break;
                case kHookDrawPrimitiveUP: gOriginalDrawPrimitiveUP = reinterpret_cast<DrawPrimitiveUPFn>(value); break;
                default: gOriginalPresent = reinterpret_cast<PresentFn>(value); break;
            }
        }

        void reset_hook_table();
        void forget_hook_table()
        {
            for (auto& slot : hooks_)
            {
                slot.target   = nullptr;
                slot.previous = nullptr;
                slot.tracked  = false;
            }
        }

        void observe_hooks(
            std::array<HookSlotView, kHookCount>& views,
            std::array<void*, kHookCount>& observed) const
        {
            for (size_t i = 0; i < kHookCount; ++i)
            {
                const auto& slot = hooks_[i];
                const bool tracked = slot.tracked && slot.target != nullptr;
                observed[i]      = tracked ? *slot.target : nullptr;
                views[i].ours    = slot.hook;
                views[i].current = observed[i];
                views[i].tracked = tracked;
            }
        }

        bool pin_module()
        {
            if (modulePinned_)
                return true;
            HMODULE self = nullptr;
            if (!::GetModuleHandleExA(
                    GET_MODULE_HANDLE_EX_FLAG_PIN | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                    reinterpret_cast<LPCSTR>(&hook_draw_primitive_up), &self))
                return false;
            modulePinned_ = self != nullptr;
            if (modulePinned_)
                log_line("SpectralFix (Windower) module pinned for process-lifetime hook safety.");
            return modulePinned_;
        }

        static bool write_hook_slot(void** slot, void* value)
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

        void log_hook_installed(const HookSlot& slot)
        {
            std::ostringstream out;
            out << slot.name << " hook installed: slot=0x" << std::hex << std::uppercase
                << reinterpret_cast<uintptr_t>(slot.target) << " original=0x"
                << reinterpret_cast<uintptr_t>(slot.previous) << " hook=0x"
                << reinterpret_cast<uintptr_t>(slot.hook) << std::dec
                << " owner=" << module_path_for_address(slot.previous);
            log_line(out.str());
        }

        bool install_hooks()
        {
            auto*** object = reinterpret_cast<void***>(device_);
            if (object == nullptr || *object == nullptr)
                return false;

            reset_hook_table();
            for (auto& slot : hooks_)
            {
                slot.target   = &(*object)[slot.vtableIndex];
                slot.previous = *slot.target;
                if (slot.previous == nullptr || slot.previous == slot.hook)
                {
                    forget_hook_table();
                    return false;
                }
            }
            for (size_t i = 0; i < kHookCount; ++i)
                publish_original(i, hooks_[i].previous);

            const auto result = install_hook_transaction(
                hooks_.data(), hooks_.size(),
                [](void** target, void* value) { return write_hook_slot(target, value); });
            if (result != HookTransactionResult::installed)
            {
                if (result == HookTransactionResult::rollbackIncomplete)
                {
                    hookRollbackIncomplete_ = true;
                    hooksPublished_         = true;
                    for (const auto& slot : hooks_)
                    {
                        if (slot.tracked && slot.target != nullptr)
                            log_line(std::string(slot.name) + " hook remained published after rollback failure.");
                    }
                }
                else
                {
                    forget_hook_table();
                }
                return false;
            }
            for (const auto& slot : hooks_)
                log_hook_installed(slot);
            return true;
        }

        bool slot_is_ours(const size_t index) const
        {
            const auto& slot = hooks_[index];
            return slot.tracked && slot.target != nullptr && *slot.target == slot.hook;
        }

        bool hooks_intact() const
        {
            if (!hooksPublished_)
                return true;
            std::array<HookSlotView, kHookCount> views{};
            std::array<void*, kHookCount> observed{};
            observe_hooks(views, observed);
            return tracked_hooks_intact(views.data(), views.size());
        }

        void log_displaced_slot(const char* name, void** slot, void* expected, void* current)
        {
            if (slot == nullptr || current == expected)
                return;
            std::ostringstream out;
            out << "Hook displacement: " << name << " slot=0x" << std::hex << std::uppercase
                << reinterpret_cast<uintptr_t>(slot) << " expected=0x"
                << reinterpret_cast<uintptr_t>(expected) << " current=0x"
                << reinterpret_cast<uintptr_t>(current) << std::dec
                << " current_owner=" << module_path_for_address(current);
            log_line(out.str());
        }

        void log_displaced_hooks()
        {
            for (const auto& slot : hooks_)
            {
                const auto current = slot.tracked && slot.target != nullptr ? *slot.target : nullptr;
                const HookSlotView view{slot.hook, current, slot.tracked && slot.target != nullptr};
                if (slot_is_displaced(view))
                    log_displaced_slot(slot.name, slot.target, slot.hook, current);
            }
        }

        // Handles both native-D3D8 restoring SetTexture and some other Windower
        // hook taking it: hand the slot back, mark untracked, fall back to
        // querying stage zero on demand.
        bool activate_native_stage_zero_query_mode()
        {
            if (nativeStageZeroQueryMode_)
                return false;
            auto& setTexture = hooks_[kHookSetTexture];
            if (!setTexture.tracked || setTexture.target == nullptr)
                return false;
            for (size_t i = 0; i < kHookCount; ++i)
            {
                if (i == kHookSetTexture)
                    continue;
                const auto& slot = hooks_[i];
                if (!slot.tracked || slot.target == nullptr || *slot.target != slot.hook)
                    return false;
            }
            void* const current = *setTexture.target;
            if (current == nullptr || current == setTexture.hook)
                return false;

            const auto owner = lower_copy(module_path_for_address(current));
            const bool isKnownReflowOwner =
                owner.find("\\windows\\system32\\d3d8.dll") != std::string::npos
                || owner.find("\\windows\\syswow64\\d3d8.dll") != std::string::npos
                || owner.find("windower") != std::string::npos;
            if (!isKnownReflowOwner)
                return false;

            log_displaced_slot(setTexture.name, setTexture.target, setTexture.hook, current);
            setTexture.previous = current;
            publish_original(kHookSetTexture, current);
            setTexture.tracked = false;
            setTexture.target  = nullptr;
            trackedStageZeroSize_     = 0;
            trackedStageZeroAura_     = false;
            nativeStageZeroQueryMode_ = true;
            return true;
        }

        bool restore_hooks()
        {
            bool restored = true;
            for (size_t i = kHookCount; i > 0; --i)
            {
                auto& slot = hooks_[i - 1];
                if (!slot.tracked)
                    continue;
                if (slot.target == nullptr || slot.previous == nullptr)
                {
                    restored = false;
                    continue;
                }
                if (*slot.target == slot.hook)
                {
                    if (write_hook_slot(slot.target, slot.previous))
                    {
                        slot.tracked = false;
                        log_line(std::string(slot.name) + " hook restored.");
                    }
                    else
                    {
                        restored = false;
                        log_line(std::string(slot.name) + " hook restoration failed. SpectralFix must remain resident until client exit.");
                    }
                }
                else
                {
                    slot.tracked = false;
                    log_line(std::string(slot.name) + " slot not restored because another component owns it. Client exit is required.");
                }
            }
            if (restored)
                forget_hook_table();
            return restored;
        }

        // -- CreateTexture: allocation selection / enlargement ---------

        CreatePlan before_create(
            IDirect3DDevice8* device, const UINT width, const UINT height, const UINT levels,
            const DWORD usage, const D3DFORMAT format, const D3DPOOL pool, void* caller)
        {
            CreatePlan plan{};
            plan.requestedWidth  = width;
            plan.requestedHeight = height;
            plan.actualWidth     = width;
            plan.actualHeight    = height;

            const bool candidateShape = width == kOriginalSize && height == kOriginalSize
                && levels == 1 && (usage & D3DUSAGE_RENDERTARGET) != 0
                && format == D3DFMT_A8R8G8B8 && pool == D3DPOOL_DEFAULT;
            if (!candidateShape)
                return plan;

            plan.callerRva = address_rva(caller);
            plan.stackHash = capture_stack_hash();
            const bool deviceMatches = device_ != nullptr && device == device_;
            if (!candidate_context_is_trusted(deviceMatches, plan.stackHash))
            {
                ++candidateContextRejections_;
                return plan;
            }

            plan.candidate   = true;
            plan.candidateId = ++candidateCount_;
            const uint64_t signature = (static_cast<uint64_t>(plan.callerRva) << 32) | plan.stackHash;
            auto signatureIt = signatureCounts_.find(signature);
            if (signatureIt == signatureCounts_.end())
            {
                if (signatureCounts_.size() >= kMaxSignatureEntries)
                {
                    plan.candidate = false;
                    ++signatureTrackingDrops_;
                    return plan;
                }
                signatureIt = signatureCounts_.emplace(signature, 0).first;
            }
            plan.signatureOrdinal = ++signatureIt->second;

            if (should_arm_ordinal_one_default(selector_.valid, plan.candidateId, plan.signatureOrdinal))
            {
                selector_ = Selector{
                    module_.timestamp, module_.size, plan.callerRva, plan.stackHash,
                    plan.signatureOrdinal, configuredTargetSize_, true,
                };
                selectorFromOrdinalDefault_  = true;
                selectorVerificationPending_ = true;
                log_line("Ordinal-1 startup default armed for candidate 1; awaiting aura-activity verification.");
            }

            if (allowNewEnlargements_ && selector_.valid
                && selector_.moduleTimestamp == module_.timestamp
                && selector_.moduleSize == module_.size
                && selector_.callerRva == plan.callerRva
                && selector_.stackHash == plan.stackHash
                && selector_.signatureOrdinal == plan.signatureOrdinal)
            {
                plan.resize       = true;
                plan.actualWidth  = selector_.targetSize;
                plan.actualHeight = selector_.targetSize;
            }
            return plan;
        }

        void after_create(
            CreatePlan& plan, const HRESULT firstResult, const HRESULT finalResult,
            IDirect3DTexture8* texture, const bool fellBack)
        {
            if (!plan.candidate)
                return;
            if (fellBack)
            {
                plan.actualWidth  = plan.requestedWidth;
                plan.actualHeight = plan.requestedHeight;
                ++resizeFailures_;
            }

            ResourceMarker marker{
                kMarkerMagic, kMarkerVersion, plan.candidateId, module_.timestamp, module_.size,
                plan.callerRva, plan.stackHash, plan.signatureOrdinal, kOriginalSize, plan.actualWidth,
            };

            HRESULT textureMarkerResult = E_FAIL;
            HRESULT surfaceMarkerResult = E_FAIL;
            if (SUCCEEDED(finalResult) && texture != nullptr)
            {
                textureMarkerResult = texture->SetPrivateData(kMarkerGuid, &marker, sizeof(marker), 0);
                IDirect3DSurface8* surface = nullptr;
                if (SUCCEEDED(texture->GetSurfaceLevel(0, &surface)) && surface != nullptr)
                {
                    surfaceMarkerResult = surface->SetPrivateData(kMarkerGuid, &marker, sizeof(marker), 0);
                    surface->Release();
                }
            }

            std::ostringstream out;
            out << "candidate #" << plan.candidateId
                << " caller=0x" << std::hex << std::uppercase << plan.callerRva
                << " stack=0x" << plan.stackHash << std::dec
                << " ordinal=" << plan.signatureOrdinal
                << " request=" << plan.requestedWidth << 'x' << plan.requestedHeight
                << " actual=" << plan.actualWidth << 'x' << plan.actualHeight
                << " first_hr=0x" << std::hex << std::uppercase << static_cast<uint32_t>(firstResult)
                << " final_hr=0x" << static_cast<uint32_t>(finalResult)
                << " texture_marker_hr=0x" << static_cast<uint32_t>(textureMarkerResult)
                << " surface_marker_hr=0x" << static_cast<uint32_t>(surfaceMarkerResult)
                << " texture=0x" << reinterpret_cast<uintptr_t>(texture) << std::dec;
            if (fellBack)
                out << " RESIZE_FAILED_FELL_BACK_TO_256";
            else if (plan.resize)
                out << " RESIZED";
            log_line(out.str());

            if (plan.resize && !fellBack && SUCCEEDED(finalResult))
                ++resizedAllocations_;
        }

        void note_enlargement_published() { enlargementPublished_ = true; }

        void note_create_exception()
        {
            ++callbackFailures_;
            if (!createExceptionReported_)
            {
                createExceptionReported_ = true;
                log_line("CreateTexture post-processing raised an exception; the original D3D result was preserved.");
            }
        }

        void note_draw_exception()
        {
            ++callbackFailures_;
            drawExceptionNotice_.record();
            drawFailureNotice_.record();
        }

        void note_set_texture_exception()
        {
            trackedStageZeroSize_ = 0;
            trackedStageZeroAura_ = false;
            ++callbackFailures_;
            setTextureExceptionNotice_.record();
        }

        void note_draw_submission_failure()
        {
            ++drawFailures_;
            drawFailureNotice_.record();
        }

        // -- SetTexture: stage-zero aura tracking ---------------

        ResourceMarker selector_marker() const
        {
            return ResourceMarker{
                kMarkerMagic, kMarkerVersion, selector_.signatureOrdinal, selector_.moduleTimestamp,
                selector_.moduleSize, selector_.callerRva, selector_.stackHash,
                selector_.signatureOrdinal, kOriginalSize, selector_.targetSize,
            };
        }

        bool marker_matches_selector(const ResourceMarker& marker) const
        {
            return selector_.valid && selector_identity_matches(
                selector_.moduleTimestamp, selector_.moduleSize, selector_.callerRva,
                selector_.stackHash, selector_.signatureOrdinal,
                marker.moduleTimestamp, marker.moduleSize, marker.callerRva,
                marker.stackHash, marker.signatureOrdinal);
        }

        void track_stage_zero_binding(IDirect3DDevice8* device, const DWORD stage, IDirect3DBaseTexture8* texture)
        {
            if (device != device_ || stage != 0)
                return;
            ++stageZeroBindings_;
            trackedStageZeroSize_ = 0;
            trackedStageZeroAura_ = false;
            if (texture == nullptr)
                return;

            ResourceMarker marker{};
            DWORD markerSize = sizeof(marker);
            const auto markerResult = texture->GetPrivateData(kMarkerGuid, &marker, &markerSize);
            if (SUCCEEDED(markerResult) && marker_valid(marker, markerSize)
                && selected_aura_marker_is_trackable(
                    selector_.valid, marker_matches_selector(marker), marker.actualSize, marker.originalSize))
            {
                trackedStageZeroSize_ = marker.actualSize;
                trackedStageZeroAura_ = true;
                ++textureMarkerHits_;
                ++auraStageZeroBindings_;
                return;
            }
            if (texture->GetType() != D3DRTYPE_TEXTURE || !selector_.valid)
                return;
            D3DSURFACE_DESC desc{};
            if (FAILED(static_cast<IDirect3DTexture8*>(texture)->GetLevelDesc(0, &desc)))
            {
                ++textureQueryFailures_;
                return;
            }
            if (selector_.targetSize > kOriginalSize
                && desc.Width == selector_.targetSize && desc.Height == selector_.targetSize
                && (desc.Usage & D3DUSAGE_RENDERTARGET) != 0)
            {
                trackedStageZeroSize_ = selector_.targetSize;
                trackedStageZeroAura_ = true;
                ++textureDimensionFallbacks_;
                ++auraStageZeroBindings_;
            }
        }

        uint32_t query_stage_zero_size()
        {
            ++stageZeroQueries_;
            if (device_ == nullptr)
                return 0;
            IDirect3DBaseTexture8* texture = nullptr;
            if (FAILED(device_->GetTexture(0, &texture)))
            {
                ++textureQueryFailures_;
                return 0;
            }
            if (texture == nullptr)
                return 0;

            uint32_t result = 0;
            ResourceMarker marker{};
            DWORD markerSize = sizeof(marker);
            const auto markerResult = texture->GetPrivateData(kMarkerGuid, &marker, &markerSize);
            if (SUCCEEDED(markerResult) && marker_valid(marker, markerSize)
                && selected_aura_marker_is_trackable(
                    selector_.valid, marker_matches_selector(marker), marker.actualSize, marker.originalSize))
            {
                result = marker.actualSize;
                ++textureMarkerHits_;
            }
            else if (texture->GetType() == D3DRTYPE_TEXTURE && selector_.valid && selector_.targetSize > kOriginalSize)
            {
                D3DSURFACE_DESC desc{};
                if (FAILED(static_cast<IDirect3DTexture8*>(texture)->GetLevelDesc(0, &desc)))
                    ++textureQueryFailures_;
                else if (desc.Width == selector_.targetSize && desc.Height == selector_.targetSize
                    && (desc.Usage & D3DUSAGE_RENDERTARGET) != 0)
                {
                    result = selector_.targetSize;
                    ++textureDimensionFallbacks_;
                }
            }
            texture->Release();
            if (result != 0)
                ++auraStageZeroQueries_;
            return result;
        }

        // -- DrawPrimitiveUP: downsample / tap / center-composite correction --

        bool rewrite_downsample(const void* vertexData, const UINT stride, const uint32_t count, const uint32_t targetSize)
        {
            return rewrite_downsample_vertices(
                vertexData, stride, count, targetSize, kOriginalSize, scratch_.data(), scratch_.size());
        }

        TapRewriteResult rewrite_taps(const void* vertexData, const UINT stride, const uint32_t count, const UINT renderTargetWidth)
        {
            const auto automatic = std::clamp(
                static_cast<float>(renderTargetWidth) / static_cast<float>(kSpreadBase),
                kMinManualSpread, kMaxManualSpread);
            const auto spread = spreadOverride_ > 0.0F ? spreadOverride_ : automatic;
            return rewrite_tap_vertices(vertexData, stride, count, spread, opacityPercent_ / 100.0F, scratch_.data(), scratch_.size());
        }

        bool current_target_marker(ResourceMarker& marker, D3DSURFACE_DESC& desc, bool& depthNull)
        {
            depthNull = false;
            IDirect3DSurface8* target = nullptr;
            if (FAILED(device_->GetRenderTarget(&target)))
            {
                ++targetQueryFailures_;
                return false;
            }
            if (target == nullptr)
                return false;
            if (FAILED(target->GetDesc(&desc)))
            {
                ++targetQueryFailures_;
                target->Release();
                return false;
            }

            IDirect3DSurface8* depth = nullptr;
            const auto depthResult = device_->GetDepthStencilSurface(&depth);
            depthNull = FAILED(depthResult) || depth == nullptr;
            if (depth != nullptr)
                depth->Release();

            const bool possibleAuraTarget = (desc.Width == kOriginalSize && desc.Height == kOriginalSize)
                || (selector_.valid && desc.Width == selector_.targetSize && desc.Height == selector_.targetSize);
            if (!possibleAuraTarget)
            {
                target->Release();
                return false;
            }

            DWORD surfaceMarkerSize = sizeof(marker);
            const auto surfaceMarkerResult = target->GetPrivateData(kMarkerGuid, &marker, &surfaceMarkerSize);
            if (SUCCEEDED(surfaceMarkerResult) && marker_valid(marker, surfaceMarkerSize))
            {
                ++surfaceMarkerHits_;
                target->Release();
                return true;
            }

            IDirect3DTexture8* texture = nullptr;
            const auto containerResult = target->GetContainer(kTexture8Guid, reinterpret_cast<void**>(&texture));
            target->Release();
            if (SUCCEEDED(containerResult) && texture != nullptr)
            {
                DWORD textureMarkerSize = sizeof(marker);
                const auto textureMarkerResult = texture->GetPrivateData(kMarkerGuid, &marker, &textureMarkerSize);
                texture->Release();
                if (SUCCEEDED(textureMarkerResult) && marker_valid(marker, textureMarkerSize))
                {
                    ++textureMarkerHits_;
                    return true;
                }
            }
            else if (texture != nullptr)
            {
                texture->Release();
            }

            if (selector_.valid && selector_.targetSize > kOriginalSize && depthNull
                && desc.Width == selector_.targetSize && desc.Height == selector_.targetSize
                && (desc.Usage & D3DUSAGE_RENDERTARGET) != 0)
            {
                marker = selector_marker();
                ++targetDimensionFallbacks_;
                return true;
            }
            return false;
        }

        bool center_composite_state_matches(IDirect3DDevice8* device) const
        {
            const auto alphaBlend = read_render_state(device, D3DRS_ALPHABLENDENABLE);
            const auto alphaTest  = read_render_state(device, D3DRS_ALPHATESTENABLE);
            const auto alphaRef   = read_render_state(device, D3DRS_ALPHAREF);
            const auto alphaFunc  = read_render_state(device, D3DRS_ALPHAFUNC);
            const auto colorOp    = read_texture_state(device, D3DTSS_COLOROP);
            const auto alphaOp    = read_texture_state(device, D3DTSS_ALPHAOP);
            return state_is(alphaBlend, FALSE) && state_is(alphaTest, TRUE)
                && state_is(alphaRef, 96) && state_is(alphaFunc, D3DCMP_GREATER)
                && state_is(colorOp, D3DTOP_SELECTARG2) && state_is(alphaOp, D3DTOP_SELECTARG2);
        }

        void note_composite_state_failure()
        {
            ++centerCompositeStateFailures_;
            compositeAdjustmentDisabledThisSession_ = true;
            pendingCompositeStateFailure_ = true;
        }

        bool draw_center_composite(
            IDirect3DDevice8* device, const DrawPrimitiveUPFn original, const D3DPRIMITIVETYPE primitiveType,
            const UINT primitiveCount, const void* vertexData, const UINT stride, HRESULT& result)
        {
            if (compositeOpacityPercent_ <= 0.0001F)
            {
                ++centerCompositeSkipped_;
                if (centerCompositeSkipped_ == 1)
                    pendingFirstComposite_ = true;
                result = D3D_OK;
                return true;
            }

            const auto count = vertex_count(primitiveType, primitiveCount);
            if (!rewrite_uniform_alpha(vertexData, stride, count, compositeOpacityPercent_, scratch_.data(), scratch_.size()))
                return false;

            CompositeStateScope scope(device, &compositeRestoreFailed_);
            if (!scope.captured())
            {
                note_composite_state_failure();
                return false;
            }
            if (!scope.apply())
            {
                scope.restore();
                note_composite_state_failure();
                return false;
            }

            const auto drawResult = original(device, primitiveType, primitiveCount, scratch_.data(), stride);
            if (!scope.restore())
            {
                note_composite_state_failure();
                result = drawResult;
                return true;
            }
            if (FAILED(drawResult))
            {
                note_draw_submission_failure();
                return false;
            }

            ++centerCompositeAdjusted_;
            if (centerCompositeAdjusted_ == 1)
                pendingFirstComposite_ = true;
            result = drawResult;
            return true;
        }

        HRESULT handle_draw_primitive_up(
            IDirect3DDevice8* device, const DrawPrimitiveUPFn original, const D3DPRIMITIVETYPE primitiveType,
            const UINT primitiveCount, const void* vertexData, const UINT stride)
        {
            ++interceptedDraws_;
            if (device_ == nullptr || device != device_ || original == nullptr || vertexData == nullptr)
                return original != nullptr
                    ? original(device, primitiveType, primitiveCount, vertexData, stride)
                    : D3DERR_INVALIDCALL;

            if (!enlargementPublished_ && !auraFeaturesEnabled_)
                return original(device, primitiveType, primitiveCount, vertexData, stride);

            const auto count = vertex_count(primitiveType, primitiveCount);
            const bool supportedAuraShape = primitiveType == D3DPT_TRIANGLESTRIP && primitiveCount == 2
                && count == 4 && stride >= 20 && stride <= kMaxStride;
            if (!supportedAuraShape)
                return original(device, primitiveType, primitiveCount, vertexData, stride);

            ResourceMarker targetMarker{};
            D3DSURFACE_DESC targetDesc{};
            bool depthNull = false;
            const bool markedTarget = current_target_marker(targetMarker, targetDesc, depthNull);

            if (markedTarget)
            {
                auto activityIt = activity_.find(targetMarker.candidateId);
                if (activityIt == activity_.end() && activity_.size() < kMaxActivityEntries)
                    activityIt = activity_.emplace(targetMarker.candidateId, CandidateActivity{}).first;
                if (activityIt == activity_.end())
                {
                    ++activityTrackingDrops_;
                }
                else
                {
                    auto& activity = activityIt->second;
                    if (depthNull)
                        ++activity.dsNullUpDraws;
                    if (!activity.firstSeenPending)
                    {
                        activity.firstSeenPending = true;
                        pendingCandidateSeen_      = true;
                        pendingCandidateMarker_   = targetMarker;
                        pendingCandidateDepthNull_ = depthNull;
                    }
                    if (depthNull && activity.dsNullUpDraws >= 4 && selectorVerificationPending_)
                    {
                        const bool matches = selector_identity_matches(
                            selector_.moduleTimestamp, selector_.moduleSize, selector_.callerRva,
                            selector_.stackHash, selector_.signatureOrdinal,
                            targetMarker.moduleTimestamp, targetMarker.moduleSize, targetMarker.callerRva,
                            targetMarker.stackHash, targetMarker.signatureOrdinal);
                        const auto decision = evaluate_selector_activity(true, matches);
                        selectorVerificationPending_ = false;
                        if (decision == SelectorActivityDecision::confirmed)
                        {
                            pendingSelectorConfirmation_ = true;
                        }
                        else if (decision == SelectorActivityDecision::mismatch)
                        {
                            selectorMismatch_               = true;
                            allowNewEnlargements_           = false;
                            auraFeaturesEnabled_            = false;
                            pendingMismatchMarker_          = targetMarker;
                            pendingSelectorMismatchWarning_ = true;
                        }
                    }
                    else if (!selector_.valid && !selectorLearnedThisRun_ && depthNull
                        && activity.dsNullUpDraws >= 4 && !pendingLearn_)
                    {
                        pendingMarker_ = targetMarker;
                        pendingLearn_  = true;
                    }
                }

                if (targetMarker.actualSize > kOriginalSize && depthNull)
                {
                    if (rewrite_downsample(vertexData, stride, count, targetMarker.actualSize))
                    {
                        const auto hr = original(device, primitiveType, primitiveCount, scratch_.data(), stride);
                        if (SUCCEEDED(hr))
                        {
                            ++scaledDraws_;
                            if (scaledDraws_ == 1)
                                pendingFirstScale_ = true;
                            return hr;
                        }
                        note_draw_submission_failure();
                    }
                }
            }
            else if (targetDesc.Width == kOriginalSize && targetDesc.Height == kOriginalSize && depthNull)
            {
                ++unmarkedBlurDraws_;
            }

            if (!auraFeaturesEnabled_)
                return original(device, primitiveType, primitiveCount, vertexData, stride);

            auto stageZeroSize = trackedStageZeroSize_;
            auto stageZeroAura = trackedStageZeroAura_;
            (void)stageZeroSize;
            auto rewrite = TapRewriteResult{};
            bool centerGeometry = false;
            if (stageZeroAura || nativeStageZeroQueryMode_)
                centerGeometry = matches_center_composite_vertices(vertexData, stride, count, targetDesc.Width, targetDesc.Height);
            if (stageZeroAura)
            {
                rewrite = rewrite_taps(vertexData, stride, count, targetDesc.Width);
            }
            else if (nativeStageZeroQueryMode_)
            {
                const auto probe = rewrite_taps(vertexData, stride, count, targetDesc.Width);
                if (probe.matched || centerGeometry)
                {
                    const auto queried = query_stage_zero_size();
                    if (queried != 0)
                    {
                        stageZeroAura = true;
                        rewrite = probe;
                    }
                }
            }

            if (stageZeroAura && centerGeometry && !markedTarget && !depthNull && center_composite_state_matches(device))
            {
                ++centerCompositeDraws_;
                if (compositeOpacityOverride_ && !compositeAdjustmentDisabledThisSession_)
                {
                    HRESULT compositeResult = D3D_OK;
                    if (draw_center_composite(device, original, primitiveType, primitiveCount, vertexData, stride, compositeResult))
                        return compositeResult;
                }
            }

            if (stageZeroAura && rewrite.matched)
            {
                ++tapDraws_;
                lastEffectiveSpread_ = rewrite.effectiveSpread;
                if (rewrite.rewritten)
                {
                    const auto hr = original(device, primitiveType, primitiveCount, scratch_.data(), stride);
                    if (SUCCEEDED(hr))
                    {
                        if (rewrite.spreadAdjusted)
                        {
                            ++spreadDraws_;
                            if (spreadDraws_ == 1)
                                pendingFirstSpread_ = true;
                        }
                        if (rewrite.opacityAdjusted)
                        {
                            ++opacityDraws_;
                            if (opacityDraws_ == 1)
                                pendingFirstOpacity_ = true;
                        }
                        return hr;
                    }
                    note_draw_submission_failure();
                }
            }

            return original(device, primitiveType, primitiveCount, vertexData, stride);
        }

        // -- status / settings text ----------------------

        std::string mode_text() const
        {
            if (selectorMismatch_)
                return enlargementPublished_
                    ? "allocation mismatch; adjustments stopped, enlarged allocation still corrected, immediate client exit required"
                    : "disabled after allocation mismatch; immediate client exit required";
            if (releaseRefused_)
                return "unload refused while an enlarged allocation is live; correction continues until client exit";
            if (correctionLost_)
                return "hook displaced out of the draw chain; enlarged allocation can no longer be corrected, immediate client exit required";
            if (displacementVerdictPending_)
                return "hook displaced; measuring whether the draw hook is still called";
            if (hookDisplaced_)
                return enlargementPublished_
                    ? "hook displaced; no new enlargements, enlarged allocation still corrected"
                    : "disabled after hook displacement";
            if (targetUnsupported_)
                return "disabled because requested target exceeds device limits";
            if (selectorFromOrdinalDefault_ && selectorVerificationPending_)
                return "ordinal-1 startup default active; verification pending";
            if (selectorFromOrdinalDefault_ && selectorVerifiedThisRun_)
                return "ordinal-1 startup default verified";
            if (selector_.valid)
                return selectorLoadedFromConfig_ ? "saved selector armed for this client build" : "selector armed for this client build";
            if (selectorLearnedThisRun_)
                return "selector learned; full client restart required";
            return candidateCount_ == 0 ? "ordinal-1 startup default waiting for allocation" : "observe-only fallback learning mode";
        }

        std::string settings_text() const
        {
            std::ostringstream out;
            out << std::fixed << std::setprecision(2) << "SpectralFix settings: spread=";
            if (spreadOverride_ > 0.0F)
                out << spreadOverride_ << " (manual)";
            else
                out << "auto";
            out << ", target=";
            const auto activeTargetSize = selector_.valid ? selector_.targetSize : configuredTargetSize_;
            if (activeTargetSize == kMediumTargetSize)
                out << "medium resolution";
            else if (activeTargetSize == kUltraTargetSize)
                out << "ultra resolution (experimental)";
            else
                out << "high resolution";
            if (selector_.valid && configuredTargetSize_ != selector_.targetSize)
                out << " (change to " << configuredTargetSize_ << " pending restart)";
            out << ", effective=";
            if (tapDraws_ == 0)
                out << "pending";
            else
                out << lastEffectiveSpread_;
            out << ", tap opacity=" << opacityPercent_ << "%";
            out << ", center composite=";
            if (compositeOpacityOverride_)
                out << compositeOpacityPercent_ << "%";
            else
                out << "stock";
            if (compositeAdjustmentDisabledThisSession_)
                out << " (disabled after state failure)";
            return out.str();
        }

        std::string status_text() const
        {
            std::ostringstream out;
            out << "SpectralFix: " << mode_text()
                << "; candidates=" << candidateCount_
                << ", resized=" << resizedAllocations_
                << ", intercepted draws=" << interceptedDraws_
                << ", aura texture binds=" << auraStageZeroBindings_
                << ", aura texture queries=" << auraStageZeroQueries_
                << ", scale draws=" << scaledDraws_
                << ", tap draws=" << tapDraws_
                << ", spread draws=" << spreadDraws_
                << ", opacity draws=" << opacityDraws_
                << ", center composites=" << centerCompositeDraws_
                << ", center adjusted=" << centerCompositeAdjusted_
                << ", center skipped=" << centerCompositeSkipped_
                << ", draw failures=" << drawFailures_
                << ", unmarked blur draws=" << unmarkedBlurDraws_ << '.';
            return out.str();
        }

        void log_status(const char* reason)
        {
            std::ostringstream out;
            out << "status[" << reason << "] frames=" << frames_
                << " mode=" << mode_text()
                << " candidates=" << candidateCount_
                << " resized=" << resizedAllocations_
                << " intercepted=" << interceptedDraws_
                << " stage0_trustworthy=" << (stage_zero_tracking_trustworthy() ? "true" : "false")
                << " native_stage0_query_mode=" << (nativeStageZeroQueryMode_ ? "true" : "false")
                << " hooks_published=" << (hooksPublished_ ? "true" : "false")
                << " enlargement_published=" << (enlargementPublished_ ? "true" : "false")
                << " draw_failures=" << drawFailures_
                << " callback_failures=" << callbackFailures_;
            if (pendingCandidateSeen_)
            {
                out << " last_candidate=" << pendingCandidateMarker_.candidateId
                    << " last_dsnull=" << (pendingCandidateDepthNull_ ? "true" : "false");
                pendingCandidateSeen_ = false;
            }
            log_line(out.str());
        }

        void show_help()
        {
            chat(std::string("SpectralFix (Windower) v") + kVersionString + " settings:");
            chat("//spectralfix target <medium|high|ultra> - 1024, 2048, or 4096 (restart required)");
            chat("//spectralfix spread <auto|1.0-16.0> - aura spread");
            chat("//spectralfix opacity <stock|0-100> - shifted glow opacity percent");
            chat("//spectralfix composite opacity <stock|0-100> - center glow opacity percent");
            chat("Defaults: high (2048), spread 2.0, stock glow opacity, center 25%.");
        }

        bool stage_zero_tracking_trustworthy() const
        {
            return nativeStageZeroQueryMode_ || slot_is_ours(kHookSetTexture);
        }

        void handle_hook_displacement()
        {
            if (activate_native_stage_zero_query_mode())
            {
                log_line("SetTexture observer released; native stage-zero query mode is active.");
                return;
            }
            if (hookDisplaced_)
            {
                if (enlargementPublished_ && !correctionLost_ && !slot_is_ours(kHookDrawPrimitiveUP) && !displacementVerdictPending_)
                {
                    displacementVerdictPending_ = true;
                    interceptedAtDisplacement_  = interceptedDraws_;
                }
                return;
            }

            hookDisplaced_        = true;
            allowNewEnlargements_ = false;
            log_displaced_hooks();

            if (!stage_zero_tracking_trustworthy())
            {
                auraFeaturesEnabled_  = false;
                trackedStageZeroAura_ = false;
                trackedStageZeroSize_ = 0;
                log_line("Stage-zero tracking is no longer trustworthy; tap and center-composite correction stopped to avoid adjusting unrelated draws.");
            }

            if (!enlargementPublished_)
            {
                log_line("HOOK DISPLACED: another component owns a required D3D8 slot. SpectralFix left that owner in place. Nothing had been enlarged, so stock rendering is retained.");
                chat("SpectralFix hit a Direct3D hook conflict before it changed anything. Stock rendering retained; restart the client.");
                return;
            }
            if (slot_is_ours(kHookDrawPrimitiveUP))
            {
                drawChainSurvivalReported_ = true;
                log_line("HOOK DISPLACED: the DrawPrimitiveUP slot is still owned by SpectralFix, so correction of the live enlarged allocation continues.");
                chat("SpectralFix hit a Direct3D hook conflict. The current aura remains corrected; restart the client when convenient.");
                return;
            }

            displacementVerdictPending_ = true;
            interceptedAtDisplacement_  = interceptedDraws_;
            log_line("HOOK DISPLACED: another component owns a required D3D8 slot. SpectralFix left that owner in place; no further allocations will be enlarged, and draw-chain forwarding is being monitored.");
        }

        void resolve_displacement_verdict()
        {
            const auto sample = evaluate_draw_chain_sample(
                slot_is_ours(kHookDrawPrimitiveUP), interceptedDraws_, interceptedAtDisplacement_,
                missedDrawChainWindows_, kDrawChainMissThreshold);
            missedDrawChainWindows_ = sample.consecutiveMisses;

            if (sample.health == DrawChainHealth::owned)
            {
                displacementVerdictPending_ = false;
                return;
            }
            if (sample.health == DrawChainHealth::active)
            {
                if (!drawChainSurvivalReported_)
                {
                    drawChainSurvivalReported_ = true;
                    log_line("HOOK DISPLACED: SpectralFix is still in the draw chain; correction of the live enlarged allocation continues under continuous monitoring.");
                    chat("SpectralFix hit a Direct3D hook conflict. The current aura keeps rendering correctly; restart the client when convenient.");
                }
                interceptedAtDisplacement_ = interceptedDraws_;
                return;
            }
            if (sample.health == DrawChainHealth::inconclusive)
            {
                interceptedAtDisplacement_ = interceptedDraws_;
                return;
            }

            displacementVerdictPending_ = false;
            correctionLost_ = true;
            log_line("HOOK DISPLACED: SpectralFix received no DrawPrimitiveUP calls for three consecutive monitoring windows and can no longer confirm correction of the allocation it already enlarged. The aura may render incorrectly until the client is restarted.");
            chat("SpectralFix can no longer confirm that its Direct3D draw hook is active.");
            chat("Exit the entire client now; the enlarged aura may render incorrectly until you do.");
        }

        // -- static vtable trampolines ---------------------

        static HRESULT __stdcall hook_create_texture(
            IDirect3DDevice8* device, UINT width, UINT height, UINT levels, DWORD usage,
            D3DFORMAT format, D3DPOOL pool, IDirect3DTexture8** output)
        {
            const auto original = gOriginalCreateTexture;
            auto* core = WindowerCore::instance();
            if (original == nullptr)
                return D3DERR_INVALIDCALL;
            if (core == nullptr || gInsideCreateTexture)
                return original(device, width, height, levels, usage, format, pool, output);

            auto* impl = core->impl_;
            gInsideCreateTexture = true;
            CreatePlan plan{};
            try
            {
                plan = impl->before_create(device, width, height, levels, usage, format, pool, _ReturnAddress());
            }
            catch (...)
            {
                plan = CreatePlan{};
            }

            const auto firstResult = original(
                device, plan.candidate ? plan.actualWidth : width, plan.candidate ? plan.actualHeight : height,
                levels, usage, format, pool, output);

            auto finalResult = firstResult;
            bool fellBack    = false;
            if (plan.resize && FAILED(firstResult))
            {
                if (output != nullptr)
                    *output = nullptr;
                finalResult = original(device, width, height, levels, usage, format, pool, output);
                fellBack    = true;
            }

            if (plan.resize && !fellBack && SUCCEEDED(finalResult))
                impl->note_enlargement_published();

            try
            {
                impl->after_create(plan, firstResult, finalResult, output != nullptr ? *output : nullptr, fellBack);
            }
            catch (...)
            {
                impl->note_create_exception();
            }
            gInsideCreateTexture = false;
            return finalResult;
        }

        static HRESULT __stdcall hook_set_texture(IDirect3DDevice8* device, const DWORD stage, IDirect3DBaseTexture8* texture)
        {
            const auto original = gOriginalSetTexture;
            auto* core = WindowerCore::instance();
            if (original == nullptr)
                return D3DERR_INVALIDCALL;
            if (core == nullptr || gInsideSetTexture)
                return original(device, stage, texture);

            gInsideSetTexture = true;
            const auto result = original(device, stage, texture);
            if (SUCCEEDED(result))
            {
                try
                {
                    core->impl_->track_stage_zero_binding(device, stage, texture);
                }
                catch (...)
                {
                    core->impl_->note_set_texture_exception();
                }
            }
            gInsideSetTexture = false;
            return result;
        }

        static HRESULT __stdcall hook_draw_primitive_up(
            IDirect3DDevice8* device, const D3DPRIMITIVETYPE primitiveType, const UINT primitiveCount,
            const void* vertexData, const UINT stride)
        {
            const auto original = gOriginalDrawPrimitiveUP;
            auto* core = WindowerCore::instance();
            if (original == nullptr)
                return D3DERR_INVALIDCALL;
            if (core == nullptr || gInsideDrawPrimitiveUP)
                return original(device, primitiveType, primitiveCount, vertexData, stride);

            gInsideDrawPrimitiveUP = true;
            HRESULT result = D3DERR_INVALIDCALL;
            try
            {
                result = core->impl_->handle_draw_primitive_up(device, original, primitiveType, primitiveCount, vertexData, stride);
            }
            catch (...)
            {
                core->impl_->note_draw_exception();
                result = original(device, primitiveType, primitiveCount, vertexData, stride);
            }
            gInsideDrawPrimitiveUP = false;
            return result;
        }

        // Stands in for Ashita's IPlugin::Direct3DPresent - no host to hand us a
        // per-frame tick, so we hook Present to make our own.
        static HRESULT __stdcall hook_present(
            IDirect3DDevice8* device, const RECT* srcRect, const RECT* destRect, HWND destWindow, const RGNDATA* dirtyRegion)
        {
            const auto original = gOriginalPresent;
            auto* core = WindowerCore::instance();
            if (original == nullptr)
                return D3DERR_INVALIDCALL;
            const auto result = original(device, srcRect, destRect, destWindow, dirtyRegion);
            if (core != nullptr && !gInsidePresent)
            {
                gInsidePresent = true;
                try
                {
                    core->impl_->present_tick_impl();
                }
                catch (...)
                {
                    // don't let a tick failure take Present down with it
                }
                gInsidePresent = false;
            }
            return result;
        }

        // Lua addon can't reach this DLL in-process, so it drops a one-line
        // request file instead (windower/lua/spectralfix.lua); polled here on
        // the regular tick and run through the same do_handle_command().
        void poll_command_file()
        {
            std::ifstream file(commandPath_);
            if (!file.is_open())
                return;
            std::string line;
            std::getline(file, line);
            file.close();
            ::DeleteFileA(commandPath_.c_str());
            if (line.empty())
                return;

            std::vector<std::string> args;
            std::istringstream tokens(line);
            std::string token;
            while (tokens >> token)
                args.push_back(token);
            if (!args.empty())
                do_handle_command(args);
        }

        void present_tick_impl()
        {
            ++frames_;
            poll_command_file();
            if (pendingLogDisabledWarning_.exchange(false))
                chat("SpectralFix file logging stopped after a rotation or write failure. The fix remains active; status counters are still available via //spectralfix status.");

            if ((frames_ % 60) == 0)
            {
                const bool verdictWasPending = displacementVerdictPending_;
                if (!hooks_intact())
                    handle_hook_displacement();
                if (verdictWasPending && displacementVerdictPending_)
                    resolve_displacement_verdict();
            }

            if (pendingLearn_)
            {
                pendingLearn_ = false;
                if (save_selector(pendingMarker_))
                {
                    selectorLearnedThisRun_ = true;
                    log_line("SpectralFix allocation learned and saved. Restart the entire client to apply enlargement.");
                    chat("SpectralFix learned the aura allocation. Exit and relaunch the client once to apply the fix.");
                }
                else
                {
                    log_line("Failed to save the learned allocation selector; stock rendering retained.");
                    chat("SpectralFix found the aura allocation but could not save spectralfix.ini. See spectralfix.log.");
                }
            }

            if (pendingSelectorConfirmation_)
            {
                pendingSelectorConfirmation_ = false;
                selectorVerifiedThisRun_      = true;
                if (selectorFromOrdinalDefault_)
                {
                    const bool saved = save_settings();
                    log_line(saved
                        ? "Ordinal-1 startup default confirmed by aura activity and saved for this client."
                        : "Ordinal-1 startup default confirmed by aura activity; spectralfix.ini could not be saved, but correction remains active.");
                }
                else
                {
                    log_line("Saved selector confirmed by aura activity for this session.");
                }
            }

            if (pendingSelectorMismatchWarning_)
            {
                pendingSelectorMismatchWarning_ = false;
                const bool saved = save_selector(pendingMismatchMarker_);
                log_line(saved
                    ? "SELECTOR MISMATCH: another allocation produced the aura activity. The override was saved; immediate full client exit is required."
                    : "SELECTOR MISMATCH: another allocation produced the aura activity, but the override could not be saved. Immediate full client exit is required.");
                chat("WARNING: SpectralFix detected an unexpected aura allocation and stopped adjusting it.");
                chat(saved
                    ? "Exit the entire client now. The corrected allocation was saved and will apply after relaunch."
                    : "Exit the entire client now. SpectralFix could not save the corrected allocation; see spectralfix.log.");
            }

            if (pendingFirstScale_) { pendingFirstScale_ = false; log_line("First enlarged downsample quad corrected successfully."); }
            if (pendingFirstSpread_) { pendingFirstSpread_ = false; log_line("First enlarged blur-tap offset corrected successfully."); }
            if (pendingFirstOpacity_) { pendingFirstOpacity_ = false; log_line("First blur-tap opacity adjustment applied successfully."); }
            if (pendingFirstComposite_)
            {
                pendingFirstComposite_ = false;
                log_line(compositeOpacityPercent_ <= 0.0001F
                    ? "First classified center composite omitted successfully."
                    : "First classified center composite opacity adjustment applied successfully.");
            }
            if (compositeRestoreFailed_) { compositeRestoreFailed_ = false; note_composite_state_failure(); }
            if (pendingCompositeStateFailure_)
            {
                pendingCompositeStateFailure_ = false;
                log_line("Center-composite state override or restoration failed; composite adjustment was disabled for this session.");
                chat("Center-composite opacity disabled after a Direct3D state failure; stock composite retained.");
            }
            if (drawExceptionNotice_.consume())
                log_line("DrawPrimitiveUP interception raised an exception; the original draw was preserved.");
            if (setTextureExceptionNotice_.consume())
                log_line("SetTexture observation raised an exception; stage-zero aura tracking was cleared.");
            if (drawFailureNotice_.consume())
            {
                log_line("A corrected DrawPrimitiveUP submission failed; the original stock draw was allowed through.");
                chat("SpectralFix draw correction failed and fell back to the original draw. Run //spectralfix status; diagnostic details are logged when available.");
            }
            if ((frames_ % 600) == 0)
                log_status("periodic");
        }

        // -- lifecycle -----------------------------

        bool do_initialize(const std::string& basePath)
        {
            installPath_ = basePath;
            if (!installPath_.empty() && installPath_.back() != '\\' && installPath_.back() != '/')
                installPath_ += '\\';

            const auto logDir = installPath_ + "logs\\spectralfix";
            ::CreateDirectoryA((installPath_ + "logs").c_str(), nullptr);
            ::CreateDirectoryA(logDir.c_str(), nullptr);
            logPath_        = logDir + "\\spectralfix.log";
            logArchivePath_ = logDir + "\\spectralfix.previous.log";
            const auto configDir = installPath_ + "config";
            ::CreateDirectoryA(configDir.c_str(), nullptr);
            configPath_ = configDir + "\\spectralfix.ini";
            outboxPath_ = logDir + "\\spectralfix_outbox.txt";
            ::DeleteFileA(outboxPath_.c_str()); // start each launch with an empty outbox
            commandPath_ = logDir + "\\spectralfix_command.txt";
            ::DeleteFileA(commandPath_.c_str()); // discard any stale request from a previous launch

            logBytesWritten_ = file_size_or_zero(logPath_);
            if (logBytesWritten_ >= kMaxLogBytes)
            {
                if (::MoveFileExA(logPath_.c_str(), logArchivePath_.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
                    logBytesWritten_ = 0;
                else
                {
                    logRotationFailedThisRun_ = true;
                    loggingDisabledAtLimit_   = true;
                }
            }
            if (!loggingDisabledAtLimit_)
                log_.open(logPath_, std::ios::out | std::ios::app);
            loggingAvailable_ = log_.is_open();

            module_ = read_module_identity();
            signatureCounts_.reserve(kMaxSignatureEntries);
            activity_.reserve(kMaxActivityEntries);
            load_selector();

            log_line(std::string("=== SpectralFix (Windower) v") + kVersionString + " initialized ===");
            log_line("Install path: " + installPath_);
            log_line(module_.valid
                ? "FFXiMain identity captured at initialize time."
                : "FFXiMain identity unavailable during initialize; will retry at device attach.");
            if (selector_.valid)
            {
                std::ostringstream out;
                out << "Learned selector loaded: timestamp=0x" << std::hex << std::uppercase
                    << selector_.moduleTimestamp << " size=0x" << selector_.moduleSize
                    << " caller=0x" << selector_.callerRva << " stack=0x" << selector_.stackHash
                    << std::dec << " ordinal=" << selector_.signatureOrdinal
                    << " target=" << selector_.targetSize << 'x' << selector_.targetSize;
                log_line(out.str());
            }
            else
            {
                log_line("No saved selector override: ordinal-1 startup default will arm on the first matching FFXiMain allocation.");
            }
            log_line(settings_text());
            return true;
        }

        bool do_attach_device(IDirect3DDevice8* device)
        {
            device_ = device;
            if (!module_.valid)
                module_ = read_module_identity();
            if (device_ == nullptr || !module_.valid)
            {
                log_line("attach_device refused: device or FFXiMain identity unavailable; stock rendering retained.");
                return false;
            }

            if (selector_.valid && (selector_.moduleTimestamp != module_.timestamp || selector_.moduleSize != module_.size))
            {
                selector_.valid = false;
                selectorLoadedFromConfig_ = false;
                log_line("Saved selector belongs to a different client build; ordinal-1 startup default will be used.");
            }
            if (selector_.valid)
                selectorVerificationPending_ = true;

            D3DCAPS8 caps{};
            if (SUCCEEDED(device_->GetDeviceCaps(&caps)))
            {
                targetCapsKnown_  = true;
                maxTextureWidth_  = caps.MaxTextureWidth;
                maxTextureHeight_ = caps.MaxTextureHeight;
                std::ostringstream capsLine;
                capsLine << "D3D8 texture caps: max=" << maxTextureWidth_ << 'x' << maxTextureHeight_;
                log_line(capsLine.str());

                const auto requestedTarget = selector_.valid ? selector_.targetSize : configuredTargetSize_;
                if (requestedTarget > maxTextureWidth_ || requestedTarget > maxTextureHeight_)
                {
                    targetUnsupported_    = true;
                    allowNewEnlargements_ = false;
                    auraFeaturesEnabled_  = false;
                    log_line("TARGET UNSUPPORTED: requested target exceeds the D3D8 device limit; stock rendering retained.");
                    chat("Requested aura target exceeds this D3D8 device's limit. Stock rendering retained; choose a smaller target and restart.");
                }
            }
            else
            {
                log_line("D3D8 texture caps query failed; CreateTexture failure fallback remains active.");
            }

            if (!pin_module())
            {
                allowNewEnlargements_ = false;
                auraFeaturesEnabled_  = false;
                log_line("Module pinning failed; D3D8 hooks were not installed and stock rendering was retained.");
                chat("SpectralFix could not secure its hook lifetime. Stock rendering retained; restart the client.");
                return false;
            }

            if (!install_hooks())
            {
                allowNewEnlargements_ = false;
                auraFeaturesEnabled_  = false;
                if (hookRollbackIncomplete_)
                {
                    log_line("D3D8 hook transaction failed and rollback was incomplete. No aura allocation was enlarged; SpectralFix retained accurate hook ownership and immediate client exit is required.");
                    chat("SpectralFix could not safely roll back a D3D8 hook. Exit the entire client now.");
                }
                else
                {
                    log_line("D3D8 hook transaction failed and rolled back completely; stock rendering retained.");
                }
                return false;
            }

            hooksPublished_ = true;
            log_line("attach_device complete; wrapper-neutral CreateTexture, SetTexture observation, DrawPrimitiveUP, and Present interception active.");
            chat("SpectralFix loaded. Use //spectralfix help for settings.");
            return true;
        }

        void do_handle_command(const std::vector<std::string>& args)
        {
            if (args.empty() || args[0].empty())
            {
                show_help();
                return;
            }
            const auto action = lower_copy(args[0]);
            if (action == "help")
            {
                show_help();
                return;
            }
            if (action == "status")
            {
                log_status("command");
                chat(settings_text());
                chat(status_text());
                chat("Diagnostics: " + logPath_);
                return;
            }
            if (selectorMismatch_)
            {
                chat("SpectralFix detected an allocation mismatch. Exit the entire client now; commands are disabled until relaunch.");
                return;
            }

            const auto value = args.size() > 1 ? args[1] : std::string();
            const auto extra = args.size() > 2 ? args[2] : std::string();

            if (action == "spread" && !value.empty())
            {
                float requested = 0.0F;
                if (value == "auto")
                    spreadOverride_ = 0.0F;
                else if (!parse_float(value, requested) || requested < kMinManualSpread || requested > kMaxManualSpread)
                {
                    chat("Usage: //spectralfix spread <auto|1.0-16.0>");
                    return;
                }
                else
                    spreadOverride_ = requested;
                const bool saved = save_settings();
                log_line("User changed " + settings_text() + (saved ? " (saved)." : " (not saved)."));
                chat(settings_text() + (saved ? " Saved." : " Active for this session only; spectralfix.ini could not be saved."));
                log_status("spread-command");
                return;
            }
            if (action == "target" && !value.empty())
            {
                const auto lowered = lower_copy(value);
                if (lowered == "medium" || lowered == "1024")
                    configuredTargetSize_ = kMediumTargetSize;
                else if (lowered == "high" || lowered == "2048")
                    configuredTargetSize_ = kDefaultTargetSize;
                else if (lowered == "ultra" || lowered == "4096")
                    configuredTargetSize_ = kUltraTargetSize;
                else
                {
                    chat("Usage: //spectralfix target <medium|high|ultra>");
                    return;
                }
                const bool saved = save_settings();
                log_line(std::string("User selected a new target size") + (saved ? " for the next launch (saved)." : " for the next launch (not saved)."));
                chat(saved ? "Target mode saved. Exit and relaunch the entire client to apply it."
                           : "Target mode could not be saved; see spectralfix.log.");
                log_status("target-command");
                return;
            }
            if (action == "opacity" && !value.empty())
            {
                float requested = 0.0F;
                const auto lowered = lower_copy(value);
                if (lowered == "stock" || lowered == "auto")
                    opacityPercent_ = kDefaultOpacityPercent;
                else if (!parse_float(value, requested) || requested < 0.0F || requested > 100.0F)
                {
                    chat("Usage: //spectralfix opacity <stock|0-100>");
                    return;
                }
                else
                    opacityPercent_ = requested;
                const bool saved = save_settings();
                log_line("User changed " + settings_text() + (saved ? " (saved)." : " (not saved)."));
                chat(settings_text() + (saved ? " Saved." : " Active for this session only; spectralfix.ini could not be saved."));
                log_status("opacity-command");
                return;
            }
            if (action == "composite" && lower_copy(value) == "opacity" && !extra.empty())
            {
                float requested = 0.0F;
                const auto lowered = lower_copy(extra);
                if (lowered == "stock" || lowered == "auto")
                {
                    compositeOpacityOverride_ = false;
                    compositeOpacityPercent_  = kDefaultOpacityPercent;
                }
                else if (!parse_float(extra, requested) || requested < 0.0F || requested > 100.0F)
                {
                    chat("Usage: //spectralfix composite opacity <stock|0-100>");
                    return;
                }
                else
                {
                    compositeOpacityOverride_ = true;
                    compositeOpacityPercent_  = requested;
                    compositeAdjustmentDisabledThisSession_ = false;
                }
                const bool saved = save_settings();
                log_line("User changed " + settings_text() + (saved ? " (saved)." : " (not saved)."));
                chat(settings_text() + (saved ? " Saved." : " Active for this session only; spectralfix.ini could not be saved."));
                log_status("composite-opacity-command");
                return;
            }

            chat("Unknown command. Use //spectralfix help.");
        }

        bool do_release()
        {
            if (must_retain_hooks_on_release(hooksPublished_, enlargementPublished_))
            {
                releaseRefused_ = true;
                log_line("RELEASE REFUSED: an enlarged allocation is still live, so SpectralFix kept its D3D8 hooks and stayed resident. Draw correction continues, but the Present hook's periodic tick stops noting anything new.");
                chat("SpectralFix cannot unload while its enlarged aura is live. Exit the entire client now; correction is being retained only to keep shutdown safe.");
                log_status("release-refused");
                return false;
            }
            auraFeaturesEnabled_ = false;
            if (!restore_hooks())
            {
                releaseRefused_ = true;
                log_line("RELEASE REFUSED: one or more D3D8 hooks could not be restored. SpectralFix stayed resident; immediate client exit is required.");
                chat("SpectralFix could not safely finish unloading its D3D8 hooks. Exit the entire client now.");
                log_status("release-refused-hook-rollback");
                return false;
            }
            log_status("release");
            log_line("=== SpectralFix (Windower) released ===");
            std::unique_lock<std::mutex> lock(logMutex_);
            loggingAvailable_ = false;
            if (log_.is_open())
                log_.close();
            return true;
        }
    };

    void WindowerCore::Impl::reset_hook_table()
    {
        hooks_[kHookCreateTexture] = HookSlot{
            "CreateTexture", kCreateTextureSlot, reinterpret_cast<void*>(&hook_create_texture), nullptr, nullptr, false};
        hooks_[kHookSetTexture] = HookSlot{
            "SetTexture", kSetTextureSlot, reinterpret_cast<void*>(&hook_set_texture), nullptr, nullptr, false};
        hooks_[kHookDrawPrimitiveUP] = HookSlot{
            "DrawPrimitiveUP", kDrawPrimitiveUPSlot, reinterpret_cast<void*>(&hook_draw_primitive_up), nullptr, nullptr, false};
        hooks_[kHookPresent] = HookSlot{
            "Present", kPresentSlot, reinterpret_cast<void*>(&hook_present), nullptr, nullptr, false};
    }

    // -- WindowerCore public forwarders --------------------

    WindowerCore::WindowerCore() : impl_(new Impl())
    {
        instance_ = this;
    }

    WindowerCore::~WindowerCore()
    {
        if (instance_ == this)
            instance_ = nullptr;
        // release() refused means hooks are still live in the device vtable -
        // deleting impl_ would turn the next draw into a use-after-free. Leak it
        // deliberately; only reached at process exit anyway.
        if (!releaseRefused_)
            delete impl_;
    }

    bool WindowerCore::initialize(const std::string& basePath)
    {
        return impl_->do_initialize(basePath);
    }

    bool WindowerCore::attach_device(IDirect3DDevice8* device)
    {
        return impl_->do_attach_device(device);
    }

    void WindowerCore::present_tick()
    {
        impl_->present_tick_impl();
    }

    void WindowerCore::handle_command(const std::vector<std::string>& args)
    {
        impl_->do_handle_command(args);
    }

    bool WindowerCore::release()
    {
        const auto ok = impl_->do_release();
        releaseRefused_ = impl_->releaseRefused_;
        return ok;
    }

    bool WindowerCore::release_refused() const
    {
        return releaseRefused_;
    }
}
