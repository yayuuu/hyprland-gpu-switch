#pragma once
#ifndef GPU_SWITCH_DIAGNOSTICS
#define GPU_SWITCH_DIAGNOSTICS 0
#endif
#if GPU_SWITCH_DIAGNOSTICS != 0 && GPU_SWITCH_DIAGNOSTICS != 1
#error "GPU_SWITCH_DIAGNOSTICS must be 0 or 1"
#endif
namespace GpuSwitch::Diagnostics {
#if GPU_SWITCH_DIAGNOSTICS
void trace(const char* phase) noexcept;
#else
inline void trace(const char*) noexcept {}
#endif
}
