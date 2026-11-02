#define WLR_USE_UNSTABLE

#include <bits/stdc++.h>

// Match the private-access convention used by the existing local plugins.
// No uprobes or OA counters. Optional capture pacing hooks share() only.
#define private public
#define protected public
#include <aquamarine/backend/DRM.hpp>
#include <hyprland/src/render/OpenGL.hpp>
#include <hyprland/src/render/pass/Pass.hpp>
#include <hyprland/src/protocols/LinuxDMABUF.hpp>
#include <hyprland/src/protocols/types/DMABuffer.hpp>
#include <hyprland/src/render/gl/GLTexture.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/output/Monitor.hpp>
#include <hyprland/src/output/MonitorResources.hpp>
#include <hyprland/src/managers/screenshare/ScreenshareManager.hpp>
#undef protected
#undef private

#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/desktop/view/window/WindowMetadata.hpp>
#include <hyprland/src/config/ConfigValue.hpp>
#include <hyprland/src/ipc/s1/S1.hpp>
#include <hyprland/src/debug/log/Logger.hpp>
#include <hyprland/src/desktop/view/WLSurface.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/output/Monitor.hpp>
#include <hyprland/src/pointer/PointerManager.hpp>
#include <hyprland/src/plugins/PluginAPI.hpp>
#include "function-hooks.hpp"
#include "client-transfer.hpp"
#include "rendering-optimizations.hpp"
#include <hyprland/src/plugins/PluginSystem.hpp>
#include <dlfcn.h>
#include "version.hpp"
#include "json.hpp"
#include <hyprland/src/protocols/ColorManagement.hpp>
#include <hyprland/src/protocols/core/Compositor.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/render/Shader.hpp>
#include <hyprland/src/render/SyncFDManager.hpp>
#include <hyprland/src/render/pass/SurfacePassElement.hpp>
#include <hyprland/src/state/MonitorState.hpp>
#include <hyprland/src/protocols/DRMSyncobj.hpp>
#include <hyprland/src/managers/eventLoop/EventLoopManager.hpp>
#include <hyprland/src/managers/eventLoop/EventLoopTimer.hpp>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <linux/dma-buf.h>




#include "transfer/client.hpp"
#include "transfer/shared_output.hpp"
#include "transfer/queued_state.hpp"
#include "transfer/formats.hpp"
#include "transfer/session-pool.hpp"
#include "transfer/retained-source.hpp"
#include "transfer/idle-policy.hpp"
#include "probe.hpp"

static HANDLE g_handle=nullptr;
static CFunctionHook* g_passHook=nullptr;
struct SState {
    CHyprSignalListener closeListener;
    uint64_t subpassFrames=0;
    bool enabled=true;
    SP<CEventLoopTimer> offloadTimer;
    struct SNV;
    struct SRetainedInput {
        SNV* owner=nullptr;
        Offload::FD fence;
        CHLBufferReference input;
        wl_event_source* source = nullptr;
        SRetainedInput(SNV* session,Offload::FD&& fd, const CHLBufferReference& buffer) : owner(session),fence(std::move(fd)), input(buffer) {}
        ~SRetainedInput() { if (source) wl_event_source_remove(source); }
    };
    std::vector<UP<SRetainedInput>> retainedInputs;
    struct SProducerRecord { WP<IHLBuffer> buffer; Offload::GPUDevice device; };
    struct SProducerWatch { WP<CWLSurfaceResource> surface; CHyprSignalListener queued; };
    std::map<IHLBuffer*,SProducerRecord> bufferProducers;
    std::map<CWLSurfaceResource*,UP<SProducerWatch>> producerWatches;
    CHyprSignalListener newProducerSurface;
    bool producerTracking=false;
    struct SNVSlot {
        SP<Render::ITexture> texture;
        std::unique_ptr<Offload::SharedOutput> externalOutput;
        Offload::FD readComplete;
        Offload::FD outputFD, conversionComplete;
        uint32_t width = 0, height = 0, stride = 0, offset = 0, format = DRM_FORMAT_XBGR2101010;
        bool awaitingFence = false;
        bool materializationPending = true;
    };
    struct SNV {
        WP<CWLSurfaceResource> identitySurface;
        PHLWINDOWREF identityWindow;
        Offload::Client client;
        std::array<SNVSlot, 3> slots;
        PHLWINDOWREF owner;
        pid_t ownerPID = 0;
        bool flag = false; // Eligible cross-GPU route; retained status key.
        std::optional<Offload::GPUDevice> sourceGPU, rendererGPU;
        bool enabled = true;
        bool stopRequested = false;
        std::string lastStopReason;
        bool eager = true;
        bool earlyRelease = false;
        unsigned profileRemaining = 0;
        uint64_t eagerFrames = 0;
        unsigned nextSlot = 0;
        uint64_t frames = 0, reused = 0, windowFrames = 0, captureFrames = 0;
#if GPU_SWITCH_DIAGNOSTICS
        uint64_t colorConfigurations = 0, materializations = 0, externalCopies = 0;
#endif
        WP<CWLSurfaceResource> sourceSurface;
        CHyprSignalListener sourceCommit, sourceDestroy, sourceQueued;
        uint64_t generation = 0, convertedGeneration = UINT64_MAX, convertedSource = 0;
        std::array<float, 16> convertedColor{};
        std::optional<unsigned> cachedSlot;
        Vector2D pendingSize;
        std::chrono::steady_clock::time_point sizeSince{};
        std::string reason = "disabled";
        std::string route = "idle";
        std::chrono::steady_clock::time_point seen{}, lastDraw{}, lastCommit{};
    };
    Offload::SessionPool<SNV> sessions{128};
    SNV idle;
    SNV* active=nullptr;
    SNV* last=nullptr;
    std::string reason="idle";
    struct SVramInput {
        SNV* session=nullptr;
        bool feedback = false, protectNew = false, insideBind = false, nativeLayout = false;
        bool automatic = true;
        std::chrono::steady_clock::time_point retryAfter{};
        pid_t pid = 0;
        dev_t device = 0;
        WP<CWLSurfaceResource> surface;
        PHLWINDOWREF owner;
        SP<Render::ITexture> safeTexture;
        uint64_t protectedTextures = 0, fallbackBinds = 0, blankBinds = 0, feedbackSent = 0;
        struct SProxy {
            SNV* session=nullptr;
            WP<Render::ITexture> texture;
            NColorManagement::PImageDescription savedDescription;
            std::optional<decltype(g_pHyprRenderer->context().m_cmSettingsCache)> savedCMCache;
            std::optional<unsigned> slot;
            bool bound = false, nativeLayout = false;
            std::string layoutReason;
            std::unique_ptr<Offload::RetainedSource> source;
        };
        std::map<Render::GL::CGLTexture*, SProxy> proxies;
        std::string layoutDiagnostic;
    } vram;
};
static UP<SState> g_state;
static SState::SNV& currentNV(){return g_state->active?*g_state->active:g_state->idle;}
struct SessionScope {
    SState::SNV* previous;
    explicit SessionScope(SState::SNV* session):previous(g_state->active){g_state->active=session;}
    ~SessionScope(){g_state->active=previous;}
};
struct PreparedTexture {SState::SNV* session;unsigned slot;};
static SState::SNV* findSession(SP<CWLSurfaceResource> surface,PHLWINDOW window){
    auto* session=g_state->sessions.acquire([&](const auto& candidate){return candidate.identitySurface.lock()==surface && candidate.identityWindow.lock()==window;},[&]{
        auto created=std::make_unique<SState::SNV>();created->identitySurface=surface;created->identityWindow=window;return created;
    });
    if(session)g_state->last=session;
    return session;
}
static void setVramInputInactiveForSession(SState::SNV*);
static void retireVramSession();

