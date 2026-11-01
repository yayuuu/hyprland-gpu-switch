#include "native.hpp"
#include <iostream>
#include <stdexcept>
#include <cstring>
using namespace GpuSwitch::Native;
int main(int argc,char** argv) {
    if (argc!=3) {std::cerr<<"usage: native-test SOURCE_RENDER_NODE TARGET_RENDER_NODE\n";return 2;}
    try {
        CurrentGuard guard;
        Context source(argv[1]),target(argv[2]);source.current();
        TextureData data;data.width=16;data.height=16;data.params={GL_CLAMP_TO_EDGE,GL_CLAMP_TO_EDGE,GL_NEAREST,GL_NEAREST,GL_RED,GL_GREEN,GL_BLUE,GL_ALPHA};
        data.pixels.resize(16*16*4);
        for (size_t i=0;i<data.pixels.size();++i) data.pixels[i]=static_cast<std::byte>((i*37)%251);
        GLuint old=uploadTexture(data);waitIdle();
        const auto metadata=readTexture(old,GL_TEXTURE_2D,false);
        if(!metadata.discard || !metadata.pixels.empty() || metadata.width!=data.width)throw std::runtime_error("Metadata query must not read pending client pixels");
        auto snapshot=readTexture(old,GL_TEXTURE_2D);target.current();
        GLuint migrated=uploadTexture(snapshot);waitIdle();
        auto result=readTexture(migrated,GL_TEXTURE_2D);
        if (result.pixels!=snapshot.pixels) throw std::runtime_error("Migrated texture contents differ");
        auto changed=snapshot;
        for(auto& pixel:changed.pixels)pixel^=std::byte{0x5a};
        if(uploadTexture(changed,migrated)!=migrated)throw std::runtime_error("Reusable upload changed texture identity");
        waitIdle();
        if(readTexture(migrated,GL_TEXTURE_2D).pixels!=changed.pixels)throw std::runtime_error("Reusable upload retained stale pixels");
        auto resized=changed;resized.width=8;resized.height=8;resized.pixels.resize(8*8*4);
        uploadTexture(resized,migrated);waitIdle();
        auto resizedResult=readTexture(migrated,GL_TEXTURE_2D);
        if(resizedResult.width!=8 || resizedResult.height!=8 || resizedResult.pixels!=resized.pixels)throw std::runtime_error("Reusable upload failed to resize storage");
        uploadTexture(snapshot,migrated);waitIdle();
        std::cout<<"PASS: reusable texture upload, pixel refresh and storage resize\n";
        auto returnData=readTexture(migrated,GL_TEXTURE_2D);source.current();
        auto returned=uploadTexture(returnData);waitIdle();
        if (readTexture(returned,GL_TEXTURE_2D).pixels!=data.pixels) throw std::runtime_error("Return migration differs");
        glDeleteTextures(1,&old);glDeleteTextures(1,&returned);target.current();glDeleteTextures(1,&migrated);
        source.current();
        TextureData lut;lut.target=GL_TEXTURE_3D;lut.internal=GL_RGBA16F;lut.format=GL_RGBA;lut.type=GL_FLOAT;
        lut.width=lut.height=lut.depth=4;lut.params={GL_CLAMP_TO_EDGE,GL_CLAMP_TO_EDGE,GL_NEAREST,GL_NEAREST,GL_RED,GL_GREEN,GL_BLUE,GL_ALPHA,GL_CLAMP_TO_EDGE};
        std::vector<float> values(4*4*4*4);for(size_t i=0;i<values.size();++i)values[i]=static_cast<float>(i%32)/32;
        lut.pixels.resize(values.size()*sizeof(float));std::memcpy(lut.pixels.data(),values.data(),lut.pixels.size());
        GLuint sourceLut=uploadTexture(lut);waitIdle();auto lutPixels=readTexture(sourceLut,GL_TEXTURE_3D);
        if(lutPixels.type!=GL_HALF_FLOAT && lutPixels.type!=GL_FLOAT)throw std::runtime_error("Float16 LUT readback lost floating-point precision");
        const size_t lutPixelBytes=lutPixels.type==GL_HALF_FLOAT?8:16;
        if(lutPixels.pixels.size()!=4*4*4*lutPixelBytes)throw std::runtime_error("Incorrect float16 LUT staging size");
        target.current();GLuint targetLut=uploadTexture(lutPixels);waitIdle();
        if(readTexture(targetLut,GL_TEXTURE_3D).pixels!=lutPixels.pixels)throw std::runtime_error("3D LUT differs after migration");
        glDeleteTextures(1,&targetLut);source.current();glDeleteTextures(1,&sourceLut);
        std::cout<<"PASS: metadata-only allocation and float16 3D LUT migration\n";
        source.current();
        TextureData packed;packed.internal=GL_RGB10_A2;packed.format=GL_RGBA;packed.type=GL_UNSIGNED_INT_2_10_10_10_REV;
        packed.width=packed.height=8;packed.params=data.params;
        std::vector<uint32_t> tenBits(64);for(size_t i=0;i<tenBits.size();++i)tenBits[i]=(3u<<30)|((static_cast<uint32_t>(i)*7u&1023u)<<20)|((static_cast<uint32_t>(i)*11u&1023u)<<10)|(static_cast<uint32_t>(i)*13u&1023u);
        packed.pixels.resize(tenBits.size()*sizeof(uint32_t));std::memcpy(packed.pixels.data(),tenBits.data(),packed.pixels.size());
        GLuint source10=uploadTexture(packed);waitIdle();auto hdrPixels=readTexture(source10,GL_TEXTURE_2D);
        target.current();GLuint target10=uploadTexture(hdrPixels);waitIdle();
        if(readTexture(target10,GL_TEXTURE_2D).pixels!=hdrPixels.pixels)throw std::runtime_error("10-bit texture differs after migration");
        glDeleteTextures(1,&target10);source.current();glDeleteTextures(1,&source10);
        std::cout<<"PASS: packed 10-bit texture migration\n";
        std::cout<<"PASS: "<<source.renderer<<" -> "<<target.renderer<<" -> "<<source.renderer<<"; texture contents preserved\n";
    } catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
