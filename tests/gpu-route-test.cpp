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
    ClientProducerProbe probe;
    auto now=std::chrono::steady_clock::time_point{};
    unsigned scans=0;
    std::set<std::string> clientIDs{intel.id,nvidia.id};
    auto scan=[&](pid_t){++scans;return clientIDs;};
    require(!probe.select("drm",pair,100,now,scan) && scans==1,"Ambiguous snapshot must not select a producer");
    require(!probe.select("drm",pair,100,now+std::chrono::milliseconds(99),scan) && scans==1,"Repeated failed scans must be throttled");
    clientIDs={nvidia.id};
    require(probe.select("drm",pair,100,now+std::chrono::milliseconds(100),scan)->id==nvidia.id && scans==2,"New client device state must be retried after backoff");
    clientIDs={intel.id};
    require(probe.select("drm",pair,100,now+std::chrono::milliseconds(101),scan)->id==intel.id && scans==3,"A successful result must not leave stale positive identity cached");
    require(probe.select("drm",pair,200,now,scan)->id==intel.id && scans==4,"Other clients must remain independent");
    require(!probe.select("drm",pair,0,now,scan) && scans==4,"Missing client identity must not scan a process");
    std::cout<<"PASS: exporter aliases, provenance ambiguity, bounded failed-scan retries and client GPU changes\n";
    for(const auto& d:gpuDevices())std::cout<<d.node<<' '<<d.id<<' '<<d.driver<<' '<<d.renderMajor<<':'<<d.renderMinor<<'\n';
}
