#include <bits/stdc++.h>
#define private public
#include <hyprland/src/managers/eventLoop/EventLoopManager.hpp>
#undef private
#include <hyprland/src/plugins/PluginAPI.hpp>
#include <cxxabi.h>
#if !GPU_SWITCH_DIAGNOSTICS
#error Timer inspection requires a diagnostic build
#endif
static HANDLE owner;
static SP<IPC::Socket1::SCommand> command;
APICALL EXPORT std::string PLUGIN_API_VERSION(){return HYPRLAND_API_VERSION;}
APICALL EXPORT PLUGIN_DESCRIPTION_INFO PLUGIN_INIT(HANDLE h){
    if(std::string(__hyprland_api_get_hash())!=std::string(__hyprland_api_get_client_hash()))throw std::runtime_error("ABI mismatch");
    owner=h;
    command=HyprlandAPI::registerHyprCtlCommand(h,{.name="gpu-transfer-timers",.handler=[](const IPC::Socket1::SRequest&)->IPC::Socket1::SResponse{
        std::string report;
        for(const auto& timer:g_pEventLoopManager->m_timers.timers){
            const auto name=std::string{timer->m_cb.target_type().name()};
            if(!name.contains("GpuSwitch"))continue;
            int status=0;
            std::unique_ptr<char,decltype(&std::free)> demangled(abi::__cxa_demangle(name.c_str(),nullptr,nullptr,&status),&std::free);
            report+=std::format("{} armed={} cancelled={} left_us={}\n",status==0?demangled.get():name.c_str(),timer->armed(),timer->cancelled(),timer->leftUs());
        }
        return report;
    }});
    return {"gpu-transfer-timers","Read-only inspection of GPU-switch event-loop timers","daniel","0.1"};
}
APICALL EXPORT void PLUGIN_EXIT(){if(command)HyprlandAPI::unregisterHyprCtlCommand(owner,command);command.reset();}
