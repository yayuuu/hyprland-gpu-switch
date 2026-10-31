#include <hyprland/src/plugins/PluginAPI.hpp>
#include <dlfcn.h>
static HANDLE owner=nullptr;
static SP<SHyprCtlCommand> command;
static std::string data;
APICALL EXPORT std::string PLUGIN_API_VERSION(){return HYPRLAND_API_VERSION;}
APICALL EXPORT PLUGIN_DESCRIPTION_INFO PLUGIN_INIT(HANDLE handle){
    owner=handle;
    for(const auto& item:HyprlandAPI::findFunctionsByName(handle,"bind"))
        if(item.signature.contains("CGLTexture"))data+=std::format("{} | {} | {}\n",item.signature,item.demangled,item.address);
    data+=std::format("exact dlsym: {}\n",dlsym(RTLD_DEFAULT,"_ZN6Render2GL10CGLTexture4bindEv"));
    command=HyprlandAPI::registerHyprCtlCommand(handle,SHyprCtlCommand{.name="gpu-hook-probe",.exact=true,.fn=[](eHyprCtlOutputFormat,std::string){return data;}});
    return {"gpu-hook-probe","Read-only function symbol diagnostics","daniel","0.1"};
}
APICALL EXPORT void PLUGIN_EXIT(){if(command)HyprlandAPI::unregisterHyprCtlCommand(owner,command);command.reset();}
