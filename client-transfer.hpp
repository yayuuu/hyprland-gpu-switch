#pragma once
#include <hyprland/src/plugins/PluginAPI.hpp>
namespace GpuSwitch {
void initClientTransfer(HANDLE handle);
void exitClientTransfer();
std::string clientTransferStatus();
}

namespace Aquamarine { struct SDMABUFAttrs; }
namespace Render { class ITexture; namespace GL { class CGLTexture; } }
namespace GpuSwitch {
using TransferTextureFactory=Hyprutils::Memory::CSharedPointer<Render::ITexture> (*)(void*,const Aquamarine::SDMABUFAttrs&,bool);
using TransferTextureBind=void (*)(Render::GL::CGLTexture*);
Hyprutils::Memory::CSharedPointer<Render::ITexture> createClientTransferTexture(void*,const Aquamarine::SDMABUFAttrs&,bool,TransferTextureFactory,bool& handled);
bool bindClientTransferTexture(Render::GL::CGLTexture*,TransferTextureBind);
}

class IPassElement;
namespace GpuSwitch {
bool drawTransferredSurface(Hyprutils::Memory::CUniquePointer<IPassElement>&,const CRegion&,bool fullscreen);
}
