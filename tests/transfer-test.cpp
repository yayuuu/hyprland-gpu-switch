#include <bits/stdc++.h>
#define private public
#include <aquamarine/backend/Backend.hpp>
#include <aquamarine/backend/Null.hpp>
#include <aquamarine/allocator/GBM.hpp>
#include <aquamarine/allocator/Swapchain.hpp>
#include "vendor/aquamarine/Renderer.hpp"
#undef private
#include "native.hpp"
#include "aq-transfer.hpp"
#include "diagnostics/transfer-opt.hpp"
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
using namespace Hyprutils::Memory;
using namespace Aquamarine;
using namespace GpuSwitch;
struct Device {
    CSharedPointer<CBackend> backend;
    CSharedPointer<CGBMAllocator> allocator;
    CSharedPointer<CDRMRenderer> renderer;
    CSharedPointer<CSwapchain> chain;
    CSharedPointer<IBuffer> buffer;
    explicit Device(const char* node) {
        SBackendOptions options;options.logFunction=[](eBackendLogLevel level,std::string text) {if(level>=AQ_LOG_ERROR)std::cerr<<text<<'\n';};
        SBackendImplementationOptions implementation;implementation.backendType=AQ_BACKEND_NULL;implementation.backendRequestMode=AQ_BACKEND_REQUEST_MANDATORY;
        backend=CBackend::create({implementation},options);
        int fd=open(node,O_RDWR|O_CLOEXEC);if(fd<0)throw std::runtime_error("Cannot open test render node");
        allocator=CGBMAllocator::create(fd,backend);if(!allocator)throw std::runtime_error("Cannot create test allocator");
        renderer=CDRMRenderer::attempt(backend,allocator);if(!renderer)throw std::runtime_error("Cannot create test renderer");renderer->self=renderer;
        auto nullBackend=dynamicPointerCast<CNullBackend>(backend->getImplementations().front());
        std::vector<SDRMFormat> formats;
        for(const auto& f:renderer->formats) if(f.drmFormat==DRM_FORMAT_XRGB8888 && !f.external) {
            auto it=std::ranges::find_if(formats,[&](const auto& item){return item.drmFormat==f.drmFormat;});
            if(it==formats.end())formats.push_back({f.drmFormat,{f.modifier}});else it->modifiers.push_back(f.modifier);
        }
        if(formats.empty())throw std::runtime_error("No renderable XRGB8888 test format");
        nullBackend->setFormats(formats);
        chain=CSwapchain::create(allocator,nullBackend);
        SSwapchainOptions config;config.length=2;config.size={64,48};config.format=DRM_FORMAT_XRGB8888;
        if(!chain->reconfigure(config))throw std::runtime_error("Cannot allocate test swapchain");
        buffer=chain->next(nullptr);
    }
    void pattern(bool alternate) {
        CEglContextGuard context(*renderer);
        auto image=renderer->createEGLImage(buffer->dmabuf());if(!image)throw std::runtime_error("Cannot import GPU-local test buffer");
        GLuint rbo=0,fb=0;glGenRenderbuffers(1,&rbo);glBindRenderbuffer(GL_RENDERBUFFER,rbo);
        renderer->proc.glEGLImageTargetRenderbufferStorageOES(GL_RENDERBUFFER,image);
        glGenFramebuffers(1,&fb);glBindFramebuffer(GL_FRAMEBUFFER,fb);glFramebufferRenderbuffer(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_RENDERBUFFER,rbo);
        if(glCheckFramebufferStatus(GL_FRAMEBUFFER)!=GL_FRAMEBUFFER_COMPLETE)throw std::runtime_error("Incomplete test framebuffer");
        glDisable(GL_SCISSOR_TEST);glClearColor(alternate?0.8f:0.2f,0.4f,0.7f,1);glClear(GL_COLOR_BUFFER_BIT);
        glEnable(GL_SCISSOR_TEST);glScissor(3,7,11,19);glClearColor(1,0,0,1);glClear(GL_COLOR_BUFFER_BIT);glDisable(GL_SCISSOR_TEST);
        Native::waitIdle();glBindFramebuffer(GL_FRAMEBUFFER,0);glDeleteFramebuffers(1,&fb);glDeleteRenderbuffers(1,&rbo);renderer->proc.eglDestroyImageKHR(renderer->egl.display,image);
    }
    void checkTextureReadback(const std::vector<uint8_t>& expected) {
        CEglContextGuard context(*renderer);
        auto image=renderer->createEGLImage(buffer->dmabuf());
        if(!image)throw std::runtime_error("Cannot import client-style test image");
        GLuint texture=0;glGenTextures(1,&texture);glBindTexture(GL_TEXTURE_2D,texture);
        renderer->proc.glEGLImageTargetTexture2DOES(GL_TEXTURE_2D,image);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
        Native::check("client-style image import");
        auto metadata=Native::readTexture(texture,GL_TEXTURE_2D,false);
        if(!metadata.discard || !metadata.pixels.empty())throw std::runtime_error("Client metadata touched pixel data");
        auto readback=Native::readTexture(texture,GL_TEXTURE_2D);
        const bool same=readback.pixels.size()==expected.size() && std::memcmp(readback.pixels.data(),expected.data(),expected.size())==0;
        glDeleteTextures(1,&texture);renderer->proc.eglDestroyImageKHR(renderer->egl.display,image);
        if(!same)throw std::runtime_error("Client-style image readback differs");
    }
    std::vector<uint8_t> pixels() {
        std::vector<uint8_t> result(64*48*4,0x55);renderer->readBuffer(buffer,result);return result;
    }
};
static void transfer(Device& source,Device& target,bool forceCpu,bool optimize=false) {
    source.pattern(false);target.pattern(true);
    const auto expected=source.pixels();
    source.checkTextureReadback(expected);
    if(expected==target.pixels())throw std::runtime_error("Test patterns must differ");
    auto route=prepareTransferInput(source.buffer,target.renderer,forceCpu);
    static TransferStats stats;
    auto original=+[](CDRMRenderer* r,CSharedPointer<IBuffer> a,CSharedPointer<IBuffer> b,CSharedPointer<CDRMRenderer> s,int fd){return r->blit(a,b,s,fd);};
    auto result=optimize?optimizedTransfer(target.renderer.get(),source.buffer,target.buffer,source.renderer,-1,original,true,stats):original(target.renderer.get(),source.buffer,target.buffer,source.renderer,-1);
    if(optimize && !stats.mapped)throw std::runtime_error("Mapped destination path was not exercised");
    if(optimize)std::cout<<"PASS: mapped destination path exercised, frames="<<stats.mapped<<"\n";
    if(!result.success)throw std::runtime_error("GPU transfer failed");
    if(result.syncFD) {pollfd pfd{*result.syncFD,POLLIN,0};if(poll(&pfd,1,1000)<=0 || !(pfd.revents&POLLIN))throw std::runtime_error("Transfer fence timed out");}
    else {CEglContextGuard guard(*target.renderer);Native::waitIdle();}
    const auto actual=target.pixels();
    if(expected!=actual) {
        size_t mismatch=0;for(size_t i=0;i<actual.size();++i)mismatch+=actual[i]!=expected[i];
        throw std::runtime_error("Cross-GPU pixel mismatch: "+std::to_string(mismatch));
    }
    std::cout<<"PASS: "<<(route==TransferMode::CpuReadback?"CPU-staged":"direct")<<" copy; all "<<actual.size()<<" bytes match\n";
}
static void sharedLinearTransfer(Device& source,Device& target) {
    auto implementation=dynamicPointerCast<CNullBackend>(target.backend->getImplementations().front());
    auto chain=CSwapchain::create(target.allocator,implementation);
    SSwapchainOptions config;config.length=2;config.size={64,48};config.format=DRM_FORMAT_XRGB8888;config.multigpu=true;config.scanout=true;
    if(!chain->reconfigure(config))throw std::runtime_error("Cannot allocate shared linear test buffers");
    auto buffer=chain->next(nullptr);auto attrs=buffer->dmabuf();
    if(attrs.modifier!=DRM_FORMAT_MOD_LINEAR)throw std::runtime_error("Shared test allocator did not honor LINEAR");
    const bool supported=std::ranges::any_of(source.renderer->formats,[&](const auto& f){return f.drmFormat==attrs.format && f.modifier==attrs.modifier && !f.external;});
    if(!supported) {std::cout<<"SKIP: primary GPU does not advertise renderable destination LINEAR buffers\n";return;}
    auto saved=source.buffer;source.buffer=buffer;
    transfer(source,target,false);
    auto attachment=buffer->attachments.get<CDRMRendererBufferInputAttachment>();
    if(!attachment || !attachment->intermediateBuf.empty())throw std::runtime_error("Shared linear path unexpectedly used CPU readback");
    source.buffer=saved;
    std::cout<<"PASS: primary GPU rendered destination-owned LINEAR memory; local copy without CPU readback\n";
}
int main(int argc,char** argv) {
    if(argc!=3){std::cerr<<"usage: transfer-test SOURCE_RENDER_NODE DESTINATION_RENDER_NODE\n";return 2;}
    try {
        // NULL backend has no session, output, modeset, Wayland connection or IPC.
        Native::CurrentGuard guard;
        Device source(argv[1]),target(argv[2]);
        transfer(source,target,false);transfer(source,target,true);transfer(target,source,false);transfer(target,source,true);
        sharedLinearTransfer(source,target);
        transfer(source,target,true,true);transfer(source,target,true,true);
        {
            Native::CurrentGuard restore;
            eglMakeCurrent(source.renderer->egl.display,EGL_NO_SURFACE,EGL_NO_SURFACE,EGL_NO_CONTEXT);
            eglDestroyContext(source.renderer->egl.display,source.renderer->egl.context);
            if(transferRendererValid(source.renderer))throw std::runtime_error("Destroyed test context must fail validation");
            recoverTransferRenderer(source.backend,source.allocator->drmFD(),source.renderer);
            if(!transferRendererValid(source.renderer))throw std::runtime_error("Recovered transfer context is invalid");
        }
        transfer(source,target,false);
        std::cout<<"PASS: recovery of an invalid transfer EGL context\n";
        std::cout<<"PASS: bidirectional transfer without exporting any buffer to a compositor\n";
    } catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
