#pragma once
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <set>
#include <string>
#include <vector>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <linux/sync_file.h>
#include <linux/dma-buf.h>
#include <cctype>
#include <sys/sysmacros.h>

namespace Offload {
struct GPUDevice {
    std::string node, id, driver;
    uint32_t renderMajor=0, renderMinor=0;
    std::array<uint32_t,4> pci{};
};
inline std::string exporterDriver(std::string name) {
    std::transform(name.begin(),name.end(),name.begin(),[](unsigned char c){return std::tolower(c);});
    if(name=="nvidia-drm" || name=="nv_drm")return "nvidia";
    return name;
}
inline std::optional<GPUDevice> selectProducer(const std::string& exporter, const std::vector<GPUDevice>& devices, const std::set<std::string>& clientDevices={}) {
    if(exporter.empty())return std::nullopt;
    std::vector<GPUDevice> matches;
    for(const auto& device:devices)if(exporter=="drm" || exporterDriver(device.driver)==exporterDriver(exporter))matches.push_back(device);
    if(matches.size()==1 && exporter!="drm")return matches.front();
    std::erase_if(matches,[&](const auto& device){return !clientDevices.contains(device.id);});
    return matches.size()==1?std::optional(matches.front()):std::nullopt;
}
inline std::optional<GPUDevice> fenceProducer(int fd,const std::vector<GPUDevice>& devices) {
    if(fd<0)return std::nullopt;
    sync_file_info info{};if(ioctl(fd,SYNC_IOC_FILE_INFO,&info) || !info.num_fences || info.num_fences>64)return std::nullopt;
    std::vector<sync_fence_info> fences(info.num_fences);info.sync_fence_info=reinterpret_cast<uint64_t>(fences.data());
    if(ioctl(fd,SYNC_IOC_FILE_INFO,&info) || info.num_fences>fences.size())return std::nullopt;
    std::optional<GPUDevice> producer;
    for(size_t i=0;i<info.num_fences;++i) {
        const auto length=strnlen(fences[i].driver_name,sizeof(fences[i].driver_name));
        const auto candidate=selectProducer(std::string(fences[i].driver_name,length),devices);
        if(!candidate || (producer && producer->id!=candidate->id))return std::nullopt;
        producer=candidate;
    }
    return producer;
}
inline std::string deviceID(dev_t dev) {
    std::error_code error;
    const auto path=std::filesystem::canonical("/sys/dev/char/"+std::to_string(major(dev))+":"+std::to_string(minor(dev))+"/device",error);
    return error?std::string{}:path.filename().string();
}
inline const std::vector<GPUDevice>& gpuDevices() {
    static std::vector<GPUDevice> cache;
    static std::chrono::steady_clock::time_point refreshed{};
    const auto now=std::chrono::steady_clock::now();
    if(!cache.empty() && now-refreshed<std::chrono::seconds(1))return cache;
    std::vector<GPUDevice> devices;std::error_code error;
    for(const auto& entry:std::filesystem::directory_iterator("/sys/class/drm",error)) {
        const auto name=entry.path().filename().string();
        if(!name.starts_with("renderD"))continue;
        GPUDevice d;d.node="/dev/dri/"+name;struct stat st{};
        if(stat(d.node.c_str(),&st))continue;
        d.id=deviceID(st.st_rdev);if(d.id.empty())continue;
        d.renderMajor=major(st.st_rdev);d.renderMinor=minor(st.st_rdev);
        auto driver=std::filesystem::canonical(entry.path()/"device/driver",error);
        if(error){error.clear();continue;}d.driver=driver.filename().string();
        unsigned domain,bus,device,function;
        if(std::sscanf(d.id.c_str(),"%x:%x:%x.%x",&domain,&bus,&device,&function)==4)d.pci={domain,bus,device,function};
        devices.push_back(std::move(d));
    }
    cache=std::move(devices);refreshed=now;return cache;
}
inline std::optional<GPUDevice> rendererDevice(int fd) {
    struct stat st{};if(fstat(fd,&st))return std::nullopt;
    const auto id=deviceID(st.st_rdev);
    for(const auto& device:gpuDevices())if(device.id==id)return device;
    return std::nullopt;
}
inline std::optional<GPUDevice> producerDevice(int fd, pid_t clientPID, int acquireFence=-1) {
    std::ifstream info("/proc/self/fdinfo/"+std::to_string(fd));std::string exporter;
    for(std::string line;std::getline(info,line);)if(line.starts_with("exp_name:")) {
        auto pos=line.find_first_not_of(" \t",9);if(pos!=std::string::npos)exporter=line.substr(pos);
    }
    auto found=selectProducer(exporter,gpuDevices());if(found)return found;
    if(auto fence=fenceProducer(acquireFence,gpuDevices()))return fence;
    dma_buf_export_sync_file sync{};sync.flags=DMA_BUF_SYNC_READ;sync.fd=-1;
    if(!ioctl(fd,DMA_BUF_IOCTL_EXPORT_SYNC_FILE,&sync) && sync.fd>=0) {
        auto fence=fenceProducer(sync.fd,gpuDevices());close(sync.fd);
        if(fence)return fence;
    }
    if(exporter.empty())return std::nullopt;
    // Disambiguate multiple GPUs using the same exporter only when the actual
    // Wayland client has DRM descriptors for exactly one matching device.
    std::set<std::string> ids;std::error_code error;
    if(clientPID>0)for(const auto& entry:std::filesystem::directory_iterator("/proc/"+std::to_string(clientPID)+"/fdinfo",error)) {
        struct stat st{};
        const auto fdPath=std::filesystem::path("/proc")/std::to_string(clientPID)/"fd"/entry.path().filename();
        if(!stat(fdPath.c_str(),&st) && S_ISCHR(st.st_mode)) {
            const auto id=deviceID(st.st_rdev);if(!id.empty())ids.insert(id);
        }
        std::ifstream stream(entry.path());
        for(std::string line;std::getline(stream,line);)if(line.starts_with("drm-pdev:")) {
            auto pos=line.find_first_not_of(" \t",9);if(pos!=std::string::npos)ids.insert(line.substr(pos));
        }
    }
    return selectProducer(exporter,gpuDevices(),ids);
}
}
