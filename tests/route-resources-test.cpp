#include "route-resources.hpp"
#include <stdexcept>
#include <iostream>
static void check(bool ok){if(!ok)throw std::runtime_error("DPMS route resource regression");}
int main(){
    bool ready=true, fail=false;int allocator=1,chain=1,inits=0,replacements=0;
    auto restore=[&]{
        GpuSwitch::restoreRouteResources(
            [&]{return ready;},
            [&]{++inits;if(fail)throw std::runtime_error("unavailable");ready=true;++allocator;},
            [&]{check(ready);},
            [&]{return chain==allocator;},
            [&]{check(ready);chain=allocator;++replacements;});
    };
    restore();check(inits==0 && replacements==0);
    for(int cycle=0;cycle<100;++cycle){
        ready=false;restore();check(chain==allocator && ready);
        restore();check(inits==cycle+1 && replacements==cycle+1);
    }
    // Another output may have already recreated the shared backend.
    ++allocator;restore();check(inits==100 && replacements==101);
    ready=false;fail=true;int oldChain=chain;
    bool failed=false;
    try{restore();}catch(const std::runtime_error&){failed=true;}
    check(failed);
    check(chain==oldChain && !ready);
    fail=false;restore();check(ready && chain==allocator && replacements==102);
    std::cout<<"PASS: repeated DPMS, shared-backend recreation and failed initialization/retry\n";
}