static void maintainOffload();
static void wrapOffloadSurfaces(Render::CRenderContext& ctx, bool subpass=false);
static void unwrapOffloadSurfaces(Render::CRenderContext& ctx, Render::CRenderPass* pass=nullptr);
static void preconvertCommittedFrame();
static Render::CRenderContext* g_transferContext=nullptr;
static Render::CRenderContext& transferContext() { return g_transferContext?*g_transferContext:g_pHyprRenderer->context(); }
static Aquamarine::SDMABUFAttrs transferSourceAttributes(const SP<IHLBuffer>& buffer);
static bool isProtectedInput(Render::ITexture* texture);
static bool isNativeInput(Render::ITexture* texture);
static std::string describeVramInput(Render::ITexture* texture);
static void retainCaptureInput(const Hyprutils::OS::CFileDescriptor& fence, const CHLBufferReference& input);
static void requestOffloadStop() {
    if (!g_state) return;
    currentNV().lastStopReason = currentNV().reason;
    currentNV().stopRequested = true;
    if (g_state->offloadTimer) g_state->offloadTimer->updateTimeout(std::chrono::milliseconds(1));
}

static NColorManagement::PImageDescription sourceDescription(CSurfacePassElement& element) {
    if (element.m_data.texture && !isProtectedInput(element.m_data.texture.get()) && element.m_data.texture->m_imageDescription)
        return element.m_data.texture->m_imageDescription;
    const auto& surface = element.m_data.surface;
    if (surface && surface->m_colorManagement.valid() && surface->m_colorManagement->hasImageDescription())
        return NColorManagement::CImageDescription::from(surface->m_colorManagement->imageDescription());
    return NColorManagement::getDefaultImageDescription();
}

static bool workerCanStayWarm(const SState::SNV& nv,PHLWINDOW owner) {
    if(!nv.sourceGPU)return false;
    bool displayActive=false,sourceHasActiveOutput=false;
    for(const auto& monitor:State::monitorState()->monitors()) {
        if(!monitor || !monitor->m_enabled || !monitor->m_dpmsStatus || !monitor->m_output)continue;
        displayActive=true;
        const auto output=dynamicPointerCast<Aquamarine::CDRMOutput>(monitor->m_output);
        const auto backend=output?dynamicPointerCast<Aquamarine::CDRMBackend>(output->getBackend()):nullptr;
        if(!backend || !backend->gpu)continue;
        struct stat info{};
        if(!fstat(backend->gpu->fd,&info) && Offload::deviceID(info.st_rdev)==nv.sourceGPU->id)sourceHasActiveOutput=true;
    }
    return Offload::keepWorkerWarm(owner && owner->mapped(),g_state->enabled && nv.enabled,
        displayActive,GpuSwitch::knownGpuRole(nv.sourceGPU->id)==GpuSwitch::Role::Integrated,sourceHasActiveOutput);
}

static void maintainOffload() {
    if (!g_state) return;
    auto& nv = currentNV();
    nv.client.reap();
    const auto owner = nv.owner.lock();
    const auto now = std::chrono::steady_clock::now();
    const bool haveTextures = std::ranges::any_of(nv.slots, [](const auto& slot) { return bool(slot.texture) || slot.outputFD.value >= 0 || slot.conversionComplete.value >= 0; });
    const bool idle = !g_state->enabled || !nv.enabled || !owner || !owner->mapped() ||
        (!workerCanStayWarm(nv,owner) && now - nv.lastDraw > std::chrono::milliseconds(500));
    if (!nv.stopRequested && !(idle && nv.client.running()) && !(haveTextures && !nv.client.running())) {
        if (nv.client.running()) nv.client.checkReady();
        return;
    }
    if(!nv.stopRequested)nv.lastStopReason=idle?"surface inactive or source output detached":"worker stopped";
    nv.stopRequested = true;
    setVramInputInactiveForSession(&nv);
    for (const auto& slot : nv.slots) {
        if (slot.awaitingFence) return;
        if (slot.readComplete.value >= 0) {
            pollfd p{slot.readComplete.value, POLLIN, 0};
            if (poll(&p, 1, 0) == 0) return;
        }
    }
    if (haveTextures && Render::GL::g_pHyprOpenGL) Render::GL::g_pHyprOpenGL->makeEGLCurrent();
    if(g_state->vram.session==&nv)g_state->vram.safeTexture.reset();
    for (auto& slot : nv.slots) { slot.texture.reset(); slot.externalOutput.reset(); slot.readComplete.reset(); slot.outputFD.reset(); slot.conversionComplete.reset(); slot.awaitingFence = false; }
    nv.client.stop();
    if (!nv.client.running()) {
        nv.stopRequested = false; nv.owner.reset(); nv.ownerPID = 0; nv.flag = false; nv.nextSlot = 0;
        if(g_state->vram.session!=&nv){nv.sourceGPU.reset();nv.rendererGPU.reset();}
        nv.sourceCommit.reset(); nv.sourceDestroy.reset(); nv.sourceQueued.reset(); nv.sourceSurface.reset(); nv.cachedSlot.reset();
        nv.convertedGeneration = UINT64_MAX; nv.route = "idle";
        if (nv.reason == "converted" || nv.reason == "starting") nv.reason = "idle";
    }
}

