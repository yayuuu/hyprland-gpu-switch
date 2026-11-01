#pragma once
#include <hyprutils/memory/SharedPtr.hpp>
#include <memory>
#include <string>
#include <vector>
#include "diagnostics/diagnostics.hpp"
namespace Aquamarine {class IAllocator;class IBuffer;}
namespace Monitor {class CMonitor;}
namespace GpuSwitch {
#if GPU_SWITCH_DIAGNOSTICS
std::string transferContextStatus();
#endif
class DrmRoutes {
    struct State;
    std::unique_ptr<State> state_;
  public:
    explicit DrmRoutes(const std::string& target);
    ~DrmRoutes();
    DrmRoutes(const DrmRoutes&)=delete;
    DrmRoutes& operator=(const DrmRoutes&)=delete;
    Hyprutils::Memory::CSharedPointer<Aquamarine::IAllocator> allocator() const;
    std::vector<Hyprutils::Memory::CSharedPointer<Aquamarine::IBuffer>> renderBuffers() const;
    void validateTransfers();
    void commit();
    int primaryFD() const;
    void beforeCommit(Monitor::CMonitor* monitor);
    void update(Monitor::CMonitor* monitor);
    void retireDisconnected();
};
}
