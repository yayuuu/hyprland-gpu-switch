#pragma once
namespace GpuSwitch {
// A route outlives secondary renderer resources across DPMS. Restore the
// backend first, then replace chains belonging to its previous allocator.
template<typename Ready, typename Initialize, typename Validate, typename Matches, typename Replace>
void restoreRouteResources(Ready ready, Initialize initialize, Validate validate, Matches matches, Replace replace) {
    if(!ready())initialize();
    validate();
    if(!matches())replace();
}
}
