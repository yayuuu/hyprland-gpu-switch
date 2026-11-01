#include "rendering-optimizations.hpp"
#include "client-transfer.hpp"
#include "probe.hpp"
#include "migration.hpp"
#include "drm-route.hpp"
#include "diagnostics/diagnostics.hpp"
#include "version.hpp"
#include <libudev.h>
#include <cstdlib>
#include <sstream>
#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/managers/eventLoop/EventLoopManager.hpp>
#include <hyprland/src/managers/eventLoop/EventLoopTimer.hpp>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <chrono>
#include <optional>

// Migration runs from a raw Wayland idle callback, outside timer snapshots.
static HANDLE handle=nullptr;
static SP<CEventLoopTimer> timer;
static SP<SHyprCtlCommand> command;
static GpuSwitch::Roles roles;
static GpuSwitch::Json report;
static std::string previousTopology;
static std::optional<std::string> candidate;
static unsigned long generation=0;
static bool enabled=true;
static std::string lastMigrationError;
#if GPU_SWITCH_DIAGNOSTICS
static std::optional<std::string> testRole;
#endif
static udev* udevContext=nullptr;
static udev_monitor* udevMonitor=nullptr;
static wl_event_source* udevSource=nullptr;
static wl_event_source* migrationIdle=nullptr;
static unsigned idleDeferrals=0;
static void sample(bool runMigration);
static void runMigrationIdle(void*) {
    migrationIdle=nullptr;
    if(!GpuSwitch::migrationIdleReady()) {
        // Timer rescheduling queues core nudgeTimers after our first idle source.
        // Requeue at the end so that work can drain before checking again.
        // Bound this to avoid starving the event loop if another callback keeps
        // replenishing the core queue.
        if(++idleDeferrals<8)
            migrationIdle=wl_event_loop_add_idle(g_pCompositor->m_wlEventLoop,runMigrationIdle,nullptr);
        return;
    }
    idleDeferrals=0;
    sample(true);
}
static bool drainEvents() {
    bool changed=false;
    if(udevMonitor) while(auto device=udev_monitor_receive_device(udevMonitor)) {
        changed=true;++generation;udev_device_unref(device);
    }
    if(changed) {previousTopology.clear();candidate.reset();}
    return changed;
}
static std::chrono::steady_clock::time_point candidateSince;

