#include "transfer/retained-source.hpp"
#include <sys/eventfd.h>
#include <iostream>
#include <stdexcept>
static void check(bool ok){if(!ok)throw std::runtime_error("retained source ownership regression");}
int main(){
    Offload::FD original(eventfd(0,EFD_CLOEXEC));check(original.value>=0);
    Aquamarine::SDMABUFAttrs source{};source.success=true;source.planes=1;
    source.size={3440,1440};source.format=0x48344258;source.modifier=0;source.strides[0]=27648;source.fds[0]=original.value;
    int retainedFD=-1;
    {
        Offload::RetainedSource retained(source);retainedFD=retained.attrs.fds[0];
        check(retainedFD!=original.value && (fcntl(retainedFD,F_GETFD)&FD_CLOEXEC));
        original.reset();source.fds.fill(-1);source.planes=0;
        check(retained.attrs.planes==1 && retained.attrs.size==Hyprutils::Math::Vector2D(3440,1440));
        check(retained.attrs.strides[0]==27648 && retained.attrs.modifier==0);
        uint64_t value=7,readback=0;check(write(retainedFD,&value,sizeof(value))==sizeof(value));
        check(read(retainedFD,&readback,sizeof(readback))==sizeof(readback) && readback==7);
    }
    check(fcntl(retainedFD,F_GETFD)==-1 && errno==EBADF);
    bool rejected=false;try{Offload::RetainedSource invalid(source);}catch(...){rejected=true;}check(rejected);
    std::cout<<"PASS: source planes survive wl_buffer FD closure with unchanged layout; owned duplicates are closed\n";
}
