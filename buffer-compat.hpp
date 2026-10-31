#pragma once
#include <algorithm>
#include <cstdint>
#include <vector>
namespace GpuSwitch {
struct OutputFormat {uint32_t format=0;std::vector<uint64_t> modifiers;};
inline bool outputAccepts(uint32_t format,uint64_t modifier,const std::vector<OutputFormat>& formats) {
    // DRM_FORMAT_MOD_INVALID means implicit/unknown layout. Never assume it is
    // linear, especially after an allocator has fallen back from explicit modifiers.
    constexpr uint64_t invalid=(1ULL<<56)-1;
    if(modifier==invalid || formats.empty()) return false;
    for(const auto& supported:formats)
        if(supported.format==format && std::ranges::find(supported.modifiers,modifier)!=supported.modifiers.end()) return true;
    return false;
}
}
