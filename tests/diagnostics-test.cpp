#include "diagnostics/diagnostics.hpp"
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
namespace fs=std::filesystem;
static void require(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
int main(){
    char temporary[]="/tmp/hypr-gpu-diagnostics-XXXXXX";
    auto path=mkdtemp(temporary);if(!path)return 1;
    const fs::path root=path;
    try {
        setenv("XDG_STATE_HOME",root.c_str(),1);
        GpuSwitch::Diagnostics::trace("test-stage");
        auto log=root/"hyprland-gpu-switch/migration-trace.log";
#if GPU_SWITCH_DIAGNOSTICS
        require(fs::exists(log),"Diagnostic trace not created in XDG_STATE_HOME");
        std::ifstream stream(log);std::string line;std::getline(stream,line);
        require(line.find("phase=test-stage")!=std::string::npos,"Missing migration stage");
        struct stat info{};require(stat(log.c_str(),&info)==0 && (info.st_mode&0777)==0600,"Trace permissions are not private");
        fs::remove(log);
        const auto victim=root/"victim";{std::ofstream file(victim);file<<"unchanged";}
        fs::create_symlink(victim,log);
        GpuSwitch::Diagnostics::trace("must-not-follow-symlink");
        std::ifstream protectedFile(victim);std::getline(protectedFile,line);
        require(line=="unchanged","Trace followed a symlink");
        fs::remove(log);
        auto home=root/"home";fs::create_directories(home);setenv("HOME",home.c_str(),1);
        setenv("XDG_STATE_HOME","relative-state",1);
        GpuSwitch::Diagnostics::trace("home-fallback");
        require(fs::exists(home/".local/state/hyprland-gpu-switch/migration-trace.log"),"Invalid XDG path did not fall back to HOME");
        setenv("XDG_STATE_HOME","/proc/hyprland-gpu-switch-test",1);
        GpuSwitch::Diagnostics::trace("unwritable-state");
        std::cout<<"PASS: diagnostic trace, XDG/HOME paths, private permissions, symlink rejection and nonfatal I/O failure\n";
#else
        require(!fs::exists(log) && !fs::exists(root/"hyprland-gpu-switch"),"Release build created diagnostic output");
        std::cout<<"PASS: release build does not create diagnostic files or directories\n";
#endif
        fs::remove_all(root);return 0;
    }catch(const std::exception& error){std::cerr<<error.what()<<'\n';fs::remove_all(root);return 1;}
}
