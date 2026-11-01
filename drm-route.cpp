#include <bits/stdc++.h>
#define private public
#define protected public
#include <aquamarine/backend/DRM.hpp>
#include <aquamarine/backend/Backend.hpp>
#include <aquamarine/backend/Session.hpp>
#include <aquamarine/allocator/Swapchain.hpp>
#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/output/Monitor.hpp>
#include <hyprland/src/state/MonitorState.hpp>
#include "vendor/aquamarine/Renderer.hpp"
#undef protected
#undef private
#include "drm-route.hpp"
#include "aq-transfer.hpp"
#include "buffer-compat.hpp"
#include "route-plan.hpp"
#include "route-lifetime.hpp"
#include "native.hpp"
#include "probe.hpp"
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <poll.h>
using namespace Hyprutils::Memory;
using namespace Aquamarine;
using namespace GpuSwitch;
static std::string identity(int fd) {
    struct stat st{};if(fstat(fd,&st)!=0)return {};
    return deviceId(std::format("/sys/dev/char/{}:{}/device",major(st.st_rdev),minor(st.st_rdev)));
}
static void checkScanout(const SP<CSwapchain>& chain,const SP<IOutput>& output) {
    std::vector<OutputFormat> formats;
    for(const auto& f:output->getRenderFormats())formats.push_back({f.drmFormat,f.modifiers});
    for(const auto& b:chain->buffers) {
        auto a=b->dmabuf();
        if(!a.success || !outputAccepts(a.format,a.modifier,formats))
            throw std::runtime_error(std::format("Output {} cannot scan out actual format/modifier 0x{:x}/0x{:x}",output->name,a.format,a.modifier));
    }
}
// Avoid asking an incompatible driver to import the foreign modifier at all.
// Aquamarine's existing blit then reads on the source GPU and uploads locally.
static void prepareCopy(const SP<CSwapchain>& render,const SP<CDRMBackend>& destination) {
    for(const auto& buffer:render->buffers)prepareTransferInput(buffer,destination->rendererState.renderer);
}
struct DrmRoutes::State {
    struct Node {SP<CDRMBackend> backend;std::string id;};
    struct Output {
        RouteMonitorRef<Monitor::CMonitor> monitor;
        SP<CDRMBackend> backend;
        SP<CSwapchain> render,scanout;
        SP<CDRMOutput> output;
    };
    SP<CDRMBackend> root;
    std::vector<Node> nodes;
    std::vector<Output> outputs;
    std::vector<SP<IBackendImplementation>> order;
    bool active=false;
    void stageOutput(Monitor::CMonitor* monitor,Output& result) {
        auto output=dynamicPointerCast<CDRMOutput>(monitor->m_output);
        if(!output)throw std::runtime_error("Only native DRM outputs can migrate");
        auto backend=dynamicPointerCast<CDRMBackend>(output->getBackend());
        if(!backend || !backend->rendererState.renderer || !backend->rendererState.allocator)
            throw std::runtime_error("Output copy resources are not initialized");
        auto options=output->swapchain->currentOptions();
        options.multigpu=false;
        options.scanout=backend==root;
        options.scanoutOutput=backend==root?WP<IOutput>{output}:WP<IOutput>{};
        auto render=CSwapchain::create(root->rendererState.allocator,root);
        if(!render || !render->reconfigure(options))throw std::runtime_error("Cannot allocate GPU-local render buffers");
        SP<CSwapchain> scanout;
        if(backend==root)checkScanout(render,output);
        else {
            scanout=CSwapchain::create(backend->rendererState.allocator,backend);
            options.scanout=true;options.scanoutOutput=output;
            if(!scanout || !scanout->reconfigure(options))throw std::runtime_error("Cannot allocate GPU-local scanout buffers");
            checkScanout(scanout,output);prepareCopy(render,backend);
        }
        result={monitor->m_self,backend,render,scanout,output};
    }
};
DrmRoutes::DrmRoutes(const std::string& target):state_(std::make_unique<State>()) {
    Native::CurrentGuard restore;
    auto& s=*state_;
    auto backend=g_pCompositor->m_aqBackend;
    s.order=backend->implementations;
    std::vector<std::string> ids;
    for(const auto& impl:s.order) if(auto drm=dynamicPointerCast<CDRMBackend>(impl)) {
        const auto id=identity(drm->gpu->fd);
        if(id.empty())throw std::runtime_error("A DRM GPU disappeared during preparation");
        ids.push_back(id);s.nodes.push_back({drm,id});
        if(id==target)s.root=drm;
    }
    const auto plan=planRoutes(ids,target);(void)plan;
    if(!s.root)throw std::runtime_error("Selected GPU has no DRM backend");
    for(auto& node:s.nodes) {
        if(!node.backend->rendererState.renderer || !node.backend->rendererState.allocator)
            if(!node.backend->initMgpu())throw std::runtime_error("Cannot initialize GPU-local transfer resources");
        if(!node.backend->rendererState.allocator)throw std::runtime_error("GPU-local allocator unavailable");
        recoverTransferRenderer(backend,node.backend->gpu->renderNodeFd>=0?node.backend->gpu->renderNodeFd:node.backend->gpu->fd,node.backend->rendererState.renderer);
    }
    std::stable_sort(s.order.begin(),s.order.end(),[&](const auto& a,const auto& b){return a==s.root && b!=s.root;});
    for(const auto& mon : ::State::monitorState()->monitors()) {
        if(!mon->m_output || !mon->m_enabled || mon->m_output->swapchain->currentOptions().length==0)continue;
        State::Output o;s.stageOutput(mon.get(),o);s.outputs.push_back(std::move(o));
    }
}
DrmRoutes::~DrmRoutes()=default;
SP<IAllocator> DrmRoutes::allocator() const {return state_->root->rendererState.allocator;}
std::vector<SP<IBuffer>> DrmRoutes::renderBuffers() const {
    std::vector<SP<IBuffer>> buffers;
    for(auto& o:state_->outputs)for(auto& b:o.render->buffers)buffers.push_back(b);
    return buffers;
}
void DrmRoutes::validateTransfers() {
    auto& s=*state_;
    for(auto& o:s.outputs) {
        if(!o.scanout)continue;
        auto from=o.render->buffers.front(),to=o.scanout->buffers.front();
        const auto result=o.backend->rendererState.renderer->blit(from,to,s.root->rendererState.renderer,-1);
        if(!result.success)throw std::runtime_error("Preflight GPU-local scanout transfer failed");
        if(result.syncFD) {
            pollfd wait{*result.syncFD,POLLIN,0};int rc;
            do{rc=poll(&wait,1,750);}while(rc<0 && errno==EINTR);
            if(rc<=0 || !(wait.revents&POLLIN) || (wait.revents&(POLLERR|POLLNVAL)))throw std::runtime_error("GPU transfer fence failed or timed out");
        } else {
            CEglContextGuard context(*o.backend->rendererState.renderer);Native::waitIdle();
        }
    }
}
void DrmRoutes::commit() {
    auto& s=*state_;
    for(auto& n:s.nodes)n.backend->primary=n.backend==s.root?WP<CDRMBackend>{}:WP<CDRMBackend>{s.root};
    g_pCompositor->m_aqBackend->implementations.swap(s.order);
    g_pCompositor->m_aqBackend->primaryAllocator=s.root->rendererState.allocator;
    for(auto& o:s.outputs) {
        o.output->swapchain=o.render;
        o.output->mgpu.swapchain=o.scanout;
        o.output->mgpu.cursorSwapchain.reset();
        o.output->state->setBuffer(nullptr);o.output->state->resetExplicitFences();
        if(auto monitor=o.monitor.lock())monitor->m_inFence.reset();
    }
    s.active=true;
}
void DrmRoutes::retireDisconnected() {
    retireDisconnectedRoutes(state_->outputs);
}
void DrmRoutes::update(Monitor::CMonitor* monitor) {
    auto& s=*state_;
    if(!s.active || !monitor || !monitor->m_output || !monitor->m_enabled)return;
    retireDisconnected();
    auto it=std::ranges::find_if(s.outputs,[&](const auto& o){return o.monitor.get()==monitor;});
    if(it==s.outputs.end()) {
        auto backend=dynamicPointerCast<CDRMBackend>(monitor->m_output->getBackend());
        if(!backend)throw std::runtime_error("Non-DRM output added while migration routing is active");
        if(!backend->rendererState.renderer || !backend->rendererState.allocator)
            if(!backend->initMgpu())throw std::runtime_error("Cannot initialize hotplug transfer resources");
        recoverTransferRenderer(g_pCompositor->m_aqBackend,backend->gpu->renderNodeFd>=0?backend->gpu->renderNodeFd:backend->gpu->fd,backend->rendererState.renderer);
        State::Output o;s.stageOutput(monitor,o);o.output->swapchain=o.render;o.output->mgpu.swapchain=o.scanout;
        s.outputs.push_back(std::move(o));return;
    }
    if(!it->scanout) {checkScanout(monitor->m_output->swapchain,monitor->m_output);return;}
    auto options=monitor->m_output->swapchain->currentOptions();options.scanout=true;options.multigpu=false;options.scanoutOutput=monitor->m_output;
    if(!it->scanout->reconfigure(options))throw std::runtime_error("Cannot resize scanout copy buffers");
    checkScanout(it->scanout,monitor->m_output);prepareCopy(monitor->m_output->swapchain,it->backend);
    monitor->m_output->swapchain->options.multigpu=false;
    it->output->mgpu.swapchain=it->scanout;
}