// Read fence provenance while the client's queued state is still waiting.
// Completed sync files may already contain only a detached-driver stub.
static void observeProducerSurface(SP<CWLSurfaceResource> surface) {
    if(!g_state || !surface)return;
    std::erase_if(g_state->producerWatches,[](const auto& entry){return entry.second->surface.expired();});
    if(g_state->producerWatches.contains(surface.get()))return;
    auto watch=makeUnique<SState::SProducerWatch>();watch->surface=surface;
    watch->queued=surface->m_events.stateCommit2.listen([weak=WP<CWLSurfaceResource>{surface}](WP<SSurfaceState> queued) {
        try {
        const auto surface=weak.lock();auto* state=Offload::borrowQueuedState(queued);
        if(!g_state || !surface || !state || !state->buffer)return;
        const auto attrs=state->buffer->dmabuf();
        if(!attrs.success || attrs.planes!=1 || !Offload::transportFormat(attrs.format))return;
        const auto known=g_state->bufferProducers.find(state->buffer.m_buffer.get());
        if(known!=g_state->bufferProducers.end() && known->second.buffer.lock()==state->buffer.m_buffer)return;
        Offload::FD acquire;if(state->acquire){auto fd=state->acquire.exportAsFD();if(fd.isValid())acquire.reset(fd.take());}
        pid_t pid=0;wl_client_get_credentials(surface->client(),&pid,nullptr,nullptr);
        auto producer=Offload::producerDevice(attrs.fds[0],pid,acquire.value);
        if(!producer)for(const auto& fd:state->buffer->m_syncFds)if((producer=Offload::fenceProducer(fd.get(),Offload::gpuDevices())))break;
        if(!producer)return;
        std::erase_if(g_state->bufferProducers,[](const auto& entry){return entry.second.buffer.expired();});
        if(g_state->bufferProducers.size()>=512)g_state->bufferProducers.erase(g_state->bufferProducers.begin());
        g_state->bufferProducers[state->buffer.m_buffer.get()]={state->buffer.m_buffer,*producer};
        }catch(...){if(g_state)currentNV().reason="producer tracking unavailable";}
    });
    g_state->producerWatches.emplace(surface.get(),std::move(watch));
}
static bool installProducerTracking() {
    if(!g_state)return false;
    if(g_state->producerTracking)return true;
    if(!PROTO::compositor)return false;
    g_state->newProducerSurface=PROTO::compositor->m_events.newSurface.listen([](SP<CWLSurfaceResource> surface){observeProducerSurface(surface);});
    PROTO::compositor->forEachSurface([](SP<CWLSurfaceResource> surface){observeProducerSurface(surface);});
    g_state->producerTracking=true;return true;
}

static Aquamarine::SDMABUFAttrs transferSourceAttributes(const SP<IHLBuffer>& buffer) {
    if(buffer && buffer->m_texture) {
        const auto* texture=dynamic_cast<Render::GL::CGLTexture*>(buffer->m_texture.get());
        const auto found=g_state->vram.proxies.find(const_cast<Render::GL::CGLTexture*>(texture));
        if(found!=g_state->vram.proxies.end() && found->second.texture.lock()==buffer->m_texture && found->second.source)
            return found->second.source->attrs;
    }
    return buffer?buffer->dmabuf():Aquamarine::SDMABUFAttrs{};
}

static bool materializeOffload(SState::SNVSlot& slot, NColorManagement::PImageDescription packed, Hyprutils::Math::eTransform transform) {
    auto fail = [&](const std::string& reason) { currentNV().reason = reason; currentNV().client.fail(reason); requestOffloadStop(); return false; };
    if (!slot.texture) {
        slot.materializationPending=true;
        Aquamarine::SDMABUFAttrs output{}; output.success = true; output.size = {int(slot.width), int(slot.height)}; output.planes = 1;
        output.format = slot.format; output.modifier = DRM_FORMAT_MOD_LINEAR;
        output.fds[0] = slot.outputFD.value; output.strides[0] = slot.stride; output.offsets[0] = slot.offset;
        if(Offload::externalOutput(Render::GL::g_pHyprOpenGL->m_eglDisplay,output.format)) {
            try {
                slot.externalOutput=std::make_unique<Offload::SharedOutput>();
                slot.externalOutput->import(slot.width,slot.height,output.format,slot.outputFD.value,slot.offset,slot.stride);
                const bool opaque=Offload::transportFormat(output.format).value().opaque;
                slot.texture=g_pHyprRenderer->createTexture(opaque);
                auto* texture=dynamic_cast<Render::GL::CGLTexture*>(slot.texture.get());
                if(!texture)throw std::runtime_error("shared output requires GL texture");
                texture->allocate(output.size,output.format);texture->m_type=opaque?Render::TEXTURE_RGBX:Render::TEXTURE_RGBA;texture->m_isSynchronous=false;
                GLint binding=0;glGetIntegerv(GL_TEXTURE_BINDING_2D,&binding);glBindTexture(GL_TEXTURE_2D,texture->m_texID);
                glTexStorage2D(GL_TEXTURE_2D,1,Offload::transportFormat(slot.format).value().gl,slot.width,slot.height);glBindTexture(GL_TEXTURE_2D,binding);
                if(glGetError()!=GL_NO_ERROR)throw std::runtime_error("shared output target allocation failed");
            }catch(const std::exception& e){slot.texture.reset();slot.externalOutput.reset();return fail(e.what());}
        }else slot.texture = g_pHyprRenderer->createTexture(output, true);
        if (!slot.texture || !slot.texture->ok()) return fail("cannot import shared RAM output");
    }
    if (slot.conversionComplete.value >= 0) {
        auto create = reinterpret_cast<PFNEGLCREATESYNCKHRPROC>(eglGetProcAddress("eglCreateSyncKHR"));
        auto wait = reinterpret_cast<PFNEGLWAITSYNCKHRPROC>(eglGetProcAddress("eglWaitSyncKHR"));
        auto destroy = reinterpret_cast<PFNEGLDESTROYSYNCKHRPROC>(eglGetProcAddress("eglDestroySyncKHR"));
        if (!create || !wait || !destroy) return fail("native fence operations unavailable");
        const EGLint syncAttrs[]{EGL_SYNC_NATIVE_FENCE_FD_ANDROID, slot.conversionComplete.value, EGL_NONE};
        const auto display = Render::GL::g_pHyprOpenGL->m_eglDisplay;
        auto sync = create(display, EGL_SYNC_NATIVE_FENCE_ANDROID, syncAttrs);
        if (sync == EGL_NO_SYNC_KHR) return fail("cannot import conversion fence");
        slot.conversionComplete.release();
        auto waited = wait(display, sync, 0); destroy(display, sync);
        if (!waited) return fail("cannot wait for conversion");
    }
    glMemoryBarrier(GL_TEXTURE_FETCH_BARRIER_BIT);
    if(slot.externalOutput && (slot.materializationPending || !slot.externalOutput->current())) {
        try {
            if(!slot.externalOutput->current())slot.externalOutput->import(slot.width,slot.height,slot.format,slot.outputFD.value,slot.offset,slot.stride);
            slot.externalOutput->copy(dynamic_cast<Render::GL::CGLTexture*>(slot.texture.get())->m_texID);
#if GPU_SWITCH_DIAGNOSTICS
            ++currentNV().externalCopies;
#endif
        }catch(const std::exception& e){return fail(e.what());}
    }
#if GPU_SWITCH_DIAGNOSTICS
    if(slot.materializationPending)++currentNV().materializations;
#endif
    slot.materializationPending=false;
    slot.texture->m_imageDescription = packed;
    slot.texture->m_transform = transform;
    if (g_state->vram.session==&currentNV() && g_state->vram.pid > 0 && g_state->vram.pid == currentNV().ownerPID) g_state->vram.safeTexture = slot.texture;
    return true;
}

