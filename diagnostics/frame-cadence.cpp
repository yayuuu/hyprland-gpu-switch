#include <bits/stdc++.h>
#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/output/Monitor.hpp>
#include <hyprland/src/state/MonitorState.hpp>
#include <hyprland/src/protocols/core/Compositor.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/ipc/s1/S1.hpp>
#include "../transfer/queued_state.hpp"
static HANDLE handle;
static SP<IPC::Socket1::SCommand> command;
static CHyprSignalListener presentation,stages;
using Clock=std::chrono::steady_clock;
static std::vector<double> presents,renders,durations;
static Clock::time_point start;
static std::optional<double> begin;
static double now(){return std::chrono::duration<double,std::milli>(Clock::now()-start).count();}
static void record(std::vector<double>& samples,double value){if(samples.size()<8000)samples.push_back(value);}
struct Watch {
    WP<CWLSurfaceResource> surface;
    pid_t pid=0;
    CHyprSignalListener queued,committed;
    std::vector<double> queueTimes,commitTimes;
};
static std::vector<std::unique_ptr<Watch>> watches;
static void detach(){presentation.reset();stages.reset();for(auto& w:watches){w->queued.reset();w->committed.reset();}}
static std::string stats(const std::vector<double>& samples,bool intervals=true){
    std::vector<double> d;
    if(intervals){for(size_t i=1;i<samples.size();++i)if(samples[i]>samples[i-1])d.push_back(samples[i]-samples[i-1]);}else d=samples;
    if(d.empty())return "empty";
    std::sort(d.begin(),d.end());double avg=std::accumulate(d.begin(),d.end(),0.0)/d.size();
    std::ostringstream out;out<<"samples="<<samples.size()<<" avg_ms="<<avg<<" hz="<<1000/avg<<" p50_ms="<<d[d.size()/2]<<" p95_ms="<<d[size_t((d.size()-1)*.95)]<<" p99_ms="<<d[size_t((d.size()-1)*.99)]<<" max_ms="<<d.back()<<" over10ms="<<std::count_if(d.begin(),d.end(),[](double x){return x>10;});return out.str();
}
APICALL EXPORT std::string PLUGIN_API_VERSION(){return HYPRLAND_API_VERSION;}
APICALL EXPORT PLUGIN_DESCRIPTION_INFO PLUGIN_INIT(HANDLE h){
    if(std::string(__hyprland_api_get_hash())!=std::string(__hyprland_api_get_client_hash()))throw std::runtime_error("ABI mismatch");
    handle=h;
    command=HyprlandAPI::registerHyprCtlCommand(h,{.name="gpu-frame-probe",.match=IPC::Socket1::COMMAND_MATCH_PREFIX,.handler=[](const IPC::Socket1::SRequest& ipcRequest) -> IPC::Socket1::SResponse {
 const auto& request=ipcRequest.command;
        std::istringstream words(request);std::string name,action;words>>name>>action;
        if(action=="start"){
            detach();watches.clear();presents.clear();renders.clear();durations.clear();begin.reset();start=Clock::now();
            for(auto& m:State::monitorState()->monitors())if(m->m_name=="DP-1")presentation=m->m_events.presented.listen([](const Time::steady_tp& time){record(presents,std::chrono::duration<double,std::milli>(time-start).count());});
            stages=Event::bus()->m_events.render.stage.listen([](const Event::SRenderStageEvent& event){
                const auto stage=event.stage;
                if(stage==RENDER_BEGIN){const auto m=event.monitor;if(m && m->m_name=="DP-1"){begin=now();record(renders,*begin);}}
                else if(stage==RENDER_POST && begin){record(durations,now()-*begin);begin.reset();}
            });
            PROTO::compositor->forEachSurface([](SP<CWLSurfaceResource> surface){
                if(!surface->m_current.buffer)return;
                auto a=surface->m_current.buffer->dmabuf();
                if(!a.success || a.size.x<3000 || a.size.y<1000)return;
                auto w=std::make_unique<Watch>();w->surface=surface;wl_client_get_credentials(surface->client(),&w->pid,nullptr,nullptr);auto ptr=w.get();
                w->queueTimes.reserve(2000);w->commitTimes.reserve(2000);
                w->queued=surface->m_events.stateCommit2.listen([ptr](WP<SSurfaceState> state){auto* s=Offload::borrowQueuedState(state);if(s && s->updated.bits.buffer)record(ptr->queueTimes,now());});
                w->committed=surface->m_events.commit.listen([ptr]{record(ptr->commitTimes,now());});watches.push_back(std::move(w));
            });
            return std::string("ok");
        }
        if(action=="stop")detach();
        std::ostringstream out;out<<"presentation "<<stats(presents)<<"\nrender "<<stats(renders)<<"\nrender_duration "<<stats(durations,false)<<'\n';
        for(const auto& w:watches)out<<"pid="<<w->pid<<" surface="<<w->surface.get()<<" queued "<<stats(w->queueTimes)<<" committed "<<stats(w->commitTimes)<<'\n';
        return out.str();
    }});
    return {"gpu-frame-probe","Read-only frame cadence measurements","daniel","1"};
}
APICALL EXPORT void PLUGIN_EXIT(){detach();watches.clear();if(command)HyprlandAPI::unregisterHyprCtlCommand(handle,command);}
