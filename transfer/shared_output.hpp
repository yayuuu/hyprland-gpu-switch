#pragma once
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl32.h>
#include <GLES2/gl2ext.h>
#include <array>
#include <stdexcept>
#include <vector>

namespace Offload {
inline bool externalOutput(EGLDisplay display,uint32_t format) {
    auto query=reinterpret_cast<PFNEGLQUERYDMABUFMODIFIERSEXTPROC>(eglGetProcAddress("eglQueryDmaBufModifiersEXT"));
    EGLint count=0;if(!query || !query(display,format,0,nullptr,nullptr,&count) || count<=0)return false;
    std::vector<EGLuint64KHR> mods(count);std::vector<EGLBoolean> external(count);
    if(!query(display,format,count,mods.data(),external.data(),&count))return false;
    for(int i=0;i<count;++i)if(mods[i]==0)return external[i];
    return false;
}
// Owns only the external import/sampling objects. The ordinary destination
// texture remains core-owned and can be migrated with the compositor.
class SharedOutput {
    EGLDisplay display=EGL_NO_DISPLAY;EGLContext context=EGL_NO_CONTEXT;
    EGLImageKHR image=EGL_NO_IMAGE_KHR;GLuint texture=0,program=0,fbo=0,vao=0;
    unsigned width=0,height=0;
    static GLuint shader(GLenum type,const char* code) {
        GLuint sh=glCreateShader(type);glShaderSource(sh,1,&code,nullptr);glCompileShader(sh);GLint ok=0;glGetShaderiv(sh,GL_COMPILE_STATUS,&ok);
        if(!ok){glDeleteShader(sh);throw std::runtime_error("shared output shader compilation failed");}return sh;
    }
  public:
    SharedOutput()=default;SharedOutput(const SharedOutput&)=delete;SharedOutput& operator=(const SharedOutput&)=delete;
    ~SharedOutput(){reset();}
    void reset() noexcept {
        auto currentDisplay=eglGetCurrentDisplay();auto currentContext=eglGetCurrentContext();auto draw=eglGetCurrentSurface(EGL_DRAW),read=eglGetCurrentSurface(EGL_READ);
        if(display!=EGL_NO_DISPLAY) {
            if(eglMakeCurrent(display,EGL_NO_SURFACE,EGL_NO_SURFACE,context)) {
                if(texture)glDeleteTextures(1,&texture);
                if(program)glDeleteProgram(program);
                if(fbo)glDeleteFramebuffers(1,&fbo);
                if(vao)glDeleteVertexArrays(1,&vao);
            }
            auto destroy=reinterpret_cast<PFNEGLDESTROYIMAGEKHRPROC>(eglGetProcAddress("eglDestroyImageKHR"));if(image && destroy)destroy(display,image);
            if(currentDisplay!=EGL_NO_DISPLAY)eglMakeCurrent(currentDisplay,draw,read,currentContext);
            else eglMakeCurrent(display,EGL_NO_SURFACE,EGL_NO_SURFACE,EGL_NO_CONTEXT);
        }
        display=EGL_NO_DISPLAY;context=EGL_NO_CONTEXT;image=EGL_NO_IMAGE_KHR;texture=program=fbo=vao=0;
    }
    bool current()const{return display==eglGetCurrentDisplay() && context==eglGetCurrentContext();}
    void import(unsigned w,unsigned h,uint32_t format,int fd,uint32_t offset,uint32_t stride) {
        reset();display=eglGetCurrentDisplay();context=eglGetCurrentContext();width=w;height=h;
        GLint active=0,oldTexture=0;glGetIntegerv(GL_ACTIVE_TEXTURE,&active);glActiveTexture(GL_TEXTURE0);glGetIntegerv(GL_TEXTURE_BINDING_EXTERNAL_OES,&oldTexture);
        struct Restore{GLint active,texture;~Restore(){glBindTexture(GL_TEXTURE_EXTERNAL_OES,texture);glActiveTexture(active);}}restore{active,oldTexture};
        const EGLint attrs[]{EGL_WIDTH,EGLint(w),EGL_HEIGHT,EGLint(h),EGL_LINUX_DRM_FOURCC_EXT,EGLint(format),EGL_DMA_BUF_PLANE0_FD_EXT,fd,EGL_DMA_BUF_PLANE0_OFFSET_EXT,EGLint(offset),EGL_DMA_BUF_PLANE0_PITCH_EXT,EGLint(stride),EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT,0,EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT,0,EGL_NONE};
        auto create=reinterpret_cast<PFNEGLCREATEIMAGEKHRPROC>(eglGetProcAddress("eglCreateImageKHR"));auto bind=reinterpret_cast<PFNGLEGLIMAGETARGETTEXTURE2DOESPROC>(eglGetProcAddress("glEGLImageTargetTexture2DOES"));
        if(!create || !bind)throw std::runtime_error("external output import unavailable");
        image=create(display,EGL_NO_CONTEXT,EGL_LINUX_DMA_BUF_EXT,nullptr,attrs);
        if(image==EGL_NO_IMAGE_KHR)throw std::runtime_error("external output image import failed");
        glGenTextures(1,&texture);glBindTexture(GL_TEXTURE_EXTERNAL_OES,texture);glTexParameteri(GL_TEXTURE_EXTERNAL_OES,GL_TEXTURE_MIN_FILTER,GL_NEAREST);glTexParameteri(GL_TEXTURE_EXTERNAL_OES,GL_TEXTURE_MAG_FILTER,GL_NEAREST);glTexParameteri(GL_TEXTURE_EXTERNAL_OES,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);glTexParameteri(GL_TEXTURE_EXTERNAL_OES,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);bind(GL_TEXTURE_EXTERNAL_OES,image);
        if(glGetError()!=GL_NO_ERROR)throw std::runtime_error("external output texture import failed");
        const char* vs="#version 320 es\nvoid main(){vec2 p=vec2((gl_VertexID<<1)&2,gl_VertexID&2);gl_Position=vec4(p*2.0-1.0,0,1);}";
        const char* fs="#version 320 es\n#extension GL_OES_EGL_image_external_essl3 : require\nprecision highp float;uniform highp samplerExternalOES source;uniform vec2 extent;layout(location=0) out vec4 color;void main(){color=texture(source,gl_FragCoord.xy/extent);}";
        auto vertex=shader(GL_VERTEX_SHADER,vs);GLuint fragment=0;
        try{fragment=shader(GL_FRAGMENT_SHADER,fs);}catch(...){glDeleteShader(vertex);throw;}
        program=glCreateProgram();glAttachShader(program,vertex);glAttachShader(program,fragment);glLinkProgram(program);glDeleteShader(vertex);glDeleteShader(fragment);GLint ok=0;glGetProgramiv(program,GL_LINK_STATUS,&ok);
        if(!ok)throw std::runtime_error("external output program link failed");
        glGenFramebuffers(1,&fbo);glGenVertexArrays(1,&vao);
    }
    void copy(GLuint destination) {
        if(!current())throw std::runtime_error("external output belongs to another renderer");
        GLint draw=0,read=0,oldProgram=0,active=0,oldTexture=0,oldVAO=0;std::array<GLint,4> viewport{};std::array<GLboolean,4> mask{};
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING,&draw);glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING,&read);glGetIntegerv(GL_CURRENT_PROGRAM,&oldProgram);glGetIntegerv(GL_ACTIVE_TEXTURE,&active);glGetIntegerv(GL_VIEWPORT,viewport.data());glGetBooleanv(GL_COLOR_WRITEMASK,mask.data());glGetIntegerv(GL_VERTEX_ARRAY_BINDING,&oldVAO);
        glActiveTexture(GL_TEXTURE0);glGetIntegerv(GL_TEXTURE_BINDING_EXTERNAL_OES,&oldTexture);
        const std::array<GLenum,7> capabilities{GL_BLEND,GL_SCISSOR_TEST,GL_DEPTH_TEST,GL_STENCIL_TEST,GL_CULL_FACE,GL_DITHER,GL_RASTERIZER_DISCARD};std::array<GLboolean,7> enabled{};
        for(size_t i=0;i<capabilities.size();++i){enabled[i]=glIsEnabled(capabilities[i]);glDisable(capabilities[i]);}
        struct Restore {
            GLint draw,read,program,active,texture,vao;std::array<GLint,4> viewport;std::array<GLboolean,4> mask;decltype(capabilities)& caps;decltype(enabled)& enabledFlags;
            ~Restore(){glBindFramebuffer(GL_DRAW_FRAMEBUFFER,draw);glBindFramebuffer(GL_READ_FRAMEBUFFER,read);glUseProgram(program);glBindVertexArray(vao);glViewport(viewport[0],viewport[1],viewport[2],viewport[3]);glColorMask(mask[0],mask[1],mask[2],mask[3]);glBindTexture(GL_TEXTURE_EXTERNAL_OES,texture);glActiveTexture(active);for(size_t i=0;i<caps.size();++i)if(enabledFlags[i])glEnable(caps[i]);}
        }restore{draw,read,oldProgram,active,oldTexture,oldVAO,viewport,mask,capabilities,enabled};
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER,fbo);glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,destination,0);
        if(glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER)!=GL_FRAMEBUFFER_COMPLETE)throw std::runtime_error("shared output target framebuffer incomplete");
        glViewport(0,0,width,height);glColorMask(GL_TRUE,GL_TRUE,GL_TRUE,GL_TRUE);glBindVertexArray(vao);glUseProgram(program);glBindTexture(GL_TEXTURE_EXTERNAL_OES,texture);glUniform1i(glGetUniformLocation(program,"source"),0);glUniform2f(glGetUniformLocation(program,"extent"),width,height);glDrawArrays(GL_TRIANGLES,0,3);
        if(glGetError()!=GL_NO_ERROR)throw std::runtime_error("shared output GPU copy failed");
    }
};
}