static std::optional<PreparedTexture> prepareOffloadTexture(Render::CRenderContext& ctx, CSurfacePassElement& element, PHLMONITOR monitor, bool fullscreen, bool submissionOnly = false) {
    auto& data = element.m_data;
    if (!g_state->enabled || !monitor || !data.texture || !data.texture->ok() || !data.pWindow || !data.surface || !data.surface->m_current.buffer || !g_pHyprRenderer->explicitSyncSupported()) return std::nullopt;
    static auto cmEnabled = CConfigValue<Config::INTEGER>("render:cm_enabled");
    const bool hdrConversion=*cmEnabled && !monitor->doesNoShaderCM() && monitor->m_imageDescription && monitor->m_imageDescription->value().transferFunction==NColorManagement::CM_TRANSFER_FUNCTION_ST2084_PQ;
    const auto pid = data.pWindow->backend().pid();
    auto attrs = transferSourceAttributes(data.surface->m_current.buffer.m_buffer);
    if(!attrs.success)return std::nullopt;
    pid_t clientPID=0;wl_client_get_credentials(data.surface->client(),&clientPID,nullptr,nullptr);
    std::optional<Offload::GPUDevice> producer;
    auto cached=g_state->bufferProducers.find(data.surface->m_current.buffer.m_buffer.get());
    if(cached!=g_state->bufferProducers.end() && cached->second.buffer.lock()==data.surface->m_current.buffer.m_buffer)producer=cached->second.device;
    if(!producer && data.surface->m_current.buffer->m_texture) {
        const auto* texture=dynamic_cast<Render::GL::CGLTexture*>(data.surface->m_current.buffer->m_texture.get());
        const auto proxy=g_state->vram.proxies.find(const_cast<Render::GL::CGLTexture*>(texture));
        // Native allocations may lose generic fence/exporter provenance. Their
        // construction was already scoped to this producer and inspected by
        // the worker on that exact GPU. Retain that identity with the texture.
        if(proxy!=g_state->vram.proxies.end() && proxy->second.texture.lock()==data.surface->m_current.buffer->m_texture && proxy->second.nativeLayout) {
            const auto* origin=proxy->second.session;
            if(origin && origin->identitySurface.lock()==data.surface && origin->identityWindow.lock()==data.pWindow)
                producer=origin->sourceGPU;
        }
    }
    if(!producer) {
        Offload::FD provenanceFence;
        if(data.surface->m_current.acquire) {
            auto exported=data.surface->m_current.acquire.exportAsFD();
            if(exported.isValid())provenanceFence.reset(exported.take());
        }
        producer=Offload::producerDevice(attrs.fds[0],clientPID,provenanceFence.value);
    }
    if(producer && (cached==g_state->bufferProducers.end() || cached->second.buffer.lock()!=data.surface->m_current.buffer.m_buffer)) {
        std::erase_if(g_state->bufferProducers,[](const auto& entry){return entry.second.buffer.expired();});
        if(g_state->bufferProducers.size()>=512)g_state->bufferProducers.erase(g_state->bufferProducers.begin());
        g_state->bufferProducers[data.surface->m_current.buffer.m_buffer.get()]={data.surface->m_current.buffer.m_buffer,*producer};
    }
    const auto renderer=Offload::rendererDevice(g_pCompositor->m_drmRenderNode.fd);
    if(!producer || !renderer || producer->id==renderer->id) {
        g_state->reason=(!producer || !renderer)?"GPU provenance unavailable or ambiguous":"producer and compositor use the same GPU";
        return std::nullopt;
    }
    g_state->reason="eligible cross-GPU surface";
    auto* selected=findSession(data.surface,data.pWindow);
    if(!selected){g_state->reason="surface session limit reached";return std::nullopt;}
    SessionScope selectedScope(selected);
    auto& nv=*selected;
    if(!nv.enabled)return std::nullopt;
    if(auto* texture=dynamic_cast<Render::GL::CGLTexture*>(data.surface->m_current.buffer->m_texture.get()))
        if(auto found=g_state->vram.proxies.find(texture);found!=g_state->vram.proxies.end())found->second.session=&nv;
    if(nv.owner.lock()==data.pWindow && nv.client.running() && ((nv.sourceGPU && nv.sourceGPU->id!=producer->id) || (nv.rendererGPU && nv.rendererGPU->id!=renderer->id))) {
        nv.reason="GPU route changed";requestOffloadStop();return std::nullopt;
    }
    if (nv.owner.lock() != data.pWindow || nv.ownerPID != pid) {
        nv.owner = data.pWindow; nv.ownerPID = pid; nv.flag = true;
    }
    nv.sourceGPU=producer;nv.rendererGPU=renderer;nv.flag=true;
    if(attrs.planes!=1 || !Offload::transportFormat(attrs.format) || (fullscreen && attrs.size!=monitor->m_pixelSize)) {
        nv.reason="unsupported DMA-BUF layout";return std::nullopt;
    }
    const auto source=sourceDescription(element);
    using namespace NColorManagement;
    if(!source)return std::nullopt;
    const bool copyOnly=!hdrConversion || !Offload::transportFormat(attrs.format)->floating || source->value().transferFunction!=CM_TRANSFER_FUNCTION_EXT_LINEAR || attrs.modifier!=DRM_FORMAT_MOD_LINEAR || (attrs.format==DRM_FORMAT_ABGR16161616F && !data.texture->m_opaque) || source->value().icc.present;
    if(nv.client.running() && (nv.client.copyOnly!=copyOnly || (copyOnly && nv.client.transportPixelFormat!=attrs.format))){nv.reason="transport mode changed";requestOffloadStop();return std::nullopt;}
    // Native-format transport preserves pixels and their description. Its
    // worker ignores color uniforms, so only PQ conversion needs CM settings.
    auto packed=source;
    std::array<float,16> color{};
    if(!copyOnly) {
        auto packedData = source->value();
        packedData.transferFunction = CM_TRANSFER_FUNCTION_ST2084_PQ;
        packedData.transferFunctionPower = 1.0F;
        packedData.primariesNameSet = true;
        packedData.primariesNamed = CM_PRIMARIES_BT2020;
        packedData.primaries = NColorPrimaries::BT2020;
        packed = fullscreen ? monitor->m_imageDescription : CImageDescription::from(packedData);
        const auto settings = fullscreen ? g_pHyprRenderer->getCMSettings(ctx, source, packed, data.surface, true,
            monitor->m_sdrMinLuminance, monitor->m_sdrMaxLuminance, true) :
            g_pHyprRenderer->getCMSettings(ctx, source, packed, nullptr, false, -1, -1, false);
#if GPU_SWITCH_DIAGNOSTICS
        ++nv.colorConfigurations;
#endif
        if (settings.sourceTF != CM_TRANSFER_FUNCTION_EXT_LINEAR || settings.targetTF != CM_TRANSFER_FUNCTION_ST2084_PQ ||
            settings.needsTonemap || settings.needsSDRmod || settings.dstTFRange.max <= settings.dstTFRange.min) {
            nv.reason = "unsupported color transform"; return std::nullopt;
        }
        for (int column = 0; column < 3; ++column)
            for (int row = 0; row < 3; ++row) color[column * 4 + row] = settings.convertMatrix[row][column];
        color[12] = settings.srcTFRange.min; color[13] = settings.srcTFRange.max - settings.srcTFRange.min;
        color[14] = settings.dstTFRange.min; color[15] = settings.dstTFRange.max - settings.dstTFRange.min;
    }
    nv.seen = std::chrono::steady_clock::now();
    if (!submissionOnly) nv.lastDraw = nv.seen;
    if (nv.stopRequested) { maintainOffload(); return std::nullopt; }
    if (nv.pendingSize != attrs.size) {
        nv.pendingSize = attrs.size; nv.sizeSince = nv.seen;
    }
    if (nv.client.running() && (nv.client.width < unsigned(attrs.size.x) || nv.client.height < unsigned(attrs.size.y))) {
        nv.reason = "output exceeds reserved capacity"; requestOffloadStop(); return std::nullopt;
    }
    if (!nv.client.running()) {
        const auto running=std::count_if(g_state->sessions.begin(),g_state->sessions.end(),[](const auto& entry){return entry->client.running();});
        if(running>=16){nv.reason="worker limit reached";return std::nullopt;}
        if (nv.seen - nv.sizeSince < std::chrono::milliseconds(200)) { nv.reason = "waiting for stable buffer size"; return std::nullopt; }
        // Keep the same helper/storage across ordinary window-size changes.
        nv.client.copyOnly=copyOnly;nv.client.transportPixelFormat=attrs.format;
        nv.client.start(std::max(attrs.size.x, monitor->m_pixelSize.x),
            std::max(attrs.size.y, monitor->m_pixelSize.y), *producer);
        if (g_state->offloadTimer) g_state->offloadTimer->updateTimeout(std::chrono::milliseconds(250));
    }
    nv.client.checkReady();
    if (!nv.client.ready) { nv.reason = nv.client.reason; return std::nullopt; }
    const auto slotIndex = nv.nextSlot % nv.slots.size();
    auto& slot = nv.slots[slotIndex];
    if (slot.awaitingFence) { nv.reason = "waiting for destination release fence"; return std::nullopt; }
    struct stat inode{};
    if (fstat(attrs.fds[0], &inode)) { nv.reason = "cannot identify source buffer"; return std::nullopt; }
    // Pass wrappers may already have replaced data.texture with our PQ output.
    // Layout belongs to the source DMA-BUF, never to that temporary draw view.
    Offload::Packet job{};
    if(copyOnly)job.fdFlags|=Offload::COPY_ONLY;
    if (isNativeInput(data.surface->m_current.buffer->m_texture.get())) job.fdFlags |= Offload::NATIVE_INPUT;
    if (g_state->vram.feedback && g_state->vram.nativeLayout && !(job.fdFlags & Offload::NATIVE_INPUT))
        g_state->vram.layoutDiagnostic = std::format("{}x{} stride={} source={} pass={} current={} ", attrs.size.x, attrs.size.y, attrs.strides[0], job.source, (void*)data.texture.get(), (void*)data.surface->m_current.texture.get()) + describeVramInput(data.surface->m_current.buffer->m_texture.get());
    job.width = attrs.size.x; job.height = attrs.size.y; job.format = attrs.format;
    job.stride = attrs.strides[0]; job.offset = attrs.offsets[0]; job.modifier = attrs.modifier;
    job.source = uint64_t(inode.st_ino) ^ (uint64_t(inode.st_dev) << 32); job.slot = slotIndex;
    job.color = color;
    if (nv.sourceSurface.lock() != data.surface) {
        nv.sourceCommit.reset(); nv.sourceDestroy.reset(); nv.sourceQueued.reset(); nv.cachedSlot.reset();
        nv.sourceSurface = data.surface; ++nv.generation;
        nv.sourceCommit = data.surface->m_events.commit.listen([session=&nv] { if (g_state) { SessionScope scope(session);++currentNV().generation; currentNV().lastCommit = std::chrono::steady_clock::now(); preconvertCommittedFrame(); } });
        nv.sourceDestroy = data.surface->m_events.destroy.listen([session=&nv] { if (g_state) { SessionScope scope(session);currentNV().cachedSlot.reset(); requestOffloadStop(); } });
    }
    if (nv.cachedSlot && nv.convertedGeneration == nv.generation && nv.convertedSource == job.source && nv.convertedColor == job.color) {
        auto& cached = nv.slots[*nv.cachedSlot];
        if (!submissionOnly && !materializeOffload(cached, packed, data.texture->m_transform)) return std::nullopt;
        ++nv.reused; nv.reason = "reused committed frame";
        return PreparedTexture{&nv,*nv.cachedSlot};
    }
    Offload::FD acquire;
    if (data.surface->m_current.acquire) {
        auto fd = data.surface->m_current.acquire.exportAsFD();
        if (!fd.isValid()) { nv.reason = "cannot export client acquire fence"; return std::nullopt; }
        acquire.reset(fd.take());
    } else {
        dma_buf_export_sync_file exportSync{}; exportSync.flags = DMA_BUF_SYNC_READ; exportSync.fd = -1;
        if (ioctl(attrs.fds[0], DMA_BUF_IOCTL_EXPORT_SYNC_FILE, &exportSync)) {
            nv.reason = "cannot export implicit acquire fence"; return std::nullopt;
        }
        acquire.reset(exportSync.fd);
    }
    std::vector<int> descriptors{attrs.fds[0]};
    if (acquire.value >= 0) { job.fdFlags |= Offload::ACQUIRE; descriptors.push_back(acquire.value); }
    if (slot.readComplete.value >= 0) { job.fdFlags |= Offload::REUSE; descriptors.push_back(slot.readComplete.value); }
    const auto input = data.surface->m_current.buffer;
    if (nv.profileRemaining) { --nv.profileRemaining; job.fdFlags |= Offload::PROFILE; }
    auto retain = [input] {};
    Offload::Packet response; std::vector<Offload::FD> received;
    if (!nv.client.convert(job, descriptors, response, received, retain)) {
        nv.reason = nv.client.reason; requestOffloadStop(); return std::nullopt;
    }
    auto fail = [&](const std::string& reason) {
        nv.client.hold(retain); nv.client.fail(reason); nv.reason = reason; requestOffloadStop();
        return std::nullopt;
    };
    if (response.width != unsigned(attrs.size.x) || response.height != unsigned(attrs.size.y) || response.modifier != 0 ||
        response.format != (copyOnly?attrs.format:DRM_FORMAT_XBGR2101010) || response.stride < attrs.size.x * (copyOnly?Offload::transportFormat(attrs.format)->bytes:4))
        return fail("invalid output layout");
    if (slot.width != response.width || slot.height != response.height || slot.stride != response.stride || slot.offset != response.offset || slot.format!=response.format) {
        slot.texture.reset();slot.externalOutput.reset(); // Reimport at the new logical size.
    }
    slot.materializationPending=true;
    slot.outputFD = std::move(received[0]);
    slot.width = response.width; slot.height = response.height; slot.stride = response.stride; slot.offset = response.offset;slot.format=response.format;
    slot.conversionComplete.reset();
    if (response.fdFlags & Offload::COMPLETE) slot.conversionComplete = std::move(received[1]);
    // Source lifetime must cover conversion even when no monitor draws this
    // commit (occlusion/newer commit). No GL commands run from a commit callback.
    if (slot.conversionComplete.value >= 0) {
        Hyprutils::OS::CFileDescriptor completion{dup(slot.conversionComplete.value)};
        if (!completion.isValid()) return fail("cannot retain source completion fence");
        for (const auto& releaser : input->m_syncReleasers) releaser->addSyncFileFd(completion);
        retainCaptureInput(completion, input);
    }
    if (!submissionOnly && !materializeOffload(slot, packed, data.texture->m_transform)) return std::nullopt;
    nv.cachedSlot = slotIndex; nv.convertedGeneration = nv.generation;
    nv.convertedSource = job.source; nv.convertedColor = job.color;
    ++nv.nextSlot; ++nv.frames; nv.reason = "converted";
    return PreparedTexture{&nv,static_cast<unsigned>(slotIndex)};
}