static void sample(bool runMigration=false) {
    drainEvents();
    GpuSwitch::maintainMigration();
    if(!enabled)GpuSwitch::cancelMigration();
    try {
        const auto gpus=GpuSwitch::scan(roles);
        const auto current=GpuSwitch::renderingDevice();
        const auto inventory=GpuSwitch::describe(gpus,roles);
        const auto fingerprint=inventory.dump();
        const auto now=std::chrono::steady_clock::now();
        auto desired=GpuSwitch::choose(gpus,current);
#if GPU_SWITCH_DIAGNOSTICS
        if(testRole) for(const auto& gpu:gpus)
            if((*testRole=="integrated" && gpu.role==GpuSwitch::Role::Integrated) || (*testRole=="discrete" && gpu.role==GpuSwitch::Role::Discrete)) desired=gpu.id;
#endif
        if (fingerprint!=previousTopology || desired!=candidate) {
            ++generation;previousTopology=fingerprint;candidate=desired;candidateSince=now;
        }
        const bool settled=now-candidateSince>=std::chrono::milliseconds(750);
        report={{"mode",enabled?"automatic":"disabled"},{"migration_supported",true},
#if GPU_SWITCH_DIAGNOSTICS
                {"generation",generation},{"settled",settled},{"gpus",inventory},
#endif
                {"current",current},{"renderer",GpuSwitch::renderingName()},{"tracking_complete",GpuSwitch::trackingComplete()},{"desired",desired?GpuSwitch::Json(*desired):GpuSwitch::Json(nullptr)},
                {"readiness",GpuSwitch::migrationReadiness()},{"last_error",lastMigrationError},
                {"state",!lastMigrationError.empty()?"blocked":!GpuSwitch::trackingComplete()?"awaiting-startup-tracking":GpuSwitch::migrationPaused()?"quiescing":!desired?"no-classified-target":current.empty()?"renderer-unknown":*desired==current?"already-on-target":!settled?"debouncing":"ready-to-migrate"}};
        if(runMigration && enabled && settled && desired && !current.empty() && *desired!=current && lastMigrationError.empty()) {
            const auto planGeneration=generation;
            const auto planFingerprint=fingerprint;
            auto result=GpuSwitch::migrate(*desired,[&] {
                if(drainEvents() || generation!=planGeneration) return false;
                return GpuSwitch::describe(GpuSwitch::scan(roles),roles).dump()==planFingerprint;
            });
            if(!result.starts_with("retry:"))lastMigrationError=result;
        } else if(!desired || *desired==current || !lastMigrationError.empty()) GpuSwitch::cancelMigration();
    } catch (const std::exception& e) {
        // Invalidate any outstanding plan, including errors halfway through unplug.
        ++generation;candidate.reset();previousTopology.clear();GpuSwitch::cancelMigration();
        report={{"mode",enabled?"automatic":"disabled"},{"migration_supported",true},
#if GPU_SWITCH_DIAGNOSTICS
                {"generation",generation},
#endif
                {"desired",nullptr},{"state","topology-unavailable"},{"error",e.what()}};
    }
}
static void cleanup() {
    if(migrationIdle) {wl_event_source_remove(migrationIdle);migrationIdle=nullptr;}
    if(udevSource) {wl_event_source_remove(udevSource);udevSource=nullptr;}
    if(udevMonitor) {udev_monitor_unref(udevMonitor);udevMonitor=nullptr;}
    if(udevContext) {udev_unref(udevContext);udevContext=nullptr;}
    GpuSwitch::exitRenderingOptimizations();
    GpuSwitch::exitClientTransfer();
    GpuSwitch::exitMigration();
    if (timer) { timer->cancel();g_pEventLoopManager->removeTimer(timer);timer.reset(); }
    if (command) { HyprlandAPI::unregisterHyprCtlCommand(handle,command);command.reset(); }
    roles.clear();candidate.reset();previousTopology.clear();report=nullptr;
}
APICALL EXPORT std::string PLUGIN_API_VERSION() { return HYPRLAND_API_VERSION; }
APICALL EXPORT PLUGIN_DESCRIPTION_INFO PLUGIN_INIT(HANDLE h) {
    if (std::string(__hyprland_api_get_hash())!=std::string(__hyprland_api_get_client_hash()))
        throw std::runtime_error("hyprland-gpu-switch: installed headers/runtime ABI mismatch");
    handle=h;
    try {
        roles=GpuSwitch::discoverRoles();
        GpuSwitch::initMigration(handle);
        GpuSwitch::initClientTransfer(handle);
        GpuSwitch::initRenderingOptimizations(handle);
        udevContext=udev_new();
        if(!udevContext) throw std::runtime_error("Cannot create udev observer");
        udevMonitor=udev_monitor_new_from_netlink(udevContext,"udev");
        if(!udevMonitor || udev_monitor_filter_add_match_subsystem_devtype(udevMonitor,"drm",nullptr)<0 || udev_monitor_enable_receiving(udevMonitor)<0)
            throw std::runtime_error("Cannot observe DRM hotplug events");
        udevSource=wl_event_loop_add_fd(g_pCompositor->m_wlEventLoop,udev_monitor_get_fd(udevMonitor),WL_EVENT_READABLE,
            [](int,uint32_t,void*) -> int {drainEvents();lastMigrationError.clear();return 0;},nullptr);
        if(!udevSource) throw std::runtime_error("Cannot register DRM event observer");
        sample();
        command=HyprlandAPI::registerHyprCtlCommand(handle,SHyprCtlCommand{
            .name="gpu-switch",.exact=false,.fn=[](eHyprCtlOutputFormat,std::string request) {
                std::istringstream words(request);std::string name,action;words>>name>>action;
#if GPU_SWITCH_DIAGNOSTICS
                if(action=="bridges")return GpuSwitch::bridgeStatus();
                if(action=="contexts")return GpuSwitch::transferContextStatus();
#endif
                if(action=="rendering"){std::string arguments;std::getline(words,arguments);return GpuSwitch::renderingOptimizationsControl(arguments);}
                if(action=="transfers")return GpuSwitch::clientTransferStatus();
                if(action=="enable") {enabled=true;lastMigrationError.clear();}
                else if(action=="disable") enabled=false;
                else if(action=="retry") lastMigrationError.clear();
#if GPU_SWITCH_DIAGNOSTICS
                else if(action=="select" && std::getenv("HYPR_GPU_SWITCH_TEST")) {
                    std::string role;words>>role;
                    if(role!="integrated" && role!="discrete") return std::string("error: use select integrated|discrete");
                    testRole=role;lastMigrationError.clear();
                }
#endif
                else if(!action.empty() && action!="status") return std::string("error: use status|enable|disable|retry|transfers|rendering");
                sample();return report.dump(2);
            }});
        if (!command) throw std::runtime_error("Cannot register gpu-switch status command");
        timer=makeShared<CEventLoopTimer>(std::chrono::milliseconds(250),[](SP<CEventLoopTimer> self,void*) {
            sample(false);
            if(enabled && !migrationIdle) {idleDeferrals=0;migrationIdle=wl_event_loop_add_idle(g_pCompositor->m_wlEventLoop,runMigrationIdle,nullptr);}
            self->updateTimeout(std::chrono::milliseconds(250));
        },nullptr);
        g_pEventLoopManager->addTimer(timer);
    } catch (...) { cleanup();throw; }
    return {"hyprland-gpu-switch","Transactional GPU migration through stock Hyprland internals (experimental)","daniel",GPU_SWITCH_VERSION};
}
APICALL EXPORT void PLUGIN_EXIT() { cleanup(); }
