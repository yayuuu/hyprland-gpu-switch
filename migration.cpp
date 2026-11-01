#include "function-hooks.hpp"
#include "rendering-optimizations.hpp"
#include "client-transfer.hpp"
#include <bits/stdc++.h>
#include <hyprgraphics/egl/Egl.hpp>
#define private public
#define protected public
#include <hyprland/src/render/OpenGL.hpp>
#include <hyprland/src/render/GLRenderer.hpp>
#include <hyprland/src/render/gl/GLTexture.hpp>
#include <hyprland/src/render/gl/GLFramebuffer.hpp>
#include <hyprland/src/render/gl/GLRenderbuffer.hpp>
#include <hyprland/src/render/Shader.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/output/MonitorResources.hpp>
#include <hyprland/src/protocols/LinuxDMABUF.hpp>
#include <hyprland/src/protocols/core/Shm.hpp>
#include <hyprland/src/protocols/core/Compositor.hpp>
#include <hyprland/src/protocols/DRMSyncobj.hpp>
#include <hyprland/src/protocols/types/DMABuffer.hpp>
#include <hyprland/src/pointer/PointerManager.hpp>
#include <hyprland/src/plugins/PluginSystem.hpp>
#include <aquamarine/allocator/Swapchain.hpp>
#include <aquamarine/allocator/GBM.hpp>
#include <aquamarine/backend/DRM.hpp>
#include <hyprland/src/output/MonitorFrameScheduler.hpp>
#include <hyprland/src/managers/eventLoop/EventLoopManager.hpp>
#undef protected
#undef private
#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/state/MonitorState.hpp>
#include <hyprland/src/debug/log/Logger.hpp>
#include "migration.hpp"
#include "gl-state-cache.hpp"
#include "diagnostics/diagnostics.hpp"
#include "native.hpp"
#include "probe.hpp"
#include "buffer-compat.hpp"
#include "drm-route.hpp"
#include <dlfcn.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>
#include <xf86drm.h>
using namespace Render;
using namespace Render::GL;
using namespace GpuSwitch;
static_assert(std::string_view(GIT_COMMIT_HASH)=="0aa6745f705f2c4b0a5004eb019ef76e5bbdb426","Only verified with Hyprland 0.57.0");
static HANDLE pluginHandle=nullptr;
static std::vector<CFunctionHook*> hooks;
static std::map<ITexture*,WP<ITexture>> textures;
static std::map<IFramebuffer*,WP<IFramebuffer>> framebuffers;
static std::map<CShader*,WP<CShader>> shaders;
static std::map<CShader*,std::pair<std::string,std::string>> sources;
struct Dma {
    Aquamarine::SDMABUFAttrs attrs;
    std::vector<Hyprutils::OS::CFileDescriptor> fds;
    explicit Dma(const Aquamarine::SDMABUFAttrs& a):attrs(a) {
        for (int i=0;i<a.planes;++i) {
            int fd=fcntl(a.fds[i],F_DUPFD_CLOEXEC,0);
            if (fd<0) throw std::runtime_error("Cannot retain DMA-BUF for GPU migration");
            fds.emplace_back(fd);attrs.fds[i]=fd;
        }
    }
};
static std::map<ITexture*,std::shared_ptr<Dma>> dmaBuffers;
struct OriginContext {
    EGLDisplay display=EGL_NO_DISPLAY;
    EGLContext context=EGL_NO_CONTEXT;
    std::vector<SDRMFormat> formats;
};
static std::vector<OriginContext> origins;
struct OriginTexture {
    OriginContext origin;
    GLuint texture=0;
    void* image=nullptr;
    GLenum target=GL_TEXTURE_2D;
    bool owned=false;
    Native::TextureData read(bool contents) const {
        Native::CurrentGuard restore;
        if(!eglMakeCurrent(origin.display,EGL_NO_SURFACE,EGL_NO_SURFACE,origin.context))throw std::runtime_error("Source context for client buffer is unavailable");
        return Native::readTexture(texture,target,contents);
    }
    ~OriginTexture() {
        if(!owned)return;
        Native::CurrentGuard restore;
        if(!eglMakeCurrent(origin.display,EGL_NO_SURFACE,EGL_NO_SURFACE,origin.context))return;
        if(texture)glDeleteTextures(1,&texture);
        if(image) {
            auto destroy=reinterpret_cast<PFNEGLDESTROYIMAGEKHRPROC>(eglGetProcAddress("eglDestroyImageKHR"));
            if(destroy)destroy(origin.display,image);
        }
    }
};
struct ClientBridge {WP<ITexture> texture;std::shared_ptr<OriginTexture> source;bool dirty=true;bool eventDriven=false;};
#if GPU_SWITCH_DIAGNOSTICS
static uint64_t bridgeCopies=0,bridgeSkips=0,bridgeBytes=0;
static uint64_t bridgeWaitNs=0,bridgeReadNs=0,bridgeUploadNs=0,bridgeMaxCopyNs=0;
#endif
static CFunctionHook* surfaceCommitHook=nullptr;
static std::map<ITexture*,ClientBridge> clientBridges;
static CFunctionHook* bindTextureHook=nullptr;
static bool matchesFormat(const std::vector<SDRMFormat>& formats,const Aquamarine::SDMABUFAttrs& attrs) {
    if(!attrs.success || attrs.modifier==DRM_FORMAT_MOD_INVALID)return false;
    return std::ranges::any_of(formats,[&](const auto& f){return f.drmFormat==attrs.format && std::ranges::find(f.modifiers,attrs.modifier)!=f.modifiers.end();});
}
static EGLImageKHR importImage(EGLDisplay display,const Aquamarine::SDMABUFAttrs& a) {
    if(!a.success || a.planes<1 || a.planes>4)return EGL_NO_IMAGE_KHR;
    static constexpr std::array<EGLint,4> fdKeys={EGL_DMA_BUF_PLANE0_FD_EXT,EGL_DMA_BUF_PLANE1_FD_EXT,EGL_DMA_BUF_PLANE2_FD_EXT,EGL_DMA_BUF_PLANE3_FD_EXT};
    static constexpr std::array<EGLint,4> offsetKeys={EGL_DMA_BUF_PLANE0_OFFSET_EXT,EGL_DMA_BUF_PLANE1_OFFSET_EXT,EGL_DMA_BUF_PLANE2_OFFSET_EXT,EGL_DMA_BUF_PLANE3_OFFSET_EXT};
    static constexpr std::array<EGLint,4> strideKeys={EGL_DMA_BUF_PLANE0_PITCH_EXT,EGL_DMA_BUF_PLANE1_PITCH_EXT,EGL_DMA_BUF_PLANE2_PITCH_EXT,EGL_DMA_BUF_PLANE3_PITCH_EXT};
    static constexpr std::array<EGLint,4> loKeys={EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT,EGL_DMA_BUF_PLANE1_MODIFIER_LO_EXT,EGL_DMA_BUF_PLANE2_MODIFIER_LO_EXT,EGL_DMA_BUF_PLANE3_MODIFIER_LO_EXT};
    static constexpr std::array<EGLint,4> hiKeys={EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT,EGL_DMA_BUF_PLANE1_MODIFIER_HI_EXT,EGL_DMA_BUF_PLANE2_MODIFIER_HI_EXT,EGL_DMA_BUF_PLANE3_MODIFIER_HI_EXT};
    std::vector<EGLint> attrs={EGL_WIDTH,static_cast<EGLint>(a.size.x),EGL_HEIGHT,static_cast<EGLint>(a.size.y),EGL_LINUX_DRM_FOURCC_EXT,static_cast<EGLint>(a.format)};
    for(int i=0;i<a.planes;++i) {
        attrs.insert(attrs.end(),{fdKeys[i],a.fds[i],offsetKeys[i],static_cast<EGLint>(a.offsets[i]),strideKeys[i],static_cast<EGLint>(a.strides[i])});
        if(a.modifier!=DRM_FORMAT_MOD_INVALID)attrs.insert(attrs.end(),{loKeys[i],static_cast<EGLint>(a.modifier&0xffffffff),hiKeys[i],static_cast<EGLint>(a.modifier>>32)});
    }
    attrs.push_back(EGL_NONE);
    auto create=reinterpret_cast<PFNEGLCREATEIMAGEKHRPROC>(eglGetProcAddress("eglCreateImageKHR"));
    return create?create(display,EGL_NO_CONTEXT,EGL_LINUX_DMA_BUF_EXT,nullptr,attrs.data()):EGL_NO_IMAGE_KHR;
}
static std::shared_ptr<OriginTexture> importAtOrigin(const Aquamarine::SDMABUFAttrs& attrs) {
    Native::CurrentGuard restore;
    for(const auto& origin:origins) {
        if(!matchesFormat(origin.formats,attrs))continue;
        if(!eglMakeCurrent(origin.display,EGL_NO_SURFACE,EGL_NO_SURFACE,origin.context))continue;
        auto source=std::make_shared<OriginTexture>();source->origin=origin;source->owned=true;
        source->image=importImage(origin.display,attrs);
        if(!source->image)continue;
        glGenTextures(1,&source->texture);glBindTexture(GL_TEXTURE_2D,source->texture);
        auto target=reinterpret_cast<PFNGLEGLIMAGETARGETTEXTURE2DOESPROC>(eglGetProcAddress("glEGLImageTargetTexture2DOES"));
        if(!target)continue;
        target(GL_TEXTURE_2D,source->image);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
        if(glGetError()!=GL_NO_ERROR)continue;
        return source;
    }
    throw std::runtime_error("No retained GPU context can read this client DMA-BUF");
}
static bool preparing=false,coverage=false;
static std::string readiness="not initialized";
static std::vector<std::unique_ptr<Native::Context>> contexts;
static std::vector<SP<Aquamarine::IAllocator>> retainedAllocators;
static std::unique_ptr<DrmRoutes> activeRoutes;
static CFunctionHook* unloadHook=nullptr;
static void unloadPlugin(CPluginSystem* system,const CPlugin* plugin,bool eject) {
    if(plugin && plugin->m_handle==pluginHandle && activeRoutes && g_pCompositor && !g_pCompositor->m_isShuttingDown) {
        readiness="cannot unload while migrated GPU resources are live; disable automatic switching instead";
        LOG(Log::ERR,"[gpu-switch] {}",readiness);
        return;
    }
    reinterpret_cast<void(*)(CPluginSystem*,const CPlugin*,bool)>(unloadHook->m_original)(system,plugin,eject);
}

