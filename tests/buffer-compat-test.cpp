#include "buffer-compat.hpp"
#include <stdexcept>
#include <iostream>
using namespace GpuSwitch;
static void check(bool ok) {if(!ok) throw std::runtime_error("buffer compatibility test failed");}
int main() {
    constexpr uint32_t xr24=0x34325258;
    constexpr uint64_t intelTiled=0x100000000000008,nvidiaTiled=0x300000000606014,invalid=(1ULL<<56)-1;
    const std::vector<OutputFormat> intel={{xr24,{0,intelTiled}}};
    const std::vector<OutputFormat> nvidia={{xr24,{nvidiaTiled}}};
    check(outputAccepts(xr24,0,intel));
    check(outputAccepts(xr24,intelTiled,intel));
    check(!outputAccepts(xr24,nvidiaTiled,intel));
    check(outputAccepts(xr24,nvidiaTiled,nvidia));
    check(!outputAccepts(xr24,0,nvidia));
    check(!outputAccepts(xr24,invalid,intel));
    check(!outputAccepts(xr24,0,{}));
    check(!outputAccepts(xr24+1,0,intel));
    std::cout<<"PASS: reject allocator fallback to incompatible modifiers, unknown layouts, unknown outputs and unsupported formats\n";
}
