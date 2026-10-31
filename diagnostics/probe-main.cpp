#include "../probe.hpp"
#include <iostream>
int main() {
    try {
        const auto roles=GpuSwitch::discoverRoles();
        const auto live=GpuSwitch::scan(roles);
        const auto target=GpuSwitch::choose(live,"");
        std::cout << GpuSwitch::Json{{"gpus",GpuSwitch::describe(live,roles)},
                                   {"desired",target?GpuSwitch::Json(*target):GpuSwitch::Json(nullptr)}}.dump(2) << '\n';
    } catch (const std::exception& e) { std::cerr<<e.what()<<'\n';return 1; }
}
