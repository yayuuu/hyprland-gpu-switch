#include "../transfer/gpu_route.hpp"
#include <iostream>
#include <stdexcept>
using namespace Offload;
static void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
int main(){
    GPUDevice intel{.node="",.id="0000:00:02.0",.driver="i915"};
    GPUDevice nvidia{.node="",.id="0000:01:00.0",.driver="nvidia"};
    GPUDevice other{.node="",.id="0000:02:00.0",.driver="nvidia"};
    std::vector<GPUDevice> pair{intel,nvidia};
    require(selectProducer("i915",pair)->id==intel.id,"Intel producer was misidentified");
    require(selectProducer("nvidia-drm",pair)->id==nvidia.id,"NVIDIA exporter alias failed");
    require(selectProducer("nv_drm",pair)->id==nvidia.id,"Legacy NVIDIA exporter alias failed");
    require(!selectProducer("",pair),"Unknown provenance must not select a GPU");
    require(!selectProducer("amdgpu",pair),"Unmatched exporter must not select a GPU");
    require(!selectProducer("drm",pair),"Generic DRM exporter must not imply a vendor");
    require(selectProducer("drm",pair,{nvidia.id})->id==nvidia.id,"Generic DRM exporter needs an unambiguous client GPU");
    pair.push_back(other);
    require(!selectProducer("nvidia-drm",pair),"Ambiguous exporters must not select a GPU");
    require(selectProducer("nvidia-drm",pair,{other.id})->id==other.id,"Client DRM device did not disambiguate producer");
    require(!selectProducer("nvidia-drm",pair,{other.id,nvidia.id}),"Multiple matching client devices must remain ambiguous");
    require(!selectProducer("nvidia-drm",pair,{intel.id}),"Unrelated client GPU must not identify producer");
    std::cout<<"PASS: exporter aliases, missing/unknown provenance and multiple-GPU ambiguity\n";
    for(const auto& d:gpuDevices())std::cout<<d.node<<' '<<d.id<<' '<<d.driver<<' '<<d.renderMajor<<':'<<d.renderMinor<<'\n';
}
