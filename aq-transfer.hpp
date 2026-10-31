#pragma once
#include <hyprutils/memory/SharedPtr.hpp>
namespace Aquamarine {class IBuffer;class CDRMRenderer;class CBackend;}
namespace GpuSwitch {
bool transferRendererValid(const Hyprutils::Memory::CSharedPointer<Aquamarine::CDRMRenderer>& renderer);
void recoverTransferRenderer(const Hyprutils::Memory::CSharedPointer<Aquamarine::CBackend>& backend,int renderFD,
                             Hyprutils::Memory::CSharedPointer<Aquamarine::CDRMRenderer>& renderer);
enum class TransferMode { DirectImport, CpuReadback };
TransferMode prepareTransferInput(const Hyprutils::Memory::CSharedPointer<Aquamarine::IBuffer>& buffer,
                                 const Hyprutils::Memory::CSharedPointer<Aquamarine::CDRMRenderer>& destination,
                                 bool forceCpu=false);
}
