#include "diagnostics.hpp"
#if GPU_SWITCH_DIAGNOSTICS
#include <EGL/egl.h>
#include <cstdlib>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fcntl.h>
#include <unistd.h>
namespace GpuSwitch::Diagnostics {
void trace(const char* phase) noexcept {
    try {
        std::filesystem::path state;
        if(const auto value=std::getenv("XDG_STATE_HOME");value && *value && std::filesystem::path(value).is_absolute())state=value;
        else if(const auto home=std::getenv("HOME");home && *home)state=std::filesystem::path(home)/".local/state";
        else return;
        state/="hyprland-gpu-switch";
        std::error_code error;
        std::filesystem::create_directories(state,error);
        if(error)return;
        const auto path=state/"migration-trace.log";
        int fd=open(path.c_str(),O_WRONLY|O_CREAT|O_APPEND|O_CLOEXEC|O_NOFOLLOW,0600);
        if(fd<0)return;
        dprintf(fd,"%lld pid=%d phase=%s display=%p context=%p\n",static_cast<long long>(time(nullptr)),getpid(),phase,eglGetCurrentDisplay(),eglGetCurrentContext());
        close(fd);
    } catch(...) {} // Diagnostics must never interrupt GPU migration.
}
}
#endif
