#pragma once
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl32.h>
#include <GLES2/gl2ext.h>
#include <string>
#include <vector>
#include <array>
#include <cstddef>
#include <chrono>
namespace GpuSwitch::Native {
class Context {
  public:
    EGLDisplay display=EGL_NO_DISPLAY;
    EGLContext context=EGL_NO_CONTEXT;
    EGLDeviceEXT device=EGL_NO_DEVICE_EXT;
    int fd=-1;
    std::string renderer;
    explicit Context(const std::string& renderNode);
    ~Context();
    Context(const Context&)=delete;
    Context& operator=(const Context&)=delete;
    void current() const;
};
class CurrentGuard {
    bool active_=true;
    EGLDisplay display_=eglGetCurrentDisplay();
    EGLContext context_=eglGetCurrentContext();
    EGLSurface draw_=eglGetCurrentSurface(EGL_DRAW),read_=eglGetCurrentSurface(EGL_READ);
  public:
    ~CurrentGuard();
    void dismiss() { active_=false; }
};
struct TextureData {
    GLenum target=GL_TEXTURE_2D,internal=GL_RGBA8,format=GL_RGBA,type=GL_UNSIGNED_BYTE;
    int width=0,height=0,depth=1;
    bool discard=false;
    std::array<GLint,9> params{};
    std::vector<std::byte> pixels;
};
TextureData readTexture(GLuint texture,GLenum target,bool contents=true);
GLuint uploadTexture(const TextureData&, GLuint reusableTexture=0);
void waitIdle(std::chrono::milliseconds timeout=std::chrono::milliseconds(750));
void check(const char* operation);
}
