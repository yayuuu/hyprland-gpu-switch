#include "policy.hpp"
#include <iostream>
#include <stdexcept>
using namespace GpuSwitch;
static void check(bool ok) { if (!ok) throw std::runtime_error("policy test failed"); }
int main() {
    std::vector<Gpu> g{{"i","/dev/dri/card9",Role::Integrated,{"eDP-1"}},
                       {"d","/dev/dri/card2",Role::Discrete,{"DP-8"}}};
    check(choose(g,"i")=="d");
    check(choose(g,"d")=="d");
    g[1].connected.clear(); check(choose(g,"d")=="i");
    g[1].connected={"HDMI-A-1"};check(choose(g,"i")=="d");
    Plan plan{choose(g,"i"),4}; check(valid(plan,g,"i",4));
    g[1].connected.clear();check(!valid(plan,g,"i",4));
    g[1].connected={"DP-8"};check(!valid(plan,g,"i",5));
    g.push_back({"x","/dev/dri/card3",Role::Discrete,{"DP-1"}});
    check(choose(g,"x")=="x");
    g[0].connected.clear();g[1].connected.clear();g[2].connected.clear();
    check(choose(g,"d")=="i");
    g[0].role=Role::Unknown;check(!choose(g,"d"));
    check(!choose({},"d"));
    g={{"other","/dev/dri/card0",Role::Unknown,{"DP-1"}}};check(!choose(g,"other"));
    std::cout<<"GPU selection, unknown roles, multiple dGPUs, port changes, abrupt removal and stale generation: PASS\n";
}
