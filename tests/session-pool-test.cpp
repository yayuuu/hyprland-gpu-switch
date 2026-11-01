#include "../transfer/session-pool.hpp"
#include <array>
#include <cassert>
#include <iostream>
struct Session {
    int surface;
    int generation=0;
    std::array<int,3> slots{};
    bool pendingFence=false;
};
int main(){
    Offload::SessionPool<Session> pool(3);
    auto acquire=[&](int surface){return pool.acquire([&](const auto& s){return s.surface==surface;},[&]{return std::make_unique<Session>(Session{surface});});};
    auto* discord=acquire(1);auto* browser=acquire(2);
    discord->generation=12;discord->slots[0]=101;discord->pendingFence=true;
    browser->generation=4;browser->slots[0]=202;
    auto* popup=acquire(3);
    assert(discord==acquire(1) && browser==acquire(2));
    assert(discord->slots[0]==101 && browser->slots[0]==202);
    assert(discord->generation==12 && browser->generation==4);
    assert(!acquire(4)); // Capacity cannot evict a pending GPU read.
    pool.eraseIf([](auto& s){return s.surface==1 && !s.pendingFence;});
    assert(pool.size()==3 && discord==acquire(1));
    pool.eraseIf([&](auto& s){return &s==popup;});
    assert(acquire(4) && browser==acquire(2) && discord==acquire(1));
    discord->pendingFence=false;
    pool.eraseIf([](auto& s){return s.surface==1 && !s.pendingFence;});
    assert(pool.size()==2 && browser==acquire(2) && browser->slots[0]==202);
    // A reused external identity receives fresh cache/slots, never old state.
    auto* replacement=acquire(1);
    assert(replacement && replacement->generation==0 && replacement->slots[0]==0);
    std::cout<<"PASS: independent surface caches, stable callbacks, bounded capacity and fence-aware retirement\n";
}
