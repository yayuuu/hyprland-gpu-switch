#pragma once
#include <string>
#include <vector>
#include <stdexcept>
namespace GpuSwitch {
struct RouteNode {std::string id,parent;};
inline std::vector<RouteNode> planRoutes(const std::vector<std::string>& ids,const std::string& root) {
    std::vector<RouteNode> result;
    bool found=false;
    for(const auto& id:ids) {
        for(const auto& prior:result) if(prior.id==id) throw std::runtime_error("duplicate GPU identity");
        result.push_back({id,id==root?"":root});found|=id==root;
    }
    if(!found) throw std::runtime_error("selected GPU disappeared");
    return result;
}
}
