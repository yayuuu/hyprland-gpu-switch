#include "../transfer/queued_state.hpp"
#include <hyprutils/signal/Signal.hpp>
#include <iostream>
#include <stdexcept>
using namespace Hyprutils::Memory;
struct State{int value=42;};
int main(){
    Hyprutils::Signal::CSignalT<CWeakPointer<State>> queued;
    auto owner=makeUnique<State>();CWeakPointer<State> weak=owner;
    unsigned calls=0;
    auto listener=queued.listen([&](CWeakPointer<State> reference){
        if(auto* state=Offload::borrowQueuedState(reference)){
            if(state->value!=42)throw std::runtime_error("Borrowed invalid queue entry");
            ++calls;
        }
    });
    queued.emit(weak);
    if(calls!=1 || Offload::borrowQueuedState(weak)!=owner.get())throw std::runtime_error("Failed to borrow unique-owned queued state");
    owner.reset();queued.emit(weak);
    if(calls!=1 || Offload::borrowQueuedState(weak))throw std::runtime_error("Expired queue entry was accessed");
    std::cout<<"PASS: unique-owned queued state is borrowed without lock(); expired events are ignored\n";
}