#include "transfer/vram_input.hpp"

// Share migration's existing core hooks rather than registering a second hook
// on createTexture/bind, which stock Hyprland explicitly rejects.
SP<Render::ITexture> GpuSwitch::createClientTransferTexture(void* self,const Aquamarine::SDMABUFAttrs& attrs,bool opaque,TransferTextureFactory original,bool& handled) {
    handled=g_state && g_vramConstructing && attrs.planes==1 && attrs.modifier==DRM_FORMAT_MOD_LINEAR &&
        Offload::transportFormat(attrs.format).has_value();
    if(!handled)return {};
    g_vramCreateOriginal=original;
    return vramCreateTexture(self,attrs,opaque);
}
bool GpuSwitch::bindClientTransferTexture(Render::GL::CGLTexture* texture,TransferTextureBind original) {
    if(!g_state || !isProtectedInput(texture))return false;
    g_vramBindOriginal=original;
    vramBind(texture);
    return true;
}

static void preconvertCommittedFrame() {
    if (!g_state) return;
    auto& nv = currentNV();
    if (!nv.eager || !g_state->enabled || !nv.enabled || nv.stopRequested || !nv.client.ready ||
        std::chrono::steady_clock::now() - nv.lastDraw > std::chrono::milliseconds(250)) return;
    const auto owner = nv.owner.lock();
    const auto surface = nv.sourceSurface.lock();
    if (!owner || !owner->mapped() || !surface || !surface->m_current.buffer || !surface->m_current.texture) return;
    const auto monitor = owner->m_monitor.lock();
    if (!monitor) return;
    CSurfacePassElement::SRenderData data{}; data.surface = surface; data.texture = surface->m_current.texture; data.pWindow = owner;
    CSurfacePassElement element{data};
    const auto before = nv.frames;
    prepareOffloadTexture(g_pHyprRenderer->context(), element, monitor, false, true);
    if (nv.frames > before) ++nv.eagerFrames;
}

