#include "singlepass-policy.hpp"
#include "capture-wait.hpp"
#include <hyprutils/memory/UniquePtr.hpp>
#include <hyprutils/memory/WeakPtr.hpp>
#include <iostream>
#include <stdexcept>
struct Frame { bool finished=false,m_copyInFlight=false; bool done()const{return finished;} };
static void check(bool ok){if(!ok)throw std::runtime_error("rendering policy regression");}
int main(){
    Singlepass::SConditions c{true,true,true,true,true,true,true,true,true};
    check(Singlepass::skipReason(c).empty());
    for(auto member:{&Singlepass::SConditions::enabled,&Singlepass::SConditions::targetOutput,
        &Singlepass::SConditions::normalGLFrame,&Singlepass::SConditions::hdrFP16,
        &Singlepass::SConditions::noCapture,&Singlepass::SConditions::noPostprocessing,
        &Singlepass::SConditions::oneOpaqueFullscreenSurface,&Singlepass::SConditions::fullDamage,
        &Singlepass::SConditions::identityInputConversion}){
        c.*member=false;check(!Singlepass::skipReason(c).empty());c.*member=true;
    }
    auto frame=Hyprutils::Memory::makeUnique<Frame>();
    Hyprutils::Memory::CWeakPointer<Frame> pending=frame;
    check(pendingCaptureFrame(pending));frame->m_copyInFlight=true;check(!pendingCaptureFrame(pending));
    frame->m_copyInFlight=false;frame->finished=true;check(!pendingCaptureFrame(pending));
    frame.reset();check(!pendingCaptureFrame(pending));
    std::cout<<"PASS: all single-pass safety gates and unique-owned capture frame lifetime\n";
}