static CFunctionHook* swapchainHook=nullptr;

static std::optional<std::chrono::steady_clock::time_point> pausedSince;
static CFunctionHook *frameHook=nullptr,*syncFiredHook=nullptr,*pendingCommitHook=nullptr;
static void onFrame(Monitor::CMonitorFrameScheduler* self) {
    if(pausedSince)return;
    reinterpret_cast<void(*)(Monitor::CMonitorFrameScheduler*)>(frameHook->m_original)(self);
}
static void onSyncFired(Monitor::CMonitorFrameScheduler* self) {
    if(pausedSince) {self->m_renderAtFrame=true;return;}
    reinterpret_cast<void(*)(Monitor::CMonitorFrameScheduler*)>(syncFiredHook->m_original)(self);
}
static bool pendingCommit(void* self,PHLMONITOR monitor,std::optional<Monitor::CDamageRing::CTransaction> damage,const CRegion& renderedDamage) {
    if(pausedSince)return false;
    if(activeRoutes && monitor) {
        try {activeRoutes->beforeCommit(monitor.get());}
        catch(const std::exception& error) {readiness=error.what();return false;}
    }
    return reinterpret_cast<bool(*)(void*,PHLMONITOR,std::optional<Monitor::CDamageRing::CTransaction>,const CRegion&)>(pendingCommitHook->m_original)(self,monitor,std::move(damage),renderedDamage);
}
void GpuSwitch::cancelMigration() {
    if(!pausedSince)return;
    pausedSince.reset();
    for(const auto& monitor:State::monitorState()->monitors()) {
        monitor->m_forceFullFrames=3;
        g_pHyprRenderer->damageMonitor(monitor);monitor->scheduleFrame(Aquamarine::IOutput::AQ_SCHEDULE_DAMAGE);
    }
}
static bool reconfigure(Monitor::CMonitorState* self) {
    const bool result=reinterpret_cast<bool(*)(Monitor::CMonitorState*)>(swapchainHook->m_original)(self);
    if(!result || !activeRoutes)return result;
    try {activeRoutes->update(self->m_owner);return true;}
    catch(const std::exception& error) {readiness=error.what();return false;}
}
static EGLDisplay initialDisplay=EGL_NO_DISPLAY;
static EGLContext initialContext=EGL_NO_CONTEXT;
static int initialRenderFD=-1;
static auto originalSyncSupport=false;
static CFunctionHook *texBoolHook=nullptr,*texPixelsHook=nullptr,*texDmaHook=nullptr,*texRawHook=nullptr,*texCairoHook=nullptr,*texLutHook=nullptr,*stencilHook=nullptr,*fbHook=nullptr,*shaderHook=nullptr,*useShaderHook=nullptr;
static SP<ITexture> track(SP<ITexture> t) {
    if(t && !preparing) {
        auto it=clientBridges.find(t.get());
        if(it!=clientBridges.end() && !it->second.texture)clientBridges.erase(it);
        textures[t.get()]=t;
    }
    return t;
}
static SP<IFramebuffer> track(SP<IFramebuffer> f) {
    if(f && !preparing) {
        framebuffers[f.get()]=f;track(f->m_tex);track(f->m_stencilTex);track(f->m_mirrorTex);
    }
    return f;
}
static SP<ITexture> texBool(void* self,bool opaque) {
    return track(reinterpret_cast<SP<ITexture>(*)(void*,bool)>(texBoolHook->m_original)(self,opaque));
}
static SP<ITexture> texPixels(void* self,uint32_t f,uint8_t* p,uint32_t stride,const Vector2D& size,bool copy,bool opaque) {
    return track(reinterpret_cast<SP<ITexture>(*)(void*,uint32_t,uint8_t*,uint32_t,const Vector2D&,bool,bool)>(texPixelsHook->m_original)(self,f,p,stride,size,copy,opaque));
}
static SP<ITexture> texDma(void* self,const Aquamarine::SDMABUFAttrs& a,bool opaque) {
    bool handled=false;
    auto t=GpuSwitch::createClientTransferTexture(self,a,opaque,reinterpret_cast<GpuSwitch::TransferTextureFactory>(texDmaHook->m_original),handled);
    if(!handled && (origins.empty() || matchesFormat(g_pHyprOpenGL->m_drmFormats,a)))
        t=reinterpret_cast<SP<ITexture>(*)(void*,const Aquamarine::SDMABUFAttrs&,bool)>(texDmaHook->m_original)(self,a,opaque);
    if(!handled && !t && !preparing && !origins.empty()) {
        try {
            auto source=importAtOrigin(a);
            auto description=source->read(false);
            g_pHyprOpenGL->makeEGLCurrent();
            t=g_pHyprRenderer->createTexture(opaque);
            auto native=static_cast<CGLTexture*>(t.get());
            native->m_texID=Native::uploadTexture(description);native->m_size=a.size;native->m_drmFormat=a.format;
            native->m_opaque=Hyprgraphics::Egl::isDrmFormatOpaque(a.format);native->m_type=native->m_opaque?TEXTURE_RGBX:TEXTURE_RGBA;native->m_isSynchronous=false;
            clientBridges[t.get()]={t,std::move(source)};
        } catch(const std::exception& e) {readiness=e.what();t.reset();}
    }
    t=track(t);
    if(t && !preparing) {
        try {dmaBuffers[t.get()]=std::make_shared<Dma>(a);}
        catch(const std::exception& e) {readiness=e.what();coverage=false;}
    }
    return t;
}
static void surfaceCommit(CWLSurfaceResource* self,SSurfaceState& state) {
    reinterpret_cast<void(*)(CWLSurfaceResource*,SSurfaceState&)>(surfaceCommitHook->m_original)(self,state);
    if(auto texture=self->m_current.texture)if(auto it=clientBridges.find(texture.get());it!=clientBridges.end()) {
        it->second.dirty=true;it->second.eventDriven=true;
    }
}
#if GPU_SWITCH_DIAGNOSTICS
std::string GpuSwitch::bridgeStatus() {
    Json rows=Json::array();
    for(const auto& [texture,bridge]:clientBridges)if(bridge.texture)
        rows.push_back({{"width",static_cast<unsigned long>(texture->m_size.x)},{"height",static_cast<unsigned long>(texture->m_size.y)},{"event_driven",bridge.eventDriven},{"dirty",bridge.dirty}});
    return Json({{"copies",bridgeCopies},{"skipped",bridgeSkips},{"bytes",bridgeBytes},{"wait_ns",bridgeWaitNs},{"read_ns",bridgeReadNs},{"upload_ns",bridgeUploadNs},{"max_copy_ns",bridgeMaxCopyNs},{"bridges",rows}}).dump();
}
#endif
static void bindTexture(CGLTexture* self) {
    if(GpuSwitch::bindClientTransferTexture(self,reinterpret_cast<GpuSwitch::TransferTextureBind>(bindTextureHook->m_original)))return;
    if(auto it=clientBridges.find(self);it!=clientBridges.end() && it->second.texture && !preparing) {
        if(!it->second.eventDriven && PROTO::compositor)PROTO::compositor->forEachSurface([&](SP<CWLSurfaceResource> surface) {
            if(surface->m_current.texture.get()==self)it->second.eventDriven=true;
        });
        if(it->second.eventDriven && !it->second.dirty) {
#if GPU_SWITCH_DIAGNOSTICS
            ++bridgeSkips;
#endif
            reinterpret_cast<void(*)(CGLTexture*)>(bindTextureHook->m_original)(self);return;
        }
        try {
            // The compositor has queued the client's acquire fence on the active
            // GPU. Finish that bounded wait before reading on the old GPU.
#if GPU_SWITCH_DIAGNOSTICS
            const auto copyStart=std::chrono::steady_clock::now();
#endif
            Native::waitIdle();
#if GPU_SWITCH_DIAGNOSTICS
            const auto waitEnd=std::chrono::steady_clock::now();
#endif
            auto pixels=it->second.source->read(true);
#if GPU_SWITCH_DIAGNOSTICS
            const auto readEnd=std::chrono::steady_clock::now();
#endif
            Native::uploadTexture(pixels,self->m_texID);
#if GPU_SWITCH_DIAGNOSTICS
            const auto uploadEnd=std::chrono::steady_clock::now();
            const auto ns=[](auto from,auto to)->uint64_t{return std::chrono::duration_cast<std::chrono::nanoseconds>(to-from).count();};
            bridgeWaitNs+=ns(copyStart,waitEnd);bridgeReadNs+=ns(waitEnd,readEnd);bridgeUploadNs+=ns(readEnd,uploadEnd);
            bridgeMaxCopyNs=std::max(bridgeMaxCopyNs,ns(copyStart,uploadEnd));
#endif
            self->m_cachedStates.fill(std::nullopt);
            it->second.dirty=false;
#if GPU_SWITCH_DIAGNOSTICS
            ++bridgeCopies;bridgeBytes+=pixels.pixels.size();
#endif
        } catch(const std::exception& e) {readiness=std::string("client copy: ")+e.what();}
    }
    reinterpret_cast<void(*)(CGLTexture*)>(bindTextureHook->m_original)(self);
}
static SP<ITexture> texRaw(void* self,int w,int h,unsigned char* p) {
    return track(reinterpret_cast<SP<ITexture>(*)(void*,int,int,unsigned char*)>(texRawHook->m_original)(self,w,h,p));
}
static SP<ITexture> texCairo(void* self,cairo_surface_t* c) {
    return track(reinterpret_cast<SP<ITexture>(*)(void*,cairo_surface_t*)>(texCairoHook->m_original)(self,c));
}
static SP<ITexture> texLut(void* self,std::span<const float> lut,size_t n) {
    return track(reinterpret_cast<SP<ITexture>(*)(void*,std::span<const float>,size_t)>(texLutHook->m_original)(self,lut,n));
}
static SP<ITexture> stencil(void* self,int w,int h) {
    return track(reinterpret_cast<SP<ITexture>(*)(void*,int,int)>(stencilHook->m_original)(self,w,h));
}
static SP<IFramebuffer> createFB(void* self,const std::string& name) {
    return track(reinterpret_cast<SP<IFramebuffer>(*)(void*,const std::string&)>(fbHook->m_original)(self,name));
}
static bool createProgram(CShader* self,const std::string& v,const std::string& f,bool dynamic,bool silent) {
    const bool ok=reinterpret_cast<bool(*)(CShader*,const std::string&,const std::string&,bool,bool)>(shaderHook->m_original)(self,v,f,dynamic,silent);
    if(ok && !preparing) sources[self]={v,f};
    return ok;
}
static WP<CShader> useShader(void* self,WP<CShader> shader) {
    if(shader && !preparing) shaders[shader.get()]=shader;
    return reinterpret_cast<WP<CShader>(*)(void*,WP<CShader>)>(useShaderHook->m_original)(self,shader);
}
static CFunctionHook* install(const std::string& name,const std::string& prefix,const std::string& token,void* replacement) {
    std::vector<SFunctionMatch> matches;
    for (auto match:GpuSwitch::findHookFunctions(pluginHandle,name)) {
        if(match.demangled.starts_with(prefix) && match.demangled.contains(token))matches.push_back(std::move(match));
    }
    if(matches.size()!=1) throw std::runtime_error("Ambiguous or missing GPU migration hook: "+prefix+token);
    auto hook=HyprlandAPI::createFunctionHook(pluginHandle,matches[0].address,replacement);
    if(!hook || !hook->hook()) {
        if(hook) HyprlandAPI::removeFunctionHook(pluginHandle,hook);
        throw std::runtime_error("Cannot install GPU migration hook: "+prefix+token);
    }
    hooks.push_back(hook);return hook;
}
static void collect() {
    auto gl=g_pHyprOpenGL.get();
    for(auto it=textures.begin();it!=textures.end();) {
        if(!it->second) {dmaBuffers.erase(it->first);it=textures.erase(it);} else ++it;
    }
    std::erase_if(framebuffers,[](const auto& p){return !p.second;});
    std::erase_if(shaders,[](const auto& p){return !p.second;});
    track(g_pHyprRenderer->m_screencopyDeniedTexture);track(g_pHyprRenderer->m_missingAssetTexture);
    track(g_pHyprRenderer->m_lockDeadTexture);track(g_pHyprRenderer->m_lockDead2Texture);
    track(g_pHyprRenderer->m_lockDead3Texture);track(g_pHyprRenderer->m_lockTtyTextTexture);
    track(Pointer::mgr()->getCurrentCursorTexture());
    for(const auto& mon:State::monitorState()->monitors()) {
        track(mon->m_splash);track(mon->m_background);
        if(auto r=mon->m_resources.get()) {
            track(r->m_mirrorTex);track(r->m_stencilTex);track(r->m_blurFB);track(r->m_monitorMirrorFB);
            for(auto& b:r->m_workBuffers.m_resources) track(b.buffer);
            for(auto& b:r->m_sizedWorkBuffers.m_resources) track(b.buffer);
        }
    }
    for(auto& [mon,fb]:gl->m_monitorBGFBs) track(fb);
    if(PROTO::shm) for(const auto& b:PROTO::shm->m_buffers) track(b->m_texture);
    if(PROTO::linuxDma) for(const auto& b:PROTO::linuxDma->m_buffers) {
        if(auto buffer=b->m_buffer) {
            track(buffer->m_texture);
            if(buffer->m_texture && !dmaBuffers.contains(buffer->m_texture.get()))
                dmaBuffers[buffer->m_texture.get()]=std::make_shared<Dma>(buffer->dmabuf());
        }
    }
    if(gl->m_shaders) for(size_t kind=0;kind<gl->m_shaders->fragVariants.size();++kind)
        for(auto& [features,shader]:gl->m_shaders->fragVariants[kind]) {
            shaders[shader.get()]=shader;
            sources[shader.get()]={gl->m_shaders->TEXVERTSRC,g_pShaderLoader->getVariantSource(static_cast<ePreparedFragmentShader>(kind),features)};
        }
    if(gl->m_finalScreenShader && gl->m_finalScreenShader->program()) {
        shaders[gl->m_finalScreenShader.get()]=gl->m_finalScreenShader;
        if(!sources.contains(gl->m_finalScreenShader.get())) throw std::runtime_error("Reload the screen shader to capture its source before migration");
    }
    // Copy before traversal: adding attachment textures does not mutate the FB map.
    for(const auto& [ptr,weak]:framebuffers) if(auto f=weak.lock()) {
        track(f->m_tex);track(f->m_stencilTex);track(f->m_mirrorTex);
    }
}
void GpuSwitch::maintainMigration() {
    if(preparing)return;
    if(activeRoutes)activeRoutes->retireDisconnected();
    for(auto it=textures.begin();it!=textures.end();) {
        if(!it->second) {clientBridges.erase(it->first);dmaBuffers.erase(it->first);it=textures.erase(it);} else ++it;
    }
    std::erase_if(framebuffers,[](const auto& entry){return !entry.second;});
    for(auto it=shaders.begin();it!=shaders.end();) {
        if(!it->second) {sources.erase(it->first);it=shaders.erase(it);} else ++it;
    }
}
struct GLState {
    EGLDisplay display;EGLContext context;EGLDeviceEXT device;
    decltype(CHyprOpenGLImpl::m_exts) exts;
    std::string extensions;
    int drmFD;
    std::vector<SDRMFormat> formats;
    bool hasModifiers;
    explicit GLState(CHyprOpenGLImpl* gl):display(gl->m_eglDisplay),context(gl->m_eglContext),device(gl->m_eglDevice),exts(gl->m_exts),
        extensions(gl->m_extensions),drmFD(gl->m_drmFD),formats(gl->m_drmFormats),hasModifiers(gl->m_hasModifiers) {}
    void apply(CHyprOpenGLImpl* gl) const {
        gl->m_eglDisplay=display;gl->m_eglContext=context;gl->m_eglDevice=device;gl->m_exts=exts;
        gl->m_extensions=extensions;gl->m_drmFD=drmFD;gl->m_drmFormats=formats;gl->m_hasModifiers=hasModifiers;
        if(!eglMakeCurrent(display,EGL_NO_SURFACE,EGL_NO_SURFACE,context)) throw std::runtime_error("Cannot restore EGL context");
        gl->m_currentProgram=0;gl->m_lastViewport.width=-1;
        gl->m_capStatus.fill(false);glDisable(GL_BLEND);glDisable(GL_SCISSOR_TEST);glDisable(GL_STENCIL_TEST);
        gl->m_blend=false;glUseProgram(0);
        synchronizeGLStateCache(*gl);
    }
};
struct TextureStage {
    SP<ITexture> texture;
    Native::TextureData data;
    std::shared_ptr<Dma> dma;
    GLuint oldID=0,newID=0;
    void* oldImage=nullptr;void* newImage=nullptr;
    GLenum target=GL_TEXTURE_2D;
    std::shared_ptr<OriginTexture> fallback;
    bool retainsOldTexture=false;
};
struct FBStage {SP<IFramebuffer> fb;GLuint oldID=0,newID=0;};
struct ShaderStage {SP<CShader> shader;std::unique_ptr<CShader> staged;};

