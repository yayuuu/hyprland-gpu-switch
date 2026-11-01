#include "../transfer/idle-policy.hpp"
#include <cassert>
#include <iostream>
int main(){
    using Offload::keepWorkerWarm;
    assert(keepWorkerWarm(true,true,true,true,false)); // Hidden iGPU window, external output only.
    assert(keepWorkerWarm(true,true,true,false,true)); // Attached dGPU producer.
    assert(!keepWorkerWarm(true,true,true,false,false)); // Abrupt dGPU output removal permits sleep.
    assert(!keepWorkerWarm(true,true,false,true,true)); // All displays in DPMS off.
    assert(!keepWorkerWarm(false,true,true,true,true)); // Closed/unmapped window.
    assert(!keepWorkerWarm(true,false,true,true,true)); // Disabled transport.
    assert(!keepWorkerWarm(true,true,true,false,false)); // Unknown source is not presumed integrated.
    std::cout<<"PASS: workspace changes keep workers warm; close, DPMS, disabled transport and detached discrete GPU release them\n";
}
