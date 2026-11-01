// Standalone benchmark of the client bridge's synchronous readback/upload path.
// Uses render-node contexts only; does not connect to Wayland or change outputs.
#include "../native.hpp"
#include <algorithm>
#include <chrono>
#include <iostream>
#include <vector>
#include <cstring>
using namespace GpuSwitch::Native;
using Clock = std::chrono::steady_clock;
static double ms(Clock::time_point a, Clock::time_point b) { return std::chrono::duration<double,std::milli>(b-a).count(); }
static double percentile(std::vector<double> values, double p) {
    std::sort(values.begin(),values.end());return values[static_cast<size_t>((values.size()-1)*p)];
}
int main(int argc,char** argv) {
    if(argc<3 || argc>5){std::cerr<<"usage: client-copy-bench SOURCE_RENDER_NODE TARGET_RENDER_NODE [reuse] [half]\n";return 2;}
    try {
        CurrentGuard guard;Context source(argv[1]),target(argv[2]);
        std::cout<<source.renderer<<" -> "<<target.renderer<<'\n';
        for(auto [width,height]:std::vector<std::pair<int,int>>{{1920,1080},{2227,1416},{3440,1440}}) {
            TextureData initial;initial.width=width;initial.height=height;
            initial.params={GL_CLAMP_TO_EDGE,GL_CLAMP_TO_EDGE,GL_LINEAR,GL_LINEAR,GL_RED,GL_GREEN,GL_BLUE,GL_ALPHA,GL_CLAMP_TO_EDGE};
            initial.pixels.resize(static_cast<size_t>(width)*height*4,std::byte{83});
            if(argc==5) {
                initial.internal=GL_RGBA16F;initial.type=GL_FLOAT;
                std::vector<float> values(static_cast<size_t>(width)*height*4,0.25f);
                initial.pixels.resize(values.size()*sizeof(float));
                std::memcpy(initial.pixels.data(),values.data(),initial.pixels.size());
            }
            source.current();GLuint original=uploadTexture(initial);waitIdle();
            const bool reuse=argc>=4;GLuint reusable=0;
            size_t transferred=0;
            std::vector<double> reads,uploads,totals;
            for(int i=0;i<18;++i) {
                target.current();auto begin=Clock::now();waitIdle();
                source.current();auto readStart=Clock::now();auto data=readTexture(original,GL_TEXTURE_2D);auto readEnd=Clock::now();transferred=data.pixels.size();
                target.current();auto uploadStart=Clock::now();GLuint copy=uploadTexture(data,reuse?reusable:0);
                if(reuse)reusable=copy;waitIdle();auto end=Clock::now();
                if(!reuse)glDeleteTextures(1,&copy);
                if(i>=3){reads.push_back(ms(readStart,readEnd));uploads.push_back(ms(uploadStart,end));totals.push_back(ms(begin,end));}
            }
            if(reusable){target.current();glDeleteTextures(1,&reusable);}
            source.current();glDeleteTextures(1,&original);
            std::cout<<width<<'x'<<height<<" reuse="<<reuse<<" bytes="<<transferred
                     <<" read_median_ms="<<percentile(reads,.5)<<" upload_complete_median_ms="<<percentile(uploads,.5)
                     <<" total_median_ms="<<percentile(totals,.5)<<" total_p95_ms="<<percentile(totals,.95)<<'\n';
        }
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
