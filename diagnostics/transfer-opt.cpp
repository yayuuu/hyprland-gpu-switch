#include <bits/stdc++.h>
#include "transfer-opt.hpp"
#include "../native.hpp"
#include <poll.h>
#include <aquamarine/allocator/GBM.hpp>
using namespace Aquamarine;
using namespace Hyprutils::Memory;
using namespace GpuSwitch;
CDRMRenderer::SBlitResult GpuSwitch::optimizedTransfer(CDRMRenderer* renderer,CSharedPointer<IBuffer> from,CSharedPointer<IBuffer> to,CSharedPointer<CDRMRenderer> source,int waitFD,BlitFunction original,bool enabled,TransferStats& stats) {
    const auto start=std::chrono::steady_clock::now();
    struct Clock {TransferStats& s;std::chrono::steady_clock::time_point t;~Clock(){++s.frames;s.nanoseconds+=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-t).count();}} clock{stats,start};
    auto attachment=from?from->attachments.get<CDRMRendererBufferInputAttachment>():nullptr;
    if(!enabled || !source || !attachment || attachment->renderer!=renderer->self || attachment->intermediateBuf.empty())return original(renderer,from,to,source,waitFD);
    // Wait before the source readback, rather than only queuing a wait in the
    // destination GPU context. Keep the original ownership of the fence FD.
    if(waitFD>=0) {pollfd fd{waitFD,POLLIN,0};int rc;do{rc=poll(&fd,1,1000);}while(rc<0 && errno==EINTR);if(rc<=0 || !(fd.revents&POLLIN))return {};}
    try {
        Native::CurrentGuard restore;
        source->readBuffer(from,attachment->intermediateBuf);
        // GBM mapping respects the destination tiling and completes CPU writes
        // on unmap. No destination GL draw/upload is required on this path.
        if(to && dynamic_cast<CGBMBuffer*>(to.get())) {
            auto dst=to->dmabuf();auto src=from->dmabuf();
            const bool bgr=dst.format==DRM_FORMAT_XRGB8888 || dst.format==DRM_FORMAT_ARGB8888;
            const bool rgb=dst.format==DRM_FORMAT_XBGR8888 || dst.format==DRM_FORMAT_ABGR8888;
            if((bgr || rgb) && src.size==dst.size) {
                auto [pixels,format,bytes]=to->beginDataPtr(2 /* GBM_BO_TRANSFER_WRITE */);
                struct Unmap {IBuffer* b;~Unmap(){b->endDataPtr();}} unmap{to.get()};
                const size_t w=dst.size.x,h=dst.size.y;
                if(pixels && format==dst.format && h && bytes%h==0 && bytes/h>=w*4 && attachment->intermediateBuf.size()==w*h*4) {
                    const size_t stride=bytes/h;
                    for(size_t y=0;y<h;++y) {
                        auto input=attachment->intermediateBuf.data()+y*w*4;auto output=pixels+y*stride;
                        if(rgb)memcpy(output,input,w*4);
                        else for(size_t x=0;x<w;++x) {output[x*4]=input[x*4+2];output[x*4+1]=input[x*4+1];output[x*4+2]=input[x*4];output[x*4+3]=input[x*4+3];}
                    }
                    ++stats.optimized;++stats.mapped;stats.bytes+=w*h*4;
                    return {.success=true,.syncFD=std::nullopt};
                }
            }
        }
        CEglContextGuard context(*renderer);
        auto& tex=*attachment->tex;
        glBindTexture(GL_TEXTURE_2D,tex.texid);
        const auto attrs=from->dmabuf();
        if(!tex.image) {
            glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,attrs.size.x,attrs.size.y,0,GL_RGBA,GL_UNSIGNED_BYTE,attachment->intermediateBuf.data());
            const EGLint options[]={EGL_GL_TEXTURE_LEVEL_KHR,0,EGL_IMAGE_PRESERVED_KHR,EGL_TRUE,EGL_NONE};
            tex.image=renderer->proc.eglCreateImageKHR(renderer->egl.display,renderer->egl.context,EGL_GL_TEXTURE_2D_KHR,reinterpret_cast<EGLClientBuffer>(static_cast<uintptr_t>(tex.texid)),options);
            ++stats.allocations;
            if(!tex.image) {eglGetError();return original(renderer,from,to,source,waitFD);}
        } else glTexSubImage2D(GL_TEXTURE_2D,0,0,0,attrs.size.x,attrs.size.y,GL_RGBA,GL_UNSIGNED_BYTE,attachment->intermediateBuf.data());
        Native::check("reuse transfer texture storage");
    } catch(...) {return original(renderer,from,to,source,waitFD);}
    // A real EGL image marks the already-populated texture valid to Aquamarine.
    // Hide CPU staging only for this call, restoring it even on exceptions.
    auto staging=std::move(attachment->intermediateBuf);
    struct Restore {decltype(attachment)& a;std::vector<uint8_t>& bytes;~Restore(){a->intermediateBuf=std::move(bytes);}} restore{attachment,staging};
    ++stats.optimized;stats.bytes+=staging.size();
    return original(renderer,from,to,source,waitFD);
}
