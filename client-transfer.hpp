#pragma once
#include <hyprland/src/plugins/PluginAPI.hpp>
namespace GpuSwitch {
void initClientTransfer(HANDLE handle);
void exitClientTransfer();
std::string clientTransferStatus();
}
