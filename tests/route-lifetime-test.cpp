#include "route-lifetime.hpp"
#include <hyprutils/memory/SharedPtr.hpp>
#include <iostream>
#include <stdexcept>
#include <vector>
using namespace Hyprutils::Memory;
struct Output {};
struct Monitor { CSharedPointer<Output> m_output; bool dpms=true; };
struct Route { GpuSwitch::RouteMonitorRef<Monitor> monitor; CSharedPointer<Output> output; };
static void check(bool ok){if(!ok)throw std::runtime_error("route lifetime regression");}
int main(){
    auto output=makeShared<Output>();auto monitor=makeShared<Monitor>();monitor->m_output=output;
    CWeakPointer<Monitor> advertisedMonitor=monitor;
    std::vector<Route> routes{{monitor,output}};
    monitor->dpms=false;
    GpuSwitch::retireDisconnectedRoutes(routes);check(routes.size()==1);
    // Core disconnect clears m_output while transient owners still exist.
    monitor->m_output.reset();
    GpuSwitch::retireDisconnectedRoutes(routes);check(routes.empty());
    monitor->m_output=output;routes.push_back({monitor,output});
    // The route itself must never extend the lifetime of an advertised monitor.
    monitor.reset();check(advertisedMonitor.expired());
    GpuSwitch::retireDisconnectedRoutes(routes);check(routes.empty());
    monitor=makeShared<Monitor>();monitor->m_output=makeShared<Output>();
    routes.push_back({monitor,output});
    GpuSwitch::retireDisconnectedRoutes(routes);check(routes.empty());
    std::cout<<"PASS: DPMS preserves live routes; disconnect, monitor destruction and replaced outputs retire stale routes without retaining monitors\n";
}
