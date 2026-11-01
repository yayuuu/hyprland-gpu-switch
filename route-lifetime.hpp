#pragma once
#include <algorithm>
#include <vector>
#include <hyprutils/memory/WeakPtr.hpp>

namespace GpuSwitch {
// Routing must not keep a compositor monitor alive after output destruction.
// A late wl_output bind must see an expired monitor, not one with m_output=null.
template<typename Monitor> using RouteMonitorRef = Hyprutils::Memory::CWeakPointer<Monitor>;

template<typename Routes> void retireDisconnectedRoutes(Routes& routes) {
    std::erase_if(routes, [](const auto& route) {
        const auto monitor=route.monitor.lock();
        return !monitor || !monitor->m_output || monitor->m_output.get()!=route.output.get();
    });
}
}
