#include "native.hpp"
#include <stdexcept>
#include <format>
#include <fstream>
#include <filesystem>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#include <algorithm>
namespace fs=std::filesystem;
namespace GpuSwitch::Native {
static constexpr std::array<GLenum,9> parameters={GL_TEXTURE_WRAP_S,GL_TEXTURE_WRAP_T,GL_TEXTURE_MAG_FILTER,GL_TEXTURE_MIN_FILTER,
                                                 GL_TEXTURE_SWIZZLE_R,GL_TEXTURE_SWIZZLE_G,GL_TEXTURE_SWIZZLE_B,GL_TEXTURE_SWIZZLE_A,GL_TEXTURE_WRAP_R};
void check(const char* operation) {
    const auto error=glGetError();
    if (error!=GL_NO_ERROR) throw std::runtime_error(std::format("{}: GL error 0x{:x}",operation,error));
}
CurrentGuard::~CurrentGuard() {
    if (!active_) return;
    if (display_!=EGL_NO_DISPLAY) eglMakeCurrent(display_,draw_,read_,context_);
    else if (auto display=eglGetCurrentDisplay();display!=EGL_NO_DISPLAY)
        eglMakeCurrent(display,EGL_NO_SURFACE,EGL_NO_SURFACE,EGL_NO_CONTEXT);
}
static std::string idFromNode(const char* path) {
    if (!path) return {};
    struct stat st{};
    if (stat(path,&st)!=0 || !S_ISCHR(st.st_mode)) return {};
    std::error_code error;
    auto result=fs::canonical(std::format("/sys/dev/char/{}:{}/device",major(st.st_rdev),minor(st.st_rdev)),error);
    return error?std::string{}:result.string();
}
Context::Context(const std::string& renderNode) {
    const auto id=idFromNode(renderNode.c_str());
    if (id.empty()) throw std::runtime_error("Target render node unavailable");
    auto query=reinterpret_cast<PFNEGLQUERYDEVICESEXTPROC>(eglGetProcAddress("eglQueryDevicesEXT"));
    auto queryString=reinterpret_cast<PFNEGLQUERYDEVICESTRINGEXTPROC>(eglGetProcAddress("eglQueryDeviceStringEXT"));
    auto platform=reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(eglGetProcAddress("eglGetPlatformDisplayEXT"));
    if (!query || !queryString || !platform) throw std::runtime_error("EGL device enumeration unavailable");
    EGLint count=0;
    if (!query(0,nullptr,&count)) throw std::runtime_error("EGL enumeration failed");
    std::vector<EGLDeviceEXT> devices(count);
    if (!query(count,devices.data(),&count)) throw std::runtime_error("EGL enumeration changed");
    devices.resize(count);
    for (auto d:devices) {
        auto extensions=queryString(d,EGL_EXTENSIONS);
        if (!extensions) continue;
        std::string ext=extensions;
        const char* node=nullptr;
        if (ext.contains("EGL_EXT_device_drm_render_node")) node=queryString(d,EGL_DRM_RENDER_NODE_FILE_EXT);
        if (!node && ext.contains("EGL_EXT_device_drm")) node=queryString(d,EGL_DRM_DEVICE_FILE_EXT);
        if (idFromNode(node)==id) {device=d;break;}
    }
    if (device==EGL_NO_DEVICE_EXT) throw std::runtime_error("No matching EGL device for target render node");
    fd=open(renderNode.c_str(),O_RDWR|O_CLOEXEC);
    if (fd<0) throw std::runtime_error("Cannot open target render node");
    CurrentGuard restore;
    try {
        display=platform(EGL_PLATFORM_DEVICE_EXT,device,nullptr);
        EGLint major=0,minor=0;
        if (display==EGL_NO_DISPLAY || !eglInitialize(display,&major,&minor)) throw std::runtime_error("Cannot initialize target EGL display");
        if (!eglBindAPI(EGL_OPENGL_ES_API)) throw std::runtime_error("Cannot bind OpenGL ES");
        const EGLint attrs[]={EGL_CONTEXT_MAJOR_VERSION,3,EGL_CONTEXT_MINOR_VERSION,2,EGL_NONE};
        context=eglCreateContext(display,EGL_NO_CONFIG_KHR,EGL_NO_CONTEXT,attrs);
        if (context==EGL_NO_CONTEXT) throw std::runtime_error("Target does not support a no-config OpenGL ES 3.2 context");
        current();
        const auto name=glGetString(GL_RENDERER);
        if (!name) throw std::runtime_error("Target renderer is unavailable");
        renderer=reinterpret_cast<const char*>(name);
    } catch (...) {
        if (context!=EGL_NO_CONTEXT) eglDestroyContext(display,context);
        close(fd);fd=-1;throw;
    }
}
Context::~Context() {
    if (context!=EGL_NO_CONTEXT) {
        if (eglGetCurrentContext()==context) eglMakeCurrent(display,EGL_NO_SURFACE,EGL_NO_SURFACE,EGL_NO_CONTEXT);
        eglDestroyContext(display,context);
    }
    // Displays can be shared with Aquamarine's secondary renderer. Never terminate
    // a display owned by another component; contexts and fds are owned here.
    if (fd>=0) close(fd);
}
void Context::current() const {
    if (!eglMakeCurrent(display,EGL_NO_SURFACE,EGL_NO_SURFACE,context)) throw std::runtime_error("Cannot activate target EGL context");
}
void waitIdle(std::chrono::milliseconds timeout) {
    auto fence=glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE,0);
    if (!fence) throw std::runtime_error("Cannot create migration fence");
    glFlush();
    auto start=std::chrono::steady_clock::now();
    while (true) {
        auto result=glClientWaitSync(fence,0,10000000);
        if (result==GL_ALREADY_SIGNALED || result==GL_CONDITION_SATISFIED) break;
        if (result==GL_WAIT_FAILED || std::chrono::steady_clock::now()-start>=timeout) {
            glDeleteSync(fence);throw std::runtime_error("GPU fence failed or timed out; migration cancelled");
        }
    }
    glDeleteSync(fence);
}
TextureData readTexture(GLuint texture,GLenum target,bool contents) {
    if (target!=GL_TEXTURE_2D && target!=GL_TEXTURE_3D) throw std::runtime_error("Unsupported texture target");
    TextureData result;result.target=target;
    GLint binding=0,readFB=0,pack=0,pbo=0,row=0,skipPixels=0,skipRows=0;
    glGetIntegerv(target==GL_TEXTURE_2D?GL_TEXTURE_BINDING_2D:GL_TEXTURE_BINDING_3D,&binding);
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING,&readFB);glGetIntegerv(GL_PACK_ALIGNMENT,&pack);
    glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING,&pbo);glGetIntegerv(GL_PACK_ROW_LENGTH,&row);
    glGetIntegerv(GL_PACK_SKIP_PIXELS,&skipPixels);glGetIntegerv(GL_PACK_SKIP_ROWS,&skipRows);
    struct Restore {
        GLenum target;GLint binding,fb,pack,pbo,row,skipPixels,skipRows;GLuint temp=0;
        ~Restore() {
            if(temp)glDeleteFramebuffers(1,&temp);
            glBindFramebuffer(GL_READ_FRAMEBUFFER,fb);glBindTexture(target,binding);
            glBindBuffer(GL_PIXEL_PACK_BUFFER,pbo);glPixelStorei(GL_PACK_ALIGNMENT,pack);
            glPixelStorei(GL_PACK_ROW_LENGTH,row);glPixelStorei(GL_PACK_SKIP_PIXELS,skipPixels);glPixelStorei(GL_PACK_SKIP_ROWS,skipRows);
        }
    } restore{target,binding,readFB,pack,pbo,row,skipPixels,skipRows};
    glBindTexture(target,texture);
    glGetTexLevelParameteriv(target,0,GL_TEXTURE_WIDTH,&result.width);
    glGetTexLevelParameteriv(target,0,GL_TEXTURE_HEIGHT,&result.height);
    GLint internal=0;glGetTexLevelParameteriv(target,0,GL_TEXTURE_INTERNAL_FORMAT,&internal);result.internal=internal;
    if(result.internal==GL_RGB)result.internal=GL_RGB8;
    else if(result.internal==GL_RGBA || result.internal==GL_BGRA_EXT)result.internal=GL_RGBA8;
    if (target==GL_TEXTURE_3D) glGetTexLevelParameteriv(target,0,GL_TEXTURE_DEPTH,&result.depth);
    for (size_t i=0;i<parameters.size();++i) { if(i==8 && target!=GL_TEXTURE_3D){result.params[i]=GL_CLAMP_TO_EDGE;continue;} glGetTexParameteriv(target,parameters[i],&result.params[i]); }
    check("query texture");
    if (!result.width || !result.height) {result.discard=true;return result;}
    // Depth/stencil is transient workspace state and is cleared before drawing.
    if (result.internal==GL_DEPTH24_STENCIL8) {
        result.discard=true;result.format=GL_DEPTH_STENCIL;result.type=GL_UNSIGNED_INT_24_8;return result;
    }
    size_t bytesPerPixel=4;
    if (result.internal==GL_RGBA16F || result.internal==GL_RGBA32F || result.internal==GL_RGB16F || result.internal==GL_RGB32F || result.internal==GL_R16F || result.internal==GL_R32F) {
        result.format=GL_RGBA;result.type=GL_FLOAT;bytesPerPixel=16;
    } else if (result.internal==GL_RGB10_A2) {result.format=GL_RGBA;result.type=GL_UNSIGNED_INT_2_10_10_10_REV;}
    else if (result.internal==GL_RGBA8 || result.internal==GL_RGB8 || result.internal==GL_RGBA || result.internal==GL_BGRA_EXT || result.internal==GL_R8) {
        result.format=GL_RGBA;result.type=GL_UNSIGNED_BYTE;
    } else throw std::runtime_error(std::format("Texture internal format 0x{:x} cannot be migrated yet",result.internal));
    if(!contents) {result.discard=true;return result;}
    // Preserve half-float storage rather than expanding every channel to float32.
    // Use only the readback pair explicitly advertised for this framebuffer.
    glGenFramebuffers(1,&restore.temp);glBindFramebuffer(GL_READ_FRAMEBUFFER,restore.temp);
    if(target==GL_TEXTURE_2D)glFramebufferTexture2D(GL_READ_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,target,texture,0);
    else glFramebufferTextureLayer(GL_READ_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,texture,0,0);
    if(glCheckFramebufferStatus(GL_READ_FRAMEBUFFER)!=GL_FRAMEBUFFER_COMPLETE)throw std::runtime_error("Texture cannot be read back for migration");
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    if(result.internal==GL_RGBA16F || result.internal==GL_RGB16F || result.internal==GL_R16F) {
        GLint readFormat=0,readType=0;
        glGetIntegerv(GL_IMPLEMENTATION_COLOR_READ_FORMAT,&readFormat);
        glGetIntegerv(GL_IMPLEMENTATION_COLOR_READ_TYPE,&readType);
        check("query native framebuffer readback format");
        if(readFormat==GL_RGBA && readType==GL_HALF_FLOAT) {
            result.type=GL_HALF_FLOAT;bytesPerPixel=8;
        }
    }
    size_t size=static_cast<size_t>(result.width)*result.height*result.depth*bytesPerPixel;
    if (size>512ULL*1024*1024) throw std::runtime_error("Texture migration exceeds 512 MiB limit");
    result.pixels.resize(size);
    glBindBuffer(GL_PIXEL_PACK_BUFFER,0);glPixelStorei(GL_PACK_ALIGNMENT,1);glPixelStorei(GL_PACK_ROW_LENGTH,0);
    glPixelStorei(GL_PACK_SKIP_PIXELS,0);glPixelStorei(GL_PACK_SKIP_ROWS,0);
    for (int layer=0;layer<result.depth;++layer) {
        if (target==GL_TEXTURE_2D) glFramebufferTexture2D(GL_READ_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,target,texture,0);
        else glFramebufferTextureLayer(GL_READ_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,texture,0,layer);
        if (glCheckFramebufferStatus(GL_READ_FRAMEBUFFER)!=GL_FRAMEBUFFER_COMPLETE) throw std::runtime_error("Texture cannot be read back for migration");
        glReadBuffer(GL_COLOR_ATTACHMENT0);
        glReadPixels(0,0,result.width,result.height,result.format,result.type,result.pixels.data()+static_cast<size_t>(layer)*result.width*result.height*bytesPerPixel);
        check("read texture");
    }
    return result;
}
GLuint uploadTexture(const TextureData& data, GLuint reusableTexture) {
    GLint pbo=0,alignment=0,row=0,skipPixels=0,skipRows=0,imageHeight=0,skipImages=0,binding=0;
    glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING,&pbo);glGetIntegerv(GL_UNPACK_ALIGNMENT,&alignment);
    glGetIntegerv(GL_UNPACK_ROW_LENGTH,&row);glGetIntegerv(GL_UNPACK_SKIP_PIXELS,&skipPixels);glGetIntegerv(GL_UNPACK_SKIP_ROWS,&skipRows);
    glGetIntegerv(GL_UNPACK_IMAGE_HEIGHT,&imageHeight);glGetIntegerv(GL_UNPACK_SKIP_IMAGES,&skipImages);
    glGetIntegerv(data.target==GL_TEXTURE_2D?GL_TEXTURE_BINDING_2D:GL_TEXTURE_BINDING_3D,&binding);
    struct Restore {
        GLenum target;GLint pbo,alignment,row,skipPixels,skipRows,imageHeight,skipImages,binding;
        ~Restore() {
            glBindBuffer(GL_PIXEL_UNPACK_BUFFER,pbo);glPixelStorei(GL_UNPACK_ALIGNMENT,alignment);glPixelStorei(GL_UNPACK_ROW_LENGTH,row);
            glPixelStorei(GL_UNPACK_SKIP_PIXELS,skipPixels);glPixelStorei(GL_UNPACK_SKIP_ROWS,skipRows);
            glPixelStorei(GL_UNPACK_IMAGE_HEIGHT,imageHeight);glPixelStorei(GL_UNPACK_SKIP_IMAGES,skipImages);glBindTexture(target,binding);
        }
    } restore{data.target,pbo,alignment,row,skipPixels,skipRows,imageHeight,skipImages,binding};
    GLuint texture=reusableTexture;
    if(!texture)glGenTextures(1,&texture);
    try {
        glBindTexture(data.target,texture);glBindBuffer(GL_PIXEL_UNPACK_BUFFER,0);
        glPixelStorei(GL_UNPACK_ALIGNMENT,1);glPixelStorei(GL_UNPACK_ROW_LENGTH,0);glPixelStorei(GL_UNPACK_SKIP_PIXELS,0);glPixelStorei(GL_UNPACK_SKIP_ROWS,0);
        glPixelStorei(GL_UNPACK_IMAGE_HEIGHT,0);glPixelStorei(GL_UNPACK_SKIP_IMAGES,0);
        if (data.width && data.height) {
            GLint width=0,height=0,depth=1,internal=0;
            if(reusableTexture) {
                glGetTexLevelParameteriv(data.target,0,GL_TEXTURE_WIDTH,&width);
                glGetTexLevelParameteriv(data.target,0,GL_TEXTURE_HEIGHT,&height);
                glGetTexLevelParameteriv(data.target,0,GL_TEXTURE_INTERNAL_FORMAT,&internal);
                if(data.target==GL_TEXTURE_3D)glGetTexLevelParameteriv(data.target,0,GL_TEXTURE_DEPTH,&depth);
            }
            const bool reuse=reusableTexture && width==data.width && height==data.height && depth==data.depth && internal==static_cast<GLint>(data.internal);
            if(reuse) {
                if(!data.discard) {
                    if(data.target==GL_TEXTURE_2D)glTexSubImage2D(data.target,0,0,0,data.width,data.height,data.format,data.type,data.pixels.data());
                    else glTexSubImage3D(data.target,0,0,0,0,data.width,data.height,data.depth,data.format,data.type,data.pixels.data());
                }
            } else {
                if(data.target==GL_TEXTURE_2D)glTexImage2D(data.target,0,data.internal,data.width,data.height,0,data.format,data.type,data.discard?nullptr:data.pixels.data());
                else glTexImage3D(data.target,0,data.internal,data.width,data.height,data.depth,0,data.format,data.type,data.discard?nullptr:data.pixels.data());
            }
        }
        for (size_t i=0;i<parameters.size();++i) {if(i==8 && data.target!=GL_TEXTURE_3D)continue;glTexParameteri(data.target,parameters[i],data.params[i]);}
        check("upload migrated texture");return texture;
    } catch (...) {if(!reusableTexture)glDeleteTextures(1,&texture);throw;}
}
}