// Own and cancel capture input waiters explicitly. Event-loop callbacks whose
// code lives in the plugin must never survive PLUGIN_EXIT/dlclose.
static void retainCaptureInput(const Hyprutils::OS::CFileDescriptor& fence, const CHLBufferReference& input) {
    auto duplicate = fence.duplicate();
    pollfd p{duplicate.get(), POLLIN, 0};
    if (poll(&p, 1, 0) > 0) return;
    auto held = makeUnique<SState::SRetainedInput>(&currentNV(),Offload::FD{duplicate.take()}, input);
    held->source = wl_event_loop_add_fd(g_pCompositor->m_wlEventLoop, held->fence.value, WL_EVENT_READABLE,
        [](int, uint32_t mask, void* data) -> int {
            auto* retained=static_cast<SState::SRetainedInput*>(data);
            SessionScope scope(retained->owner);
            if (mask & (WL_EVENT_ERROR | WL_EVENT_HANGUP)) requestOffloadStop();
            std::erase_if(g_state->retainedInputs, [data](const auto& item) { return item.get() == data; });
            return 0;
        }, held.get());
    if (!held->source) {
        // A submission-only callback has no GL wait. Retain the source until
        // the worker has been reaped rather than finishing the GL context.
        currentNV().client.hold([input] {}); requestOffloadStop(); return;
    }
    g_state->retainedInputs.emplace_back(std::move(held));
}

