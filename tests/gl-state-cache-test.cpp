#include "../gl-state-cache.hpp"
#include "../native.hpp"
#include <iostream>
#include <stdexcept>
struct Cache {
 GLenum m_activeTexture=GL_TEXTURE0;GLuint m_boundArrayBuffer=0,m_boundDrawFB=0,m_boundReadFB=0;
 GLenum m_blendSFactor=GL_ONE,m_blendDFactor=GL_ONE_MINUS_SRC_ALPHA;
 void blend(GLenum src,GLenum dst){if(src==m_blendSFactor && dst==m_blendDFactor)return;glBlendFunc(src,dst);m_blendSFactor=src;m_blendDFactor=dst;}
};
static void require(bool ok,const char* error){if(!ok)throw std::runtime_error(error);}
static GLuint shader(GLenum kind,const char* source){auto id=glCreateShader(kind);glShaderSource(id,1,&source,nullptr);glCompileShader(id);GLint ok=0;glGetShaderiv(id,GL_COMPILE_STATUS,&ok);require(ok,"shader compilation");return id;}
int main(int argc,char** argv){try{
 require(argc==3,"usage: gl-state-cache-test SOURCE_NODE TARGET_NODE");
 GpuSwitch::Native::Context source(argv[1]),target(argv[2]);target.current();
 GLuint tex=0,fbo=0;glGenTextures(1,&tex);glBindTexture(GL_TEXTURE_2D,tex);glTexStorage2D(GL_TEXTURE_2D,1,GL_RGBA8,1,1);
 glGenFramebuffers(1,&fbo);glBindFramebuffer(GL_FRAMEBUFFER,fbo);glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,tex,0);
 require(glCheckFramebufferStatus(GL_FRAMEBUFFER)==GL_FRAMEBUFFER_COMPLETE,"framebuffer");
 auto vs=shader(GL_VERTEX_SHADER,"#version 300 es\nvoid main(){vec2 p=vec2((gl_VertexID<<1)&2,gl_VertexID&2);gl_Position=vec4(p*2.-1.,0.,1.);}");
 auto fs=shader(GL_FRAGMENT_SHADER,"#version 300 es\nprecision highp float;out vec4 c;void main(){c=vec4(0.,0.,0.,0.5);}");
 auto program=glCreateProgram();glAttachShader(program,vs);glAttachShader(program,fs);glLinkProgram(program);GLint linked=0;glGetProgramiv(program,GL_LINK_STATUS,&linked);require(linked,"program link");glUseProgram(program);
 glViewport(0,0,1,1);glEnable(GL_BLEND);glBlendFunc(GL_ONE,GL_ZERO);Cache cache;
 auto draw=[&]{glClearColor(1,1,1,1);glClear(GL_COLOR_BUFFER_BIT);cache.blend(GL_ONE,GL_ONE_MINUS_SRC_ALPHA);glDrawArrays(GL_TRIANGLES,0,3);unsigned char pixel[4]{};glReadPixels(0,0,1,1,GL_RGBA,GL_UNSIGNED_BYTE,pixel);return pixel[0];};
 require(draw()==0,"stale blend cache must reproduce opaque black pixels");
 GLuint buffer=0;glGenBuffers(1,&buffer);glBindBuffer(GL_ARRAY_BUFFER,buffer);glActiveTexture(GL_TEXTURE2);
 GpuSwitch::synchronizeGLStateCache(cache);
 require(cache.m_boundDrawFB==fbo && cache.m_boundReadFB==fbo && cache.m_boundArrayBuffer==buffer && cache.m_activeTexture==GL_TEXTURE2,"binding caches mismatch");
 auto pixel=draw();require(pixel>=126 && pixel<=129,"repaired cache must preserve half-transparent shadow/cursor pixels");
 source.current();GpuSwitch::synchronizeGLStateCache(cache);
 require(cache.m_blendSFactor==GL_ONE && cache.m_blendDFactor==GL_ZERO && cache.m_boundDrawFB==0 && cache.m_boundArrayBuffer==0 && cache.m_activeTexture==GL_TEXTURE0,"return-context cache mismatch");
 target.current();glDeleteProgram(program);glDeleteShader(vs);glDeleteShader(fs);glDeleteBuffers(1,&buffer);glDeleteFramebuffers(1,&fbo);glDeleteTextures(1,&tex);
 std::cout<<"PASS: reproduced opaque alpha; repaired actual blended pixels and binding caches across GPU contexts\n";
 }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
