#pragma once
#include "protocol.hpp"
#include <aquamarine/buffer/Buffer.hpp>
#include <array>
#include <fcntl.h>
#include <stdexcept>

namespace Offload {
// wl_buffer destruction may close the core's plane FDs while its texture is
// still current. Keep a separate borrowed view backed by our own plane owners.
struct RetainedSource {
    Aquamarine::SDMABUFAttrs attrs;
    std::array<FD,4> planes;
    explicit RetainedSource(const Aquamarine::SDMABUFAttrs& source):attrs(source) {
        if(!source.success || source.planes<1 || source.planes>4)
            throw std::runtime_error("Invalid retained DMA-BUF layout");
        attrs.fds.fill(-1);
        for(int i=0;i<source.planes;++i) {
            planes[i].reset(fcntl(source.fds[i],F_DUPFD_CLOEXEC,0));
            if(planes[i].value<0)throw std::runtime_error("Cannot retain source DMA-BUF plane");
            attrs.fds[i]=planes[i].value;
        }
    }
};
}