// A pass-local wrapper is also used for window capture, which renders its own
// pass without RENDER_LAST_MOMENT/RENDER_POST. It never changes wl_surface state.
class COffloadSurfaceElement : public IPassElement {
  public:
    UP<IPassElement> original;
    bool subpass = false;
    bool fullscreen = false;
    explicit COffloadSurfaceElement(UP<IPassElement>&& element, bool early = false) : original(std::move(element)), subpass(early) {}
    CSurfacePassElement* surface() const { return dynamic_cast<CSurfacePassElement*>(original.get()); }
    bool needsLiveBlur(Render::CRenderContext& ctx) override { return original->needsLiveBlur(ctx); }
    bool needsPrecomputeBlur(Render::CRenderContext& ctx) override { return original->needsPrecomputeBlur(ctx); }
    bool undiscardable(Render::CRenderContext& ctx) override { return original->undiscardable(ctx); }
    bool disableSimplification(Render::CRenderContext& ctx) override { return original->disableSimplification(ctx); }
    bool requiresFullDamage(Render::CRenderContext& ctx) override { return original->requiresFullDamage(ctx); }
    std::optional<CBox> boundingBox(Render::CRenderContext& ctx) override { return original->boundingBox(ctx); }
    CRegion opaqueRegion(Render::CRenderContext& ctx) override { return original->opaqueRegion(ctx); }
    void discard(Render::CRenderContext& ctx) override { original->discard(ctx); }
    const char* passName() override { return "CGpuTransferSurfaceElement"; }
    ePassElementType type() override { return EK_CUSTOM; }
    std::vector<UP<IPassElement>> draw(Render::CRenderContext& ctx) override {
        auto& rd = ctx.m_data;
        const auto damage = rd.damage;
        auto* element = surface();
        const auto prepared = prepareOffloadTexture(ctx, *element, rd.pMonitor.lock(), fullscreen);
        if (!prepared) { g_pHyprRenderer->draw(ctx, original, damage); return {}; }
        SessionScope selected(prepared->session);
        auto& slot = currentNV().slots[prepared->slot];
        const auto source = currentNV().client.copyOnly ? NColorManagement::PImageDescription{} : sourceDescription(*element);
        auto texture = element->m_data.texture;
        {
            const auto used = ctx.m_usedAsyncBuffers.size();
            Render::CScopeGuard restore([&] {
                element->m_data.texture = texture;
                if (currentNV().earlyRelease) ctx.m_usedAsyncBuffers.resize(used);
            });
            element->m_data.texture = slot.texture;
            if(currentNV().client.copyOnly)g_pHyprRenderer->draw(ctx, original,damage);
            else {
            // Hyprland treats PQ -> SDR differently from scRGB -> SDR. Keep the
            // original content tone-map parameters while decoding packed PQ.
            // All primaries/TF ranges still describe the actual packed texture.
            const auto cache = ctx.m_cmSettingsCache;
            const auto previousSurface = rd.surface;
            const auto previousWindow = rd.currentWindow;
            rd.surface = element->m_data.surface;
            rd.currentWindow = element->m_data.pWindow;
            Render::CScopeGuard restoreCM([&] {
                ctx.m_cmSettingsCache = cache;
                rd.surface = previousSurface; rd.currentWindow = previousWindow;
            });
            const auto target = rd.currentFB->imageDescription();
            auto originalSettings = g_pHyprRenderer->getCMSettings(ctx, source, target, element->m_data.surface, true,
                rd.pMonitor->m_sdrMinLuminance, rd.pMonitor->m_sdrMaxLuminance, true);
            const auto packed = slot.texture->m_imageDescription;
            auto packedSettings = g_pHyprRenderer->getCMSettings(ctx, packed, target, element->m_data.surface, true,
                rd.pMonitor->m_sdrMinLuminance, rd.pMonitor->m_sdrMaxLuminance, true);
            packedSettings.needsTonemap = originalSettings.needsTonemap;
            packedSettings.tonemapMode = originalSettings.tonemapMode;
            packedSettings.maxLuminance = originalSettings.maxLuminance;
            packedSettings.dstMaxLuminance = originalSettings.dstMaxLuminance;
            for (auto& entry : ctx.m_cmSettingsCache)
                if (entry.srcDescId == packed->id() && entry.dstDescId == target->id()) entry.settings = packedSettings;
            g_pHyprRenderer->draw(ctx, original, damage);
            }
        }
        // Dedicated read fence works for normal composition AND independent
        // capture passes, including FULL_FAKE passes with no monitor in-fence.
        auto fence = g_pHyprRenderer->createSyncFDManager();
        if (fence && fence->isValid()) {
            if (ctx.m_mode == Render::RENDER_MODE_FULL_FAKE && !currentNV().earlyRelease) {
                // FULL_FAKE skips the core's async-buffer release fence. Protect
                // the source from client reuse until our conversion has ended.
                const auto input = element->m_data.surface->m_current.buffer;
                for (const auto& releaser : input->m_syncReleasers) releaser->addSyncFileFd(fence->fd());
                retainCaptureInput(fence->fd(), input);
            }
            slot.readComplete.reset(fence->takeFd().take());
        } else { glFinish(); requestOffloadStop(); }
        ++currentNV().windowFrames;
        if (subpass) ++g_state->subpassFrames;
        currentNV().route = subpass ? "subpass composition" : (ctx.m_mode == Render::RENDER_MODE_NORMAL ? "composition" : "window capture");
        if (ctx.m_mode != Render::RENDER_MODE_NORMAL) ++currentNV().captureFrames;
        return {};
    }
};

static void wrapOffloadSurfaces(Render::CRenderContext& ctx, bool subpass) {
    if (!g_state || !g_state->enabled || !currentNV().enabled || !Render::GL::g_pHyprOpenGL) return;
    // Only the main transient render pass is owned here. Nested transformer
    // passes may outlive this frame and must not retain plugin vtables.
    if (&g_pHyprRenderer->currentPass(ctx) != &ctx.m_pass) return;
    for (auto& item : g_pHyprRenderer->currentPass(ctx).m_passElements) {
        auto* surface = item.element ? dynamic_cast<CSurfacePassElement*>(item.element.get()) : nullptr;
        if (!surface || !surface->m_data.pWindow || !surface->m_data.texture) continue;
        const auto source = sourceDescription(*surface);
        if (!source) continue;
        item.element = makeUnique<COffloadSurfaceElement>(std::move(item.element), subpass);
    }
}

static void unwrapOffloadSurfaces(Render::CRenderContext& ctx, Render::CRenderPass* pass) {
    if (!pass) pass = &g_pHyprRenderer->currentPass(ctx);
    for (auto& item : pass->m_passElements)
        if (auto* wrapped = item.element ? dynamic_cast<COffloadSurfaceElement*>(item.element.get()) : nullptr) {
            auto original = std::move(wrapped->original);
            item.element = std::move(original);
        }
}

// Overview flushes its pass before RENDER_LAST_MOMENT, after adjusting child
// geometry. Intercept actual execution so those edits see ordinary elements.
// Wrappers are removed before returning; no persistent plugin-owned vtables.
using FRenderPass = CRegion (*)(Render::CRenderPass*, Render::CRenderContext&, const CRegion&);
static CRegion renderOffloadPass(Render::CRenderPass* pass, Render::CRenderContext& ctx, const CRegion& damage) {
    const auto previousContext=g_transferContext;
    g_transferContext=&ctx;
    Render::CScopeGuard restoreContext([previousContext] {g_transferContext=previousContext;});
    auto original = reinterpret_cast<FRenderPass>(g_passHook->m_original);
    const bool main = g_state && g_pHyprRenderer && pass == &ctx.m_pass;
    if (!main) return original(pass, ctx, damage);
    wrapOffloadSurfaces(ctx, true);
    Render::CScopeGuard restore([&ctx, pass] { unwrapOffloadSurfaces(ctx, pass); });
    auto result = original(pass, ctx, damage);
    unwrapOffloadSurfaces(ctx, pass);
    for(const auto& item:pass->m_passElements)
        if(!item.discard && item.element)
            if(auto* surface=dynamic_cast<CSurfacePassElement*>(item.element.get()))
                GpuSwitch::observeRenderedSurface(ctx, surface);
    return result;
}
static bool installPassHook() {
    std::vector<SFunctionMatch> matches;
    for (const auto& match : GpuSwitch::findHookFunctions(g_handle, "render"))
        if (match.demangled.find("Render::CRenderPass::render(") != std::string::npos) matches.push_back(match);
    if (matches.size() != 1) return false;
    g_passHook = HyprlandAPI::createFunctionHook(g_handle, matches[0].address, reinterpret_cast<void*>(&renderOffloadPass));
    return g_passHook && g_passHook->hook();
}


