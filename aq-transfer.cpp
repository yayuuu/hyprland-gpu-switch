#include <bits/stdc++.h>
#include <aquamarine/backend/DRM.hpp>
#include "vendor/aquamarine/Renderer.hpp"
#include "aq-transfer.hpp"
#include "native.hpp"
#include <unistd.h>
using namespace Hyprutils::Memory;
using namespace Aquamarine;
using namespace GpuSwitch;
TransferMode GpuSwitch::prepareTransferInput(const CSharedPointer<IBuffer>& buffer,const CSharedPointer<CDRMRenderer>& renderer,bool forceCpu) {
    if(!buffer || !renderer)throw std::runtime_error("Missing GPU transfer buffer or renderer");
    const auto attrs=buffer->dmabuf();
    if(!attrs.success)throw std::runtime_error("Transfer source is not a valid DMA-BUF");
    const bool imports=attrs.modifier!=DRM_FORMAT_MOD_INVALID && std::ranges::any_of(renderer->formats,[&](const auto& f) {
        return f.drmFormat==attrs.format && f.modifier==attrs.modifier;
    });
    if(imports && !forceCpu)return TransferMode::DirectImport;
    if(attrs.format!=DRM_FORMAT_XRGB8888 && attrs.format!=DRM_FORMAT_ARGB8888 && attrs.format!=DRM_FORMAT_XBGR8888 && attrs.format!=DRM_FORMAT_ABGR8888)
        throw std::runtime_error("Lossless CPU copy for this HDR/wide format is not implemented");
    auto existing=buffer->attachments.get<CDRMRendererBufferInputAttachment>();
    if(existing && existing->renderer==renderer && !existing->intermediateBuf.empty())return TransferMode::CpuReadback;
    const auto w=static_cast<size_t>(attrs.size.x),h=static_cast<size_t>(attrs.size.y);
    if(!w || !h || w>16384 || h>16384 || w*h>128ULL*1024*1024/4)throw std::runtime_error("GPU copy exceeds staging limit");
    CEglContextGuard context(*renderer);
    CGLTex texture;texture.target=GL_TEXTURE_2D;
    glGenTextures(1,&texture.texid);
    glBindTexture(GL_TEXTURE_2D,texture.texid);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
    glBindTexture(GL_TEXTURE_2D,0);Native::check("prepare source-safe CPU copy");
    buffer->attachments.add(makeShared<CDRMRendererBufferInputAttachment>(renderer,std::move(texture),std::vector<uint8_t>(w*h*4)));
    return TransferMode::CpuReadback;
}

bool GpuSwitch::transferRendererValid(const CSharedPointer<CDRMRenderer>& renderer) {
    if(!renderer || !renderer->egl.display || !renderer->egl.context)return false;
    EGLint version=0;
    const bool valid=eglQueryContext(renderer->egl.display,renderer->egl.context,EGL_CONTEXT_CLIENT_VERSION,&version)==EGL_TRUE;
    eglGetError();
    return valid && version>=2;
}
void GpuSwitch::recoverTransferRenderer(const CSharedPointer<CBackend>& backend,int renderFD,CSharedPointer<CDRMRenderer>& renderer) {
    if(transferRendererValid(renderer))return;
    Native::CurrentGuard restore;
    if(renderer) {
        // Do not let a stale renderer terminate a display used by the compositor,
        // or delete numeric GL names in another still-live context.
        if(renderer->egl.lastBlitSyncFD>=0)close(renderer->egl.lastBlitSyncFD);
        renderer->egl.lastBlitSyncFD=-1;renderer->egl.lastBlitSync=nullptr;
        renderer->egl.display=EGL_NO_DISPLAY;renderer->egl.context=EGL_NO_CONTEXT;
        renderer->shader.program=renderer->shader.shaderVao=renderer->shader.shaderVboPos=renderer->shader.shaderVboUv=0;
        renderer->shaderExt.program=renderer->shaderExt.shaderVao=renderer->shaderExt.shaderVboPos=renderer->shaderExt.shaderVboUv=0;
        renderer.reset();
    }
    renderer=CDRMRenderer::attempt(backend,renderFD);
    if(!renderer)throw std::runtime_error("Cannot recreate GPU transfer context");
    renderer->self=renderer;
    if(!transferRendererValid(renderer))throw std::runtime_error("Recreated GPU transfer context is invalid");
}
