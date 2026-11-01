#pragma once
#include <string_view>
#include <optional>
#include <span>
#include <vector>
#include <cstddef>

namespace Singlepass {
    enum class EPassEntry { Content, ScopeBegin, ScopeEnd };
    struct SPassEntry {
        EPassEntry kind = EPassEntry::Content;
        const void* scope = nullptr;
    };

    // 0.57 inserts backdrop scope markers around ordinary window surfaces.
    // Keep every drawable entry for the opacity/overlay checks; only balanced
    // scope markers may be omitted once those checks prove no backdrop is needed.
    inline std::optional<std::vector<size_t>> passContentIndices(std::span<const SPassEntry> entries) {
        std::vector<const void*> scopes;
        std::vector<size_t> content;
        for (size_t i = 0; i < entries.size(); ++i) {
            const auto& entry = entries[i];
            if (entry.kind == EPassEntry::Content) {
                content.push_back(i);
                continue;
            }
            if (!entry.scope) return std::nullopt;
            if (entry.kind == EPassEntry::ScopeBegin)
                scopes.push_back(entry.scope);
            else {
                if (scopes.empty() || scopes.back() != entry.scope) return std::nullopt;
                scopes.pop_back();
            }
        }
        if (!scopes.empty()) return std::nullopt;
        return content;
    }

    struct SConditions {
        bool enabled = false;
        bool targetOutput = false;
        bool normalGLFrame = false;
        bool hdrFP16 = false;
        bool noCapture = false;
        bool noPostprocessing = false;
        bool oneOpaqueFullscreenSurface = false;
        bool fullDamage = false;
        bool identityInputConversion = false;
    };

    constexpr std::string_view skipReason(const SConditions& c) {
        if (!c.enabled)
            return "disabled";
        if (!c.targetOutput)
            return "other output";
        if (!c.normalGLFrame)
            return "not a normal offloaded GL frame";
        if (!c.hdrFP16)
            return "output is not HDR with an FP16 work buffer";
        if (!c.noCapture)
            return "capture or mirroring requires the work buffer";
        if (!c.noPostprocessing)
            return "postprocessing, transform or non-shader CM";
        if (!c.oneOpaqueFullscreenSurface)
            return "frame contains overlays, cursor or non-opaque surfaces";
        if (!c.fullDamage)
            return "frame does not repaint the complete output";
        if (!c.identityInputConversion)
            return "input-to-workbuffer conversion is not an HDR identity";
        return {};
    }
}
