#include "route-plan.hpp"
#include <iostream>
using namespace GpuSwitch;
static void check(bool ok) {if(!ok)throw std::runtime_error("route test failed");}
int main() {
    auto r=planRoutes({"integrated","discrete"},"discrete");
    check(r[0].parent=="discrete" && r[1].parent.empty());
    r=planRoutes({"integrated","discrete"},"integrated");
    check(r[0].parent.empty() && r[1].parent=="integrated");
    r=planRoutes({"a","b","c"},"c");
    check(r[0].parent=="c" && r[1].parent=="c" && r[2].parent.empty());
    bool rejected=false;try{planRoutes({"integrated"},"discrete");}catch(...){rejected=true;}check(rejected);
    rejected=false;try{planRoutes({"a","a"},"a");}catch(...){rejected=true;}check(rejected);
    std::cout<<"PASS: primary routing, reverse routing, multiple GPUs and sudden target removal\n";
}
