#pragma once
#include "policy.hpp"
#include <map>
#include <filesystem>
#include "json.hpp"
namespace GpuSwitch {
struct RoleInfo { Role role = Role::Unknown; std::string name; };
using Roles = std::map<std::string, RoleInfo>;
Roles discoverRoles();
std::string deviceId(const std::filesystem::path& device);
std::vector<Gpu> scan(const Roles&, const std::filesystem::path& root = "/sys/class/drm");
GpuSwitch::Json describe(const std::vector<Gpu>&, const Roles&);
}
