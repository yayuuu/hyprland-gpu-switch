#pragma once
#include <GLES3/gl32.h>
namespace GpuSwitch {
// Renderer caches are per-context even though the renderer object is shared.
// Query after making the resulting context current, including rollback.
template<class Renderer> void synchronizeGLStateCache(Renderer& gl) {
    GLint value=0;
    glGetIntegerv(GL_ACTIVE_TEXTURE,&value);gl.m_activeTexture=value;
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING,&value);gl.m_boundArrayBuffer=value;
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING,&value);gl.m_boundDrawFB=value;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING,&value);gl.m_boundReadFB=value;
    glGetIntegerv(GL_BLEND_SRC_RGB,&value);gl.m_blendSFactor=value;
    glGetIntegerv(GL_BLEND_DST_RGB,&value);gl.m_blendDFactor=value;
}
}
