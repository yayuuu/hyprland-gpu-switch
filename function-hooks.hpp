#pragma once
#include <hyprland/src/plugins/PluginAPI.hpp>
#include <cxxabi.h>
#include <cstdlib>
#include <memory>

namespace GpuSwitch {
// Bind the checked signature to the same mangled symbol and address. The API's
// separately cached demangled listing can be out of step with its symbol list.
inline std::vector<SFunctionMatch> findHookFunctions(HANDLE handle, const std::string& name) {
    std::vector<SFunctionMatch> matches;
    for (auto match : HyprlandAPI::findFunctionsByName(handle, name)) {
        int status = 0;
        std::unique_ptr<char, decltype(&std::free)> demangled(
            abi::__cxa_demangle(match.signature.c_str(), nullptr, nullptr, &status), &std::free);
        if (status != 0 || !demangled) continue;
        match.demangled = demangled.get();
        matches.push_back(std::move(match));
    }
    return matches;
}
}
