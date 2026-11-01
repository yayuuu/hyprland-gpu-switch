#pragma once
#include <hyprland/src/plugins/PluginAPI.hpp>
class CSurfacePassElement;
namespace GpuSwitch {
void initRenderingOptimizations(HANDLE);
void exitRenderingOptimizations();
std::string renderingOptimizationsControl(const std::string& arguments);
void observeRenderedSurface(CSurfacePassElement*);
}
