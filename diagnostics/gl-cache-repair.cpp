#include <bits/stdc++.h>
#define private public
#include <hyprland/src/render/OpenGL.hpp>
#undef private
#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/ipc/s1/S1.hpp>
#include "../gl-state-cache.hpp"
static HANDLE owner;
static CHyprSignalListener listener;
static SP<IPC::Socket1::SCommand> command;
static std::string report="waiting for render";
static bool repair=false,done=false;
APICALL EXPORT std::string PLUGIN_API_VERSION(){return HYPRLAND_API_VERSION;}
APICALL EXPORT PLUGIN_DESCRIPTION_INFO PLUGIN_INIT(HANDLE h){
 if(std::string(__hyprland_api_get_hash())!=std::string(__hyprland_api_get_client_hash()))throw std::runtime_error("ABI mismatch");
 owner=h;
 listener=Event::bus()->m_events.render.stage.listen([](const Event::SRenderStageEvent& e){
  if(e.stage!=RENDER_BEGIN || done || !Render::GL::g_pHyprOpenGL || eglGetCurrentContext()==EGL_NO_CONTEXT)return;
  auto& gl=*Render::GL::g_pHyprOpenGL;
  GLint src=0,dst=0;glGetIntegerv(GL_BLEND_SRC_RGB,&src);glGetIntegerv(GL_BLEND_DST_RGB,&dst);
  report=std::format("cached_blend_src={} cached_blend_dst={} actual_blend_src={} actual_blend_dst={}",gl.m_blendSFactor,gl.m_blendDFactor,src,dst);
  if(repair){GpuSwitch::synchronizeGLStateCache(gl);report+="; renderer cache synchronized";done=true;}
 });
 command=HyprlandAPI::registerHyprCtlCommand(h,{.name="gpu-gl-cache",.match=IPC::Socket1::COMMAND_MATCH_PREFIX,.handler=[](const IPC::Socket1::SRequest& req)->IPC::Socket1::SResponse{
  if(req.command=="gpu-gl-cache repair"){repair=true;done=false;return std::string("scheduled for next render");}return report;
 }});
 return {"gpu-gl-cache-repair","Temporary GL cache inspection and one-shot repair","daniel","0.1"};
}
APICALL EXPORT void PLUGIN_EXIT(){listener.reset();if(command)HyprlandAPI::unregisterHyprCtlCommand(owner,command);command.reset();}
