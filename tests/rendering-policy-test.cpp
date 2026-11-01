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
    using Kind=Singlepass::EPassEntry;
    const int scope=0, nested=0;
    // Diablo II's 0.57 pass: scope begin, Wine parent, opaque HDR child,
    // scope end, software cursor. Preserve original indices for draw/restore.
    std::vector<Singlepass::SPassEntry> pass{{Kind::ScopeBegin,&scope},{},{},{Kind::ScopeEnd,&scope},{}};
    const auto content=Singlepass::passContentIndices(pass);
    check(content && *content==std::vector<size_t>{1,2,4});
    pass.insert(pass.begin()+2,{Kind::ScopeBegin,&nested});
    pass.insert(pass.begin()+4,{Kind::ScopeEnd,&nested});
    check(Singlepass::passContentIndices(pass)==std::optional{std::vector<size_t>{1,3,6}});
    // An overlay is still content; markers must never hide an extra drawable.
    pass.push_back({});
    check(Singlepass::passContentIndices(pass)==std::optional{std::vector<size_t>{1,3,6,7}});
    pass[4].scope=&scope;check(!Singlepass::passContentIndices(pass)); // mismatched nesting
    pass[4].scope=&nested;pass.erase(pass.begin());check(!Singlepass::passContentIndices(pass)); // orphan end
    pass={{Kind::ScopeBegin,&scope},{}};check(!Singlepass::passContentIndices(pass)); // unclosed
    pass={{Kind::ScopeBegin,nullptr},{Kind::ScopeEnd,nullptr}};check(!Singlepass::passContentIndices(pass));
    pass={{},{}};check(Singlepass::passContentIndices(pass)==std::optional{std::vector<size_t>{0,1}}); // 0.56
    auto frame=Hyprutils::Memory::makeUnique<Frame>();
    Hyprutils::Memory::CWeakPointer<Frame> pending=frame;
    check(pendingCaptureFrame(pending));frame->m_copyInFlight=true;check(!pendingCaptureFrame(pending));
    frame->m_copyInFlight=false;frame->finished=true;check(!pendingCaptureFrame(pending));
    frame.reset();check(!pendingCaptureFrame(pending));
    std::cout<<"PASS: single-pass safety gates, balanced 0.57 backdrop scopes, overlay preservation and capture frame lifetime\n";
}
