#include <bits/stdc++.h>
#define private public
#define protected public
#include <aquamarine/backend/DRM.hpp>
#include <aquamarine/allocator/Swapchain.hpp>
#include <hyprland/src/output/Monitor.hpp>
#include <hyprland/src/output/MonitorFrameScheduler.hpp>
#include <hyprland/src/managers/eventLoop/EventLoopTimer.hpp>
#undef protected
#undef private
#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/state/MonitorState.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/ipc/s1/S1.hpp>
#include "../json.hpp"
#include <sys/stat.h>
#include <sys/sysmacros.h>
#if !GPU_SWITCH_DIAGNOSTICS
#error This probe requires DIAGNOSTICS=1
#endif
using GpuSwitch::Json;
static HANDLE owner;
static CFunctionHook* commitHook=nullptr;
static SP<IPC::Socket1::SCommand> command;
static CHyprSignalListener added,removed;
static unsigned long serial=0,failures=0;
static std::deque<Json> events;
static std::string generation=std::to_string(std::chrono::system_clock::now().time_since_epoch().count());
struct Watch {PHLMONITORREF monitor;CHyprSignalListener dpms;};
static std::map<Monitor::CMonitor*,std::unique_ptr<Watch>> watches;
static unsigned long monoMS(){return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();}
static Json state(Monitor::CMonitor* m){
 Json result={{"name",m->m_name},{"enabled",m->m_enabled},{"dpms",m->m_dpmsStatus},
 {"pending_dpms_animation",m->m_pendingDpmsAnimation},{"dpms_animation_frames",static_cast<unsigned long>(m->m_pendingDpmsAnimationCounter)},
 {"retry_timer",bool(m->m_dpmsRetryTimer)},{"retry_armed",m->m_dpmsRetryTimer?m->m_dpmsRetryTimer->armed():false},{"pending_frame",m->m_pendingFrame},{"rendering",m->m_renderingActive},
 {"last_present_ms",static_cast<unsigned long>(std::max(0.L,static_cast<long double>(m->m_lastPresentationTimer.getMillis())))}};
 Json extra;
 if(m->m_output){
  const auto& pending=m->m_output->state->state();
  auto* output=dynamic_cast<Aquamarine::CDRMOutput*>(m->m_output.get());
  auto backend=output?dynamicPointerCast<Aquamarine::CDRMBackend>(output->getBackend()):nullptr;
  auto c=output?output->connector:nullptr;
  std::string pci;
  if(backend){struct stat st{};if(!fstat(backend->drmFD(),&st)){std::error_code ec;auto p=std::filesystem::canonical("/sys/dev/char/"+std::to_string(major(st.st_rdev))+":"+std::to_string(minor(st.st_rdev))+"/device",ec);if(!ec)pci=p.filename();}}
  extra={{"state_enabled",pending.enabled},{"pending_properties",static_cast<unsigned long>(pending.committed)},
   {"has_buffer",bool(pending.buffer)},{"in_fence",pending.explicitInFence>=0},{"out_fence",pending.explicitOutFence>=0},
   {"output_enabled",output?output->enabledState:false},{"pci",pci},
   {"copy_renderer_present",backend?bool(backend->rendererState.renderer):false},
   {"render_buffers",static_cast<unsigned long>(m->m_output->swapchain?m->m_output->swapchain->buffers.size():0)},
   {"null_render_buffers",static_cast<unsigned long>(m->m_output->swapchain?std::count_if(m->m_output->swapchain->buffers.begin(),m->m_output->swapchain->buffers.end(),[](const auto& b){return !b;}):0)},
   {"scanout_buffers",static_cast<unsigned long>(output && output->mgpu.swapchain?output->mgpu.swapchain->buffers.size():0)},
   {"connector_id",static_cast<unsigned long>(c?c->id:0)},{"cached_crtc",static_cast<unsigned long>(c && c->crtc?c->crtc->id:0)},
   {"frame_in_flight",c?c->sched.frameInFlight():false},{"frame_running",c?c->sched.frameRunning():false},
   {"frame_scheduled",c?c->sched.frameScheduled():false},{"commit_tainted",c?c->commitTainted:false},
   {"atomic_props_cached",c?c->atomic.propsCached:false},{"session_active",backend?backend->sessionActive():false}};
 }
 else extra={{"state_enabled",false},{"has_buffer",false},{"pci",""}};
 // Re-encode through an object containing the typed subobjects.
 return Json{{"monitor",result},{"backend",extra}};
}
static void record(const char* kind,Monitor::CMonitor* monitor,bool ok=true){
 events.push_back(Json{{"sequence",++serial},{"mono_ms",monoMS()},{"kind",kind},{"ok",ok},{"state",state(monitor)}});
 if(events.size()>256)events.pop_front();
}
static void watch(PHLMONITOR monitor){
 std::erase_if(watches,[](const auto& p){return p.second->monitor.expired();});
 if(!monitor || watches.contains(monitor.get()))return;
 auto w=std::make_unique<Watch>();w->monitor=monitor;
 w->dpms=monitor->m_events.dpmsChanged.listen([weak=PHLMONITORREF(monitor)]{if(auto m=weak.lock())record("dpms-intent-before-commit",m.get());});
 watches.emplace(monitor.get(),std::move(w));record("monitor-watch",monitor.get());
}
static bool commit(Monitor::CMonitorState* self){
 auto* monitor=self->m_owner;
 const bool control=monitor && monitor->m_output && (monitor->m_output->state->state().committed & Aquamarine::COutputState::AQ_OUTPUT_STATE_ENABLED);
 if(control)record("enable-commit-before",monitor);
 bool ok=reinterpret_cast<bool(*)(Monitor::CMonitorState*)>(commitHook->m_original)(self);
 if(!ok)++failures;
 if(monitor && (control || (!ok && (failures<8 || failures%120==1))))record("commit-result",monitor,ok);
 return ok;
}
static void cleanup(){
 if(commitHook){commitHook->unhook();HyprlandAPI::removeFunctionHook(owner,commitHook);commitHook=nullptr;}
 added.reset();removed.reset();watches.clear();events.clear();
 if(command)HyprlandAPI::unregisterHyprCtlCommand(owner,command);
 command.reset();
}
APICALL EXPORT std::string PLUGIN_API_VERSION(){return HYPRLAND_API_VERSION;}
APICALL EXPORT PLUGIN_DESCRIPTION_INFO PLUGIN_INIT(HANDLE h){
 if(std::string(__hyprland_api_get_hash())!=std::string(__hyprland_api_get_client_hash()))throw std::runtime_error("ABI mismatch");
 owner=h;
 try{
  std::set<void*> matches;
  for(const auto& f:HyprlandAPI::findFunctionsByName(h,"commit"))if(f.demangled=="Monitor::CMonitorState::commit()")matches.insert(f.address);
  if(matches.size()!=1)throw std::runtime_error("Ambiguous monitor commit hook");
  commitHook=HyprlandAPI::createFunctionHook(h,*matches.begin(),reinterpret_cast<void*>(&commit));
  if(!commitHook || !commitHook->hook())throw std::runtime_error("Cannot observe monitor commit");
  for(auto& m:State::monitorState()->allMonitors())watch(m);
  added=Event::bus()->m_events.monitor.added.listen([](PHLMONITOR m){watch(m);});
  removed=Event::bus()->m_events.monitor.removed.listen([](PHLMONITOR m){if(m)record("monitor-removed",m.get());});
  command=HyprlandAPI::registerHyprCtlCommand(h,{.name="wakeprobe",.match=IPC::Socket1::COMMAND_MATCH_PREFIX,.handler=[](const IPC::Socket1::SRequest&)->IPC::Socket1::SResponse{
   Json monitors=Json::array(),history=Json::array();for(auto& m:State::monitorState()->allMonitors())monitors.push_back(state(m.get()));for(auto& e:events)history.push_back(e);
   return Json{{"version","0.1.0"},{"generation",generation},{"mono_ms",monoMS()},{"global_dpms",g_pCompositor->m_dpmsStateOn},{"commit_failures",failures},{"sequence",serial},{"monitors",monitors},{"events",history}}.dump();
  }});
  if(!command)throw std::runtime_error("Cannot register wake probe");
 }catch(...){cleanup();throw;}
 return {"output-wake-probe","Passive DPMS commit and frame-queue diagnostics","daniel","0.1.0"};
}
APICALL EXPORT void PLUGIN_EXIT(){cleanup();}
