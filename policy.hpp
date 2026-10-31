#pragma once
#include <algorithm>
#include <optional>
#include <string>
#include <vector>
namespace GpuSwitch {
enum class Role { Unknown, Integrated, Discrete };
struct Gpu {
    std::string id, card;
    Role role = Role::Unknown;
    std::vector<std::string> connected;
};
inline std::optional<std::string> choose(const std::vector<Gpu>& gpus, const std::string& current) {
    // Stable ordering supplied by probe. Retain current dGPU if several qualify.
    for (const auto& g : gpus)
        if (g.id == current && g.role == Role::Discrete && !g.connected.empty()) return g.id;
    for (const auto& g : gpus)
        if (g.role == Role::Discrete && !g.connected.empty()) return g.id;
    for (const auto& g : gpus)
        if (g.id == current && g.role == Role::Integrated) return g.id;
    for (const auto& g : gpus)
        if (g.role == Role::Integrated) return g.id;
    return std::nullopt;
}
struct Plan {
    std::optional<std::string> target;
    unsigned long generation = 0;
};
// Even if a monitor disappears and reappears on the same GPU, the old plan expires.
inline bool valid(const Plan& plan, const std::vector<Gpu>& live,
                  const std::string& current, unsigned long generation) {
    return plan.generation == generation && plan.target && plan.target == choose(live, current);
}
}
