#pragma once
#include "../vendor/aquamarine/Renderer.hpp"
namespace GpuSwitch {
using BlitFunction=Aquamarine::CDRMRenderer::SBlitResult(*)(Aquamarine::CDRMRenderer*,Hyprutils::Memory::CSharedPointer<Aquamarine::IBuffer>,Hyprutils::Memory::CSharedPointer<Aquamarine::IBuffer>,Hyprutils::Memory::CSharedPointer<Aquamarine::CDRMRenderer>,int);
struct TransferStats {uint64_t frames=0,optimized=0,allocations=0,mapped=0,bytes=0,nanoseconds=0;};
Aquamarine::CDRMRenderer::SBlitResult optimizedTransfer(Aquamarine::CDRMRenderer*,Hyprutils::Memory::CSharedPointer<Aquamarine::IBuffer>,Hyprutils::Memory::CSharedPointer<Aquamarine::IBuffer>,Hyprutils::Memory::CSharedPointer<Aquamarine::CDRMRenderer>,int,BlitFunction,bool,TransferStats&);
}