void DrmRoutes::beforeCommit(Monitor::CMonitor* monitor) {
    if(!monitor || !monitor->m_output)return;
    auto backend=dynamicPointerCast<CDRMBackend>(monitor->m_output->getBackend());
    if(backend==state_->root)return;
    const auto& pending=monitor->m_output->state->state();
    if(pending.explicitInFence>=0) {
        pollfd wait{pending.explicitInFence,POLLIN,0};int result;
        do{result=poll(&wait,1,750);}while(result<0 && errno==EINTR);
        if(result<=0 || !(wait.revents&POLLIN) || (wait.revents&(POLLERR|POLLNVAL)))
            throw std::runtime_error("Source render fence did not signal before cross-GPU copy");
    } else Native::waitIdle();
}

int DrmRoutes::primaryFD() const {return state_->root->gpu->fd;}

#if GPU_SWITCH_DIAGNOSTICS
std::string GpuSwitch::transferContextStatus() {
    auto result=Json::array();
    for(const auto& impl:g_pCompositor->m_aqBackend->getImplementations())if(auto backend=dynamicPointerCast<CDRMBackend>(impl)) {
        auto renderer=backend->rendererState.renderer;
        result.push_back(Json{{"device",identity(backend->gpu->fd)},{"present",bool(renderer)},{"valid",transferRendererValid(renderer)}});
    }
    return result.dump();
}
#endif