bool GpuSwitch::drawTransferredSurface(Render::CRenderContext& ctx, UP<IPassElement>& original,const CRegion& damage,bool fullscreen) {
    if(!g_state || !g_state->enabled)return false;
    if (!original || !dynamic_cast<CSurfacePassElement*>(original.get())) return false;
    COffloadSurfaceElement wrapper(std::move(original));
    wrapper.fullscreen=fullscreen;
    Render::CScopeGuard restore([&] { original=std::move(wrapper.original); });
    const auto previous=ctx.m_data.damage;
    Render::CScopeGuard restoreDamage([&] {ctx.m_data.damage=previous;});
    ctx.m_data.damage=damage;
    wrapper.draw(ctx);
    return true;
}

void GpuSwitch::initClientTransfer(HANDLE handle) {
    g_handle=handle;
    g_state=makeUnique<SState>();
    g_state->offloadTimer=makeShared<CEventLoopTimer>(std::nullopt,[](SP<CEventLoopTimer> self,void*) {
        if(!installProducerTracking()){self->updateTimeout(std::chrono::milliseconds(10));return;}
        for(auto& entry:g_state->sessions){SessionScope selected(entry.get());maintainOffload();updateAutomaticVramInput();}
        std::erase_if(g_state->vram.proxies,[](const auto& entry){return entry.second.texture.expired();});
        retireVramSession();
        g_state->sessions.eraseIf([](auto& session){
            if(session.client.running() || session.stopRequested || (!session.identitySurface.expired() && !session.identityWindow.expired()))return false;
            if(g_state->vram.session==&session)return false;
            if(std::ranges::any_of(g_state->vram.proxies,[&](const auto& entry){return entry.second.session==&session;}))return false;
            if(std::ranges::any_of(g_state->retainedInputs,[&](const auto& input){return input->owner==&session;}))return false;
            if(g_state->last==&session)g_state->last=nullptr;
            return true;
        });
        if(g_state && g_state->sessions.size())self->updateTimeout(std::chrono::milliseconds(250));
    },nullptr);
    g_pEventLoopManager->addTimer(g_state->offloadTimer);
    if(!installProducerTracking())g_state->offloadTimer->updateTimeout(std::chrono::milliseconds(10));
    if(!installPassHook())throw std::runtime_error("Cannot hook cross-GPU render pass");
    g_state->closeListener=Event::bus()->m_events.window.close.listen([](PHLWINDOW w) {
        if(g_state && g_state->vram.owner.lock()==w){setVramInput(false);g_state->vram.safeTexture.reset();g_state->vram.owner.reset();g_state->vram.pid=0;}
        if(g_state)for(auto& entry:g_state->sessions)if(entry->identityWindow.lock()==w){SessionScope selected(entry.get());requestOffloadStop();}
    });
}

void GpuSwitch::exitClientTransfer() {
    if(!g_state)return;
    stopVramInput();g_state->enabled=false;
    if(g_passHook){g_passHook->unhook();HyprlandAPI::removeFunctionHook(g_handle,g_passHook);g_passHook=nullptr;}
    unwrapOffloadSurfaces(g_pHyprRenderer->context());
    for(auto& entry:g_state->sessions){entry->sourceCommit.reset();entry->sourceDestroy.reset();entry->sourceQueued.reset();}
    g_state->newProducerSurface.reset();g_state->producerWatches.clear();g_state->bufferProducers.clear();
    if(g_state->offloadTimer){g_state->offloadTimer->cancel();g_pEventLoopManager->removeTimer(g_state->offloadTimer);}
    g_state->closeListener.reset();
    if(Render::GL::g_pHyprOpenGL){Render::GL::g_pHyprOpenGL->makeEGLCurrent();glFinish();}
    for(auto& entry:g_state->sessions)entry->client.finish();
    g_state->retainedInputs.clear();
    for(auto& entry:g_state->sessions)for(auto& slot:entry->slots){slot.texture.reset();slot.externalOutput.reset();slot.readComplete.reset();slot.outputFD.reset();slot.conversionComplete.reset();}
    g_state->active=nullptr;g_state->last=nullptr;g_state->sessions.clear();
    g_state.reset();
}

static GpuSwitch::Json transferSessionStatus(const SState::SNV& nv) {
    GpuSwitch::Json result={{"enabled",g_state->enabled},{"ready",nv.client.ready},{"reason",nv.reason},{"worker_reason",nv.client.reason},{"eligibility_reason",g_state->reason},{"last_stop_reason",nv.lastStopReason},
        {"transport",nv.client.copyOnly?"gpu-copy":"pq10-convert"},{"worker",static_cast<unsigned long>(nv.client.workerPID)},
        {"pid",static_cast<unsigned long>(nv.ownerPID)},{"application",nv.identityWindow.lock()?nv.identityWindow.lock()->metadata().appID():""},
        {"source_gpu",nv.sourceGPU?nv.sourceGPU->id:""},{"renderer_gpu",nv.rendererGPU?nv.rendererGPU->id:""},
        {"keep_warm",workerCanStayWarm(nv,nv.owner.lock())},{"eager",nv.eager},{"pipelined_requested",nv.client.pipelined},{"pipelined_active",nv.client.ready && nv.client.pipelinedActive},
        {"prefer_local_input",nv.client.preferLocalInput},{"source_device_local",nv.client.sourceDeviceLocal},
        {"vram_input_auto",g_state->vram.automatic},{"vram_feedback",g_state->vram.session==&nv && g_state->vram.feedback},
        {"vram_native_layout",g_state->vram.session==&nv && g_state->vram.nativeLayout},{"vram_error",(!g_state->vram.session || g_state->vram.session==&nv)?g_vramError:""}
#if GPU_SWITCH_DIAGNOSTICS
        ,{"frames",nv.frames},{"reused",nv.reused},{"window_frames",nv.windowFrames},
        {"capture_frames",nv.captureFrames},{"eager_frames",nv.eagerFrames},
        {"color_configurations",nv.colorConfigurations},{"materializations",nv.materializations},{"external_copies",nv.externalCopies},
        {"pending_inputs",g_state->retainedInputs.size()},{"protected_textures",g_state->vram.protectedTextures},{"vram_layout",g_state->vram.layoutDiagnostic}
#endif
    };
    return result;
}

std::string GpuSwitch::clientTransferStatus(){
    if(!g_state)return Json({{"enabled",false},{"state","suspended-for-migration"}}).dump();
    Json entries=Json::array();unsigned long running=0,ready=0;SState::SNV* representative=g_state->last;
    for(const auto& entry:g_state->sessions){entries.push_back(transferSessionStatus(*entry));running+=entry->client.running();ready+=entry->client.ready;if(entry->client.ready)representative=entry.get();}
    auto legacy=transferSessionStatus(representative?*representative:g_state->idle).dump();legacy.pop_back();
    return legacy+",\"worker_count\":"+std::to_string(running)+",\"ready_count\":"+std::to_string(ready)+",\"session_count\":"+std::to_string(g_state->sessions.size())+",\"worker_limit\":16,\"sessions\":"+entries.dump()+"}";
}
