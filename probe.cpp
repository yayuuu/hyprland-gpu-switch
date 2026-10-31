#include "probe.hpp"
#include <vulkan/vulkan.h>
#include <fstream>
#include <format>
#include <stdexcept>
#include <cstring>
#include <regex>
namespace fs = std::filesystem;
namespace GpuSwitch {
std::string deviceId(const fs::path& p) {
    std::error_code ec;
    auto path = fs::canonical(p, ec);
    return ec ? std::string{} : path.string();
}
Roles discoverRoles() {
    VkApplicationInfo app{.sType=VK_STRUCTURE_TYPE_APPLICATION_INFO,
                          .pApplicationName="hyprland-gpu-switch-probe", .apiVersion=VK_API_VERSION_1_1};
    VkInstanceCreateInfo info{.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo=&app};
    VkInstance instance = VK_NULL_HANDLE;
    if (vkCreateInstance(&info, nullptr, &instance) != VK_SUCCESS)
        throw std::runtime_error("Cannot enumerate Vulkan devices; refusing to guess GPU roles");
    struct Guard { VkInstance i; ~Guard() { vkDestroyInstance(i,nullptr); } } guard{instance};
    uint32_t count=0;
    if (vkEnumeratePhysicalDevices(instance,&count,nullptr) != VK_SUCCESS)
        throw std::runtime_error("Vulkan device enumeration failed");
    std::vector<VkPhysicalDevice> devices(count);
    if (vkEnumeratePhysicalDevices(instance,&count,devices.data()) != VK_SUCCESS)
        throw std::runtime_error("Vulkan device list changed; retry the probe");
    devices.resize(count);
    Roles roles;
    for (auto device : devices) {
        uint32_t n=0;
        if (vkEnumerateDeviceExtensionProperties(device,nullptr,&n,nullptr) != VK_SUCCESS) continue;
        std::vector<VkExtensionProperties> extensions(n);
        if (vkEnumerateDeviceExtensionProperties(device,nullptr,&n,extensions.data()) != VK_SUCCESS) continue;
        extensions.resize(n);
        const auto supports = [&](const char* name) {
            return std::ranges::any_of(extensions,[&](const auto& e) { return std::strcmp(e.extensionName,name)==0; });
        };
        VkPhysicalDeviceDrmPropertiesEXT drm{.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRM_PROPERTIES_EXT};
        VkPhysicalDevicePCIBusInfoPropertiesEXT pci{.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PCI_BUS_INFO_PROPERTIES_EXT};
        VkPhysicalDeviceProperties2 props{.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        if (supports(VK_EXT_PHYSICAL_DEVICE_DRM_EXTENSION_NAME)) { drm.pNext=props.pNext;props.pNext=&drm; }
        if (supports(VK_EXT_PCI_BUS_INFO_EXTENSION_NAME)) { pci.pNext=props.pNext;props.pNext=&pci; }
        vkGetPhysicalDeviceProperties2(device,&props);
        Role role=Role::Unknown;
        if (props.properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU) role=Role::Integrated;
        if (props.properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) role=Role::Discrete;
        std::string id;
        if (drm.hasPrimary) id=deviceId(std::format("/sys/dev/char/{}:{}/device",drm.primaryMajor,drm.primaryMinor));
        if (id.empty() && supports(VK_EXT_PCI_BUS_INFO_EXTENSION_NAME))
            id=deviceId(std::format("/sys/bus/pci/devices/{:04x}:{:02x}:{:02x}.{}",pci.pciDomain,pci.pciBus,pci.pciDevice,pci.pciFunction));
        if (!id.empty() && role != Role::Unknown) roles[id]={role,props.properties.deviceName};
    }
    return roles;
}
std::vector<Gpu> scan(const Roles& roles, const fs::path& root) {
    std::vector<Gpu> gpus;
    // Any read failure invalidates the whole sample: never interpret an incomplete
    // directory walk as a disconnection or reset the renderer using stale data.
    for (const auto& entry : fs::directory_iterator(root)) {
        auto name=entry.path().filename().string();
        if (!std::regex_match(name,std::regex("card[0-9]+"))) continue;
        auto id=deviceId(entry.path()/"device");
        if (id.empty()) throw std::runtime_error("DRM device vanished during scan");
        Gpu g{.id=id,.card="/dev/dri/"+name};
        if (auto it=roles.find(id);it!=roles.end()) g.role=it->second.role;
        for (const auto& connector : fs::directory_iterator(root)) {
            const auto c=connector.path().filename().string();
            if (!c.starts_with(name+"-")) continue;
            std::ifstream input(connector.path()/"status");
            std::string status;
            if (!(input>>status)) throw std::runtime_error("DRM connector status unavailable");
            if (status=="connected") g.connected.push_back(c.substr(name.size()+1));
            else if (status!="disconnected" && status!="unknown") throw std::runtime_error("Unexpected DRM connector status");
        }
        std::ranges::sort(g.connected);
        gpus.push_back(std::move(g));
    }
    std::ranges::sort(gpus,{},&Gpu::id);
    return gpus;
}
GpuSwitch::Json describe(const std::vector<Gpu>& gpus,const Roles& roles) {
    auto result=GpuSwitch::Json::array();
    for (const auto& g : gpus) {
        auto it=roles.find(g.id);
        result.push_back({{"id",g.id},{"card",g.card},{"role",g.role==Role::Integrated?"integrated":g.role==Role::Discrete?"discrete":"unknown"},
                          {"name",it==roles.end()?"":it->second.name},{"connected",g.connected}});
    }
    return result;
}
}
