#include <bits/stdc++.h>
#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/plugins/HookSystem.hpp>
#include "transfer-opt.hpp"
#include <dlfcn.h>
#include <sys/mman.h>
#include <unistd.h>
static HANDLE owner=nullptr;
static void* entry=nullptr;
static void* trampoline=nullptr;
static std::array<unsigned char,14> saved;
static void writeJump(void* at,void* target) {
 const unsigned char op[]={0xff,0x25,0,0,0,0};memcpy(at,op,6);memcpy(static_cast<char*>(at)+6,&target,8);
}
static bool protectEntry(int mode) {auto size=sysconf(_SC_PAGESIZE);return mprotect(reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(entry)&~(size-1)),size,mode)==0;}
static void removeTransferHook() {
 if(!entry)return;
 if(protectEntry(PROT_READ|PROT_WRITE|PROT_EXEC)) {memcpy(entry,saved.data(),saved.size());protectEntry(PROT_READ|PROT_EXEC);}
 if(trampoline)munmap(trampoline,4096);
 entry=nullptr;trampoline=nullptr;
}
static SP<IPC::Socket1::SCommand> command;
static bool enabled=true;
static GpuSwitch::TransferStats stats;
static Aquamarine::CDRMRenderer::SBlitResult blit(Aquamarine::CDRMRenderer* renderer,SP<Aquamarine::IBuffer> from,SP<Aquamarine::IBuffer> to,SP<Aquamarine::CDRMRenderer> source,int fd) {
 return GpuSwitch::optimizedTransfer(renderer,from,to,source,fd,reinterpret_cast<GpuSwitch::BlitFunction>(trampoline),enabled,stats);
}
APICALL EXPORT std::string PLUGIN_API_VERSION(){return HYPRLAND_API_VERSION;}
APICALL EXPORT PLUGIN_DESCRIPTION_INFO PLUGIN_INIT(HANDLE h) {
 if(std::string(__hyprland_api_get_hash())!=std::string(__hyprland_api_get_client_hash()))throw std::runtime_error("Headers/runtime mismatch");
 owner=h;
 auto address=dlsym(RTLD_DEFAULT,"_ZN10Aquamarine12CDRMRenderer4blitEN9Hyprutils6Memory14CSharedPointerINS_7IBufferEEES5_NS3_IS0_EEi");
 if(!address)throw std::runtime_error("Aquamarine blit ABI unavailable");
 // The stock hook allocator is near the executable and cannot reach this DSO.
 // This exact verified prologue has no PC-relative operands; retain all 14 bytes.
 const std::array<unsigned char,14> expected={0xf3,0x0f,0x1e,0xfa,0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x55};
 if(memcmp(address,expected.data(),expected.size()))throw std::runtime_error("Unverified Aquamarine blit prologue; refusing patch");
 trampoline=mmap(nullptr,4096,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
 if(trampoline==MAP_FAILED){trampoline=nullptr;throw std::runtime_error("Cannot allocate transfer trampoline");}
 memcpy(trampoline,address,14);writeJump(static_cast<char*>(trampoline)+14,static_cast<char*>(address)+14);
 if(mprotect(trampoline,4096,PROT_READ|PROT_EXEC)){munmap(trampoline,4096);trampoline=nullptr;throw std::runtime_error("Cannot protect transfer trampoline");}
 entry=address;memcpy(saved.data(),entry,14);
 if(!protectEntry(PROT_READ|PROT_WRITE|PROT_EXEC)){munmap(trampoline,4096);entry=nullptr;trampoline=nullptr;throw std::runtime_error("Cannot patch transfer entry");}
 writeJump(entry,reinterpret_cast<void*>(&blit));protectEntry(PROT_READ|PROT_EXEC);
 command=HyprlandAPI::registerHyprCtlCommand(owner,IPC::Socket1::SCommand{.name="gpu-transfer-opt",.match=IPC::Socket1::COMMAND_MATCH_PREFIX,.handler=[](const IPC::Socket1::SRequest& ipcRequest) -> IPC::Socket1::SResponse {
 const auto& request=ipcRequest.command;
 if(request.ends_with(" disable"))enabled=false;
 if(request.ends_with(" enable"))enabled=true;
 if(request.ends_with(" reset"))stats={};
 return std::format("enabled={} frames={} optimized={} allocations={} mapped={} bytes={} cpu_wall_ms={:.3f}",enabled,stats.frames,stats.optimized,stats.allocations,stats.mapped,stats.bytes,stats.nanoseconds/1e6);
 }});
 return {"gpu-transfer-opt","daniel","Reuse cross-GPU upload storage; live A/B diagnostics","0.2.0"};
}
APICALL EXPORT void PLUGIN_EXIT(){if(command){HyprlandAPI::unregisterHyprCtlCommand(owner,command);command.reset();}removeTransferHook();}