static std::string renderNode(const std::string& id) {
    for(const auto& e:std::filesystem::directory_iterator(id+"/drm"))
        if(e.path().filename().string().starts_with("renderD")) return "/dev/dri/"+e.path().filename().string();
    throw std::runtime_error("No render node on selected GPU");
}
static void destroyTexture(TextureStage& t,bool fresh,CHyprOpenGLImpl* gl) {
    GLuint& id=fresh?t.newID:t.oldID;
    void*& image=fresh?t.newImage:t.oldImage;
    if(id) {glDeleteTextures(1,&id);id=0;}
    if(image) {gl->m_proc.eglDestroyImageKHR(gl->m_eglDisplay,image);image=nullptr;}
}
std::string GpuSwitch::renderingDevice() {
    if(!g_pHyprOpenGL) return {};
    struct stat s{};
    if(fstat(g_pHyprOpenGL->m_drmFD,&s)!=0) return {};
    return deviceId(std::format("/sys/dev/char/{}:{}/device",major(s.st_rdev),minor(s.st_rdev)));
}
std::string GpuSwitch::migrationReadiness() {return readiness;}
std::string GpuSwitch::renderingName() {
    g_pHyprOpenGL->makeEGLCurrent();
    const auto name=glGetString(GL_RENDERER);
    return name?reinterpret_cast<const char*>(name):"unavailable";
}
bool GpuSwitch::trackingComplete() {return coverage;}
bool GpuSwitch::migrationPaused() {return pausedSince.has_value();}
bool GpuSwitch::migrationIdleReady() {return g_pEventLoopManager->m_idle.fns.empty();}
void GpuSwitch::initMigration(void* handle) {
    pluginHandle=handle;
    initialDisplay=g_pHyprOpenGL->m_eglDisplay;initialContext=g_pHyprOpenGL->m_eglContext;
    initialRenderFD=g_pCompositor->m_drmRenderNode.fd;originalSyncSupport=g_pCompositor->m_drmRenderNode.syncObjSupport;
    coverage=!g_pHyprOpenGL->m_shadersInitialized;
    retainedAllocators.push_back(g_pCompositor->m_aqBackend->primaryAllocator);
    const std::string prefix="Render::GL::CHyprGLRenderer::";
    try {
        texBoolHook=install("createTexture",prefix+"createTexture(","(bool)",reinterpret_cast<void*>(&texBool));
        texPixelsHook=install("createTexture",prefix+"createTexture(","(unsigned int,",reinterpret_cast<void*>(&texPixels));
        texDmaHook=install("createTexture",prefix+"createTexture(","SDMABUFAttrs",reinterpret_cast<void*>(&texDma));
        texRawHook=install("createTexture",prefix+"createTexture(","(int, int,",reinterpret_cast<void*>(&texRaw));
        texCairoHook=install("createTexture",prefix+"createTexture(","_cairo_surface*",reinterpret_cast<void*>(&texCairo));
        texLutHook=install("createTexture",prefix+"createTexture(","std::span",reinterpret_cast<void*>(&texLut));
        stencilHook=install("createStencilTexture",prefix+"createStencilTexture(","",reinterpret_cast<void*>(&stencil));
        fbHook=install("createFB",prefix+"createFB(","",reinterpret_cast<void*>(&createFB));
        shaderHook=install("createProgram","CShader::createProgram(","",reinterpret_cast<void*>(&createProgram));
        useShaderHook=install("useShader","Render::GL::CHyprOpenGLImpl::useShader(","",reinterpret_cast<void*>(&useShader));
        swapchainHook=install("updateSwapchain","Monitor::CMonitorState::updateSwapchain(","",reinterpret_cast<void*>(&reconfigure));
        surfaceCommitHook=install("commitState","CWLSurfaceResource::commitState(","",reinterpret_cast<void*>(&surfaceCommit));
        bindTextureHook=install("bind","Render::GL::CGLTexture::bind(","",reinterpret_cast<void*>(&bindTexture));
        frameHook=install("onFrame","Monitor::CMonitorFrameScheduler::onFrame(","",reinterpret_cast<void*>(&onFrame));
        syncFiredHook=install("onSyncFired","Monitor::CMonitorFrameScheduler::onSyncFired(","",reinterpret_cast<void*>(&onSyncFired));
        pendingCommitHook=install("commitPendingAndDoExplicitSync","Render::IHyprRenderer::commitPendingAndDoExplicitSync(","",reinterpret_cast<void*>(&pendingCommit));
        // Buffer attachments contain C++ deleters instantiated in this DSO. Keep its
        // code mapped until process exit even if a plugin entry is removed at shutdown.
        auto ownPlugin=g_pPluginSystem->getPluginByHandle(handle);
        if(!ownPlugin)throw std::runtime_error("Cannot identify GPU migration module");
        auto pin=dlopen(ownPlugin->m_path.c_str(),RTLD_NOW|RTLD_NOLOAD|RTLD_NODELETE);
        if(!pin)throw std::runtime_error("Cannot retain GPU migration callback code");
        dlclose(pin);
        unloadHook=install("unloadPlugin","CPluginSystem::unloadPlugin(","",reinterpret_cast<void*>(&unloadPlugin));
        readiness=coverage?"ready":"tracking-started-after-first-frame; start plugin in test session before rendering";
    } catch (...) {exitMigration();throw;}
}
std::string GpuSwitch::migrate(const std::string& id,const std::function<bool()>& validate) {
    if(preparing) return "migration already in progress";
    // A nested compositor is a client of the real desktop. Sending an unsupported
    // DMA-BUF can crash its parent driver, so it is not an isolation boundary.
    if(!g_pCompositor->m_aqBackend->hasSession()) return "nested/Wayland-output migration disabled after unsafe DMA-BUF import was observed";
    if(!coverage) return readiness;
    auto gl=g_pHyprOpenGL.get();
    if(renderingDevice()==id) {cancelMigration();return {};}
    if(!pausedSince) {
        pausedSince=std::chrono::steady_clock::now();readiness="waiting-for-presentation-drain";
        return "retry: waiting for old frames to drain";
    }
    bool busy=false;
    for(const auto& mon:State::monitorState()->monitors()) {
        auto output=dynamicPointerCast<Aquamarine::CDRMOutput>(mon->m_output);
        if(output && (output->pendingPageFlip() || output->pendingAsyncCommit || output->asyncCommitEventPending))busy=true;
        if(mon->m_frameScheduler && mon->m_frameScheduler->m_sync && mon->m_frameScheduler->m_sync->isValid() &&
           !Hyprutils::OS::CFileDescriptor::isReadable(mon->m_frameScheduler->m_sync->fd().get()))busy=true;
    }
    if(busy) {
        if(std::chrono::steady_clock::now()-*pausedSince<std::chrono::seconds(2))return "retry: waiting for old frames to drain";
        cancelMigration();return "Old pageflips or render fences did not drain within two seconds";
    }
    Diagnostics::trace("frames-drained");
    struct Unpause {~Unpause(){GpuSwitch::cancelMigration();}} unpause;
    // Drain worker submissions and destroy raw EGL imports in the OLD context.
    // Resume only after all migration guards have restored the final context.
    struct TransferReset {
        HANDLE handle;
        explicit TransferReset(HANDLE h):handle(h){GpuSwitch::exitRenderingOptimizations();GpuSwitch::exitClientTransfer();}
        ~TransferReset(){try{GpuSwitch::initClientTransfer(handle);GpuSwitch::initRenderingOptimizations(handle);}catch(const std::exception& e){
            GpuSwitch::exitRenderingOptimizations();GpuSwitch::exitClientTransfer();LOG(Log::ERR,"[gpu-switch] Cannot restore client transport: {}",e.what());}}
    } transferReset(pluginHandle);
    // Release extension-owned rendering resources through the compositor lifecycle.
    struct PluginReset {
        struct Entry {std::string path;bool configured=false;};
        std::vector<Entry> entries;
        bool resumed=false;
        PluginReset(HANDLE ownHandle) {
            for(const auto& plugin:g_pPluginSystem->getAllPlugins())
                if(plugin->m_handle!=ownHandle)entries.push_back({plugin->m_path,plugin->m_loadedWithConfig});
            g_pHyprRenderer->context().m_pass.clear();
            for(const auto& entry:entries) {
                // Timer/idle callback destructors may still carry function manager
                // pointers into a peer DSO. Keep code mapped across its lifecycle.
                auto pin=dlopen(entry.path.c_str(),RTLD_NOW|RTLD_NOLOAD|RTLD_NODELETE);
                if(!pin)throw std::runtime_error("Cannot retain peer callback code before GPU reset");
                dlclose(pin);
            }
            for(auto it=entries.rbegin();it!=entries.rend();++it)
                if(auto plugin=g_pPluginSystem->getPluginByPath(it->path))g_pPluginSystem->unloadPlugin(plugin);
        }
        void resume() noexcept {
            if(resumed)return;
            resumed=true;
            for(const auto& entry:entries) {
                try {
                    g_pPluginSystem->loadPlugin(entry.path,SPECIAL_PID_TYPE_CONFIG)->then([entry](SP<CPromiseResult<CPlugin*>> result) {
                        if(result->hasError())LOG(Log::ERR,"[gpu-switch] Cannot restore plugin {}: {}",entry.path,result->error());
                        else result->result()->m_loadedWithConfig=entry.configured;
                    });
                } catch(const std::exception& e) {LOG(Log::ERR,"[gpu-switch] Cannot restore plugin {}: {}",entry.path,e.what());}
            }
        }
        ~PluginReset(){resume();}
    } plugins(pluginHandle);
    Diagnostics::trace("peers-unloaded");
    Native::CurrentGuard restoreCurrent;
    GLState old(gl),target(gl);
    std::vector<TextureStage> ts;
    auto nextBridges=clientBridges;
    auto nextOrigins=origins;
    std::vector<FBStage> fs;
    std::vector<ShaderStage> ss;
    std::unique_ptr<DrmRoutes> routes;
    std::unique_ptr<Native::Context> candidate;
    SP<Aquamarine::IAllocator> targetAllocator;
    auto oldAllocator=g_pCompositor->m_aqBackend->primaryAllocator;
    bool committed=false;
    try {
        old.apply(gl);
        for(const auto& mon:State::monitorState()->monitors()) if(mon->m_frameScheduler) {
            mon->m_frameScheduler->m_sync.reset();mon->m_frameScheduler->m_pendingThird=false;mon->m_frameScheduler->m_renderAtFrame=true;
        }
        for(int i=0;i<32 && glGetError()!=GL_NO_ERROR;++i) {}
        Native::waitIdle();
        // Cached output renderbuffers are disposable. Rebuild on next frame in the
        // new context; KMS/Aquamarine keep the currently scanned-out buffers alive.
        g_pHyprRenderer->context().m_currentRenderbuffer.reset();g_pHyprRenderer->m_renderbuffers.clear();
        g_pHyprRenderer->context().m_data.currentFB.reset();g_pHyprRenderer->context().m_data.mainFB.reset();g_pHyprRenderer->context().m_data.outFB.reset();
        collect();
        Diagnostics::trace("prepare-drm-routes");
        routes=std::make_unique<DrmRoutes>(id);
        Diagnostics::trace("drm-routes-ready");
        candidate=std::make_unique<Native::Context>(renderNode(id));
        Diagnostics::trace("target-context-ready");
        targetAllocator=routes->allocator();
        old.apply(gl);
        for(const auto& [ptr,weak]:textures) if(auto t=weak.lock()) {
            auto native=dynamic_cast<CGLTexture*>(t.get());
            if(!native) throw std::runtime_error("Unsupported texture implementation");
            TextureStage s{.texture=t,.oldID=t->m_texID,.oldImage=native->m_eglImage,.target=native->m_target};
            if(!s.oldID) continue;
            if(auto it=dmaBuffers.find(t.get());it!=dmaBuffers.end())s.dma=it->second;
            else if(s.oldImage)throw std::runtime_error("Live DMA-BUF import was not tracked");
            if(!s.dma)s.data=Native::readTexture(s.oldID,s.target);
            ts.push_back(std::move(s));
        }
        for(const auto& [ptr,weak]:framebuffers) if(auto f=weak.lock()) {
            auto native=dynamic_cast<CGLFramebuffer*>(f.get());
            if(!native) throw std::runtime_error("Unsupported framebuffer implementation");
            if(native->m_fbAllocated) fs.push_back({f,native->m_fb,0});
        }
        for(const auto& [ptr,weak]:shaders) if(auto shader=weak.lock()) {
            if(!shader->program()) continue;
            if(!sources.contains(shader.get())) throw std::runtime_error("A live shader has no captured source");
            ss.push_back({shader,nullptr});
        }
        Diagnostics::trace("resources-collected");
        preparing=true;
        candidate->current();
        target.display=candidate->display;target.context=candidate->context;target.device=candidate->device;target.drmFD=candidate->fd;
        target.extensions=reinterpret_cast<const char*>(glGetString(GL_EXTENSIONS));
        auto e=std::string(eglQueryString(target.display,EGL_EXTENSIONS));
        target.exts.EGL_ANDROID_native_fence_sync_ext=e.contains("EGL_ANDROID_native_fence_sync");
        target.exts.EXT_image_dma_buf_import=e.contains("EGL_EXT_image_dma_buf_import");
        target.exts.EXT_image_dma_buf_import_modifiers=e.contains("EGL_EXT_image_dma_buf_import_modifiers");
        target.exts.EXT_read_format_bgra=target.extensions.contains("GL_EXT_read_format_bgra");
        target.apply(gl);gl->initDRMFormats();target.formats=gl->m_drmFormats;target.hasModifiers=gl->m_hasModifiers;
        for(auto& t:ts) {
            if(t.dma) {
                if(matchesFormat(target.formats,t.dma->attrs))t.newImage=gl->createEGLImage(t.dma->attrs);
                if(!t.newImage) {
                    if(auto prior=clientBridges.find(t.texture.get());prior!=clientBridges.end())t.fallback=prior->second.source;
                    else {
                        t.fallback=std::make_shared<OriginTexture>();
                        t.fallback->origin={old.display,old.context,old.formats};
                        t.fallback->texture=t.oldID;t.fallback->image=t.oldImage;t.fallback->target=t.target;
                        t.retainsOldTexture=true;
                    }
                    auto description=t.fallback->read(false);
                    t.newID=Native::uploadTexture(description);
                    nextBridges[t.texture.get()]={t.texture,t.fallback};
                    continue;
                }
                nextBridges.erase(t.texture.get());
                glGenTextures(1,&t.newID);glBindTexture(t.target,t.newID);
                gl->m_proc.glEGLImageTargetTexture2DOES(t.target,t.newImage);
                glTexParameteri(t.target,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);glTexParameteri(t.target,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
                glTexParameteri(t.target,GL_TEXTURE_MIN_FILTER,GL_LINEAR);glTexParameteri(t.target,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
                Native::check("import client texture on target GPU");
            } else t.newID=Native::uploadTexture(t.data);
        }
        Diagnostics::trace("textures-staged");
        std::map<ITexture*,GLuint> ids;
        for(auto& t:ts) ids[t.texture.get()]=t.newID;
        for(auto& f:fs) {
            glGenFramebuffers(1,&f.newID);glBindFramebuffer(GL_FRAMEBUFFER,f.newID);
            if(!f.fb->m_tex) throw std::runtime_error("Live framebuffer has no migratable color texture");
            glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,ids.at(f.fb->m_tex.get()),0);
            if(f.fb->m_mirrorTex) {
                glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT1,GL_TEXTURE_2D,ids.at(f.fb->m_mirrorTex.get()),0);
                const GLenum buffers[]={GL_COLOR_ATTACHMENT0,GL_COLOR_ATTACHMENT1};glDrawBuffers(2,buffers);
            }
            if(f.fb->m_stencilTex && ids.contains(f.fb->m_stencilTex.get()))
                glFramebufferTexture2D(GL_FRAMEBUFFER,GL_DEPTH_STENCIL_ATTACHMENT,GL_TEXTURE_2D,ids.at(f.fb->m_stencilTex.get()),0);
            if(glCheckFramebufferStatus(GL_FRAMEBUFFER)!=GL_FRAMEBUFFER_COMPLETE) throw std::runtime_error("Migrated framebuffer is incomplete");
        }
        for(auto& s:ss) {
            s.staged=std::make_unique<CShader>();
            const auto& src=sources.at(s.shader.get());
            if(!s.staged->createProgram(src.first,src.second,true,true)) throw std::runtime_error("Target shader compilation failed");
        }
        Diagnostics::trace("framebuffers-shaders-staged");
        // Import every staged output buffer before changing any compositor object.
        for(const auto& buffer:routes->renderBuffers()) {
            auto rb=std::make_unique<CGLRenderbuffer>(buffer,buffer->dmabuf().format);
            if(!rb->good()) throw std::runtime_error("Target cannot render to a staged output buffer");
            rb->bind();glClearColor(0,0,0,1);glClear(GL_COLOR_BUFFER_BIT);rb->unbind();
        }
        Native::waitIdle();
        Diagnostics::trace("output-buffers-ready");
        routes->validateTransfers();
        Diagnostics::trace("transfers-validated");
        target.apply(gl);
        if(!validate()) throw std::runtime_error("Topology changed during migration; cancelled before commit");
        // Reserve allocations before changing live handles.
        contexts.reserve(contexts.size()+1);retainedAllocators.reserve(retainedAllocators.size()+1);
        if(std::ranges::none_of(nextOrigins,[&](const auto& o){return o.context==old.context;}))nextOrigins.push_back({old.display,old.context,old.formats});
        nextOrigins.push_back({target.display,target.context,target.formats});
        // All potentially failing work is above. Object identities remain unchanged.
        Diagnostics::trace("commit-begin");
        for(auto& [texture,bridge]:nextBridges)bridge.dirty=true; // Target shadows have just been recreated.
        clientBridges.swap(nextBridges);
        for(auto& t:ts) {
            if(t.retainsOldTexture) {t.fallback->owned=true;t.oldID=0;t.oldImage=nullptr;}
            auto native=static_cast<CGLTexture*>(t.texture.get());
            native->m_texID=t.newID;native->m_eglImage=t.newImage;native->m_cachedStates.fill(std::nullopt);
        }
        for(auto& f:fs) static_cast<CGLFramebuffer*>(f.fb.get())->m_fb=f.newID;
        for(auto& s:ss) {
            std::swap(s.shader->m_program,s.staged->m_program);
            std::swap(s.shader->m_uniformLocations,s.staged->m_uniformLocations);
            s.shader->uniformStatus.fill(std::monostate());
        }
        routes->commit();activeRoutes=std::move(routes);
        retainedAllocators.push_back(targetAllocator);
        g_pCompositor->m_drm.fd=activeRoutes->primaryFD();
        uint64_t displayCap=0;g_pCompositor->m_drm.syncobjSupport=drmGetCap(g_pCompositor->m_drm.fd,DRM_CAP_SYNCOBJ_TIMELINE,&displayCap)==0 && displayCap;
        g_pCompositor->m_drmRenderNode.fd=candidate->fd;
        if(PROTO::sync)PROTO::sync->m_drmFD=candidate->fd;
        uint64_t cap=0;g_pCompositor->m_drmRenderNode.syncObjSupport=drmGetCap(candidate->fd,DRM_CAP_SYNCOBJ_TIMELINE,&cap)==0 && cap;
        committed=true;
        Diagnostics::trace("commit-complete");
        old.apply(gl);
        for(auto& s:ss) s.staged.reset();
        for(auto& f:fs) glDeleteFramebuffers(1,&f.oldID);
        for(auto& t:ts) destroyTexture(t,false,gl);
        target.apply(gl);
        if(PROTO::linuxDma && PROTO::linuxDma->m_formatTable) {
            struct stat st{};fstat(candidate->fd,&st);
            PROTO::linuxDma->m_mainDevice=st.st_rdev;
            PROTO::linuxDma->m_mainDeviceFD=Hyprutils::OS::CFileDescriptor(fcntl(candidate->fd,F_DUPFD_CLOEXEC,0));
            PROTO::linuxDma->m_formatTable->m_rendererTranche.device=st.st_rdev;
            PROTO::linuxDma->m_formatTable->m_rendererTranche.formats=target.formats;
            for(auto& [monitor,tranche]:PROTO::linuxDma->m_formatTable->m_monitorTranches) if(monitor && monitor->m_output) {
                auto backend=dynamicPointerCast<Aquamarine::CDRMBackend>(monitor->m_output->getBackend());
                if(backend)tranche.device=backend->gpu->dev;
                tranche.formats=monitor->m_output->getRenderFormats();
            }
            PROTO::linuxDma->resetFormatTable();
        }
        for(const auto& mon:State::monitorState()->monitors()) {
            mon->m_forceFullFrames=3;g_pHyprRenderer->damageMonitor(mon);mon->scheduleFrame(Aquamarine::IOutput::AQ_SCHEDULE_DAMAGE);
        }
        Diagnostics::trace("feedback-updated");
        origins.swap(nextOrigins);
        contexts.push_back(std::move(candidate));preparing=false;readiness="ready";restoreCurrent.dismiss();
        Diagnostics::trace("migration-complete");
        // Guard would otherwise restore the old context; core selects target on next render.
        return {};
    } catch(const std::exception& e) {
        Diagnostics::trace(e.what());
        const std::string error=e.what();
        if(!committed) {
            if(candidate) {
                target.apply(gl);
                for(auto& s:ss) s.staged.reset();
                for(auto& f:fs) if(f.newID) glDeleteFramebuffers(1,&f.newID);
                for(auto& t:ts) destroyTexture(t,true,gl);
            }
            old.apply(gl);
        } else {
            // Commit cannot be rolled back after old resources are deleted. Keep
            // target EGL ownership alive even if post-commit feedback delivery fails.
            if(candidate) contexts.push_back(std::move(candidate));
            target.apply(gl);restoreCurrent.dismiss();
        }
        preparing=false;readiness=error;return error;
    }
}
void GpuSwitch::exitMigration() {
    cancelMigration();
    clientBridges.clear();origins.clear();
    for(auto it=hooks.rbegin();it!=hooks.rend();++it) {(*it)->unhook();HyprlandAPI::removeFunctionHook(pluginHandle,*it);}
    hooks.clear();textures.clear();framebuffers.clear();shaders.clear();sources.clear();dmaBuffers.clear();
    // Native handles stored in stock Hyprland objects remain valid after unload.
    // Transfer active context/fd ownership to the compositor; never dlclose live GL objects.
    for(auto& context:contexts) if(g_pHyprOpenGL && context->context==g_pHyprOpenGL->m_eglContext) {
        context->context=EGL_NO_CONTEXT;context->fd=-1;
    }
    contexts.clear();coverage=false;preparing=false;
    // Keep retired allocators reachable through the backend's existing allocator
    // graph while any submitted KMS buffer still has a weak allocator reference.
    // Active allocator remains owned by primaryAllocator; retirement is handled
    // only once presentations release old chains (not by unloading mid-frame).
    if(contexts.empty() && retainedAllocators.size()==1) retainedAllocators.clear();
}
